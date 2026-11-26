/*
 * mochinode_dynalib.c — expose the libdynalib C99 dynamic loader to the
 * MochiBerry engine (SEVM `mochi_modules/`).
 *
 * The C side keeps a flat handle table; the ergonomic API
 *     dynalib(file, reg_or_offset, symbol, abi_sig, opts)
 * is assembled in JavaScript by node_modules/dynalib/index.js.
 *
 * Native exports:
 *   openModule(file)              -> id | -1
 *   lastError()                   -> string
 *   moduleBase(id)                -> number
 *   moduleExports(id)             -> JSON string
 *   closeModule(id)               -> undefined
 *   resolveSymbol(modId, offset, symbol, abiSpec) -> id | -1
 *   symbolPtr/Base/Offset/Abi(id) -> number / string
 *   callSymbol(id, argc, a0..a5)  -> number
 *   freeSymbol(id)                -> undefined
 *   abiParse(spec)                -> JSON string
 *
 * Calling convention notes (honest limits, see README):
 *   - integer/pointer arguments are marshalled through the JIT trampoline;
 *   - floating-point arguments are rejected with an explicit error because the
 *     current emitter only shuffles general-purpose registers;
 *   - at most 4 register arguments (x86-64 SysV/MS64 have 4 GP argument
 *     registers); stack arguments are not forwarded.
 */

#include "mbjs_runtime.h"
#include "../libdynalib/dynalib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static MBJSRuntime rt;

/* ------------------------------------------------------------------ */
/* handle tables                                                        */
/* ------------------------------------------------------------------ */

#define MAX_MODULES 32
#define MAX_SYMBOLS 64

typedef struct {
    int          used;
    DynalibModule *mod;
} ModSlot;

typedef struct {
    int            used;
    DynalibSymbol *sym;
    void          *stub;
    void          *stub_end;
    DynalibAbiSpec abi;
    int            has_abi;   /* 0 = raw call, no type contract given */
} SymSlot;

static ModSlot g_modules[MAX_MODULES];
static SymSlot g_symbols[MAX_SYMBOLS];
static char    g_error[512];

static void set_error(const char *msg)
{
    if (!msg) msg = "unknown error";
    snprintf(g_error, sizeof g_error, "%s", msg);
}

static ModSlot *mod_by_id(int id)
{
    if (id < 0 || id >= MAX_MODULES || !g_modules[id].used) return NULL;
    return &g_modules[id];
}

static SymSlot *sym_by_id(int id)
{
    if (id < 0 || id >= MAX_SYMBOLS || !g_symbols[id].used) return NULL;
    return &g_symbols[id];
}

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static const char *arg_cstr(MBJSContext *ctx, MBJSValueConst v)
{
    return rt.ToCStringLen(ctx, NULL, v);
}

static int arg_int(MBJSContext *ctx, MBJSValueConst v, long *out)
{
    double d = 0;
    if (rt.ToFloat64(ctx, &d, v) < 0) return -1;
    *out = (long)d;
    return 0;
}

/* JSON string escaping for export names */
static void json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; in && *in && o + 8 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* module functions                                                     */
/* ------------------------------------------------------------------ */

static MBJSValue dl_open_module(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    const char *path;
    const char *err = NULL;
    DynalibModule *m;
    int slot;

    (void)this_val;
    g_error[0] = '\0';
    if (argc < 1)
        return MBJS_NewInt32(ctx, -1);

    path = arg_cstr(ctx, argv[0]);
    if (!path)
        return MBJS_NewInt32(ctx, -1);

    m = dynalib_open(path, DYNALIB_FMT_AUTO, &err);
    rt.FreeCString(ctx, path);
    if (!m) {
        set_error(err);
        return MBJS_NewInt32(ctx, -1);
    }

    for (slot = 0; slot < MAX_MODULES; slot++) {
        if (!g_modules[slot].used) {
            g_modules[slot].used = 1;
            g_modules[slot].mod = m;
            return MBJS_NewInt32(ctx, slot);
        }
    }
    dynalib_close(m);
    set_error("module table full");
    return MBJS_NewInt32(ctx, -1);
}

