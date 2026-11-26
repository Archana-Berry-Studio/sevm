#ifndef MB_MOCHISANDBOX_ABI_H
#define MB_MOCHISANDBOX_ABI_H
#ifdef __cplusplus
extern "C" {
#endif

/* ABI for mochisandbox — engine plugin. Entry point: mbjs_init_module().
   Runtime exports (verified via mbjs loadNative):
     - create()   -> string id (fresh runtime + context per sandbox)
     - setGlobal(id, nameLiteral, valueLiteral) -> undefined
     - run(id, source) -> JSON string of completion value (throws on error)
     - runAsync(id, source) -> JSON {"state":"fulfilled","value":...} or
                            {"state":"pending"}; drains the realm's microtask
                            queue; a rejected promise throws on the host side
     - free(id)   -> undefined
     - count()    -> string count of live sandboxes
   The module resolves MBJS_NewRuntime/NewContext/Eval2/SetMemoryLimit/
   SetInterruptHandler/GetException/... from the host image at load time. */
#ifdef __cplusplus
}
#endif
#endif
