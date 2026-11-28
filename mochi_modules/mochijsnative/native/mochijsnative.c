#include "mbjs_runtime.h"
#include "mbjs-jit.h"
/* libdynalib lives one directory up in the SEVM checkout; this file is also
 * copied into mochi_modules/ and .orgfix/ where the layout differs, so accept
 * every plausible include path before failing. */
#if defined(__has_include)
#  if __has_include("../libdynalib/dynalib.h")
#    include "../libdynalib/dynalib.h"
#  elif __has_include("dynalib.h")
#    include "dynalib.h"
#  else
#    include <dynalib.h>
#  endif
#else
#  include "../libdynalib/dynalib.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#endif

#define MODULE_VERSION "mochijsnative 0.4.0"

static MBJSRuntime rt;

typedef struct {
    MBJSRuntime *(*NewRuntime)(void);
    void (*FreeRuntime)(MBJSRuntime *);
    MBJSContext *(*NewContext)(MBJSRuntime *);
    void (*FreeContext)(MBJSContext *);
    void (*SetMaxStackSize)(MBJSRuntime *, size_t);
    void (*SetMemoryLimit)(MBJSRuntime *, size_t);
    MBJSValue (*Eval)(MBJSContext *, const char *, size_t, const char *, int);
    bool (*DetectModule)(const char *, size_t);
    MBJSValue (*GetException)(MBJSContext *);
    void (*FreeValue)(MBJSContext *, MBJSValue);
    MBJSValue (*DupValue)(MBJSContext *, MBJSValueConst);
    int (*ToInt32)(MBJSContext *, int32_t *, MBJSValueConst);
    int (*ToInt64)(MBJSContext *, int64_t *, MBJSValueConst);
    int (*ToFloat64)(MBJSContext *, double *, MBJSValueConst);
    MBJSValue (*NewObject)(MBJSContext *);
    MBJSValue (*GetPropertyStr)(MBJSContext *, MBJSValueConst, const char *);
    int (*SetPropertyStr)(MBJSContext *, MBJSValueConst, const char *, MBJSValueConst);
    MBJSValue (*Call)(MBJSContext *, MBJSValueConst, MBJSValueConst, int, MBJSValueConst *);
    MBJSValue (*GetGlobalObject)(MBJSContext *);
    MBJSValue (*ParseJSON)(MBJSContext *, const char *, size_t, const char *);
    MBJSValue (*JSONStringify)(MBJSContext *, MBJSValueConst, MBJSValueConst, MBJSValueConst);
    bool (*IsError)(MBJSValueConst);
    bool (*IsFunction)(MBJSContext *, MBJSValueConst);
    void (*SetModuleLoaderFunc2)(MBJSRuntime *, MBJSModuleNormalizeFunc *,
                                 MBJSModuleLoaderFunc2 *,
                                 MBJSModuleCheckSupportedImportAttributes *, void *);
    MBJSModuleDef *(*ModuleLoader)(MBJSContext *, const char *, void *);
    int (*ModuleCheckAttrs)(MBJSContext *, void *, MBJSValueConst);
    MBJSModuleDef *(*InitStd)(MBJSContext *, const char *);
    MBJSModuleDef *(*InitOs)(MBJSContext *, const char *);
    void (*StdInitHandlers)(MBJSRuntime *);
    void (*StdFreeHandlers)(MBJSRuntime *);
    void (*StdAddHelpers)(MBJSContext *, int, char **);
    int (*StdLoop)(MBJSContext *);
    int (*StdLoopOnce)(MBJSContext *);
    MBJSValue (*StdAwait)(MBJSContext *, MBJSValue);
    void (*StdDumpError)(MBJSContext *);
    MBJSJit *(*JitNew)(MBJSJitBackend, const MBJSJitOptions *);
    void (*JitFree)(MBJSJit *);
    const char *(*JitBackendName)(const MBJSJit *);
    const char *(*JitLastError)(const MBJSJit *);
    MBJSJitFn (*JitCompile)(MBJSJit *, MBJSContext *, const char *, const char *);
    MBJSValue (*JitCall)(MBJSJit *, MBJSContext *, const char *, const char *,
                         MBJSValue, int, MBJSValue *);
    void (*JitCleanupAll)(MBJSJit *, MBJSContext *);
    MBJSJitStats (*JitStats)(const MBJSJit *);
    /* v0.4: value / object / property / promise / memory control */
    int (*IsEqual)(MBJSContext *, MBJSValueConst, MBJSValueConst);
    MBJSValue (*NewArray)(MBJSContext *);
    MBJSAtom (*NewAtom)(MBJSContext *, const char *);
    void (*FreeAtom)(MBJSContext *, MBJSAtom);
    int (*HasProperty)(MBJSContext *, MBJSValueConst, MBJSAtom);
    int (*DeleteProperty)(MBJSContext *, MBJSValueConst, MBJSAtom, int);
    int (*DefinePropertyValueStr)(MBJSContext *, MBJSValueConst, const char *, MBJSValue, int);
    int (*GetOwnPropertyNames)(MBJSContext *, MBJSPropertyEnum **, uint32_t *, MBJSValueConst, int);
    void (*FreePropertyEnum)(MBJSContext *, MBJSPropertyEnum *, uint32_t);
    const char *(*AtomToCStringLen)(MBJSContext *, size_t *, MBJSAtom);
    int (*ToBool)(MBJSContext *, MBJSValueConst);
    MBJSValue (*GetPrototype)(MBJSContext *, MBJSValueConst);
    int (*SetPrototype)(MBJSContext *, MBJSValueConst, MBJSValueConst);
    MBJSValue (*ThrowRangeError)(MBJSContext *, const char *, ...);
    MBJSValue (*ThrowReferenceError)(MBJSContext *, const char *, ...);
    void (*ComputeMemoryUsage)(MBJSRuntime *, MBJSMemoryUsage *);
    bool (*HasException)(MBJSContext *);
    MBJSValue (*NewPromiseCapability)(MBJSContext *, MBJSValue *);
} MochiApi;

static MochiApi api;

static void *host_handle(void);
static void *host_symbol(const char *name);

static const char *api_resolve(void)
{
    void *p;
#define API_SYM(field, name)                                                   \
    do {                                                                       \
        p = host_symbol(name);                                                 \
        if (!p) return name;                                                   \
        memcpy(&api.field, &p, sizeof(p));                                     \
    } while (0)
/* Optional symbol: a missing one is fine -- the corresponding export degrades
 * gracefully instead of failing module initialization. */
#define API_SYM_OPT(field, name)                                                \
    do {                                                                       \
        p = host_symbol(name);                                                 \
        if (p) memcpy(&api.field, &p, sizeof(p));                              \
    } while (0)

    API_SYM(NewRuntime, "MBJS_NewRuntime");
    API_SYM(FreeRuntime, "MBJS_FreeRuntime");
    API_SYM(NewContext, "MBJS_NewContext");
    API_SYM(FreeContext, "MBJS_FreeContext");
    API_SYM(SetMaxStackSize, "MBJS_SetMaxStackSize");
    API_SYM(SetMemoryLimit, "MBJS_SetMemoryLimit");
    API_SYM(Eval, "MBJS_Eval");
    API_SYM(DetectModule, "MBJS_DetectModule");
    API_SYM(GetException, "MBJS_GetException");
    API_SYM(FreeValue, "MBJS_FreeValue");
    API_SYM(DupValue, "MBJS_DupValue");
    API_SYM(ToInt32, "MBJS_ToInt32");
    API_SYM(ToInt64, "MBJS_ToInt64");
    API_SYM(ToFloat64, "MBJS_ToFloat64");
    API_SYM(NewObject, "MBJS_NewObject");
    API_SYM(GetPropertyStr, "MBJS_GetPropertyStr");
    API_SYM(SetPropertyStr, "MBJS_SetPropertyStr");
    API_SYM(Call, "MBJS_Call");
    API_SYM(GetGlobalObject, "MBJS_GetGlobalObject");
    API_SYM(ParseJSON, "MBJS_ParseJSON");
    API_SYM(JSONStringify, "MBJS_JSONStringify");
    API_SYM(IsError, "MBJS_IsError");
    API_SYM(IsFunction, "MBJS_IsFunction");
    API_SYM(SetModuleLoaderFunc2, "MBJS_SetModuleLoaderFunc2");
    API_SYM(ModuleLoader, "mbjs_module_loader");
    API_SYM(ModuleCheckAttrs, "mbjs_module_check_attributes");
    API_SYM(InitStd, "mbjs_init_module_std");
    API_SYM(InitOs, "mbjs_init_module_os");
    API_SYM(StdInitHandlers, "mbjs_std_init_handlers");
    API_SYM(StdFreeHandlers, "mbjs_std_free_handlers");
    API_SYM(StdAddHelpers, "mbjs_std_add_helpers");
    API_SYM(StdLoop, "mbjs_std_loop");
    API_SYM(StdLoopOnce, "mbjs_std_loop_once");
    API_SYM(StdAwait, "mbjs_std_await");
    API_SYM(StdDumpError, "mbjs_std_dump_error");
    API_SYM_OPT(JitNew, "mbjs_jit_new");
    API_SYM_OPT(JitFree, "mbjs_jit_free");
    API_SYM_OPT(JitBackendName, "mbjs_jit_backend_name");
    API_SYM_OPT(JitLastError, "mbjs_jit_last_error");
    API_SYM_OPT(JitCompile, "mbjs_jit_compile");
    API_SYM_OPT(JitCall, "mbjs_jit_call");
    API_SYM_OPT(JitCleanupAll, "mbjs_jit_cleanup_all");
    API_SYM_OPT(JitStats, "mbjs_jit_stats");
    API_SYM(IsEqual, "MBJS_IsEqual");
    API_SYM(NewArray, "MBJS_NewArray");
    API_SYM(NewAtom, "MBJS_NewAtom");
    API_SYM(FreeAtom, "MBJS_FreeAtom");
    API_SYM(HasProperty, "MBJS_HasProperty");
    API_SYM(DeleteProperty, "MBJS_DeleteProperty");
    API_SYM(DefinePropertyValueStr, "MBJS_DefinePropertyValueStr");
    API_SYM(GetOwnPropertyNames, "MBJS_GetOwnPropertyNames");
    API_SYM(FreePropertyEnum, "MBJS_FreePropertyEnum");
    API_SYM(AtomToCStringLen, "MBJS_AtomToCStringLen");
    API_SYM(ToBool, "MBJS_ToBool");
    API_SYM(GetPrototype, "MBJS_GetPrototype");
    API_SYM(SetPrototype, "MBJS_SetPrototype");
    API_SYM(ComputeMemoryUsage, "MBJS_ComputeMemoryUsage");
    API_SYM(HasException, "MBJS_HasException");
    API_SYM(ThrowRangeError, "MBJS_ThrowRangeError");
    API_SYM(ThrowReferenceError, "MBJS_ThrowReferenceError");
    API_SYM(NewPromiseCapability, "MBJS_NewPromiseCapability");

#undef API_SYM_OPT
#undef API_SYM
    return NULL;
}

