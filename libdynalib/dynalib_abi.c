/*
 * dynalib_abi.c — calling-convention grammar, GC pinning and reverse
 * callbacks. Split from dynalib.c so the JIT emitter, the grammar parser and
 * the GC table each fail independently in the test harness.
 */
#include "dynalib.h"
#include "dynalib_internal.h"

#include <string.h>
#include <stdlib.h>

/* ================================================================== */
/* canonical slot order per calling convention                         */
/* ================================================================== */

/*
 * These are the slot tables the parser fills. They double as documentation
 * of the contract: everything above the reg set is stack-marshalled by the
 * caller, which is why dynalib_abi_parse() caps the register side at 6.
 */
static const DynalibReg slot_ms64[]   = { DYNALIB_REG_R0, DYNALIB_REG_R1,
                                         DYNALIB_REG_R2, DYNALIB_REG_R3 };
static const DynalibReg slot_sysv[]   = { DYNALIB_REG_R0, DYNALIB_REG_R1,
                                         DYNALIB_REG_R2, DYNALIB_REG_R3,
                                         DYNALIB_REG_R4, DYNALIB_REG_R5 };

int dynalib_platform_slots(DynalibAbiKind kind, DynalibReg *slots, size_t cap)
{
    size_t n, i;
    const DynalibReg *t;
    size_t tn;
    switch (kind) {
    case DYNALIB_ABI_MS64:
        t = slot_ms64;
        tn = sizeof slot_ms64 / sizeof slot_ms64[0];
        break;
    case DYNALIB_ABI_SYSV:
        t = slot_sysv;
        tn = sizeof slot_sysv / sizeof slot_sysv[0];
        break;
    default:
        t = NULL;
        tn = 0;
        break;
    }
    if (!t || !slots)
        return -1;
    n = (tn < cap) ? tn : cap;
    for (i = 0; i < n; i++)
        slots[i] = t[i];
    return (int)n;
}

/*
 * Map a physical register name onto its canonical slot ordinal for the given
 * convention. MS and SysV disagree about which name sits in slot 1 (rcx vs
 * rdx), so the convention is part of the lookup, not an afterthought.
 */
static int reg_name_to_idx(const char *n, size_t nl, DynalibAbiKind kind)
{
    /* SysV order: rdi rsi rdx rcx r8 r9 */
    static const char *const sysv[6] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };
    /* MS order:    rcx rdx r8  r9 */
    static const char *const ms[4] = { "rcx", "rdx", "r8", "r9" };
    const char *const *tbl;
    size_t tn, i;

    if (kind == DYNALIB_ABI_MS64) { tbl = ms; tn = 4; }
    else { tbl = sysv; tn = 6; }
    for (i = 0; i < tn; i++)
        if (strlen(tbl[i]) == nl && !strncmp(tbl[i], n, nl))
            return (int)i;
    return -1;
}

static int type_name_to_code(const char *n, size_t nl, DynalibType *out)
{
    static const struct { const char *n; DynalibType t; } tbl[] = {
        { "void", DYNALIB_T_VOID }, { "i8", DYNALIB_T_I8 }, { "u8", DYNALIB_T_U8 },
        { "i16", DYNALIB_T_I16 }, { "u16", DYNALIB_T_U16 },
        { "i32", DYNALIB_T_I32 }, { "u32", DYNALIB_T_U32 },
        { "i64", DYNALIB_T_I64 }, { "u64", DYNALIB_T_U64 },
        { "f32", DYNALIB_T_F32 }, { "f64", DYNALIB_T_F64 },
        { "ptr", DYNALIB_T_PTR }, { "str", DYNALIB_T_PTR },
        { "char", DYNALIB_T_I8 }, { "int", DYNALIB_T_I32 },
        { "uint", DYNALIB_T_U32 }, { "size", DYNALIB_T_U64 }
    };
    size_t i;
    for (i = 0; i < sizeof tbl / sizeof tbl[0]; i++)
        if (strlen(tbl[i].n) == nl && !strncmp(tbl[i].n, n, nl)) {
            *out = tbl[i].t;
            return 0;
        }
    return -1;
}

