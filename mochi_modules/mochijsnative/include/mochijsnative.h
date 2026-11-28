#ifndef MB_MOCHIJSNATIVE_H
#define MB_MOCHIJSNATIVE_H
/*
 * mochijsnative -- MochiBerry native-first module (v0.4.0).
 *
 * ABI contract (see mochiberry/module-loader-contract.md):
 *   - exports the single entry point mbjs_init_module(MBJSContext*, const char*)
 *   - registers C functions via NewCModule / AddModuleExportList / SetModuleExportList
 *   - C functions that throw MUST return MBJS_EXCEPTION (never MBJS_UNDEFINED)
 *
 * HOST REQUIREMENT (the #1 integration gotcha): the module resolves engine
 * symbols (MBJS_*) at load time from the host image, so the host exe MUST be
 * linked with -Wl,--export-all-symbols (and the JIT translation units pulled
 * in via --whole-archive). If the engine is a shared library instead, the
 * resolver falls back to mbjs.dll / libmbjs.dll / libmochiberry.dll on its
 * own. A host that exports nothing fails init with
 * "mbjs_call_module ... initialization error" -- that message means the ABI
 * was not exported, not that the DLL is missing.
 *
 * Runtime symbols are resolved through mbjs_host_symbol() from
 * <root>/native/mbjs_runtime.h at load time; do NOT link against mbjs.exe.
 *
 * Exported symbols (65, v0.4.0):
 *   meta:                version, abi, exportNames, exportSignature, hostBase, hostExport, engineVersion, lastError, handleStats, detectModule
 *   runtime/context:     runtimeNew, runtimeFree, runtimeLimits, contextNew, contextFree, contextStd, getException, dumpError, await, loop, throwError, exceptionHas, memoryUsage
 *   value:               valueNew, valueFree, valueType, valueIsException, valueToNumber, valueToInt, valueToString, valueJson, valueDup, valueToBool, valueEq, valueIsError, valueIsFunction, bigintNew
 *   objects/properties/call: newObject, newArray, getGlobal, getProp, setProp, propDefine, propHas, propDelete, propKeys, protoGet, protoSet, callValue, jsonParse, stringify, promiseNew
 *   execution:           eval, evalModule
 *   ffi:                 bind, call, unbind
 *   jit:                 jitNew, jitInfo, jitStats, jitCompile, jitInvoke, jitCall, jitCleanup, jitFree
 */
#include <stdint.h>

typedef struct MBJSContext MBJSContext;
typedef struct MBJSModuleDef MBJSModuleDef;

MBJSModuleDef *mbjs_init_module(MBJSContext *ctx, const char *name);

#endif /* MB_MOCHIJSNATIVE_H */