static MBJSValue dl_last_error(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return rt.NewStringLen(ctx, g_error, strlen(g_error));
}

static MBJSValue dl_module_base(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    long id = -1;
    ModSlot *s;
    (void)this_val;
    if (argc < 1 || arg_int(ctx, argv[0], &id) < 0)
        return MBJS_NewFloat64(ctx, 0.0);
    s = mod_by_id((int)id);
    if (!s)
        return MBJS_NewFloat64(ctx, 0.0);
    return MBJS_NewFloat64(ctx, (double)(uintptr_t)dynalib_module_base(s->mod));
}

static MBJSValue dl_module_exports(MBJSContext *ctx, MBJSValueConst this_val,
                                   int argc, MBJSValueConst *argv)
{
    long id = -1;
    ModSlot *s;
    DynalibExportEntry *entries = NULL;
    size_t total, cap = 512, i;
    char *buf;
    size_t used = 0;

    (void)this_val;
    if (argc < 1 || arg_int(ctx, argv[0], &id) < 0)
        return rt.NewStringLen(ctx, "[]", 2);
    s = mod_by_id((int)id);
    if (!s) {
        set_error("invalid module handle");
        return rt.NewStringLen(ctx, "[]", 2);
    }

    entries = (DynalibExportEntry *)calloc(cap, sizeof *entries);
    if (!entries) {
        set_error("oom");
        return rt.NewStringLen(ctx, "[]", 2);
    }

    total = dynalib_exports(s->mod, entries, cap);
    if (total > cap) {
        DynalibExportEntry *bigger =
            (DynalibExportEntry *)realloc(entries, total * sizeof *entries);
        if (bigger) {
            entries = bigger;
            total = dynalib_exports(s->mod, entries, total);
        }
    }

    buf = (char *)malloc(total * 192 + 16);
    if (!buf) {
        free(entries);
        set_error("oom");
        return rt.NewStringLen(ctx, "[]", 2);
    }

    used += (size_t)snprintf(buf + used, 16, "[");
    for (i = 0; i < total && i < cap; i++) {
        char name[DYNALIB_EXPORT_NAME_MAX * 2 + 8];
        json_escape(entries[i].name, name, sizeof name);
        used += (size_t)snprintf(buf + used, total * 192 + 16 - used,
                                 "%s{\"name\":\"%s\",\"rva\":%u,\"ordinal\":%u}",
                                 i ? "," : "", name,
                                 (unsigned)entries[i].rva,
                                 (unsigned)entries[i].ordinal);
    }
    used += (size_t)snprintf(buf + used, 16, "]");

    {
        MBJSValue v = rt.NewStringLen(ctx, buf, used);
        free(buf);
        free(entries);
        return v;
    }
}

static MBJSValue dl_close_module(MBJSContext *ctx, MBJSValueConst this_val,
                                 int argc, MBJSValueConst *argv)
{
    long id = -1;
    ModSlot *s;
    (void)this_val; (void)ctx;
    if (argc < 1 || arg_int(ctx, argv[0], &id) < 0)
        return MBJS_UNDEFINED;
    s = mod_by_id((int)id);
    if (s) {
        dynalib_close(s->mod);
        s->used = 0;
        s->mod = NULL;
    }
    return MBJS_UNDEFINED;
}

