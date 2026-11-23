/*
 * dynalib.h — libdynalib, the cross-format dynamic loader for MochiBerry.
 *
 * libdynalib is a standalone C99 subsystem that lives *inside* the MochiBerry
 * core (it is what `dynalib()` in JS binds to) and is shared with LunaBerry
 * (`luna.compile` / `luna.asm`). It unifies the dynamic linker across OSes
 * (dlopen / LoadLibrary / CherryReVA), export-table extraction for
 * .exe / .so / .lbrct, anti-ASLR base-address calculation, and custom-ABI
 * trampolines behind one API.
 *
 * Design goals:
 *   - C99, freestanding-safe where possible (no libc dependency for the
 *     pure parsers, so UBerryNix / bare-metal builds work).
 *   - No allocation in the hot path: open/resolve reuse the table-backed
 *     arena; pinning never moves memory.
 *   - The engine's GC (Sunat) is never touched directly: pinning and
 *     reverse-callbacks go through injected hooks so the subsystem can be
 *     unit-tested with a stub engine.
 *
 * Two return modes, mirroring the JS contract `dynalib(file, off, sym, ...)`:
 *   - direct : offset or symbol given  -> a DynalibSymbol (callable handle)
 *   - module : both null               -> a DynalibModule namespace handle
 *     whose exports() enumerates names + RVAs (stripped / proprietary
 *     binaries included, by parsing the container rather than the linker).
 */
#ifndef MOCHI_DYNALIB_H
#define MOCHI_DYNALIB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* container formats                                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    DYNALIB_FMT_AUTO = 0,  /* sniff the magic bytes, then behave accordingly */
    DYNALIB_FMT_ELF_SO,    /* Linux / Android .so and PIE executables      */
    DYNALIB_FMT_PE_DLL,   /* Windows .dll and .exe export tables           */
    DYNALIB_FMT_LBRCT,    /* UBerryNix CherryReVA relocatable             */
    DYNALIB_FMT_SYSCALL  /* direct kernel syscall interface (no file)      */
} DynalibFormat;

/* ------------------------------------------------------------------ */
/* calling conventions                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    DYNALIB_ABI_DEFAULT = 0, /* the OS default for this platform        */
    DYNALIB_ABI_SYSV,        /* rdi, rsi, rdx, rcx, r8, r9 -> rax        */
    DYNALIB_ABI_MS64,        /* rcx, rdx, r8, r9 -> rax (+32B shadow)    */
    DYNALIB_ABI_STDCALL,     /* x86 callee-cleanup (i386)                 */
    DYNALIB_ABI_CUSTOM_REG   /* explicit register map from an arg string */
} DynalibAbiKind;

/* Argument/result kinds the trampoline knows how to marshal. */
typedef enum {
    DYNALIB_T_VOID = 0,
    DYNALIB_T_I8, DYNALIB_T_U8,
    DYNALIB_T_I16, DYNALIB_T_U16,
    DYNALIB_T_I32, DYNALIB_T_U32,
    DYNALIB_T_I64, DYNALIB_T_U64,
    DYNALIB_T_F32, DYNALIB_T_F64,
    DYNALIB_T_PTR,          /* register-sized pointer, sign-agnostic  */
    DYNALIB_T_STRUCT_VAL    /* pass a struct by value in regs/memory   */
} DynalibType;

typedef struct {
    DynalibAbiKind kind;
    uint8_t  arg_regs[8];    /* physical register index, see DynalibReg */
    uint8_t  ret_reg;
    uint8_t  arg_count;
    uint8_t  arg_types[16]; /* DynalibType, at most 16 args supported  */
    uint8_t  ret_type;       /* DynalibType                             */
} DynalibAbiSpec;

/* Register index namespace, unified across x86_64 / AArch64 / RISC-V.
 * A trampoline only needs the *ordinal* position of a register in the
 * platform's argument slot order, so one table covers every target.   */
