/*
 * mochisandbox — native-first MochiBerry module for isolated JS execution.
 *
 * Implements a Lua-_ENV-style sandbox: each sandbox is a FRESH engine
 * RUNTIME + CONTEXT (own atom table, own GC, own memory limit, own
 * interrupt handler). Untrusted code runs in that realm; it cannot see the
 * host's globals (process, dynalib, fetch, ...) because a fresh context only
 * has the standard JS intrinsics, not the host's injected globals.
 *
 * Enforcement:
 *   - realm isolation      (MBJS_NewRuntime + MBJS_NewContext)
 *   - memory limit         (MBJS_SetMemoryLimit, sandbox runtime only)
 *   - instruction budget   (MBJS_SetInterruptHandler -> deadline check)
 *   - strict mode          (neutralize eval / Function / constructor chain)
 *
 * Values cross back to the host as JSON strings only (a value from one
 * runtime cannot safely be used in another). setGlobal(id, nameLit, valLit)
 * accepts primitives as JSON literals.
 *
 * This module resolves all MBJS_* engine symbols from the host image at load
 * time (mbjs_runtime.h pattern + the extra sandbox symbols below). It must
 * NOT link against mbjs.exe.
 */
#include "mbjs_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* Extra engine symbols the base MBJSRuntime struct does not carry.   */
/* ------------------------------------------------------------------ */
typedef struct SboxSym {
    MBJSRuntime *(*NewRuntime)(void);
    void (*FreeRuntime)(MBJSRuntime *rt);
    MBJSContext *(*NewContext)(MBJSRuntime *rt);
    void (*FreeContext)(MBJSContext *ctx);
    MBJSValue (*Eval2)(MBJSContext *ctx, const char *input, size_t len,
                       MBJSEvalOptions *options);
    void (*SetMemoryLimit)(MBJSRuntime *rt, size_t limit);
    void (*SetInterruptHandler)(MBJSRuntime *rt, MBJSInterruptHandler *cb,
                                void *opaque);
    MBJSValue (*GetGlobalObject)(MBJSContext *ctx);
    MBJSValue (*GetPropertyStr)(MBJSContext *ctx, MBJSValueConst obj, const char *name);
    MBJSValue (*GetException)(MBJSContext *ctx);
    MBJSValue (*Call)(MBJSContext *ctx, MBJSValueConst fn, MBJSValueConst this_obj,
                      int argc, MBJSValueConst *argv);
    MBJSValue (*NewStringLen)(MBJSContext *ctx, const char *str, size_t len);
    const char *(*ToCStringLen2)(MBJSContext *ctx, size_t *plen, MBJSValueConst v, int cesu8);
    int (*ToFloat64)(MBJSContext *ctx, double *d, MBJSValueConst v);
    int (*ToBool)(MBJSContext *ctx, MBJSValueConst v);
    void (*FreeValue)(MBJSContext *ctx, MBJSValue v);
    MBJSValue (*DupValue)(MBJSContext *ctx, MBJSValueConst v);
    /* microtask / promise plumbing, so sandbox code can settle promises */
    bool (*IsJobPending)(MBJSRuntime *rt);
    int (*ExecutePendingJob)(MBJSRuntime *rt, MBJSContext **pctx);
    int (*PromiseState)(MBJSContext *ctx, MBJSValueConst promise);
    MBJSValue (*PromiseResult)(MBJSContext *ctx, MBJSValueConst promise);
    void (*PromiseMarkAsHandled)(MBJSContext *ctx, MBJSValueConst promise);
} SboxSym;

static MBJSRuntime rt;          /* host runtime symbols (mbjs_runtime.h) */
static SboxSym sx;              /* extra sandbox symbols */
static int sx_ready = 0;

static void *host_image(void)
{
#ifdef _WIN32
    return (void *)GetModuleHandleA(NULL);
#else
    return dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
#endif
}

static void *sym_of(void *host, const char *name)
{
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)host, name);
#else
    return dlsym(host, name);
#endif
}