static MBJSValue dl_resolve_symbol(MBJSContext *ctx, MBJSValueConst this_val,
                                   int argc, MBJSValueConst *argv)
{
    long mod_id = -1, offset = 0;
    const char *symbol = NULL;
    const char *spec = NULL;
    const char *err = NULL;
    DynalibAbiSpec abi;
    int have_abi = 0, slot;
    ModSlot *m;
    DynalibSymbol *sym;
    void *stub_end = NULL;

    (void)this_val;
    g_error[0] = '\0';
    if (argc < 2)
        return MBJS_NewInt32(ctx, -1);
    if (arg_int(ctx, argv[0], &mod_id) < 0)
        return MBJS_NewInt32(ctx, -1);
    m = mod_by_id((int)mod_id);
    if (!m) {
        set_error("invalid module handle");
        return MBJS_NewInt32(ctx, -1);
    }
    if (arg_int(ctx, argv[1], &offset) < 0)
        offset = 0;

    if (argc >= 3 && argv[2].tag != MBJS_TAG_UNDEFINED &&
        argv[2].tag != MBJS_TAG_NULL) {
        symbol = arg_cstr(ctx, argv[2]);
    }
    if (argc >= 4 && argv[3].tag != MBJS_TAG_UNDEFINED &&
        argv[3].tag != MBJS_TAG_NULL) {
        spec = arg_cstr(ctx, argv[3]);
        if (spec && *spec) {
            if (dynalib_abi_parse(spec, &abi) != 0) {
                set_error("cannot parse ABI spec");
                if (symbol) rt.FreeCString(ctx, symbol);
                if (spec) rt.FreeCString(ctx, spec);
                return MBJS_NewInt32(ctx, -1);
            }
            have_abi = 1;
        }
    }

    sym = dynalib_resolve(m->mod, (uintptr_t)offset, symbol,
                           have_abi ? &abi : NULL, &err);
    if (symbol) rt.FreeCString(ctx, symbol);
    if (spec) rt.FreeCString(ctx, spec);

    if (!sym) {
        set_error(err ? err : "symbol unresolved");
        return MBJS_NewInt32(ctx, -1);
    }

    for (slot = 0; slot < MAX_SYMBOLS; slot++) {
        if (!g_symbols[slot].used) {
            g_symbols[slot].used = 1;
            g_symbols[slot].sym = sym;
            g_symbols[slot].abi = sym->abi;
            g_symbols[slot].has_abi = have_abi;
            g_symbols[slot].stub = dynalib_trampoline_emit(sym->fn_ptr,
                                                            &sym->abi, &stub_end);
            g_symbols[slot].stub_end = stub_end;
            return MBJS_NewInt32(ctx, slot);
        }
    }
    dynalib_symbol_free(sym);
    set_error("symbol table full");
    return MBJS_NewInt32(ctx, -1);
}

static MBJSValue dl_symbol_ptr(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    long id = -1;
    SymSlot *s;
    (void)this_val;
    if (argc < 1 || arg_int(ctx, argv[0], &id) < 0)
        return MBJS_NewFloat64(ctx, 0.0);
    s = sym_by_id((int)id);
    if (!s) return MBJS_NewFloat64(ctx, 0.0);
    return MBJS_NewFloat64(ctx, (double)(uintptr_t)s->sym->fn_ptr);
}

static MBJSValue dl_symbol_base(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    long id = -1;
    SymSlot *s;
    (void)this_val;
    if (argc < 1 || arg_int(ctx, argv[0], &id) < 0)
        return MBJS_NewFloat64(ctx, 0.0);
    s = sym_by_id((int)id);
    if (!s) return MBJS_NewFloat64(ctx, 0.0);
    return MBJS_NewFloat64(ctx,
        (double)(uintptr_t)dynalib_module_base(s->sym->module));
}

static MBJSValue dl_symbol_offset(MBJSContext *ctx, MBJSValueConst this_val,
                                  int argc, MBJSValueConst *argv)
{
    long id = -1;
    SymSlot *s;
    (void)this_val;
    if (argc < 1 || arg_int(ctx, argv[0], &id) < 0)
        return MBJS_NewFloat64(ctx, 0.0);
    s = sym_by_id((int)id);
    if (!s) return MBJS_NewFloat64(ctx, 0.0);
    return MBJS_NewFloat64(ctx, (double)(uintptr_t)s->sym->rva_offset);
}