typedef enum {
    DYNALIB_REG_R0 = 0,
    DYNALIB_REG_R1 = 1,
    DYNALIB_REG_R2 = 2,
    DYNALIB_REG_R3 = 3,
    DYNALIB_REG_R4 = 4,
    DYNALIB_REG_R5 = 5,
    DYNALIB_REG_R6 = 6,
    DYNALIB_REG_R7 = 7,
    DYNALIB_REG_ARGMAX = 8
} DynalibReg;

/* ------------------------------------------------------------------ */
/* exported symbol entries (name + RVA)                                */
/* ------------------------------------------------------------------ */

#define DYNALIB_EXPORT_NAME_MAX 128

typedef struct {
    char     name[DYNALIB_EXPORT_NAME_MAX];
    uint32_t rva;        /* relative to the mapped base */
    uint16_t ordinal;     /* 0 when the container has no ordinals  */
    bool     is_ordinal_only; /* forwarded / unnamed export        */
} DynalibExportEntry;

/* ------------------------------------------------------------------ */
/* module + symbol handles                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    void         *os_handle;    /* HMODULE / dlopen handle / CherryReVA */
    uintptr_t     base_addr;    /* actual mapped base after ASLR        */
    size_t        base_size;    /* preferred load size of the image      */
    DynalibFormat format;
    uint32_t      ref_count;
    bool          is_pinned;
    char          path[512];    /* as given, for diagnostics          */
} DynalibModule;

typedef struct {
    DynalibModule  *module;
    void           *fn_ptr;     /* final address: dlsym/GetProcAddr, or
                                   base_addr + offset when stripped   */
    uintptr_t       rva_offset;
    DynalibAbiSpec  abi;
    void           *jit_stub;   /* executable trampoline, or NULL       */
} DynalibSymbol;

/* ------------------------------------------------------------------ */
/* core C99 API                                                        */
/* ------------------------------------------------------------------ */

/*
 * Load `path` as a container of `fmt_hint` (pass DYNALIB_FMT_AUTO to sniff).
 *
 * Short names are resolved against the module search path:
 *   "ip_route"     -> ip_route.so / ip_route.dll / ip_route.lbrct found in
 *                      <cwd>/mochimodule/ then the platform default path.
 *   "project/lib/ip_route.so"  -> the file exactly as written.
 *   "kernel32"      -> a system module (LoadLibrary / dlopen).
 *
 * Returns NULL on failure and sets *perr to a diagnostic string.
 */
DynalibModule *dynalib_open(const char *path, DynalibFormat fmt_hint,
                            const char **perr);

/*
 * Resolve a callable inside `mod`.
 *
 *   symbol != NULL : try the named export first; if the image is stripped
 *                    or the symbol is absent, fall back to base + offset.
 *   symbol == NULL : jump straight to base + offset (pure RE usage).
 *   offset == 0    : symbol-only resolution (no fallback).
 *
 * `abi` may be NULL for the platform default; pass a spec when the target
 * does not follow the OS convention (proprietary SDKs, raw syscalls).
 */
DynalibSymbol *dynalib_resolve(DynalibModule *mod, uintptr_t offset,
                               const char *symbol, const DynalibAbiSpec *abi,
                               const char **perr);

/*
 * Every export the container advertises: names and RVAs, including entries
 * the host linker would refuse to hand out (ordinals, unnamed slots).
 * Fills up to `cap` entries and returns the total found.
 */
size_t dynalib_exports(const DynalibModule *mod, DynalibExportEntry *out,
                       size_t cap);

void dynalib_close(DynalibModule *mod);
void dynalib_module_unref(DynalibModule *mod);
void dynalib_symbol_free(DynalibSymbol *sym);

/* ------------------------------------------------------------------ */
/* pure container parsers — no libc, unit-testable on any host          */
/* ------------------------------------------------------------------ */

/*
 * Parse a whole-image buffer: sniff the magic, walk the export table, and
 * fill `out` with up to `cap` entries. Returns the total export count
 * (which may exceed cap). On failure returns -1 and sets *perr.
 *
 * Works on a read-only mapping, a file read into memory, or a synthetic
 * image built in a test — that is what makes the ELF path testable on
 * Windows and the PE path testable on Linux.
 */
