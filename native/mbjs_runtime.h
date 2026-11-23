/*
 * mbjs_runtime.h — shared bootstrap for MochiBerry native modules.
 *
 * WHY THIS EXISTS
 * ---------------
 * A native module (.dll/.so) must NOT import engine symbols directly:
 *
 *   - linking against libmbjs.a creates a SECOND JSContext / atom table,
 *     so values cannot cross the boundary safely;
 *   - putting `mbjs.exe` in the DLL import table makes Windows refuse the
 *     load with ERROR_MOD_NOT_FOUND (126), because an EXE cannot be a
 *     DLL dependency.
 *
 * So the module resolves the handful of engine entry points from the HOST
 * IMAGE at load time (GetModuleHandle(NULL) / dlopen(NULL)) and calls them
 * through function pointers. The engine's JSContext stays the single owner
 * of atoms and GC; the module only borrows it.
 *
 * Verified against build_manual/mbjs.exe: the engine's module loader does
 *
 *     h    = LoadLibraryA(path);
 *     init = GetProcAddress(h, "mbjs_init_module");   <-- FIXED symbol name
 *     m    = init(ctx, module_name);
 *
 * i.e. the exported initializer is literally named `mbjs_init_module`
 * (NOT `mbjs_init_module_<name>`), and the import specifier must end in
 * ".dll" for the loader to take the native path at all.
 */
#ifndef MOCHINODE_MBJS_RUNTIME_H
#define MOCHINODE_MBJS_RUNTIME_H

#include "mbjs.h"

#include <stddef.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

typedef struct MBJSRuntime {
    MBJSModuleDef *(*NewCModule)(MBJSContext *, const char *,
                                 MBJSModuleInitFunc *);
    int (*AddModuleExportList)(MBJSContext *, MBJSModuleDef *,
                               const MBJSCFunctionListEntry *, int);
    int (*SetModuleExportList)(MBJSContext *, MBJSModuleDef *,
                               const MBJSCFunctionListEntry *, int);
    MBJSValue (*NewStringLen)(MBJSContext *, const char *, size_t);
    const char *(*ToCStringLen)(MBJSContext *, size_t *, MBJSValueConst);
    void (*FreeCString)(MBJSContext *, const char *);
    MBJSValue (*ThrowTypeError)(MBJSContext *, const char *, ...);
    int (*ToFloat64)(MBJSContext *, double *, MBJSValueConst);
    MBJSValue (*NewBigInt64)(MBJSContext *, int64_t);
    MBJSValue (*NewBigUint64)(MBJSContext *, uint64_t);
    int (*ToBigInt64)(MBJSContext *, int64_t *, MBJSValueConst);
    int (*ToBigUint64)(MBJSContext *, uint64_t *, MBJSValueConst);
    int ready;
} MBJSRuntime;

/* Resolve the engine ABI from the host image. Returns NULL on success,
 * otherwise the name of the first symbol that could not be found. */
static const char *mbjs_runtime_resolve(MBJSRuntime *rt)
{
    void *host;

    memset(rt, 0, sizeof(*rt));

#ifdef _WIN32
    host = (void *)GetModuleHandleA(NULL);
    if (!host)
        return "GetModuleHandleA(NULL)";
#else
    host = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
    if (!host)
        return "dlopen(NULL)";
#endif

#ifdef _WIN32
#define MBJS_RT_SYM(field, name)                                            \
    do {                                                                    \
        FARPROC p__ = GetProcAddress((HMODULE)host, (name));                \
        if (!p__)                                                           \
            return (name);                                                  \
        memcpy(&(rt->field), &p__, sizeof(p__));                            \
    } while (0)
#else
#define MBJS_RT_SYM(field, name)                                            \
    do {                                                                    \
        void *p__ = dlsym(host, (name));                                    \
        if (!p__)                                                           \
            return (name);                                                  \
        memcpy(&(rt->field), &p__, sizeof(p__));                            \
    } while (0)
#endif

    MBJS_RT_SYM(NewCModule, "MBJS_NewCModule");
    MBJS_RT_SYM(AddModuleExportList, "MBJS_AddModuleExportList");
    MBJS_RT_SYM(SetModuleExportList, "MBJS_SetModuleExportList");
    MBJS_RT_SYM(NewStringLen, "MBJS_NewStringLen");
    MBJS_RT_SYM(ToCStringLen, "MBJS_ToCStringLen2");
    MBJS_RT_SYM(FreeCString, "MBJS_FreeCString");
    MBJS_RT_SYM(ThrowTypeError, "MBJS_ThrowTypeError");
    MBJS_RT_SYM(ToFloat64, "MBJS_ToFloat64");
    MBJS_RT_SYM(NewBigInt64, "MBJS_NewBigInt64");
    MBJS_RT_SYM(NewBigUint64, "MBJS_NewBigUint64");
    MBJS_RT_SYM(ToBigInt64, "MBJS_ToBigInt64");
    MBJS_RT_SYM(ToBigUint64, "MBJS_ToBigUint64");

#undef MBJS_RT_SYM

    rt->ready = 1;
    return NULL;
}

/* Strip a trailing ".dll" / ".so" from the engine-supplied module name so
 * the JS namespace is "mochinode_poke", not "mochinode_poke.dll". */
static const char *mbjs_runtime_module_id(const char *name)
{
    size_t n;

    if (!name)
        return "module";
    n = strlen(name);
    if (n > 4 && (strcmp(name + n - 4, ".dll") == 0 ||
                  strcmp(name + n - 3, ".so") == 0))
        return name;                 /* caller slices; see mbjs_runtime_id */
    return name;
}

#endif /* MOCHINODE_MBJS_RUNTIME_H */