int dynalib_abi_parse(const char *spec, DynalibAbiSpec *out)
{
    const char *p, *a;
    size_t nl, idx = 0;
    DynalibAbiKind kind = DYNALIB_ABI_DEFAULT;
    int have_reg_map = 0;

    memset(out, 0, sizeof *out);
    if (!spec || !*spec) {
        out->kind = DYNALIB_ABI_DEFAULT;
        return 0;
    }

    p = spec;
    while (*p == ' ' || *p == '\t') p++;

    /* convention word, possibly followed by a ":" register map */
    if (strncmp(p, "sysv", 4) == 0) { kind = DYNALIB_ABI_SYSV; p += 4; }
    else if (strncmp(p, "ms_abi", 6) == 0) { kind = DYNALIB_ABI_MS64; p += 6; }
    else if (strncmp(p, "ms64", 4) == 0) { kind = DYNALIB_ABI_MS64; p += 4; }
    else if (strncmp(p, "stdcall", 7) == 0) { kind = DYNALIB_ABI_STDCALL; p += 7; }
    else if (strncmp(p, "syscall", 7) == 0) { kind = DYNALIB_ABI_SYSV; p += 7; }
    else if (strncmp(p, "default", 7) == 0) { kind = DYNALIB_ABI_DEFAULT; p += 7; }

    while (*p == ' ' || *p == '\t') p++;

    if (*p == ':') {
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "reg:", 4) == 0) p += 4;
        /* parse the comma-separated register list up to "->" */
        a = p;
        while (*a && *a != '>') {
            const char *tok = a;
            int r;
            while (*a && *a != ',' && !((*a == '-') && a[1] == '>')) a++;
            nl = (size_t)(a - tok);
            r = reg_name_to_idx(tok, nl, kind);
            if (r < 0)
                return -1;
            if (idx >= 8)
                return -1;
            out->arg_regs[idx++] = (uint8_t)r;
                        if (*a == ',') {
                            a++;
                            continue;
                        }
                        break;                              /* hit "->" or the end */
                    }
        if (*a == '-' && a[1] == '>') {
            out->kind = DYNALIB_ABI_CUSTOM_REG;
            a += 2;
            while (*a == ' ') a++;
            nl = 0;
            while (a[nl] && a[nl] != ')' && a[nl] != ';') nl++;
            if (nl >= 3 && !strncmp(a, "rax", 3))
                out->ret_reg = 0;
            have_reg_map = 1;
        }
        out->arg_count = (uint8_t)idx;
        out->kind = have_reg_map ? out->kind : kind;
        return 0;
    }

    if (have_reg_map == 0 && kind != DYNALIB_ABI_DEFAULT && *p == 0) {
        out->kind = kind;
        return 0;
    }

    /* type contract: "i32(str)" / "void(f32, f32)" / "ptr()" */
    {
        DynalibType args[16];
        size_t nargs = 0, k = 0;
        const char *ret_name = p;
        DynalibType ret = DYNALIB_T_VOID;

        while (*p && *p != '(') p++;
        nl = (size_t)(p - ret_name);
        if (type_name_to_code(ret_name, nl, &ret) < 0)
            return -1;
        if (*p != '(')
            return -1;
        p++;
        while (*p && *p != ')') {
            const char *t = p;
            DynalibType at;
            while (*p == ' ' || *p == '\t') p++;
            t = p;
            while (*p && *p != ',' && *p != ')' && *p != ' ' && *p != '\t')
                p++;
            nl = (size_t)(p - t);
            if (nl == 0)
                break;
            if (type_name_to_code(t, nl, &at) < 0)
                return -1;
            if (nargs >= 16)
                return -1;
            args[nargs++] = at;
            if (*p == ',') p++;
        }
        out->kind = (kind != DYNALIB_ABI_DEFAULT) ? kind : DYNALIB_ABI_DEFAULT;
                out->ret_type = (uint8_t)ret;
                out->arg_count = (uint8_t)nargs;
                for (k = 0; k < nargs; k++) {
                    out->arg_types[k] = (uint8_t)args[k];
                    /* arg_regs has 8 slots: it only carries the *custom* register
                     * map, which cannot name more than 8 physical registers
                     * anyway. Writing past it would clobber ret_reg/arg_count. */
                    if (k < 8)
                        out->arg_regs[k] = (uint8_t)k;
                }
    }
    return 0;
}

/* ================================================================== */
/* GC pinning                                                          */
/* ================================================================== */