ssize_t dynalib_parse_image(const void *image, size_t len,
                            DynalibExportEntry *out, size_t cap,
                            uintptr_t *pbase_rva, const char **perr);

const char *dynalib_format_name(DynalibFormat f);
DynalibFormat dynalib_detect_format(const void *image, size_t len);

/* ------------------------------------------------------------------ */
/* calling-convention string grammar                                   */
/* ------------------------------------------------------------------ */

/*
 * Accepts, in `spec`:
 *   abi words : "sysv", "ms_abi", "stdcall", "syscall", "default"
 *   register map: "reg:rcx,rdx,r8,r9->rax"  (or rdi.. for SysV naming)
 *   type contract: "i32(str)", "u64(ptr, u32, u32)", "void(f32, f32)",
 *                  "i32()", "ptr()"
 * Returns 0 on success.
 */
int dynalib_abi_parse(const char *spec, DynalibAbiSpec *out);

/* ------------------------------------------------------------------ */
/* JIT trampolines                                                     */
/* ------------------------------------------------------------------ */

/*
 * Emit a stub that reads its arguments from a 16-slot uint64_t buffer and
 * forwards them to `target` under `abi`:
 *
 *   uint64_t stub(const uint64_t args[16]);   -- integer/pointer/void return
 *   double   stub(const uint64_t args[16]);   -- f32/f64 return
 *
 * Float arguments must be bit-cast into their slot by the caller
 * (memcpy the double/float into the slot); integer arguments are the
 * raw values. The stub classifies every slot per the convention
 * (MS64: rcx,rdx,r8,r9 / xmm0..xmm3, positional overflow behind a
 * 32-byte shadow; SysV: rdi,rsi,rdx,rcx,r8,r9 / xmm0..xmm7, class-
 * counted overflow) and returns the target's result in rax or xmm0;
 * an f32 return is widened so the double-reading caller sees the
 * right bits. Struct-by-value contracts are rejected at parse time.
 *
 * The page is written first, then flipped executable (W^X): the caller
 * never sees a writable+executable mapping, and GC movement cannot
 * invalidate it because the buffer is outside the bump arena.
 *
 * Returns the callable address, or NULL. `pend` reports the page end
 * so the caller can release it with dynalib_trampoline_free().
 */
void *dynalib_trampoline_emit(const void *target, const DynalibAbiSpec *abi,
                              void **pend);

void  dynalib_trampoline_free(void *stub, void *stub_end);

/* ------------------------------------------------------------------ */
/* Sunat GC safety: pinning + reverse callbacks                        */
/* ------------------------------------------------------------------ */

/*
 * Pin a JS-owned buffer so its address stays put while C walks it. The
 * subsystem records the slot in a fixed-size table and forwards the range
 * to the GC hook installed by the engine. Returns the pinned pointer
 * (usually identical to buf) or NULL when the pin table is full.
 */
void *dynalib_pin_buffer(void *js_ctx, void *buf_ptr, size_t len);
void  dynalib_unpin_buffer(void *js_ctx, void *pinned_ptr);

typedef void (*DynalibGCHook)(void *js_ctx, void *lo, void *hi, bool pinned);
void dynalib_set_gc_hook(DynalibGCHook hook, void *arg);

/*
 * Marshal a JS function into a C function pointer of signature `abi`. The
 * engine installs a dispatcher that knows how to enter the VM (thread
 * check, stack save, JSValue conversion); dynalib only owns the thunk that
 * hands the registers to that dispatcher.
 */
typedef void (*DynalibCallbackDispatch)(uintptr_t slot_id, void *const *args,
                                        int argc);
void dynalib_set_callback_dispatch(DynalibCallbackDispatch fn);
void *dynalib_create_callback(void *js_ctx, const DynalibAbiSpec *abi,
                             void *js_fn);
void  dynalib_destroy_callback(void *slot);

/* ------------------------------------------------------------------ */
/* misc                                                                */
/* ------------------------------------------------------------------ */

uintptr_t dynalib_module_base(const DynalibModule *mod);
size_t    dynalib_pin_capacity(void);

#ifdef __cplusplus
}
#endif

#endif /* MOCHI_DYNALIB_H */
