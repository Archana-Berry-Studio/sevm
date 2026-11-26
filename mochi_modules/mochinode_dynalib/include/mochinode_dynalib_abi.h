#ifndef MB_MOCHINODE_DYNALIB_ABI_H
#define MB_MOCHINODE_DYNALIB_ABI_H
#ifdef __cplusplus
extern "C" {
#endif

/* ABI for mochinode_dynalib — engine plugin. Entry point: mbjs_init_module().
   Runtime exports (verified via mbjs loadNative): */
/*   - abiParse */
/*   - callSymbol */
/*   - closeModule */
/*   - freeSymbol */
/*   - lastError */
/*   - moduleBase */
/*   - moduleExports */
/*   - openModule */
/*   - resolveSymbol */
/*   - symbolAbi */
/*   - symbolBase */
/*   - symbolOffset */
/*   - symbolPtr */
#ifdef __cplusplus
}
#endif
#endif