#define DYNALIB_PIN_SLOTS 64

typedef struct {
    void  *ptr;
    size_t len;
    bool   used;
} DynalibPinSlot;

static DynalibPinSlot g_pin[DYNALIB_PIN_SLOTS];
static size_t         g_pin_top = 0;
static DynalibGCHook  g_gc_hook = NULL;
static void          *g_gc_arg = NULL;

size_t dynalib_pin_capacity(void)
{
    return DYNALIB_PIN_SLOTS;
}

void *dynalib_pin_buffer(void *js_ctx, void *buf_ptr, size_t len)
{
    size_t i;
    if (!buf_ptr)
        return NULL;
    for (i = 0; i < g_pin_top; i++) {
        if (g_pin[i].used && g_pin[i].ptr == buf_ptr) {
            g_pin[i].len = len;
            return buf_ptr;                   /* already pinned: refresh span */
        }
    }
    if (g_pin_top >= DYNALIB_PIN_SLOTS)
        return NULL;
    i = g_pin_top++;
    g_pin[i].ptr = buf_ptr;
    g_pin[i].len = len;
    g_pin[i].used = true;
    if (g_gc_hook)
        g_gc_hook(js_ctx, buf_ptr, (char *)buf_ptr + len, true);
    return buf_ptr;
}

void dynalib_unpin_buffer(void *js_ctx, void *pinned_ptr)
{
    size_t i;
    for (i = 0; i < g_pin_top; i++) {
        if (g_pin[i].used && g_pin[i].ptr == pinned_ptr) {
            if (g_gc_hook)
                g_gc_hook(js_ctx, g_pin[i].ptr,
                          (char *)g_pin[i].ptr + g_pin[i].len, false);
            g_pin[i].used = false;
            g_pin[i].ptr = NULL;
            g_pin[i].len = 0;
            return;
        }
    }
}

void dynalib_set_gc_hook(DynalibGCHook hook, void *arg)
{
    g_gc_hook = hook;
    g_gc_arg = arg;
}

/* ================================================================== */
/* reverse callbacks (C calling back into JS)                          */
/* ================================================================== */

#define DYNALIB_CB_SLOTS 64

typedef struct {
    void          *js_ctx;
    void          *js_fn;
    DynalibAbiSpec abi;
    bool            used;
} DynalibCbSlot;

static DynalibCbSlot g_cb[DYNALIB_CB_SLOTS];
static size_t g_cb_top = 0;
static DynalibCallbackDispatch g_cb_dispatch = NULL;

void dynalib_set_callback_dispatch(DynalibCallbackDispatch fn)
{
    g_cb_dispatch = fn;
}

/*
 * The dispatcher the stub jumps into. The engine installs this; it owns the
 * thread check, the stack save and the JSValue marshalling. libdynalib only
 * owns the slot lookup and the thunk. `args` points at the native caller's
 * register arguments, saved by the emitted stub.
 */
void dynalib_cb_dispatch(uintptr_t slot_id, void *const *args, int argc)
{
    (void)slot_id;
    (void)args;
    (void)argc;
    if (g_cb_dispatch != NULL)
        g_cb_dispatch(slot_id, args, argc);
}

void *dynalib_create_callback(void *js_ctx, const DynalibAbiSpec *abi, void *js_fn)
{
    size_t i;
    for (i = 0; i < DYNALIB_CB_SLOTS; i++) {
        if (!g_cb[i].used) {
            g_cb[i].used = true;
            g_cb[i].js_ctx = js_ctx;
            g_cb[i].js_fn = js_fn;
            g_cb[i].abi = abi ? *abi : (DynalibAbiSpec){0};
            if (g_cb_top > i)
                g_cb_top = i + 1;
            return dynalib_trampoline_cb(i, (const void *)dynalib_cb_dispatch,
                                         abi, NULL);
        }
    }
    return NULL;
}

void dynalib_destroy_callback(void *slot)
{
    size_t i;
    for (i = 0; i < DYNALIB_CB_SLOTS; i++) {
        if (g_cb[i].used && g_cb[i].js_fn == slot) {
            g_cb[i].used = false;
            g_cb[i].js_fn = NULL;
            g_cb[i].js_ctx = NULL;
            return;
        }
    }
}