#define MAX_BINDS 64
#define MAX_RTS 16
#define MAX_CTXS 32
#define MAX_VALUES 512
#define MAX_JITS 16
#define MAX_JITFNS 64

typedef struct {
    int used;
    void *stub;
    void *stub_end;
    DynalibAbiSpec abi;
} BindSlot;

typedef struct {
    int used;
    MBJSRuntime *rt;
} RtSlot;

typedef struct {
    int used;
    int rtId;
    MBJSContext *ctx;
    int stdReady;
} CtxSlot;

typedef struct {
    int used;
    int ctxId;
    MBJSValue value;
} ValSlot;

typedef struct {
    int used;
    MBJSJit *jit;
    char cc[256];
    char cc_flags[256];
    char inc_dir[520];
    char lib_dir[520];
    char tmp_dir[520];
    char cache_dir[520];
} JitSlot;

typedef struct {
    int used;
    int jitId;
    int ctxId;
    MBJSJitFn fn;
} JitFnSlot;

static BindSlot g_binds[MAX_BINDS];
static RtSlot g_rts[MAX_RTS];
static CtxSlot g_ctxs[MAX_CTXS];
static ValSlot g_values[MAX_VALUES];
static JitSlot g_jits[MAX_JITS];
static JitFnSlot g_jitfns[MAX_JITFNS];
static char g_error[512];

static void context_release(int id);

static void set_error(const char *msg)
{
    snprintf(g_error, sizeof(g_error), "%s", msg ? msg : "unknown error");
}

static const char *arg_cstr(MBJSContext *ctx, MBJSValueConst v)
{
    return rt.ToCStringLen(ctx, NULL, v);
}

static int arg_u64(MBJSContext *ctx, MBJSValueConst v, uint64_t *out)
{
    double d = 0;
    if (MBJS_IsNumber(v)) {
        if (rt.ToFloat64(ctx, &d, v) == 0) {
            *out = (uint64_t)(int64_t)d;
            return 0;
        }
        return -1;
    }
    if (MBJS_IsBigInt(v) && rt.ToBigUint64(ctx, out, v) == 0)
        return 0;
    return -1;
}

static int arg_int(MBJSContext *ctx, MBJSValueConst v, int *out)
{
    uint64_t raw = 0;
    if (arg_u64(ctx, v, &raw) < 0)
        return -1;
    *out = (int)(int64_t)raw;
    return 0;
}

static MBJSValue cstr_result(MBJSContext *ctx, const char *s)
{
    return rt.NewStringLen(ctx, s ? s : "", s ? strlen(s) : 0);
}

static char *nul_copy(const char *s, size_t *len_out)
{
    size_t n = strlen(s);
    char *buf = (char *)malloc(n + 1);
    if (!buf)
        return NULL;
    memcpy(buf, s, n);
    buf[n] = '\0';
    if (len_out)
        *len_out = n;
    return buf;
}

static void *host_handle(void)
{
#ifdef _WIN32
    return (void *)GetModuleHandleA(NULL);
#else
    return dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
#endif
}

/* Engine symbols are exported by the host image when it is linked with
 * -Wl,--export-all-symbols. When the engine is instead a shared library
 * (mbjs.dll / libmbjs.dll) they live there, so fall back to that module.
 * Shipped via mbjs_runtime.h so every MochiBerry native module benefits. */
static void *host_symbol(const char *name)
{
    return mbjs_host_symbol(name);
}

