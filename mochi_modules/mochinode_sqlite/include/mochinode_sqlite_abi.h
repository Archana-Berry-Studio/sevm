#ifndef MB_MOCHINODE_SQLITE_ABI_H
#define MB_MOCHINODE_SQLITE_ABI_H
#ifdef __cplusplus
extern "C" {
#endif

/* ABI for mochinode_sqlite — engine plugin. Entry point: mbjs_init_module().
   Runtime exports (verified via mbjs loadNative): */
/*   - bindDouble */
/*   - bindInt */
/*   - bindNull */
/*   - bindParameterCount */
/*   - bindText */
/*   - clearBindings */
/*   - close */
/*   - columnBlobHex */
/*   - columnCount */
/*   - columnDouble */
/*   - columnInt */
/*   - columnName */
/*   - columnText */
/*   - columnType */
/*   - errmsg */
/*   - exec */
/*   - finalize */
/*   - open */
/*   - prepare */
/*   - reset */
/*   - step */
/*   - version */
#ifdef __cplusplus
}
#endif
#endif