static MBJSValue dl_symbol_abi(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    long id = -1;
    SymSlot *s;
    char buf[256];
    (void)this_val;
    if (argc < 1 || arg_int(ctx, argv[0], &id) < 0)
        return rt.NewStringLen(ctx, "", 0);
    s = sym_by_id((int)id);
    if (!s) return rt.NewStringLen(ctx, "", 0);
    snprintf(buf, sizeof buf, "kind=%d args=%u ret=%u",
             (int)s->abi.kind, (unsigned)s->abi.arg_count,
             (unsigned)s->abi.ret_type);
    return rt.NewStringLen(ctx, buf, strlen(buf));
}

/*
 * callSymbol(id, argc, a0, a1, ...) — invoke the resolved symbol through the
 * JIT trampoline.
 *
 * The trampoline is buffer-based: the bridge fills a flat 16-slot
 * uint64_t array (floats are bit-cast into their slot) and calls the
 * stub with that pointer, so the stub can classify every slot into the
 * right register class (GP / xmm / stack) for the target convention.
 *
 * Argument marshalling:
 *   - i64/u64 parameters accept a JS BigInt (full 64-bit) or a Number
 *     (precision above 2^53 is lost, and the bridge says so in the docs);
 *   - every other integer parameter takes a Number, or a BigInt that is
 *     truncated to 64 bits;
 *   - f32/f64 parameters take a Number and are bit-cast into the slot.
 *
 * Return marshalling:
 *   - u64 -> BigInt, i64 -> BigInt, void -> undefined,
 *   - f32/f64 -> Number (the stub widens an f32 return for us),
 *   - everything else -> Number.
 */
#define DL_CALL_MAX 12
#define DL_ARG_SLOTS 16

/* the emitted stub takes a flat argument buffer; float-returning
 * contracts are read through the double-returning twin               */
typedef uint64_t (*dl_call_t)(const uint64_t *args);
typedef double   (*dl_call_f)(const uint64_t *args);

/* how many register argument slots the platform convention has */
static int dl_reg_slots(DynalibAbiKind kind)
{
    switch (kind) {
    case DYNALIB_ABI_SYSV: return 6;
    case DYNALIB_ABI_MS64:
    case DYNALIB_ABI_STDCALL: return 4;
    default: return 0;
    }
}

static int dl_marshal_arg(MBJSContext *ctx, MBJSValueConst v, uint8_t type,
                           uint64_t *out)
{
    int64_t si = 0;
    uint64_t ui = 0;
    double d = 0;

    if (type == DYNALIB_T_F32 || type == DYNALIB_T_F64) {
        /* bit-cast into the slot: the stub loads it into an xmm register
         * with MOVQ (low 32 bits carry an f32, all 64 carry an f64)    */
        if (rt.ToFloat64(ctx, &d, v) < 0)
            return -1;
        if (type == DYNALIB_T_F32) {
            float f = (float)d;
            memcpy(out, &f, sizeof f);
        } else {
            memcpy(out, &d, sizeof d);
        }
        return 0;
    }

    if (type == DYNALIB_T_I64 || type == DYNALIB_T_U64) {
        if (MBJS_IsBigInt(v)) {
            if (type == DYNALIB_T_I64) {
                if (rt.ToBigInt64(ctx, &si, v) < 0)
                    return -1;
                *out = (uint64_t)si;
            } else {
                if (rt.ToBigUint64(ctx, &ui, v) < 0)
                    return -1;
                *out = ui;
            }
            return 0;
        }
        if (rt.ToFloat64(ctx, &d, v) < 0)
            return -1;
        *out = (type == DYNALIB_T_I64) ? (uint64_t)(int64_t)d : (uint64_t)d;
        return 0;
    }

    if (MBJS_IsBigInt(v)) {
        if (rt.ToBigUint64(ctx, &ui, v) < 0)
            return -1;
        *out = ui;
        return 0;
    }
    if (rt.ToFloat64(ctx, &d, v) < 0)
        return -1;
    *out = (uint64_t)(int64_t)d;
    return 0;
}

