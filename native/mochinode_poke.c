/*
 * mochinode_poke.c — feasibility probe: is a native `.dll` module loadable by
 * the MochiBerry engine at runtime?
 *
 * Exported as `mbjs_init_module`, the fixed symbol name the engine's loader
 * looks up (see mbjs_runtime.h for the disassembly-backed rationale).
 *
 * Import contract (verified by running the engine):
 *   - the import specifier MUST end in ".dll" or the loader treats it as a
 *     plain JS module and fails with "could not load module filename";
 *   - a relative specifier is loaded as "./<specifier>", i.e. relative to
 *     the process working directory.
 */
#include "mbjs_runtime.h"

#include <stdio.h>

static MBJSRuntime rt;

#ifdef MBJS_POKE_TRACE
#include <windows.h>

static LONG WINAPI poke_veh(EXCEPTION_POINTERS *ep)
{
    void *rip = (void *)ep->ContextRecord->Rip;
    void *here = NULL, *host = NULL;
    HMODULE h;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)rip, &h))
        here = (void *)h;
    host = (void *)GetModuleHandleA(NULL);

    fprintf(stderr,
            "[poke] FAULT code=%08lX rip=%p rsp=%p\n"
            "[poke]   in-module? %s | exe=%p | module=%p\n",
            (unsigned long)ep->ExceptionRecord->ExceptionCode, rip,
            (void *)ep->ContextRecord->Rsp,
            (here == host) ? "EXE" : (here ? "DLL" : "?"), host, here);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        AddVectoredExceptionHandler(1, poke_veh);
    return TRUE;
}

#define TRACE(...) do { fprintf(stderr, "[poke] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)
#else
#define TRACE(...) do { } while (0)
#endif

/* ------------------------------------------------------------------ */
/* the module itself                                                   */
/* ------------------------------------------------------------------ */

static MBJSValue poke_fn(MBJSContext *ctx, MBJSValueConst this_val,
                         int argc, MBJSValueConst *argv)
{
    (void)this_val;
    if (argc > 0) {
        double x = 0;
        rt.ToFloat64(ctx, &x, argv[0]);
        return MBJS_NewFloat64(ctx, x + 1);   /* header-only inline */
    }
    return MBJS_NewFloat64(ctx, 42.5);
}

static MBJSValue label_fn(MBJSContext *ctx, MBJSValueConst this_val,
                          int argc, MBJSValueConst *argv)
{
    static const char msg[] = "mochinode native module";
    (void)this_val;
    (void)argc;
    (void)argv;
    return rt.NewStringLen(ctx, msg, sizeof(msg) - 1);
}

/* Engine ordering (verified by reading mbjs-parser.c and the exe):
 *
 *   MBJS_EvalFunctionInternal → mbjs_create_module_function → creates a
 *   var_ref for EVERY export entry the module already has, then links,
 *   then EVALUATES, and only during evaluation calls m->init_func(ctx, m).
 *
 * So exports added inside init_func arrive AFTER the var_refs are made:
 * each entry keeps u.local.var_ref == NULL, and later
 * mbjs_get_local_export_var_ref1() dereferences the module's func_obj
 * (UNDEFINED for a C module) → access violation.
 *
 * The built-in C modules avoid this by adding their exports directly in
 * their mbjs_init_module_* (compare mbjs_init_module_std), so their export
 * entries exist BEFORE mbjs_create_module_function runs.
 *
 * Conclusion: add exports HERE, in the loader entry point; the module's
 * init_func stays a no-op. */
static int poke_mod_init(MBJSContext *ctx, MBJSModuleDef *m)
{
    const MBJSCFunctionListEntry tab[] = {
        MBJS_CFUNC_DEF("poke", 1, poke_fn),
        MBJS_CFUNC_DEF("label", 0, label_fn),
    };
    /* The export VALUES (the C functions) can only be created here, at
     * evaluate time, because mbjs_create_module_function() builds the
     * per-export var_refs only when the module is instantiated — after
     * this init_func is invoked. This mirrors the canonical QuickJS
     * pattern: AddModuleExportList() in the loader entry point (names),
     * SetModuleExportList() in the module's init func (values). */
    return rt.SetModuleExportList(ctx, m, tab, 2);
}

MBJS_MODULE_EXTERN MBJSModuleDef *mbjs_init_module(MBJSContext *ctx,
                                                    const char *name)
{
    const MBJSCFunctionListEntry tab[] = {
        MBJS_CFUNC_DEF("poke", 1, poke_fn),
        MBJS_CFUNC_DEF("label", 0, label_fn),
    };
    MBJSModuleDef *m;
    const char *missing = mbjs_runtime_resolve(&rt);

    TRACE("init_module name=%s missing=%s", name ? name : "(null)",
          missing ? missing : "none");
    if (missing)
        return NULL;                  /* engine ABI not fully present */

    m = rt.NewCModule(ctx, name ? name : "mochinode_poke", poke_mod_init);
    if (!m)
        return NULL;
    if (rt.AddModuleExportList(ctx, m, tab, 2) != 0)
        return NULL;
    TRACE("module %p registered with 2 exports", (void *)m);
    return m;
}