static const char *sbox_resolve(void)
{
    void *host;
    if (sx_ready) return NULL;
    host = host_image();
    if (!host) return "host image";
#define SYM(field, name) do { \
        void *p__ = sym_of(host, (name)); \
        if (!p__) return (name); \
        memcpy(&(sx.field), &p__, sizeof(p__)); \
    } while (0)
    SYM(NewRuntime, "MBJS_NewRuntime");
    SYM(FreeRuntime, "MBJS_FreeRuntime");
    SYM(NewContext, "MBJS_NewContext");
    SYM(FreeContext, "MBJS_FreeContext");
    SYM(Eval2, "MBJS_Eval2");
    SYM(SetMemoryLimit, "MBJS_SetMemoryLimit");
    SYM(SetInterruptHandler, "MBJS_SetInterruptHandler");
    SYM(GetGlobalObject, "MBJS_GetGlobalObject");
    SYM(GetPropertyStr, "MBJS_GetPropertyStr");
    SYM(GetException, "MBJS_GetException");
    SYM(Call, "MBJS_Call");
    SYM(NewStringLen, "MBJS_NewStringLen");
    SYM(ToCStringLen2, "MBJS_ToCStringLen2");
    SYM(ToFloat64, "MBJS_ToFloat64");
    SYM(ToBool, "MBJS_ToBool");
    SYM(FreeValue, "MBJS_FreeValue");
        SYM(DupValue, "MBJS_DupValue");
        SYM(IsJobPending, "MBJS_IsJobPending");
        SYM(ExecutePendingJob, "MBJS_ExecutePendingJob");
        SYM(PromiseState, "MBJS_PromiseState");
        SYM(PromiseResult, "MBJS_PromiseResult");
        SYM(PromiseMarkAsHandled, "MBJS_PromiseMarkAsHandled");
    #undef SYM
    sx_ready = 1;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Sandbox registry                                                    */
/* ------------------------------------------------------------------ */
typedef struct Sandbox {
    uint64_t id;
    MBJSRuntime *rt2;
    MBJSContext *ctx2;
    int64_t budget_ms;          /* 0 = unlimited */
    int64_t deadline_ms;
    int strict;
    int alive;
} Sandbox;

#define MAX_SANDBOX 64
static Sandbox sboxes[MAX_SANDBOX];
static uint64_t next_id = 1;