static MBJSValue dl_wrap_result(MBJSContext *ctx, uint8_t ret_type,
                                 uint64_t r)
{
    switch (ret_type) {
    case DYNALIB_T_U64:
        return rt.NewBigUint64(ctx, r);
    case DYNALIB_T_I64:
        return rt.NewBigInt64(ctx, (int64_t)r);
    case DYNALIB_T_VOID:
        return MBJS_UNDEFINED;
    default:
        return MBJS_NewFloat64(ctx, (double)r);
    }
}

static MBJSValue dl_call_symbol(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    long id = -1, n = 0, i;
    SymSlot *s;
    uint64_t a[DL_ARG_SLOTS];
    uint64_t r;

    (void)this_val;
    g_error[0] = '\0';
    if (argc < 2)
        return MBJS_NewFloat64(ctx, 0.0);
    if (arg_int(ctx, argv[0], &id) < 0)
        return MBJS_NewFloat64(ctx, 0.0);
    s = sym_by_id((int)id);
    if (!s) {
        set_error("invalid symbol handle");
        return MBJS_NewFloat64(ctx, 0.0);
    }
    if (arg_int(ctx, argv[1], &n) < 0)
        n = 0;
    if (n > DL_CALL_MAX) {
        set_error("more than 12 arguments are not supported");
        return MBJS_NewFloat64(ctx, 0.0);
    }
    if (n > s->abi.arg_count) {
        set_error("call has more arguments than the ABI contract declares");
        return MBJS_NewFloat64(ctx, 0.0);
    }
    if (s->abi.kind == DYNALIB_ABI_CUSTOM_REG &&
        (int)s->abi.arg_count > dl_reg_slots(DYNALIB_ABI_MS64) + 4) {
        set_error("custom register maps cannot use stack slots");
        return MBJS_NewFloat64(ctx, 0.0);
    }

    if (s->abi.ret_type == DYNALIB_T_STRUCT_VAL) {
        set_error("struct-by-value returns are not supported yet");
        return MBJS_NewFloat64(ctx, 0.0);
    }

    memset(a, 0, sizeof a);
    for (i = 0; i < n; i++) {
        uint8_t t = (i < (long)s->abi.arg_count) ? s->abi.arg_types[i]
                                                  : (uint8_t)DYNALIB_T_U64;
        if (argc < (int)(i + 2))
            break;
        if (dl_marshal_arg(ctx, argv[i + 2], t, &a[i]) < 0) {
            set_error("cannot marshal argument");
            return MBJS_NewFloat64(ctx, 0.0);
        }
    }

    if (!s->stub) {
        set_error("no callable trampoline for this symbol");
        return MBJS_NewFloat64(ctx, 0.0);
    }

    if (s->has_abi &&
        (s->abi.ret_type == DYNALIB_T_F32 || s->abi.ret_type == DYNALIB_T_F64)) {
        /* the stub returns through xmm0; the double twin reads it back */
        double d = ((dl_call_f)s->stub)(a);
        return MBJS_NewFloat64(ctx, d);
    }
    r = ((dl_call_t)s->stub)(a);
    if (!s->has_abi) {
        /* no type contract: hand back the raw 64-bit result as a Number */
        return MBJS_NewFloat64(ctx, (double)r);
    }
    return dl_wrap_result(ctx, s->abi.ret_type, r);
}

