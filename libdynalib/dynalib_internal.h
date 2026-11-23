/*
 * dynalib_internal.h — private API between libdynalib translation units.
 * Not part of the installed interface; see dynalib.h for the public ABI.
 */
#ifndef MOCHI_DYNALIB_INTERNAL_H
#define MOCHI_DYNALIB_INTERNAL_H

#include "dynalib.h"

#ifdef __cplusplus
extern "C" {
#endif

/* platform slot order for the current architecture (dynalib_trampoline.c) */
int dynalib_platform_slots(DynalibAbiKind kind, DynalibReg *slots, size_t cap);

/*
 * Emit an executable stub that forwards its first `arg_count` register
 * arguments to `target`. `*pend` receives the page base, which is what
 * dynalib_trampoline_free() needs in order to release the mapping.
 */
void *dynalib_trampoline_emit(const void *target, const DynalibAbiSpec *abi,
                              void **pend);

/*
 * Emit a stub that places `slot_id` into the first argument register and
 * jumps to `dispatcher`: the reverse-callback trampoline. Same W^X page
 * discipline as the call stub above.
 */
void *dynalib_trampoline_cb(uintptr_t slot_id, const void *dispatcher,
                            const DynalibAbiSpec *abi, void **pend);

#ifdef __cplusplus
}
#endif

#endif /* MOCHI_DYNALIB_INTERNAL_H */