#ifdef _WIN32
static IMAGE_EXPORT_DIRECTORY *host_exports(void)
{
    HMODULE h = (HMODULE)host_handle();
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    if (!h)
        return NULL;
    dos = (IMAGE_DOS_HEADER *)h;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;
    nt = (IMAGE_NT_HEADERS *)((unsigned char *)h + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return NULL;
    return (IMAGE_EXPORT_DIRECTORY *)((unsigned char *)h +
          nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
}
#endif

static int host_export_count(void)
{
#ifdef _WIN32
    IMAGE_EXPORT_DIRECTORY *exp = host_exports();
    return exp ? (int)exp->NumberOfFunctions : 0;
#else
    return 257;
#endif
}

static const char *known_signature(const char *name)
{
    static const struct { const char *name; const char *sig; } tab[] = {
        { "MBJS_GetVersion", "ptr()" },
        { "MBJS_NewRuntime", "ptr()" },
        { "MBJS_FreeRuntime", "void(ptr)" },
        { "MBJS_SetMaxStackSize", "void(ptr,u64)" },
        { "MBJS_SetMemoryLimit", "void(ptr,u64)" },
        { "MBJS_NewContext", "ptr(ptr)" },
        { "MBJS_FreeContext", "void(ptr)" },
        { "MBJS_NewObject", "u64(ptr)" },
        { "MBJS_GetGlobalObject", "u64(ptr)" },
        { "MBJS_GetException", "u64(ptr)" },
        { "MBJS_FreeValue", "void(ptr,u64)" },
        { "MBJS_DupValue", "u64(ptr,u64)" },
        { "MBJS_ToInt32", "i32(ptr,ptr,u64)" },
        { "MBJS_ToInt64", "i32(ptr,ptr,u64)" },
        { "MBJS_ToFloat64", "i32(ptr,ptr,u64)" },
        { "MBJS_GetPropertyStr", "u64(ptr,u64,ptr)" },
        { "MBJS_SetPropertyStr", "i32(ptr,u64,ptr,u64)" },
        { "MBJS_Call", "u64(ptr,u64,u64,i32,ptr)" },
        { "MBJS_ParseJSON", "u64(ptr,ptr,u64,ptr)" },
        { "MBJS_JSONStringify", "u64(ptr,u64,u64,u64)" },
        { "MBJS_DetectModule", "i32(ptr,u64)" },
        { "MBJS_IsError", "i32(u64)" },
        { "MBJS_IsFunction", "i32(ptr,u64)" },
        { "mbjs_std_loop", "i32(ptr)" },
        { "mbjs_std_loop_once", "i32(ptr)" },
        { "mbjs_std_await", "u64(ptr,u64)" },
        { "mbjs_std_dump_error", "void(ptr)" },
        { "MBJS_Eval", "u64(ptr,ptr,u64,ptr,i32)" },
        { "MBJS_EvalFunction", "u64(ptr,u64)" },
        { "mbjs_jit_new", "ptr(i32,ptr)" },
        { "mbjs_jit_compile", "ptr(ptr,ptr,ptr,ptr)" },
        { "mbjs_jit_call", "u64(ptr,ptr,ptr,ptr,u64,i32,ptr)" },
        { "mbjs_jit_free", "void(ptr)" },
        { "mbjs_jit_cleanup_all", "void(ptr,ptr)" }
    };
    size_t i;
    if (!name)
        return "";
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (strcmp(tab[i].name, name) == 0)
            return tab[i].sig;
    return "";
}

static const char *value_type_name(MBJSValueConst v)
{
    switch (MBJS_VALUE_GET_TAG(v)) {
    case MBJS_TAG_BIG_INT: return "bigint";
    case MBJS_TAG_SYMBOL: return "symbol";
    case MBJS_TAG_STRING: return "string";
    case MBJS_TAG_STRING_ROPE: return "string_rope";
    case MBJS_TAG_MODULE: return "module";
    case MBJS_TAG_FUNCTION_BYTECODE: return "bytecode";
    case MBJS_TAG_OBJECT: return "object";
    case MBJS_TAG_INT: return "int";
    case MBJS_TAG_BOOL: return "bool";
    case MBJS_TAG_NULL: return "null";
    case MBJS_TAG_UNDEFINED: return "undefined";
    case MBJS_TAG_UNINITIALIZED: return "uninitialized";
    case MBJS_TAG_EXCEPTION: return "exception";
    case MBJS_TAG_SHORT_BIG_INT: return "bigint";
    case MBJS_TAG_FLOAT64: return "float64";
    default: return "unknown";
    }
}

static int value_store(int ctxId, MBJSValue value)
{
    int i;
    for (i = 0; i < MAX_VALUES; i++) {
        if (!g_values[i].used) {
            g_values[i].used = 1;
            g_values[i].ctxId = ctxId;
            g_values[i].value = value;
            return i;
        }
    }
    api.FreeValue(g_ctxs[ctxId].ctx, value);
    set_error("value table full");
    return -1;
}

static ValSlot *value_slot(MBJSContext *ctx, MBJSValueConst v)
{
    int id = -1;
    if (arg_int(ctx, v, &id) < 0 || id < 0 || id >= MAX_VALUES || !g_values[id].used) {
        set_error("invalid value id");
        return NULL;
    }
    return &g_values[id];
}

static int value_slot_id(MBJSContext *ctx, MBJSValueConst v)
{
    int id = -1;
    if (arg_int(ctx, v, &id) < 0 || id < 0 || id >= MAX_VALUES || !g_values[id].used)
        return -1;
    return id;
}

static CtxSlot *ctx_slot(MBJSContext *ctx, MBJSValueConst v)
{
    int id = -1;
    if (arg_int(ctx, v, &id) < 0 || id < 0 || id >= MAX_CTXS || !g_ctxs[id].used) {
        set_error("invalid context id");
        return NULL;
    }
    return &g_ctxs[id];
}

static RtSlot *rt_slot(MBJSContext *ctx, MBJSValueConst v)
{
    int id = -1;
    if (arg_int(ctx, v, &id) < 0 || id < 0 || id >= MAX_RTS || !g_rts[id].used) {
        set_error("invalid runtime id");
        return NULL;
    }
    return &g_rts[id];
}

static void free_values_for_ctx(int ctxId)
{
    int i;
    for (i = 0; i < MAX_VALUES; i++) {
        if (g_values[i].used && g_values[i].ctxId == ctxId) {
            api.FreeValue(g_ctxs[ctxId].ctx, g_values[i].value);
            memset(&g_values[i], 0, sizeof(g_values[i]));
        }
    }
}

static void context_release(int id)
{
    int i;
    CtxSlot *c;
    if (id < 0 || id >= MAX_CTXS || !g_ctxs[id].used)
        return;
    c = &g_ctxs[id];
    for (i = 0; i < MAX_JITS; i++)
        if (g_jits[i].used)
            api.JitCleanupAll(g_jits[i].jit, c->ctx);
    for (i = 0; i < MAX_JITFNS; i++)
        if (g_jitfns[i].used && g_jitfns[i].ctxId == id)
            memset(&g_jitfns[i], 0, sizeof(g_jitfns[i]));
    free_values_for_ctx(id);
    api.FreeContext(c->ctx);
    memset(c, 0, sizeof(*c));
}

static MBJSValue mn_version(MBJSContext *ctx, MBJSValueConst this_val,
                            int argc, MBJSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return cstr_result(ctx, MODULE_VERSION);
}

static MBJSValue mn_abi(MBJSContext *ctx, MBJSValueConst this_val,
                        int argc, MBJSValueConst *argv)
{
    static char buf[12288];
    int n;
    (void)this_val; (void)argc; (void)argv;
    n = snprintf(buf, sizeof(buf),
        "{\"engine\":\"MochiBerry\",\"abi\":\"ms64\",\"pointer\":8,"
        "\"export_count\":%d,\"runtime\":\"host-image\","
        "\"trampoline\":\"uint64_t stub(const uint64_t args[16])\","
        "\"float_return\":\"bits in uint64_t\","
        "\"deep\":{\"runtime\":true,\"context\":true,\"eval\":true,"
        "\"values\":true,\"properties\":true,\"call\":true,"
        "\"json\":true,\"exceptions\":true,\"promise\":true,"
        "\"memory\":true,\"export_enumeration\":true,\"jit\":true},"
        "\"module_version\":\"0.4.0\","
        "\"deep_legacy\":{\"runtime\":true,\"context\":true,\"eval\":true,"
        "\"values\":true,\"properties\":true,\"call\":true,"
        "\"json\":true,\"exceptions\":true,\"jit\":true,"
        "\"export_enumeration\":true},"
        "\"symbols\":{"
        "\"MBJS_GetVersion\":\"ptr()\","
        "\"MBJS_NewRuntime\":\"ptr()\","
        "\"MBJS_FreeRuntime\":\"void(ptr)\","
        "\"MBJS_NewContext\":\"ptr(ptr)\","
        "\"MBJS_FreeContext\":\"void(ptr)\","
        "\"MBJS_Eval\":\"u64(ptr,ptr,u64,ptr,i32)\","
        "\"MBJS_Call\":\"u64(ptr,u64,u64,i32,ptr)\","
        "\"MBJS_GetPropertyStr\":\"u64(ptr,u64,ptr)\","
        "\"MBJS_SetPropertyStr\":\"i32(ptr,u64,ptr,u64)\","
        "\"MBJS_JSONStringify\":\"u64(ptr,u64,u64,u64)\","
        "\"MBJS_ParseJSON\":\"u64(ptr,ptr,u64,ptr)\","
        "\"mbjs_std_await\":\"u64(ptr,u64)\","
        "\"mbjs_std_loop\":\"i32(ptr)\"," 
        "\"mbjs_jit_call\":\"u64(ptr,ptr,ptr,ptr,u64,i32,ptr)\"}}",
        host_export_count());
    return rt.NewStringLen(ctx, buf, (size_t)n);
}

static MBJSValue mn_host_base(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return rt.NewBigUint64(ctx, (uint64_t)(uintptr_t)host_handle());
}

static MBJSValue mn_host_export(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    const char *name;
    void *p;
    (void)this_val;
    g_error[0] = '\0';
    if (argc < 1)
        return rt.ThrowTypeError(ctx, "hostExport: missing symbol name");
    name = arg_cstr(ctx, argv[0]);
    if (!name)
        return rt.ThrowTypeError(ctx, "hostExport: invalid symbol name");
    p = host_symbol(name);
    rt.FreeCString(ctx, name);
    if (!p) {
        set_error("hostExport: symbol not found");
        return rt.NewBigUint64(ctx, 0);
    }
    return rt.NewBigUint64(ctx, (uint64_t)(uintptr_t)p);
}

static MBJSValue mn_engine_version(MBJSContext *ctx, MBJSValueConst this_val,
                                   int argc, MBJSValueConst *argv)
{
    const char *(*fn)(void);
    void *p = host_symbol("MBJS_GetVersion");
    const char *s;
    (void)this_val; (void)argc; (void)argv;
    if (!p)
        return cstr_result(ctx, "unknown");
    memcpy(&fn, &p, sizeof(fn));
    s = fn();
    return cstr_result(ctx, s);
}

static MBJSValue mn_export_names(MBJSContext *ctx, MBJSValueConst this_val,
                                 int argc, MBJSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
#ifdef _WIN32
    IMAGE_EXPORT_DIRECTORY *exp = host_exports();
    char *buf;
    size_t cap, used = 0;
    DWORD i;
    if (!exp)
        return cstr_result(ctx, "[]");
    cap = 4096 + (size_t)exp->NumberOfNames * 160;
    buf = (char *)malloc(cap);
    if (!buf)
        return cstr_result(ctx, "[]");
    used += (size_t)snprintf(buf + used, cap - used, "[");
    for (i = 0; i < exp->NumberOfNames; i++) {
        DWORD rva = ((DWORD *)((unsigned char *)host_handle() + exp->AddressOfNames))[i];
        const char *name = (const char *)((unsigned char *)host_handle() + rva);
        used += (size_t)snprintf(buf + used, cap - used, "%s\"%s\"",
                                 i ? "," : "", name);
    }
    used += (size_t)snprintf(buf + used, cap - used, "]");
    {
        MBJSValue r = rt.NewStringLen(ctx, buf, used);
        free(buf);
        return r;
    }
#else
    return cstr_result(ctx, "[]");
#endif
}

static MBJSValue mn_export_signature(MBJSContext *ctx, MBJSValueConst this_val,
                                     int argc, MBJSValueConst *argv)
{
    const char *name;
    (void)this_val;
    if (argc < 1)
        return cstr_result(ctx, "");
    name = arg_cstr(ctx, argv[0]);
    if (!name)
        return cstr_result(ctx, "");
    {
        MBJSValue r = cstr_result(ctx, known_signature(name));
        rt.FreeCString(ctx, name);
        return r;
    }
}

static MBJSValue mn_bind(MBJSContext *ctx, MBJSValueConst this_val,
                         int argc, MBJSValueConst *argv)
{
    const char *name;
    const char *spec;
    DynalibAbiSpec abi;
    void *target;
    void *stub;
    void *end = NULL;
    int slot;
    (void)this_val;
    g_error[0] = '\0';
    if (argc < 1)
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    name = arg_cstr(ctx, argv[0]);
    if (!name)
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    target = host_symbol(name);
    if (!target) {
        set_error("bind: symbol not found");
        rt.FreeCString(ctx, name);
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    }
    spec = argc > 1 ? arg_cstr(ctx, argv[1]) : NULL;
    if (!spec || dynalib_abi_parse(spec, &abi) != 0) {
        if (spec) rt.FreeCString(ctx, spec);
        rt.FreeCString(ctx, name);
        set_error("bind: invalid ABI spec");
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    }
    rt.FreeCString(ctx, spec);
    rt.FreeCString(ctx, name);
    stub = dynalib_trampoline_emit(target, &abi, &end);
    if (!stub) {
        set_error("bind: trampoline emit failed");
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    }
    for (slot = 0; slot < MAX_BINDS; slot++) {
        if (!g_binds[slot].used) {
            g_binds[slot].used = 1;
            g_binds[slot].stub = stub;
            g_binds[slot].stub_end = end;
            g_binds[slot].abi = abi;
            return rt.NewBigUint64(ctx, (uint64_t)(int64_t)slot);
        }
    }
    dynalib_trampoline_free(stub, end);
    set_error("bind: table full");
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
}

static MBJSValue mn_call(MBJSContext *ctx, MBJSValueConst this_val,
                         int argc, MBJSValueConst *argv)
{
    uint64_t args[16];
    uint64_t result = 0;
    BindSlot *s;
    uint64_t raw_id = 0;
    int id;
    int i;
    (void)this_val;
    g_error[0] = '\0';
    if (argc < 1 || arg_u64(ctx, argv[0], &raw_id) < 0)
        return rt.NewBigUint64(ctx, 0);
    id = (int)(int64_t)raw_id;
    if (id < 0 || id >= MAX_BINDS || !g_binds[id].used) {
        set_error("call: invalid bind id");
        return rt.NewBigUint64(ctx, 0);
    }
    s = &g_binds[id];
    memset(args, 0, sizeof(args));
    for (i = 1; i < argc && i <= 8; i++) {
        if (arg_u64(ctx, argv[i], &args[i - 1]) < 0) {
            set_error("call: invalid argument");
            return rt.NewBigUint64(ctx, 0);
        }
    }
    if (s->abi.ret_type == DYNALIB_T_VOID) {
        void (*stub)(const uint64_t *) = NULL;
        memcpy(&stub, &s->stub, sizeof(stub));
        stub(args);
        return rt.NewBigUint64(ctx, 0);
    }
    if (s->abi.ret_type == DYNALIB_T_F32 || s->abi.ret_type == DYNALIB_T_F64) {
        double (*stub)(const uint64_t *) = NULL;
        double d;
        memcpy(&stub, &s->stub, sizeof(stub));
        d = stub(args);
        memcpy(&result, &d, sizeof(result));
        return rt.NewBigUint64(ctx, result);
    }
    {
        uint64_t (*stub)(const uint64_t *) = NULL;
        memcpy(&stub, &s->stub, sizeof(stub));
        result = stub(args);
    }
    return rt.NewBigUint64(ctx, result);
}

static MBJSValue mn_unbind(MBJSContext *ctx, MBJSValueConst this_val,
                           int argc, MBJSValueConst *argv)
{
    uint64_t raw_id = 0;
    int id;
    (void)this_val;
    if (argc < 1 || arg_u64(ctx, argv[0], &raw_id) < 0)
        return MBJS_UNDEFINED;
    id = (int)(int64_t)raw_id;
    if (id >= 0 && id < MAX_BINDS && g_binds[id].used) {
        dynalib_trampoline_free(g_binds[id].stub, g_binds[id].stub_end);
        memset(&g_binds[id], 0, sizeof(g_binds[id]));
    }
    return MBJS_UNDEFINED;
}

static MBJSValue mn_last_error(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return cstr_result(ctx, g_error);
}

static MBJSValue mn_runtime_new(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    MBJSRuntime *r;
    int slot;
    (void)this_val; (void)argc; (void)argv;
    r = api.NewRuntime();
    if (!r)
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    api.StdInitHandlers(r);
    api.SetModuleLoaderFunc2(r, NULL,
        (MBJSModuleLoaderFunc2 *)api.ModuleLoader, api.ModuleCheckAttrs, NULL);
    for (slot = 0; slot < MAX_RTS; slot++) {
        if (!g_rts[slot].used) {
            g_rts[slot].used = 1;
            g_rts[slot].rt = r;
            return rt.NewBigUint64(ctx, (uint64_t)(int64_t)slot);
        }
    }
    api.StdFreeHandlers(r);
    api.FreeRuntime(r);
    set_error("runtime table full");
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
}

static MBJSValue mn_runtime_free(MBJSContext *ctx, MBJSValueConst this_val,
                                 int argc, MBJSValueConst *argv)
{
    RtSlot *r;
    int i;
    (void)this_val;
    if (argc < 1 || !(r = rt_slot(ctx, argv[0])))
        return MBJS_UNDEFINED;
    for (i = 0; i < MAX_CTXS; i++)
        if (g_ctxs[i].used && g_ctxs[i].rtId == (int)(int64_t)(uintptr_t)(r - g_rts))
            context_release(i);
    api.StdFreeHandlers(r->rt);
    api.FreeRuntime(r->rt);
    memset(r, 0, sizeof(*r));
    return MBJS_UNDEFINED;
}

static MBJSValue mn_runtime_limits(MBJSContext *ctx, MBJSValueConst this_val,
                                   int argc, MBJSValueConst *argv)
{
    RtSlot *r;
    int stack = 0;
    int memory = 0;
    (void)this_val;
    if (argc < 1 || !(r = rt_slot(ctx, argv[0])))
        return MBJS_UNDEFINED;
    if (argc > 1 && arg_int(ctx, argv[1], &stack) == 0 && stack > 0)
        api.SetMaxStackSize(r->rt, (size_t)stack);
    if (argc > 2 && arg_int(ctx, argv[2], &memory) == 0 && memory > 0)
        api.SetMemoryLimit(r->rt, (size_t)memory);
    return MBJS_UNDEFINED;
}

static void context_prepare_std(CtxSlot *c)
{
    if (c->stdReady)
        return;
    api.InitStd(c->ctx, "mbjs:std");
    api.InitOs(c->ctx, "mbjs:os");
    api.StdAddHelpers(c->ctx, 0, NULL);
    c->stdReady = 1;
}

static MBJSValue mn_context_new(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    RtSlot *r;
    CtxSlot *c;
    MBJSContext *native;
    int withStd = 1;
    int rtId;
    int slot;
    (void)this_val;
    if (argc < 1 || !(r = rt_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    if (argc > 1 && arg_int(ctx, argv[1], &withStd) < 0)
        withStd = 1;
    native = api.NewContext(r->rt);
    if (!native)
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    rtId = (int)(int64_t)(uintptr_t)(r - g_rts);
    for (slot = 0; slot < MAX_CTXS; slot++) {
        if (!g_ctxs[slot].used) {
            g_ctxs[slot].used = 1;
            g_ctxs[slot].rtId = rtId;
            g_ctxs[slot].ctx = native;
            if (withStd)
                context_prepare_std(&g_ctxs[slot]);
            return rt.NewBigUint64(ctx, (uint64_t)(int64_t)slot);
        }
    }
    api.FreeContext(native);
    set_error("context table full");
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
}

static MBJSValue mn_context_free(MBJSContext *ctx, MBJSValueConst this_val,
                                 int argc, MBJSValueConst *argv)
{
    int id = -1;
    (void)this_val;
    if (argc < 1 || arg_int(ctx, argv[0], &id) == 0)
        context_release(id);
    return MBJS_UNDEFINED;
}

static MBJSValue mn_context_std(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    (void)this_val;
    if (argc < 1 || !(c = ctx_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    context_prepare_std(c);
    return rt.NewBigUint64(ctx, 0);
}

static MBJSValue mn_value_new(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    const char *kind;
    CtxSlot *c;
    MBJSValue v = MBJS_UNDEFINED;
    (void)this_val;
    if (argc < 2 || !(c = ctx_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    kind = arg_cstr(ctx, argv[1]);
    if (!kind)
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    if (strcmp(kind, "int") == 0) {
        int n = 0;
        if (argc > 2) arg_int(ctx, argv[2], &n);
        v = MBJS_NewInt32(c->ctx, (int32_t)n);
    } else if (strcmp(kind, "float") == 0) {
        double d = 0;
        if (argc > 2) {
            if (MBJS_IsBigInt(argv[2])) {
                uint64_t bits = 0;
                if (rt.ToBigUint64(ctx, &bits, argv[2]) == 0)
                    memcpy(&d, &bits, sizeof(d));
            } else {
                rt.ToFloat64(ctx, &d, argv[2]);
            }
        }
        v = MBJS_NewFloat64(c->ctx, d);
    } else if (strcmp(kind, "string") == 0) {
        const char *s = argc > 2 ? arg_cstr(ctx, argv[2]) : NULL;
        size_t n = s ? strlen(s) : 0;
        v = rt.NewStringLen(c->ctx, s ? s : "", n);
        if (s) rt.FreeCString(ctx, s);
    } else if (strcmp(kind, "bool") == 0) {
        int b = 0;
        if (argc > 2) arg_int(ctx, argv[2], &b);
        v = MBJS_NewBool(c->ctx, b != 0);
    } else if (strcmp(kind, "null") == 0) {
        v = MBJS_NULL;
    } else if (strcmp(kind, "object") == 0) {
        v = api.NewObject(c->ctx);
    } else if (strcmp(kind, "undefined") == 0) {
        v = MBJS_UNDEFINED;
    }
    rt.FreeCString(ctx, kind);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store((int)(int64_t)(uintptr_t)(c - g_ctxs), v));
}

static MBJSValue mn_value_free(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    ValSlot *v;
    (void)this_val;
    if (argc < 1 || !(v = value_slot(ctx, argv[0])))
        return MBJS_UNDEFINED;
    api.FreeValue(g_ctxs[v->ctxId].ctx, v->value);
    memset(v, 0, sizeof(*v));
    return MBJS_UNDEFINED;
}

static MBJSValue mn_value_type(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    ValSlot *v;
    (void)this_val;
    if (argc < 1 || !(v = value_slot(ctx, argv[0])))
        return cstr_result(ctx, "invalid");
    return cstr_result(ctx, value_type_name(v->value));
}

static MBJSValue mn_value_is_exception(MBJSContext *ctx, MBJSValueConst this_val,
                                       int argc, MBJSValueConst *argv)
{
    ValSlot *v;
    (void)this_val;
    if (argc < 1 || !(v = value_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, 0);
    return rt.NewBigUint64(ctx, MBJS_IsException(v->value) ? 1 : 0);
}

static MBJSValue mn_value_to_number(MBJSContext *ctx, MBJSValueConst this_val,
                                    int argc, MBJSValueConst *argv)
{
    ValSlot *v;
    double d = 0;
    (void)this_val;
    if (argc < 1 || !(v = value_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, 0);
    if (api.ToFloat64(g_ctxs[v->ctxId].ctx, &d, v->value) < 0) {
        uint64_t bits;
        memcpy(&bits, &d, sizeof(bits));
        return rt.NewBigUint64(ctx, bits);
    }
    {
        uint64_t bits;
        memcpy(&bits, &d, sizeof(bits));
        return rt.NewBigUint64(ctx, bits);
    }
}

static MBJSValue mn_value_to_int(MBJSContext *ctx, MBJSValueConst this_val,
                                 int argc, MBJSValueConst *argv)
{
    ValSlot *v;
    int64_t n = 0;
    (void)this_val;
    if (argc < 1 || !(v = value_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, 0);
    if (argc > 1) {
        int wide = 0;
        arg_int(ctx, argv[1], &wide);
        if (wide) {
            if (api.ToInt64(g_ctxs[v->ctxId].ctx, &n, v->value) < 0) n = 0;
        } else {
            int32_t n32 = 0;
            if (api.ToInt32(g_ctxs[v->ctxId].ctx, &n32, v->value) < 0) n32 = 0;
            n = n32;
        }
    } else if (api.ToInt64(g_ctxs[v->ctxId].ctx, &n, v->value) < 0) {
        n = 0;
    }
    return rt.NewBigUint64(ctx, (uint64_t)n);
}

static MBJSValue mn_value_to_string(MBJSContext *ctx, MBJSValueConst this_val,
                                    int argc, MBJSValueConst *argv)
{
    ValSlot *v;
    const char *s;
    MBJSValue r;
    (void)this_val;
    if (argc < 1 || !(v = value_slot(ctx, argv[0])))
        return cstr_result(ctx, "");
    s = rt.ToCStringLen(g_ctxs[v->ctxId].ctx, NULL, v->value);
    r = cstr_result(ctx, s);
    if (s) rt.FreeCString(g_ctxs[v->ctxId].ctx, s);
    return r;
}

static MBJSValue mn_value_json(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    ValSlot *v;
    MBJSValue r;
    const char *s;
    (void)this_val;
    if (argc < 1 || !(v = value_slot(ctx, argv[0])))
        return cstr_result(ctx, "null");
    r = api.JSONStringify(g_ctxs[v->ctxId].ctx, v->value, MBJS_UNDEFINED, MBJS_UNDEFINED);
    if (MBJS_IsException(r)) {
        api.FreeValue(g_ctxs[v->ctxId].ctx, r);
        return cstr_result(ctx, "null");
    }
    s = rt.ToCStringLen(g_ctxs[v->ctxId].ctx, NULL, r);
    {
        MBJSValue out = cstr_result(ctx, s);
        if (s) rt.FreeCString(g_ctxs[v->ctxId].ctx, s);
        api.FreeValue(g_ctxs[v->ctxId].ctx, r);
        return out;
    }
}

static MBJSValue mn_eval(MBJSContext *ctx, MBJSValueConst this_val,
                         int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    const char *source;
    const char *filename;
    char *buf;
    size_t len;
    MBJSValue result;
    int flags = 0;
    int freeFilename = 0;
    (void)this_val;
    if (argc < 2 || !(c = ctx_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    source = arg_cstr(ctx, argv[1]);
    if (!source)
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    filename = argc > 2 ? arg_cstr(ctx, argv[2]) : "<eval>";
    freeFilename = argc > 2;
    if (argc > 3) arg_int(ctx, argv[3], &flags);
    buf = nul_copy(source, &len);
    rt.FreeCString(ctx, source);
    if (!buf) {
        if (freeFilename && filename) rt.FreeCString(ctx, filename);
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    }
    result = api.Eval(c->ctx, buf, len, filename ? filename : "<eval>", flags);
    free(buf);
    if (freeFilename && filename) rt.FreeCString(ctx, filename);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store(
        (int)(int64_t)(uintptr_t)(c - g_ctxs), result));
}

static MBJSValue mn_eval_module(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    const char *source;
    const char *filename;
    char *buf;
    size_t len;
    MBJSValue result;
    (void)this_val;
    if (argc < 3 || !(c = ctx_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    source = arg_cstr(ctx, argv[1]);
    filename = arg_cstr(ctx, argv[2]);
    if (!source || !filename) {
        if (source) rt.FreeCString(ctx, source);
        if (filename) rt.FreeCString(ctx, filename);
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    }
    buf = nul_copy(source, &len);
    rt.FreeCString(ctx, source);
    if (!buf) {
        rt.FreeCString(ctx, filename);
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    }
    result = api.Eval(c->ctx, buf, len, filename, MBJS_EVAL_TYPE_MODULE);
    free(buf);
    rt.FreeCString(ctx, filename);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store(
        (int)(int64_t)(uintptr_t)(c - g_ctxs), result));
}

static MBJSValue mn_detect_module(MBJSContext *ctx, MBJSValueConst this_val,
                                  int argc, MBJSValueConst *argv)
{
    const char *source;
    char *buf;
    bool r;
    (void)this_val;
    if (argc < 1 || !(source = arg_cstr(ctx, argv[0])))
        return rt.NewBigUint64(ctx, 0);
    buf = nul_copy(source, NULL);
    rt.FreeCString(ctx, source);
    if (!buf)
        return rt.NewBigUint64(ctx, 0);
    r = api.DetectModule(buf, strlen(buf));
    free(buf);
    return rt.NewBigUint64(ctx, r ? 1 : 0);
}

static MBJSValue mn_get_exception(MBJSContext *ctx, MBJSValueConst this_val,
                                  int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    (void)this_val;
    if (argc < 1 || !(c = ctx_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store(
        (int)(int64_t)(uintptr_t)(c - g_ctxs), api.GetException(c->ctx)));
}

static MBJSValue mn_dump_error(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    (void)this_val;
    if (argc < 1 || !(c = ctx_slot(ctx, argv[0])))
        return MBJS_UNDEFINED;
    api.StdDumpError(c->ctx);
    return MBJS_UNDEFINED;
}

static MBJSValue mn_await(MBJSContext *ctx, MBJSValueConst this_val,
                          int argc, MBJSValueConst *argv)
{
    ValSlot *v;
    MBJSValue result;
    int ctxId;
    (void)this_val;
    if (argc < 2 || !(v = value_slot(ctx, argv[1])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    ctxId = v->ctxId;
    result = v->value;
    memset(v, 0, sizeof(*v));
    result = api.StdAwait(g_ctxs[ctxId].ctx, result);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store(ctxId, result));
}

static MBJSValue mn_loop(MBJSContext *ctx, MBJSValueConst this_val,
                         int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    int once = 0;
    (void)this_val;
    if (argc < 1 || !(c = ctx_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    if (argc > 1) arg_int(ctx, argv[1], &once);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)(
        once ? api.StdLoopOnce(c->ctx) : api.StdLoop(c->ctx)));
}

static MBJSValue mn_call_value(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    ValSlot *fn;
    MBJSValue thisValue = MBJS_UNDEFINED;
    MBJSValue values[16];
    MBJSValueConst *args = NULL;
    int callArgc = 0;
    int i;
    (void)this_val;
    if (argc < 2 || !(c = ctx_slot(ctx, argv[0])) ||
        !(fn = value_slot(ctx, argv[1])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    if (argc > 2) {
        int thisId = value_slot_id(ctx, argv[2]);
        if (thisId >= 0) thisValue = g_values[thisId].value;
    }
    for (i = 3; i < argc && callArgc < 16; i++) {
        int id = value_slot_id(ctx, argv[i]);
        if (id < 0)
            return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
        values[callArgc++] = g_values[id].value;
    }
    if (callArgc) args = (MBJSValueConst *)values;
    {
        MBJSValue result = api.Call(c->ctx, fn->value, thisValue, callArgc, args);
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store(
            (int)(int64_t)(uintptr_t)(c - g_ctxs), result));
    }
}

static MBJSValue mn_get_global(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    (void)this_val;
    if (argc < 1 || !(c = ctx_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store(
        (int)(int64_t)(uintptr_t)(c - g_ctxs), api.GetGlobalObject(c->ctx)));
}

static MBJSValue mn_get_prop(MBJSContext *ctx, MBJSValueConst this_val,
                             int argc, MBJSValueConst *argv)
{
    ValSlot *obj;
    const char *key;
    MBJSValue result;
    (void)this_val;
    if (argc < 3 || !(obj = value_slot(ctx, argv[1])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    key = arg_cstr(ctx, argv[2]);
    if (!key)
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    result = api.GetPropertyStr(g_ctxs[obj->ctxId].ctx, obj->value, key);
    rt.FreeCString(ctx, key);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store(obj->ctxId, result));
}

static MBJSValue mn_set_prop(MBJSContext *ctx, MBJSValueConst this_val,
                             int argc, MBJSValueConst *argv)
{
    ValSlot *obj;
    ValSlot *val;
    const char *key;
    int r;
    (void)this_val;
    if (argc < 4 || !(obj = value_slot(ctx, argv[1])) ||
        !(val = value_slot(ctx, argv[3])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    key = arg_cstr(ctx, argv[2]);
    if (!key)
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    /* SetPropertyStr/DefinePropertyValueStr CONSUME their value argument, so
     * hand them a duplicate and keep the caller's slot valid. */
    r = api.SetPropertyStr(g_ctxs[obj->ctxId].ctx, obj->value, key,
                           api.DupValue(g_ctxs[obj->ctxId].ctx, val->value));
    rt.FreeCString(ctx, key);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)r);
}

static MBJSValue mn_json_parse(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    const char *text;
    char *buf;
    size_t len;
    MBJSValue result;
    (void)this_val;
    if (argc < 2 || !(c = ctx_slot(ctx, argv[0])) || !(text = arg_cstr(ctx, argv[1])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    buf = nul_copy(text, &len);
    rt.FreeCString(ctx, text);
    if (!buf)
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    result = api.ParseJSON(c->ctx, buf, len, "<json>");
    free(buf);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store(
        (int)(int64_t)(uintptr_t)(c - g_ctxs), result));
}

static void opt_str(char *dst, size_t cap, MBJSContext *ctx,
                    MBJSValueConst *argv, int index)
{
    const char *s;
    dst[0] = '\0';
    if (index >= 0 && index < 10000) {
        s = arg_cstr(ctx, argv[index]);
        if (s) {
            snprintf(dst, cap, "%s", s);
            rt.FreeCString(ctx, s);
        }
    }
}

static MBJSValue mn_jit_new(MBJSContext *ctx, MBJSValueConst this_val,
                            int argc, MBJSValueConst *argv)
{
    MBJSJitOptions opts;
    MBJSJit *jit;
    int backend = 0;
    int hot = 0;
    int slot;
    (void)this_val;
    if (argc > 0) arg_int(ctx, argv[0], &backend);
    if (argc > 5) arg_int(ctx, argv[5], &hot);
    memset(&opts, 0, sizeof(opts));
    for (slot = 0; slot < MAX_JITS; slot++) {
        if (!g_jits[slot].used)
            break;
    }
    if (slot == MAX_JITS)
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    opt_str(g_jits[slot].cc, sizeof(g_jits[slot].cc), ctx, argv, 1);
    opt_str(g_jits[slot].cc_flags, sizeof(g_jits[slot].cc_flags), ctx, argv, 2);
    opt_str(g_jits[slot].inc_dir, sizeof(g_jits[slot].inc_dir), ctx, argv, 3);
    opt_str(g_jits[slot].lib_dir, sizeof(g_jits[slot].lib_dir), ctx, argv, 4);
    opt_str(g_jits[slot].tmp_dir, sizeof(g_jits[slot].tmp_dir), ctx, argv, 6);
    opt_str(g_jits[slot].cache_dir, sizeof(g_jits[slot].cache_dir), ctx, argv, 7);
    opts.cc = g_jits[slot].cc[0] ? g_jits[slot].cc : NULL;
    opts.cc_flags = g_jits[slot].cc_flags[0] ? g_jits[slot].cc_flags : NULL;
    opts.inc_dir = g_jits[slot].inc_dir[0] ? g_jits[slot].inc_dir : NULL;
    opts.lib_dir = g_jits[slot].lib_dir[0] ? g_jits[slot].lib_dir : NULL;
    opts.tmp_dir = g_jits[slot].tmp_dir[0] ? g_jits[slot].tmp_dir : NULL;
    opts.cache_dir = g_jits[slot].cache_dir[0] ? g_jits[slot].cache_dir : NULL;
    opts.hot_threshold = hot > 0 ? (unsigned)hot : 0;
    jit = api.JitNew((MBJSJitBackend)backend, &opts);
    if (!jit)
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    g_jits[slot].used = 1;
    g_jits[slot].jit = jit;
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)slot);
}

static JitSlot *jit_slot(MBJSContext *ctx, MBJSValueConst v)
{
    int id = -1;
    if (arg_int(ctx, v, &id) < 0 || id < 0 || id >= MAX_JITS || !g_jits[id].used) {
        set_error("invalid jit id");
        return NULL;
    }
    return &g_jits[id];
}

static MBJSValue mn_jit_info(MBJSContext *ctx, MBJSValueConst this_val,
                             int argc, MBJSValueConst *argv)
{
    JitSlot *j;
    int which;
    (void)this_val;
    if (argc < 2 || !(j = jit_slot(ctx, argv[0])))
        return cstr_result(ctx, "");
    if (arg_int(ctx, argv[1], &which) < 0)
        which = 0;
    if (which == 0)
        return cstr_result(ctx, api.JitBackendName(j->jit));
    return cstr_result(ctx, api.JitLastError(j->jit));
}

static MBJSValue mn_jit_stats(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    JitSlot *j;
    MBJSJitStats s;
    char buf[1024];
    int n;
    (void)this_val;
    if (argc < 1 || !(j = jit_slot(ctx, argv[0])))
        return cstr_result(ctx, "{}");
    s = api.JitStats(j->jit);
    n = snprintf(buf, sizeof(buf),
        "{\"compile_attempts\":%lu,\"compile_ok\":%lu,\"compile_failed\":%lu,"
        "\"native_calls\":%lu,\"interp_calls\":%lu,\"hot_tracked\":%lu,"
        "\"hot_compiled\":%lu,\"cache_hits\":%lu,\"cache_disk_hits\":%lu,"
        "\"cache_disk_misses\":%lu,\"cache_disk_writes\":%lu,"
        "\"cache_disk_errors\":%lu}",
        s.compile_attempts, s.compile_ok, s.compile_failed,
        s.native_calls, s.interp_calls, s.hot_tracked,
        s.hot_compiled, s.cache_hits, s.cache_disk_hits,
        s.cache_disk_misses, s.cache_disk_writes, s.cache_disk_errors);
    return rt.NewStringLen(ctx, buf, (size_t)n);
}

static MBJSValue mn_jit_compile(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    JitSlot *j;
    CtxSlot *c;
    const char *source;
    const char *name;
    char *buf;
    MBJSJitFn fn;
    int slot;
    int jitId;
    int ctxId;
    (void)this_val;
    if (argc < 4 || !(j = jit_slot(ctx, argv[0])) || !(c = ctx_slot(ctx, argv[1])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    source = arg_cstr(ctx, argv[2]);
    name = arg_cstr(ctx, argv[3]);
    if (!source || !name) {
        if (source) rt.FreeCString(ctx, source);
        if (name) rt.FreeCString(ctx, name);
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    }
    buf = nul_copy(source, NULL);
    rt.FreeCString(ctx, source);
    if (!buf) {
        rt.FreeCString(ctx, name);
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    }
    fn = api.JitCompile(j->jit, c->ctx, buf, name);
    free(buf);
    rt.FreeCString(ctx, name);
    if (!fn)
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    jitId = (int)(int64_t)(uintptr_t)(j - g_jits);
    ctxId = (int)(int64_t)(uintptr_t)(c - g_ctxs);
    for (slot = 0; slot < MAX_JITFNS; slot++) {
        if (!g_jitfns[slot].used) {
            g_jitfns[slot].used = 1;
            g_jitfns[slot].jitId = jitId;
            g_jitfns[slot].ctxId = ctxId;
            g_jitfns[slot].fn = fn;
            return rt.NewBigUint64(ctx, (uint64_t)(int64_t)slot);
        }
    }
    set_error("jit function table full");
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
}

static MBJSValue mn_jit_invoke(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    JitFnSlot *f;
    MBJSValue thisValue = MBJS_UNDEFINED;
    MBJSValue values[16];
    MBJSValue *args = NULL;
    int fnargc = 0;
    int i;
    int id = -1;
    (void)this_val;
    if (argc < 1 || arg_int(ctx, argv[0], &id) < 0 ||
        id < 0 || id >= MAX_JITFNS || !g_jitfns[id].used)
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    f = &g_jitfns[id];
    if (argc > 1) {
        int thisId = value_slot_id(ctx, argv[1]);
        if (thisId >= 0) thisValue = g_values[thisId].value;
    }
    for (i = 2; i < argc && fnargc < 16; i++) {
        int slotId = value_slot_id(ctx, argv[i]);
        if (slotId < 0)
            return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
        values[fnargc++] = g_values[slotId].value;
    }
    if (fnargc) args = values;
    {
        MBJSValue result = f->fn(g_ctxs[f->ctxId].ctx, thisValue, fnargc, args);
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store(f->ctxId, result));
    }
}

static MBJSValue mn_jit_call(MBJSContext *ctx, MBJSValueConst this_val,
                             int argc, MBJSValueConst *argv)
{
    JitSlot *j;
    CtxSlot *c;
    const char *source;
    const char *name;
    char *buf;
    MBJSValue thisValue = MBJS_UNDEFINED;
    MBJSValue values[16];
    MBJSValue *args = NULL;
    int fnargc = 0;
    int i;
    MBJSValue result;
    (void)this_val;
    if (argc < 4 || !(j = jit_slot(ctx, argv[0])) || !(c = ctx_slot(ctx, argv[1])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    source = arg_cstr(ctx, argv[2]);
    name = arg_cstr(ctx, argv[3]);
    if (!source || !name) {
        if (source) rt.FreeCString(ctx, source);
        if (name) rt.FreeCString(ctx, name);
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    }
    if (argc > 4) {
        int thisId = value_slot_id(ctx, argv[4]);
        if (thisId >= 0) thisValue = g_values[thisId].value;
    }
    for (i = 5; i < argc && fnargc < 16; i++) {
        int slotId = value_slot_id(ctx, argv[i]);
        if (slotId < 0) {
            rt.FreeCString(ctx, source);
            rt.FreeCString(ctx, name);
            return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
        }
        values[fnargc++] = g_values[slotId].value;
    }
    if (fnargc) args = values;
    buf = nul_copy(source, NULL);
    rt.FreeCString(ctx, source);
    result = api.JitCall(j->jit, c->ctx, buf ? buf : "", name, thisValue, fnargc, args);
    free(buf);
    rt.FreeCString(ctx, name);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store(
        (int)(int64_t)(uintptr_t)(c - g_ctxs), result));
}

static MBJSValue mn_jit_cleanup(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    JitSlot *j;
    CtxSlot *c;
    int i;
    (void)this_val;
    if (argc < 2 || !(j = jit_slot(ctx, argv[0])) || !(c = ctx_slot(ctx, argv[1])))
        return MBJS_UNDEFINED;
    api.JitCleanupAll(j->jit, c->ctx);
    for (i = 0; i < MAX_JITFNS; i++)
        if (g_jitfns[i].used && g_jitfns[i].jitId == (int)(int64_t)(uintptr_t)(j - g_jits) &&
            g_jitfns[i].ctxId == (int)(int64_t)(uintptr_t)(c - g_ctxs))
            memset(&g_jitfns[i], 0, sizeof(g_jitfns[i]));
    return MBJS_UNDEFINED;
}

static MBJSValue mn_jit_free(MBJSContext *ctx, MBJSValueConst this_val,
                             int argc, MBJSValueConst *argv)
{
    JitSlot *j;
    int jitId;
    int i;
    (void)this_val;
    if (argc < 1 || !(j = jit_slot(ctx, argv[0])))
        return MBJS_UNDEFINED;
    jitId = (int)(int64_t)(uintptr_t)(j - g_jits);
    for (i = 0; i < MAX_JITFNS; i++) {
        if (g_jitfns[i].used && g_jitfns[i].jitId == jitId) {
            if (g_ctxs[g_jitfns[i].ctxId].used)
                api.JitCleanupAll(j->jit, g_ctxs[g_jitfns[i].ctxId].ctx);
            memset(&g_jitfns[i], 0, sizeof(g_jitfns[i]));
        }
    }
    api.JitFree(j->jit);
    memset(j, 0, sizeof(*j));
    return MBJS_UNDEFINED;
}

/* ------------------------------------------------------------------ *
 * v0.4 engine-control additions (value / object / property / promise /
 * memory introspection) — "ngendaliin engine" surface.
 * ------------------------------------------------------------------ */

static CtxSlot *value_owner(ValSlot *v)
{
    if (!v || v->ctxId < 0 || v->ctxId >= MAX_CTXS || !g_ctxs[v->ctxId].used) {
        set_error("value has no live context");
        return NULL;
    }
    return &g_ctxs[v->ctxId];
}

static MBJSValue mn_value_dup(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    ValSlot *v;
    CtxSlot *c;
    (void)this_val;
    if (argc < 1 || !(v = value_slot(ctx, argv[0])) || !(c = value_owner(v)))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store(
        v->ctxId, api.DupValue(c->ctx, v->value)));
}

static MBJSValue mn_value_to_bool(MBJSContext *ctx, MBJSValueConst this_val,
                                  int argc, MBJSValueConst *argv)
{
    ValSlot *v;
    CtxSlot *c;
    int b;
    (void)this_val;
    if (argc < 1 || !(v = value_slot(ctx, argv[0])) || !(c = value_owner(v)))
        return rt.NewBigUint64(ctx, 0);
    b = api.ToBool(c->ctx, v->value);
    return rt.NewBigUint64(ctx, (uint64_t)(b > 0 ? 1 : 0));
}

static MBJSValue mn_value_eq(MBJSContext *ctx, MBJSValueConst this_val,
                             int argc, MBJSValueConst *argv)
{
    ValSlot *a;
    ValSlot *b;
    CtxSlot *c;
    int r;
    (void)this_val;
    if (argc < 2 || !(a = value_slot(ctx, argv[0])) || !(b = value_slot(ctx, argv[1])) ||
        !(c = value_owner(a)))
        return rt.NewBigUint64(ctx, 0);
    if (a->ctxId != b->ctxId) {
        set_error("valueEq: values belong to different contexts");
        return rt.NewBigUint64(ctx, 0);
    }
    r = api.IsEqual(c->ctx, a->value, b->value);
    return rt.NewBigUint64(ctx, (uint64_t)(r > 0 ? 1 : 0));
}

static MBJSValue mn_value_is_error(MBJSContext *ctx, MBJSValueConst this_val,
                                   int argc, MBJSValueConst *argv)
{
    ValSlot *v;
    (void)this_val;
    if (argc < 1 || !(v = value_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, 0);
    return rt.NewBigUint64(ctx, api.IsError(v->value) ? 1 : 0);
}

static MBJSValue mn_value_is_function(MBJSContext *ctx, MBJSValueConst this_val,
                                      int argc, MBJSValueConst *argv)
{
    ValSlot *v;
    CtxSlot *c;
    (void)this_val;
    if (argc < 1 || !(v = value_slot(ctx, argv[0])) || !(c = value_owner(v)))
        return rt.NewBigUint64(ctx, 0);
    return rt.NewBigUint64(ctx, api.IsFunction(c->ctx, v->value) ? 1 : 0);
}

static MBJSValue mn_new_object(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    (void)this_val;
    if (argc < 1 || !(c = ctx_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store(
        (int)(int64_t)(uintptr_t)(c - g_ctxs), api.NewObject(c->ctx)));
}

static MBJSValue mn_new_array(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    (void)this_val;
    if (argc < 1 || !(c = ctx_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store(
        (int)(int64_t)(uintptr_t)(c - g_ctxs), api.NewArray(c->ctx)));
}

static MBJSValue mn_prop_define(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    ValSlot *obj;
    ValSlot *val;
    CtxSlot *c;
    const char *key;
    int flags = MBJS_PROP_C_W_E;
    int r;
    (void)this_val;
    if (argc < 3 || !(obj = value_slot(ctx, argv[0])) ||
        !(val = value_slot(ctx, argv[2])) || !(c = value_owner(obj)))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    key = arg_cstr(ctx, argv[1]);
    if (!key)
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    if (argc > 3) arg_int(ctx, argv[3], &flags);
    r = api.DefinePropertyValueStr(c->ctx, obj->value, key,
                                   api.DupValue(c->ctx, val->value), flags);
    rt.FreeCString(ctx, key);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)(r > 0 ? 1 : r));
}

static MBJSValue mn_prop_has(MBJSContext *ctx, MBJSValueConst this_val,
                             int argc, MBJSValueConst *argv)
{
    ValSlot *obj;
    CtxSlot *c;
    const char *key;
    MBJSAtom atom;
    int r;
    (void)this_val;
    if (argc < 2 || !(obj = value_slot(ctx, argv[0])) || !(c = value_owner(obj)))
        return rt.NewBigUint64(ctx, 0);
    key = arg_cstr(ctx, argv[1]);
    if (!key)
        return rt.NewBigUint64(ctx, 0);
    atom = api.NewAtom(c->ctx, key);
    rt.FreeCString(ctx, key);
    r = api.HasProperty(c->ctx, obj->value, atom);
    api.FreeAtom(c->ctx, atom);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)r);
}

static MBJSValue mn_prop_delete(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    ValSlot *obj;
    CtxSlot *c;
    const char *key;
    MBJSAtom atom;
    int r;
    (void)this_val;
    if (argc < 2 || !(obj = value_slot(ctx, argv[0])) || !(c = value_owner(obj)))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    key = arg_cstr(ctx, argv[1]);
    if (!key)
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    atom = api.NewAtom(c->ctx, key);
    rt.FreeCString(ctx, key);
    r = api.DeleteProperty(c->ctx, obj->value, atom, 0);
    api.FreeAtom(c->ctx, atom);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)(r > 0 ? 1 : r));
}

static MBJSValue mn_prop_keys(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    ValSlot *obj;
    CtxSlot *c;
    MBJSPropertyEnum *tab = NULL;
    uint32_t len = 0, i;
    char *buf;
    size_t cap, used = 0;
    MBJSValue out;
    (void)this_val;
    if (argc < 1 || !(obj = value_slot(ctx, argv[0])) || !(c = value_owner(obj)))
        return cstr_result(ctx, "[]");
    if (api.GetOwnPropertyNames(c->ctx, &tab, &len, obj->value,
                                MBJS_GPN_STRING_MASK) < 0)
        return cstr_result(ctx, "[]");
    cap = 64 + (size_t)len * 96;
    buf = (char *)malloc(cap);
    if (!buf) {
        api.FreePropertyEnum(c->ctx, tab, len);
        return cstr_result(ctx, "[]");
    }
    used += (size_t)snprintf(buf + used, cap - used, "[");
    for (i = 0; i < len; i++) {
        const char *name = api.AtomToCStringLen(c->ctx, NULL, tab[i].atom);
        if (name) {
            used += (size_t)snprintf(buf + used, cap - used, "%s\"%s\"",
                                     i ? "," : "", name);
            rt.FreeCString(c->ctx, name);
        } else {
            used += (size_t)snprintf(buf + used, cap - used, "%s\"\"", i ? "," : "");
        }
    }
    used += (size_t)snprintf(buf + used, cap - used, "]");
    api.FreePropertyEnum(c->ctx, tab, len);
    out = rt.NewStringLen(ctx, buf, used);
    free(buf);
    return out;
}

static MBJSValue mn_proto_get(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    ValSlot *v;
    CtxSlot *c;
    (void)this_val;
    if (argc < 1 || !(v = value_slot(ctx, argv[0])) || !(c = value_owner(v)))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store(
        v->ctxId, api.GetPrototype(c->ctx, v->value)));
}

static MBJSValue mn_proto_set(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    ValSlot *obj;
    ValSlot *proto;
    CtxSlot *c;
    int r;
    (void)this_val;
    if (argc < 2 || !(obj = value_slot(ctx, argv[0])) ||
        !(proto = value_slot(ctx, argv[1])) || !(c = value_owner(obj)))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    if (obj->ctxId != proto->ctxId) {
        set_error("protoSet: values belong to different contexts");
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    }
    r = api.SetPrototype(c->ctx, obj->value, proto->value);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)r);
}

static MBJSValue mn_throw_error(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    const char *kind;
    const char *msg;
    (void)this_val;
    if (argc < 1 || !(c = ctx_slot(ctx, argv[0])))
        return MBJS_UNDEFINED;
    kind = argc > 1 ? arg_cstr(ctx, argv[1]) : NULL;
    msg = argc > 2 ? arg_cstr(ctx, argv[2]) : NULL;
    if (kind && strcmp(kind, "range") == 0)
        api.ThrowRangeError(c->ctx, "%s", msg ? msg : "error");
    else if (kind && strcmp(kind, "reference") == 0)
        api.ThrowReferenceError(c->ctx, "%s", msg ? msg : "error");
    else if (kind && strcmp(kind, "syntax") == 0)
        rt.ThrowTypeError(c->ctx, "%s", msg ? msg : "error");
    else
        rt.ThrowTypeError(c->ctx, "%s", msg ? msg : "error");
    if (kind) rt.FreeCString(ctx, kind);
    if (msg) rt.FreeCString(ctx, msg);
    return MBJS_UNDEFINED;
}

static MBJSValue mn_exception_has(MBJSContext *ctx, MBJSValueConst this_val,
                                  int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    (void)this_val;
    if (argc < 1 || !(c = ctx_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, 0);
    return rt.NewBigUint64(ctx, api.HasException(c->ctx) ? 1 : 0);
}

static MBJSValue mn_memory_usage(MBJSContext *ctx, MBJSValueConst this_val,
                                 int argc, MBJSValueConst *argv)
{
    RtSlot *r;
    MBJSMemoryUsage s;
    char buf[2048];
    int n;
    (void)this_val;
    if (argc < 1 || !(r = rt_slot(ctx, argv[0])))
        return cstr_result(ctx, "{}");
    memset(&s, 0, sizeof(s));
    api.ComputeMemoryUsage(r->rt, &s);
    n = snprintf(buf, sizeof(buf),
        "{\"malloc_size\":%lld,\"malloc_limit\":%lld,\"memory_used_size\":%lld,"
        "\"malloc_count\":%lld,\"memory_used_count\":%lld,"
        "\"atom_count\":%lld,\"atom_size\":%lld,"
        "\"str_count\":%lld,\"str_size\":%lld,"
        "\"obj_count\":%lld,\"obj_size\":%lld,"
        "\"prop_count\":%lld,\"prop_size\":%lld,"
        "\"shape_count\":%lld,\"shape_size\":%lld,"
        "\"func_count\":%lld,\"func_size\":%lld,\"func_code_size\":%lld,"
        "\"c_func_count\":%lld,\"array_count\":%lld,"
        "\"fast_array_count\":%lld,\"fast_array_elements\":%lld}",
        (long long)s.malloc_size, (long long)s.malloc_limit,
        (long long)s.memory_used_size, (long long)s.malloc_count,
        (long long)s.memory_used_count, (long long)s.atom_count,
        (long long)s.atom_size, (long long)s.str_count, (long long)s.str_size,
        (long long)s.obj_count, (long long)s.obj_size,
        (long long)s.prop_count, (long long)s.prop_size,
        (long long)s.shape_count, (long long)s.shape_size,
        (long long)s.mbjs_func_count, (long long)s.mbjs_func_size,
        (long long)s.mbjs_func_code_size, (long long)s.c_func_count,
        (long long)s.array_count, (long long)s.fast_array_count,
        (long long)s.fast_array_elements);
    return rt.NewStringLen(ctx, buf, (size_t)n);
}

static MBJSValue mn_bigint_new(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    uint64_t bits = 0;
    (void)this_val;
    if (argc < 2 || !(c = ctx_slot(ctx, argv[0])))
        return rt.NewBigUint64(ctx, (uint64_t)(int64_t)-1);
    if (argc > 2) arg_u64(ctx, argv[2], &bits);
    return rt.NewBigUint64(ctx, (uint64_t)(int64_t)value_store(
        (int)(int64_t)(uintptr_t)(c - g_ctxs), rt.NewBigUint64(c->ctx, bits)));
}

static MBJSValue mn_stringify(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    ValSlot *v;
    MBJSValue space = MBJS_UNDEFINED;
    MBJSValue r;
    const char *s;
    (void)this_val;
    if (argc < 2 || !(c = ctx_slot(ctx, argv[0])) || !(v = value_slot(ctx, argv[1])))
        return cstr_result(ctx, "null");
    if (argc > 2 && MBJS_IsNumber(argv[2])) {
        double d = 0;
        rt.ToFloat64(ctx, &d, argv[2]);
        if (d >= 0) space = MBJS_NewInt32(c->ctx, (int32_t)d);
    }
    r = api.JSONStringify(c->ctx, v->value, MBJS_UNDEFINED, space);
    if (MBJS_IsException(r)) {
        api.FreeValue(c->ctx, r);
        return cstr_result(ctx, "null");
    }
    s = rt.ToCStringLen(c->ctx, NULL, r);
    {
        MBJSValue out = cstr_result(ctx, s);
        if (s) rt.FreeCString(c->ctx, s);
        api.FreeValue(c->ctx, r);
        return out;
    }
}

static MBJSValue mn_handle_stats(MBJSContext *ctx, MBJSValueConst this_val,
                                 int argc, MBJSValueConst *argv)
{
    int i, c_rts = 0, c_ctxs = 0, c_vals = 0, c_binds = 0, c_jits = 0, c_jitfns = 0;
    char buf[512];
    int n;
    (void)this_val; (void)argc; (void)argv;
    for (i = 0; i < MAX_RTS; i++) c_rts += g_rts[i].used;
    for (i = 0; i < MAX_CTXS; i++) c_ctxs += g_ctxs[i].used;
    for (i = 0; i < MAX_VALUES; i++) c_vals += g_values[i].used;
    for (i = 0; i < MAX_BINDS; i++) c_binds += g_binds[i].used;
    for (i = 0; i < MAX_JITS; i++) c_jits += g_jits[i].used;
    for (i = 0; i < MAX_JITFNS; i++) c_jitfns += g_jitfns[i].used;
    n = snprintf(buf, sizeof(buf),
        "{\"runtimes\":%d,\"contexts\":%d,\"values\":%d,\"binds\":%d,"
        "\"jits\":%d,\"jitfns\":%d,\"caps\":{\"runtimes\":%d,\"contexts\":%d,"
        "\"values\":%d,\"binds\":%d,\"jits\":%d,\"jitfns\":%d}}",
        c_rts, c_ctxs, c_vals, c_binds, c_jits, c_jitfns,
        MAX_RTS, MAX_CTXS, MAX_VALUES, MAX_BINDS, MAX_JITS, MAX_JITFNS);
    return rt.NewStringLen(ctx, buf, (size_t)n);
}

static MBJSValue mn_promise_new(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    CtxSlot *c;
    MBJSValue funcs[2];
    MBJSValue promise;
    char buf[256];
    int n, pId, rId, jId;
    (void)this_val;
    if (argc < 1 || !(c = ctx_slot(ctx, argv[0])))
        return cstr_result(ctx, "{\"promise\":-1}");
    promise = api.NewPromiseCapability(c->ctx, funcs);
    if (MBJS_IsException(promise))
        return cstr_result(ctx, "{\"promise\":-1}");
    pId = value_store((int)(int64_t)(uintptr_t)(c - g_ctxs), promise);
    rId = value_store((int)(int64_t)(uintptr_t)(c - g_ctxs), funcs[0]);
    jId = value_store((int)(int64_t)(uintptr_t)(c - g_ctxs), funcs[1]);
    n = snprintf(buf, sizeof(buf),
        "{\"promise\":%d,\"resolve\":%d,\"reject\":%d}", pId, rId, jId);
    return rt.NewStringLen(ctx, buf, (size_t)n);
}

#define MN_TAB_LEN 65

static const MBJSCFunctionListEntry mn_tab[MN_TAB_LEN] = {
    MBJS_CFUNC_DEF("version", 0, mn_version),
    MBJS_CFUNC_DEF("abi", 0, mn_abi),
    MBJS_CFUNC_DEF("exportNames", 0, mn_export_names),
    MBJS_CFUNC_DEF("exportSignature", 1, mn_export_signature),
    MBJS_CFUNC_DEF("hostBase", 0, mn_host_base),
    MBJS_CFUNC_DEF("hostExport", 1, mn_host_export),
    MBJS_CFUNC_DEF("engineVersion", 0, mn_engine_version),
    MBJS_CFUNC_DEF("bind", 2, mn_bind),
    MBJS_CFUNC_DEF("call", 9, mn_call),
    MBJS_CFUNC_DEF("unbind", 1, mn_unbind),
    MBJS_CFUNC_DEF("lastError", 0, mn_last_error),
    MBJS_CFUNC_DEF("runtimeNew", 0, mn_runtime_new),
    MBJS_CFUNC_DEF("runtimeFree", 1, mn_runtime_free),
    MBJS_CFUNC_DEF("runtimeLimits", 3, mn_runtime_limits),
    MBJS_CFUNC_DEF("contextNew", 2, mn_context_new),
    MBJS_CFUNC_DEF("contextFree", 1, mn_context_free),
    MBJS_CFUNC_DEF("contextStd", 1, mn_context_std),
    MBJS_CFUNC_DEF("valueNew", 3, mn_value_new),
    MBJS_CFUNC_DEF("valueFree", 1, mn_value_free),
    MBJS_CFUNC_DEF("valueType", 1, mn_value_type),
    MBJS_CFUNC_DEF("valueIsException", 1, mn_value_is_exception),
    MBJS_CFUNC_DEF("valueToNumber", 1, mn_value_to_number),
    MBJS_CFUNC_DEF("valueToInt", 2, mn_value_to_int),
    MBJS_CFUNC_DEF("valueToString", 1, mn_value_to_string),
    MBJS_CFUNC_DEF("valueJson", 1, mn_value_json),
    MBJS_CFUNC_DEF("eval", 4, mn_eval),
    MBJS_CFUNC_DEF("evalModule", 3, mn_eval_module),
    MBJS_CFUNC_DEF("detectModule", 1, mn_detect_module),
    MBJS_CFUNC_DEF("getException", 1, mn_get_exception),
    MBJS_CFUNC_DEF("dumpError", 1, mn_dump_error),
    MBJS_CFUNC_DEF("await", 2, mn_await),
    MBJS_CFUNC_DEF("loop", 2, mn_loop),
    MBJS_CFUNC_DEF("callValue", 18, mn_call_value),
    MBJS_CFUNC_DEF("getGlobal", 1, mn_get_global),
    MBJS_CFUNC_DEF("getProp", 3, mn_get_prop),
    MBJS_CFUNC_DEF("setProp", 4, mn_set_prop),
    MBJS_CFUNC_DEF("jsonParse", 2, mn_json_parse),
    MBJS_CFUNC_DEF("jitNew", 8, mn_jit_new),
    MBJS_CFUNC_DEF("jitInfo", 2, mn_jit_info),
    MBJS_CFUNC_DEF("jitStats", 1, mn_jit_stats),
    MBJS_CFUNC_DEF("jitCompile", 4, mn_jit_compile),
    MBJS_CFUNC_DEF("jitInvoke", 18, mn_jit_invoke),
    MBJS_CFUNC_DEF("jitCall", 21, mn_jit_call),
    MBJS_CFUNC_DEF("jitCleanup", 2, mn_jit_cleanup),
    MBJS_CFUNC_DEF("jitFree", 1, mn_jit_free),
    MBJS_CFUNC_DEF("valueDup", 1, mn_value_dup),
    MBJS_CFUNC_DEF("valueToBool", 1, mn_value_to_bool),
    MBJS_CFUNC_DEF("valueEq", 2, mn_value_eq),
    MBJS_CFUNC_DEF("valueIsError", 1, mn_value_is_error),
    MBJS_CFUNC_DEF("valueIsFunction", 1, mn_value_is_function),
    MBJS_CFUNC_DEF("newObject", 1, mn_new_object),
    MBJS_CFUNC_DEF("newArray", 1, mn_new_array),
    MBJS_CFUNC_DEF("propDefine", 4, mn_prop_define),
    MBJS_CFUNC_DEF("propHas", 2, mn_prop_has),
    MBJS_CFUNC_DEF("propDelete", 2, mn_prop_delete),
    MBJS_CFUNC_DEF("propKeys", 1, mn_prop_keys),
    MBJS_CFUNC_DEF("protoGet", 1, mn_proto_get),
    MBJS_CFUNC_DEF("protoSet", 2, mn_proto_set),
    MBJS_CFUNC_DEF("throwError", 3, mn_throw_error),
    MBJS_CFUNC_DEF("exceptionHas", 1, mn_exception_has),
    MBJS_CFUNC_DEF("memoryUsage", 1, mn_memory_usage),
    MBJS_CFUNC_DEF("bigintNew", 3, mn_bigint_new),
    MBJS_CFUNC_DEF("stringify", 3, mn_stringify),
    MBJS_CFUNC_DEF("handleStats", 0, mn_handle_stats),
    MBJS_CFUNC_DEF("promiseNew", 1, mn_promise_new)
};

static int mn_mod_init(MBJSContext *ctx, MBJSModuleDef *m)
{
    return rt.SetModuleExportList(ctx, m, mn_tab, MN_TAB_LEN);
}

MBJS_MODULE_EXTERN MBJSModuleDef *mbjs_init_module(MBJSContext *ctx,
                                                    const char *name)
{
    const char *missing;
    MBJSModuleDef *m;
    missing = mbjs_runtime_resolve(&rt);
    if (missing)
        return NULL;
    missing = api_resolve();
    if (missing)
        return NULL;
    m = rt.NewCModule(ctx, name ? name : "mochijsnative", mn_mod_init);
    if (!m)
        return NULL;
    if (rt.AddModuleExportList(ctx, m, mn_tab, MN_TAB_LEN) != 0)
        return NULL;
    return m;
}