static int64_t now_ms(void)
{
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* interrupt handler: return non-zero to abort the running eval */
static int sbox_interrupt(MBJSContext *ctx, void *opaque)
{
    Sandbox *sb = (Sandbox *)opaque;
    (void)ctx;
    /* deadline_ms == 0 means the budget is not armed (boot/setGlobal paths) */
    if (sb && sb->budget_ms > 0 && sb->deadline_ms > 0 &&
        now_ms() >= sb->deadline_ms)
        return 1;
    return 0;
}

static Sandbox *find_sbox(uint64_t id)
{
    int i;
    for (i = 0; i < MAX_SANDBOX; i++)
        if (sboxes[i].alive && sboxes[i].id == id)
            return &sboxes[i];
    return NULL;
}

static int alloc_sbox(void)
{
    int i;
    for (i = 0; i < MAX_SANDBOX; i++)
        if (!sboxes[i].alive)
            return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* JS helpers                                                          */
/* ------------------------------------------------------------------ */
/* IMPORTANT (engine contract, verified in mbjs-core.c):
 *   MBJS_ToCStringLen2 returns a pointer INTO a MBJSString. For a plain
 *   ASCII string that is a BORROWED view (do NOT FreeCString), for anything
 *   needing conversion it is an owned allocation (FreeCString required).
 *   The caller cannot distinguish the two, so the SAFE pattern is to copy
 *   the bytes into a malloc'd buffer and NEVER call MBJS_FreeCString on the
 *   returned pointer. (Leaking the rare non-ASCII conversion string is
 *   bounded by the sandbox lifetime and strictly better than corrupting the
 *   heap by underflowing the string refcount.)
 */
static char *copy_cstr(const char *p, size_t len)
{
    char *out;
    if (!p) return NULL;
    out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, p, len);
    out[len] = 0;
    return out;
}

static MBJSValue host_str(MBJSContext *ctx, const char *s, size_t len)
{
    return rt.NewStringLen(ctx, s ? s : "", s ? len : 0);
}

static char *host_cstr(MBJSContext *ctx, MBJSValueConst v, size_t *out_len)
{
    size_t len = 0;
    const char *p = rt.ToCStringLen(ctx, &len, v);
    char *out = copy_cstr(p, len);       /* never FreeCString */
    if (out_len) *out_len = len;
    return out;
}

static MBJSValue host_error(MBJSContext *ctx, const char *what)
{
    rt.ThrowTypeError(ctx, "mochisandbox: %s", what);
    return MBJS_EXCEPTION; /* contract: a C func that threw MUST return MBJS_EXCEPTION */
}

/* Eval a source in a sandbox ctx with proper MBJSEvalOptions. */
static MBJSValue sbox_eval(Sandbox *sb, const char *src, size_t len,
                           const char *filename, int flags)
{
    MBJSEvalOptions opts;
    opts.version = MBJS_EVAL_OPTIONS_VERSION;
    opts.eval_flags = flags;
    opts.filename = filename ? filename : "<sandbox>";
    opts.line_num = 1;
    return sx.Eval2(sb->ctx2, src, len, &opts);
}

/* Read an exception message from a sandbox ctx into a malloc'd string. */
static char *sbox_exc_message(Sandbox *sb)
{
    MBJSValue ex = sx.GetException(sb->ctx2);
    const char *p;
    size_t len = 0;
    char *out;
    if (MBJS_IsException(ex)) { sx.FreeValue(sb->ctx2, ex); return NULL; }
    p = sx.ToCStringLen2(sb->ctx2, &len, ex, 0);
    out = copy_cstr(p, len);             /* never FreeCString */
    sx.FreeValue(sb->ctx2, ex);
    return out;
}

/* JSON-stringify a sandbox value into a malloc'd string (sandbox ctx). */
static char *sbox_json(Sandbox *sb, MBJSValueConst v)
{
    MBJSValue g, json, jsonfn, r;
    MBJSValue args[1];
    char *out = NULL;
    size_t len = 0;
    const char *p;

    g = sx.GetGlobalObject(sb->ctx2);
    json = sx.GetPropertyStr(sb->ctx2, g, "JSON");
    sx.FreeValue(sb->ctx2, g);
    if (MBJS_IsException(json)) { sx.FreeValue(sb->ctx2, json); return NULL; }
    jsonfn = sx.GetPropertyStr(sb->ctx2, json, "stringify");
    sx.FreeValue(sb->ctx2, json);
    if (MBJS_IsException(jsonfn)) { sx.FreeValue(sb->ctx2, jsonfn); return NULL; }
    args[0] = sx.DupValue(sb->ctx2, v);
    r = sx.Call(sb->ctx2, jsonfn, MBJS_UNDEFINED, 1, args);
    sx.FreeValue(sb->ctx2, args[0]);
    sx.FreeValue(sb->ctx2, jsonfn);
    if (MBJS_IsException(r)) { sx.FreeValue(sb->ctx2, r); return NULL; }
    p = sx.ToCStringLen2(sb->ctx2, &len, r, 0);
    out = copy_cstr(p, len);             /* never FreeCString */
    sx.FreeValue(sb->ctx2, r);
    return out;
}

/* Neutralize dynamic code (eval / Function) in a sandbox ctx. */
static int sbox_neutralize(Sandbox *sb)
{
    static const char boot[] =
        "(function(){var realFn=Function;"
        "function deny(){throw new Error('mochisandbox: dynamic code disabled');}"
        /* Capture every code-compiling intrinsic BEFORE neutering: the plain
           Function ctor is not the only one - GeneratorFunction / AsyncFunction
           / AsyncGeneratorFunction compile source too, and they are reachable
           via Object.getPrototypeOf(fn).constructor. */
        "var gens=[];"
        "try{gens.push(Object.getPrototypeOf(function*(){}).constructor);}catch(e){}"
        "try{gens.push(Object.getPrototypeOf(async function(){}).constructor);}catch(e){}"
        "try{gens.push(Object.getPrototypeOf(async function*(){}).constructor);}catch(e){}"
        "try{Object.defineProperty(Object.prototype,'constructor',"
        "{value:deny,writable:false,configurable:false});}catch(e){}"
        "try{Object.defineProperty(realFn.prototype,'constructor',"
        "{value:deny,writable:false,configurable:false});}catch(e){}"
        "for(var i=0;i<gens.length;i++){var C=gens[i];"
        "if(typeof C!=='function')continue;"
        "try{Object.defineProperty(C.prototype,'constructor',"
        "{value:deny,writable:false,configurable:false});}catch(e){}}"
        "globalThis.eval=deny;globalThis.Function=deny;"
        "return true;})()";
    MBJSValue r = sbox_eval(sb, boot, sizeof(boot) - 1, "<sbox-boot>", 0);
    if (MBJS_IsException(r)) { sx.FreeValue(sb->ctx2, r); return 0; }
    sx.FreeValue(sb->ctx2, r);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Exported C functions (called from JS with the HOST ctx)             */
/* ------------------------------------------------------------------ */
static MBJSValue sbox_create(MBJSContext *ctx, MBJSValueConst this_val,
                             int argc, MBJSValueConst *argv)
{
    double memlimit = 0, budget = 0;
    int strict = 0, idx;
    Sandbox *sb;
    const char *missing = sbox_resolve();

    (void)this_val;
    if (missing) return host_error(ctx, "symbols unavailable");
    if (argc >= 1) {
        MBJSValue v;
        v = sx.GetPropertyStr(ctx, argv[0], "memLimit");
        if (!MBJS_IsException(v)) { sx.ToFloat64(ctx, &memlimit, v); sx.FreeValue(ctx, v); }
        v = sx.GetPropertyStr(ctx, argv[0], "budgetMs");
        if (!MBJS_IsException(v)) { sx.ToFloat64(ctx, &budget, v); sx.FreeValue(ctx, v); }
        v = sx.GetPropertyStr(ctx, argv[0], "strict");
        if (!MBJS_IsException(v)) { strict = sx.ToBool(ctx, v) > 0; sx.FreeValue(ctx, v); }
    }
    idx = alloc_sbox();
    if (idx < 0) return host_error(ctx, "too many sandboxes");
    sb = &sboxes[idx];
    memset(sb, 0, sizeof(*sb));
    sb->rt2 = sx.NewRuntime();
    if (!sb->rt2) return host_error(ctx, "NewRuntime failed");
    sb->ctx2 = sx.NewContext(sb->rt2);
    if (!sb->ctx2) { sx.FreeRuntime(sb->rt2); return host_error(ctx, "NewContext failed"); }
    if (memlimit > 0) sx.SetMemoryLimit(sb->rt2, (size_t)memlimit);
    sb->budget_ms = budget > 0 ? (int64_t)budget : 0;
    sb->strict = strict;
    if (budget > 0) sx.SetInterruptHandler(sb->rt2, sbox_interrupt, sb);
    if (strict) sbox_neutralize(sb);
    sb->id = next_id++;
    sb->alive = 1;
    {
        char idbuf[32];
        int n = snprintf(idbuf, sizeof(idbuf), "%llu", (unsigned long long)sb->id);
        return host_str(ctx, idbuf, (size_t)n);
    }
}

/* setGlobal(id, nameLiteral, valueLiteral) — nameLiteral and valueLiteral are
   JSON literals produced by the JS binding (e.g. "\"log\"", "\"text\""). */
static MBJSValue sbox_setGlobal(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    Sandbox *sb;
    char *idstr = NULL, *nameLit = NULL, *valLit = NULL;
    char expr[1600];
    int n;
    MBJSValue res;

    (void)this_val;
    if (argc < 3) return host_error(ctx, "setGlobal(id, nameLiteral, valueLiteral)");
    idstr = host_cstr(ctx, argv[0], NULL);
    nameLit = host_cstr(ctx, argv[1], NULL);
    valLit = host_cstr(ctx, argv[2], NULL);
    if (!idstr || !nameLit || !valLit) {
        free(idstr); free(nameLit); free(valLit);
        return host_error(ctx, "arg parse");
    }
    sb = find_sbox((uint64_t)strtoull(idstr, NULL, 10));
    if (!sb || !sb->alive) {
        free(idstr); free(nameLit); free(valLit);
        return host_error(ctx, "unknown sandbox id");
    }
    n = snprintf(expr, sizeof(expr), "globalThis[%s]=%s;", nameLit, valLit);
    free(idstr); free(nameLit); free(valLit);
    if (n < 0 || (size_t)n >= sizeof(expr)) return host_error(ctx, "expr too long");
    res = sbox_eval(sb, expr, (size_t)n, "<sbox-set>", 0);
    if (MBJS_IsException(res)) { sx.FreeValue(sb->ctx2, res); return host_error(ctx, "setGlobal failed"); }
    sx.FreeValue(sb->ctx2, res);
    return MBJS_UNDEFINED;
}

static MBJSValue sbox_run(MBJSContext *ctx, MBJSValueConst this_val,
                          int argc, MBJSValueConst *argv)
{
    Sandbox *sb;
    char *idstr = NULL, *src = NULL;
    char *json = NULL;
    MBJSValue res;
    size_t srclen = 0;

    (void)this_val;
    if (argc < 2) return host_error(ctx, "run(id, source)");
    idstr = host_cstr(ctx, argv[0], NULL);
    src = host_cstr(ctx, argv[1], &srclen);
    if (!idstr || !src) { free(idstr); free(src); return host_error(ctx, "arg parse"); }
    sb = find_sbox((uint64_t)strtoull(idstr, NULL, 10));
    if (!sb || !sb->alive) { free(idstr); free(src); return host_error(ctx, "unknown sandbox id"); }

    sb->deadline_ms = sb->budget_ms > 0 ? (now_ms() + sb->budget_ms) : 0;
    res = sbox_eval(sb, src, srclen, "<sandbox>", 0);
    free(idstr); free(src);

    if (MBJS_IsException(res)) {
        char *msg = sbox_exc_message(sb);
        sx.FreeValue(sb->ctx2, res);
        if (msg) {
            MBJSValue e = host_error(ctx, msg);
            free(msg);
            return e;
        }
        return host_error(ctx, "exception in sandbox");
    }
    json = sbox_json(sb, res);
    sx.FreeValue(sb->ctx2, res);
    if (!json) return host_error(ctx, "result serialization failed");
    {
        MBJSValue out = host_str(ctx, json, strlen(json));
        free(json);
        return out;
    }
}

/* convert any sandbox value to a malloc'd C string (borrowed-pointer safe) */
static char *sbox_value_str(Sandbox *sb, MBJSValueConst v)
{
    size_t len = 0;
    const char *p = sx.ToCStringLen2(sb->ctx2, &len, v, 0);
    return copy_cstr(p, len);
}

/* runAsync(id, src) — evaluate a script in the realm; if the completion value
   is a promise, drain that realm's microtask queue (under the same interrupt
   deadline) and report the settled result.
   Returns a JSON string:
     {"state":"fulfilled","value":...}   settled OK (or a plain non-promise value)
     {"state":"pending"}                still pending after the pump cap
   A rejected promise throws on the host side with the rejection message. */
#define SBOX_PUMP_MAX 10000
static MBJSValue sbox_runAsync(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    Sandbox *sb;
    char *idstr = NULL, *src = NULL;
    char *value = NULL;
    int value_owned = 0;
    MBJSValue v;
    size_t srclen = 0;
    int jobs = 0;
    int st;

    (void)this_val;
    if (argc < 2) return host_error(ctx, "runAsync(id, source)");
    idstr = host_cstr(ctx, argv[0], NULL);
    src = host_cstr(ctx, argv[1], &srclen);
    if (!idstr || !src) { free(idstr); free(src); return host_error(ctx, "arg parse"); }
    sb = find_sbox((uint64_t)strtoull(idstr, NULL, 10));
    if (!sb || !sb->alive) { free(idstr); free(src); return host_error(ctx, "unknown sandbox id"); }

    sb->deadline_ms = sb->budget_ms > 0 ? (now_ms() + sb->budget_ms) : 0;
    v = sbox_eval(sb, src, srclen, "<sandbox-async>", 0);
    free(idstr); free(src);

    if (MBJS_IsException(v)) {
        char *msg = sbox_exc_message(sb);
        sx.FreeValue(sb->ctx2, v);
        if (msg) { MBJSValue e = host_error(ctx, msg); free(msg); return e; }
        return host_error(ctx, "exception in sandbox");
    }

    /* PromiseState returns NOT_A_PROMISE (-1) for a non-promise value. */
    st = sx.PromiseState(sb->ctx2, v);
    if (st == MBJS_PROMISE_NOT_A_PROMISE) {
        value = sbox_json(sb, v);
        sx.FreeValue(sb->ctx2, v);
        if (!value) value = (char *)"null";
        else value_owned = 1;
        goto fulfilled;
    }

    /* It is a promise: run this realm's microtasks. Each job re-enters the VM,
       so the interrupt handler keeps enforcing the same deadline. A job that
       throws must be cleared, else the realm keeps a pending exception. */
    while (jobs < SBOX_PUMP_MAX && sx.IsJobPending(sb->rt2)) {
        MBJSContext *jc = NULL;
        MBJSValue exc;
        if (!sx.ExecutePendingJob(sb->rt2, &jc))
            break;
        if (++jobs >= SBOX_PUMP_MAX)
            break;
        exc = sx.GetException(sb->ctx2);
        if (!MBJS_IsException(exc))
            sx.FreeValue(sb->ctx2, exc);
    }

    sx.PromiseMarkAsHandled(sb->ctx2, v);
    st = sx.PromiseState(sb->ctx2, v);
    if (st == MBJS_PROMISE_REJECTED) {
        char *reason = NULL;
        MBJSValue r = sx.PromiseResult(sb->ctx2, v);
        if (!MBJS_IsException(r)) {
            reason = sbox_value_str(sb, r);
            sx.FreeValue(sb->ctx2, r);
        }
        sx.FreeValue(sb->ctx2, v);
        if (reason) { MBJSValue e = host_error(ctx, reason); free(reason); return e; }
        return host_error(ctx, "promise rejected in sandbox");
    }
    if (st == MBJS_PROMISE_FULFILLED) {
        MBJSValue r = sx.PromiseResult(sb->ctx2, v);
        if (!MBJS_IsException(r)) {
            value = sbox_json(sb, r);
            sx.FreeValue(sb->ctx2, r);
            if (!value) value = (char *)"null";
            else value_owned = 1;
        }
    }
    sx.FreeValue(sb->ctx2, v);

fulfilled:
    if (!value) {
        MBJSValue ret = host_str(ctx, "{\"state\":\"pending\"}", 22);
        return ret;
    }
    {
        char buf[16384];
        int n = snprintf(buf, sizeof(buf),
                         "{\"state\":\"fulfilled\",\"value\":%s}", value);
        MBJSValue ret;
        if (value_owned) free(value);
        if (n < 0 || (size_t)n >= sizeof(buf))
            return host_error(ctx, "result too large");
        ret = host_str(ctx, buf, (size_t)n);
        return ret;
    }
}

static MBJSValue sbox_free(MBJSContext *ctx, MBJSValueConst this_val,
                           int argc, MBJSValueConst *argv)
{
    Sandbox *sb;
    char *idstr = NULL;
    (void)this_val;
    if (argc < 1) return MBJS_UNDEFINED;
    idstr = host_cstr(ctx, argv[0], NULL);
    if (!idstr) return MBJS_UNDEFINED;
    sb = find_sbox((uint64_t)strtoull(idstr, NULL, 10));
    free(idstr);
    if (sb && sb->alive) {
        if (sb->ctx2) sx.FreeContext(sb->ctx2);
        if (sb->rt2) sx.FreeRuntime(sb->rt2);
        memset(sb, 0, sizeof(*sb));
    }
    return MBJS_UNDEFINED;
}

static MBJSValue sbox_count(MBJSContext *ctx, MBJSValueConst this_val,
                            int argc, MBJSValueConst *argv)
{
    int n = 0, i;
    char buf[32];
    int k;
    (void)this_val; (void)argc; (void)argv;
    for (i = 0; i < MAX_SANDBOX; i++) if (sboxes[i].alive) n++;
    k = snprintf(buf, sizeof(buf), "%d", n);
    return host_str(ctx, buf, (size_t)k);
}

/* ------------------------------------------------------------------ */
/* Module registration                                                 */
/* ------------------------------------------------------------------ */
static const MBJSCFunctionListEntry sbox_tab[] = {
    MBJS_CFUNC_DEF("create", 1, sbox_create),
    MBJS_CFUNC_DEF("setGlobal", 3, sbox_setGlobal),
    MBJS_CFUNC_DEF("run", 2, sbox_run),
    MBJS_CFUNC_DEF("runAsync", 2, sbox_runAsync),
    MBJS_CFUNC_DEF("free", 1, sbox_free),
    MBJS_CFUNC_DEF("count", 0, sbox_count),
};
#define SBOX_TAB_N ((int)(sizeof(sbox_tab) / sizeof(sbox_tab[0])))

static int sbox_mod_init(MBJSContext *ctx, MBJSModuleDef *m)
{
    return rt.SetModuleExportList(ctx, m, sbox_tab, SBOX_TAB_N);
}

MBJS_MODULE_EXTERN MBJSModuleDef *mbjs_init_module(MBJSContext *ctx, const char *name)
{
    const char *missing = mbjs_runtime_resolve(&rt);
    MBJSModuleDef *m;
    if (missing) return NULL;
    if (sbox_resolve()) return NULL;
    m = rt.NewCModule(ctx, name ? name : "mochisandbox", sbox_mod_init);
    if (!m) return NULL;
    if (rt.AddModuleExportList(ctx, m, sbox_tab, SBOX_TAB_N) != 0)
        return NULL;
    return m;
}