static MBJSValue dl_free_symbol(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    long id = -1;
    SymSlot *s;
    (void)this_val; (void)ctx;
    if (argc < 1 || arg_int(ctx, argv[0], &id) < 0)
        return MBJS_UNDEFINED;
    s = sym_by_id((int)id);
    if (s) {
        if (s->stub)
            dynalib_trampoline_free(s->stub, s->stub_end);
        dynalib_symbol_free(s->sym);
        s->used = 0;
        s->sym = NULL;
        s->stub = NULL;
        s->stub_end = NULL;
    }
    return MBJS_UNDEFINED;
}

static MBJSValue dl_abi_parse(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    const char *spec;
    DynalibAbiSpec abi;
    char buf[512];
    size_t used = 0;
    int i;

    (void)this_val;
    if (argc < 1)
        return rt.NewStringLen(ctx, "{}", 2);
    spec = arg_cstr(ctx, argv[0]);
    if (!spec)
        return rt.NewStringLen(ctx, "{}", 2);
    if (dynalib_abi_parse(spec, &abi) != 0) {
        rt.FreeCString(ctx, spec);
        return rt.NewStringLen(ctx, "{\"error\":\"parse failed\"}", 21);
    }
    rt.FreeCString(ctx, spec);

    used += (size_t)snprintf(buf + used, sizeof buf - used,
                             "{\"kind\":%d,\"arg_count\":%u,\"ret_type\":%u,\"arg_types\":[",
                             (int)abi.kind, (unsigned)abi.arg_count,
                             (unsigned)abi.ret_type);
    for (i = 0; i < (int)abi.arg_count && i < 16; i++)
        used += (size_t)snprintf(buf + used, sizeof buf - used, "%s%u",
                                 i ? "," : "", (unsigned)abi.arg_types[i]);
    used += (size_t)snprintf(buf + used, sizeof buf - used, "]}");
    return rt.NewStringLen(ctx, buf, used);
}

/* ------------------------------------------------------------------ */
/* module plumbing                                                     */
/* ------------------------------------------------------------------ */

#define DL_TAB_LEN 13

static const MBJSCFunctionListEntry dl_tab[DL_TAB_LEN] = {
    MBJS_CFUNC_DEF("openModule", 1, dl_open_module),
    MBJS_CFUNC_DEF("lastError", 0, dl_last_error),
    MBJS_CFUNC_DEF("moduleBase", 1, dl_module_base),
    MBJS_CFUNC_DEF("moduleExports", 1, dl_module_exports),
    MBJS_CFUNC_DEF("closeModule", 1, dl_close_module),
    MBJS_CFUNC_DEF("resolveSymbol", 4, dl_resolve_symbol),
    MBJS_CFUNC_DEF("symbolPtr", 1, dl_symbol_ptr),
    MBJS_CFUNC_DEF("symbolBase", 1, dl_symbol_base),
    MBJS_CFUNC_DEF("symbolOffset", 1, dl_symbol_offset),
    MBJS_CFUNC_DEF("symbolAbi", 1, dl_symbol_abi),
    MBJS_CFUNC_DEF("callSymbol", 14, dl_call_symbol),
    MBJS_CFUNC_DEF("freeSymbol", 1, dl_free_symbol),
    MBJS_CFUNC_DEF("abiParse", 1, dl_abi_parse),
};

static int dl_mod_init(MBJSContext *ctx, MBJSModuleDef *m)
{
    return rt.SetModuleExportList(ctx, m, dl_tab, DL_TAB_LEN);
}

MBJS_MODULE_EXTERN MBJSModuleDef *mbjs_init_module(MBJSContext *ctx,
                                                    const char *name)
{
    const char *missing = mbjs_runtime_resolve(&rt);
    MBJSModuleDef *m;

    if (missing)
        return NULL;
    m = rt.NewCModule(ctx, name ? name : "mochinode_dynalib", dl_mod_init);
    if (!m)
        return NULL;
    if (rt.AddModuleExportList(ctx, m, dl_tab, DL_TAB_LEN) != 0)
        return NULL;
    return m;
}
