/*
 * dynalib_trampoline.c — JIT stubs for libdynalib.
 *
 * Emits the tiny executable pages the ABI layer needs:
 *
 *   - a call stub that forwards its first N register arguments to a target
 *     address (the `dynalib(file, offset, sym, abi)` path), and
 *   - a callback stub that stamps a slot id and jumps to the dispatcher the
 *     engine installed (the C-calls-back-into-JS path).
 *
 * Page discipline is W^X: the mapping is created writable, the bytes are
 * written, then it is flipped executable. The caller never sees a page that
 * is both, and the page lives outside the bump arena so the Sunat collector
 * cannot move it from under a live C loop.
 *
 * Status, stated plainly: the x86_64 emitters are exercised by the test
 * harness on this host (both MS64 and SysV). The AArch64 emitter is built
 * from the same table but has not been executed on an AArch64 host here, so
 * treat it as compile-clean, not run-verified.
 */
#include "dynalib.h"
#include "dynalib_internal.h"

#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <sys/types.h>
#endif

/* ================================================================== */
/* code buffer                                                       */
/* ================================================================== */

#define DYNALIB_MAX_STUB 512

typedef struct {
    unsigned char b[DYNALIB_MAX_STUB];
    size_t        n;
} CodeBuf;

static void cb_byte(CodeBuf *c, unsigned v)
{
    if (c->n < sizeof c->b) c->b[c->n++] = (unsigned char)v;
}

static void cb_u16(CodeBuf *c, unsigned v)
{
    cb_byte(c, v & 0xff);
    cb_byte(c, (v >> 8) & 0xff);
}

static void cb_u32(CodeBuf *c, unsigned v)
{
    cb_byte(c, v & 0xff);
    cb_byte(c, (v >> 8) & 0xff);
    cb_byte(c, (v >> 16) & 0xff);
    cb_byte(c, (v >> 24) & 0xff);
}

static void cb_u64(CodeBuf *c, uintptr_t v)
{
    cb_u32(c, (unsigned)v);
    cb_u32(c, (unsigned)(v >> 32));
}

/* 64-bit register encodings (x86_64) */
enum {
    X_RAX = 0, X_RCX = 1, X_RDX = 2, X_RBX = 3,
    X_RSP = 4, X_RBP = 5, X_RSI = 6, X_RDI = 7,
    X_R8 = 8, X_R9 = 9, X_R10 = 10, X_R11 = 11
};

/* physical slot order per convention (index = canonical slot ordinal) */
static int slot_reg(DynalibAbiKind kind, size_t slot)
{
    switch (kind) {
    case DYNALIB_ABI_MS64:
        switch (slot) {
        case 0: return X_RCX;
        case 1: return X_RDX;
        case 2: return X_R8;
        case 3: return X_R9;
        }
        return -1;
    case DYNALIB_ABI_SYSV:
        switch (slot) {
        case 0: return X_RDI;
        case 1: return X_RSI;
        case 2: return X_RDX;
        case 3: return X_RCX;
        case 4: return X_R8;
        case 5: return X_R9;
        }
        return -1;
    default:
        return -1;
    }
}

static DynalibAbiKind platform_default(void)
{
#if defined(_WIN32)
    return DYNALIB_ABI_MS64;
#else
    return DYNALIB_ABI_SYSV;
#endif
}

/* mov [base+disp32], r64 */
static void enc_mem_store_base(CodeBuf *c, int src, int base, int32_t disp)
{
    unsigned rex = 0x48;
    if (src >= 8) rex |= 0x04;                 /* REX.R — reg field */
    if (base >= 8) rex |= 0x01;                /* REX.B — r/m field */
    cb_byte(c, rex);
    cb_byte(c, 0x89);                          /* mov r/m64, r64 */
    /* mod=10 (disp32); r/m=101 -> rbp */
    if (base == X_RSP) {
        /* [rsp+disp] needs a SIB byte: index=none, base=rsp */
        cb_byte(c, 0x80 | ((src & 7) << 3) | 0x04);
        cb_byte(c, 0x24);                      /* scale=1, index=none, base=rsp */
    } else {
        cb_byte(c, 0x80 | ((src & 7) << 3) | (base & 7));
    }
    cb_u32(c, (unsigned)disp);
}

/* mov [rbp+disp32], r64 */
static void enc_mem_store(CodeBuf *c, int src, int32_t disp)
{
    enc_mem_store_base(c, src, X_RBP, disp);
}

/* mov r64, [base+disp32] */
static void enc_mem_load_base(CodeBuf *c, int dst, int base, int32_t disp)
{
    unsigned rex = 0x48;
    if (dst >= 8) rex |= 0x04;                 /* REX.R */
    if (base >= 8) rex |= 0x01;                /* REX.B */
    cb_byte(c, rex);
    cb_byte(c, 0x8B);                          /* mov r64, r/m64 */
    if (base == X_RSP) {
        cb_byte(c, 0x80 | ((dst & 7) << 3) | 0x04);
        cb_byte(c, 0x24);                      /* scale=1, index=none, base=rsp */
    } else {
        cb_byte(c, 0x80 | ((dst & 7) << 3) | (base & 7));
    }
    cb_u32(c, (unsigned)disp);
}

/* mov r64, r64 — general register move (REX.W 89 /r) */
static void enc_mov_rr(CodeBuf *c, int src, int dst)
{
    unsigned rex = 0x48;
    if (src >= 8) rex |= 0x04;                 /* REX.R */
    if (dst >= 8) rex |= 0x01;                 /* REX.B */
    cb_byte(c, rex);
    cb_byte(c, 0x89);
    cb_byte(c, 0xC0 | ((src & 7) << 3) | (dst & 7));
}

/* movq xmm, [base+disp32] — SSE2: F3 REX.W 0F 7E /r.
 *
 * NOTE: 0F 6E with the F3 prefix is MOVQ xmm,xmm (register source only);
 * loading from MEMORY is 0F 7E. Using 6E with a memory operand raises
 * #UD (0xC000001D) — verified on this host, not guessed.
 *
 * Loads the low 64 bits of the slot into the xmm register; for double
 * arguments that is the whole value, for float arguments the low 32 bits
 * are the float itself (the upper half is irrelevant).                 */
static void enc_movq_xmm_mem(CodeBuf *c, int xmm, int base, int32_t disp)
{
    unsigned rex = 0x48;
    if (xmm >= 8) rex |= 0x04;                 /* REX.R */
    if (base >= 8) rex |= 0x01;                /* REX.B */
    cb_byte(c, 0xF3);
    cb_byte(c, rex);
    cb_byte(c, 0x0F);
    cb_byte(c, 0x7E);
    cb_byte(c, 0x80 | ((xmm & 7) << 3) | (base & 7));
    cb_u32(c, (unsigned)disp);
}

/* cvtss2sd xmm0, xmm0 — widen a single-precision return so the C caller
 * reading xmm0 as a double sees the right bits (F3 0F 5A C0).            */
static void enc_cvtss2sd_xmm0(CodeBuf *c)
{
    cb_byte(c, 0xF3);
    cb_byte(c, 0x0F);
    cb_byte(c, 0x5A);
    cb_byte(c, 0xC0);
}

/* mov r64, [rbp+disp32] */
static void enc_mem_load(CodeBuf *c, int dst, int32_t disp)
{
    enc_mem_load_base(c, dst, X_RBP, disp);
}

/* mov r64, imm64 (reg 0..7 without an extra REX.B) */
static void enc_reg_imm64(CodeBuf *c, int reg, uintptr_t v)
{
    cb_byte(c, 0x48 | (reg >= 8 ? 0x01 : 0x00));
    cb_byte(c, 0xB8 + (reg & 7));
    cb_u64(c, v);
}

/* mov r64, imm32 (C7 /0) — the target register lives in the ModRM r/m
 * field, so the extension bit is REX.B; REX.R would change the opcode
 * extension from /0 to /1 and break the instruction entirely.            */
static void enc_reg_imm32(CodeBuf *c, int reg, unsigned v)
{
    unsigned rex = 0x48;                    /* REX.W: 64-bit destination   */
    if (reg >= 8) rex |= 0x01;             /* REX.B — r/m field           */
    cb_byte(c, rex);
    cb_byte(c, 0xC7);
    cb_byte(c, 0xC0 | (reg & 7));
    cb_u32(c, v);
}

/* lea r64, [rbp+disp8] */
static void enc_lea_disp8(CodeBuf *c, int reg, int8_t disp)
{
    unsigned rex = 0x48;
    if (reg >= 8) rex |= 0x04;                 /* REX.R */
    cb_byte(c, rex);
    cb_byte(c, 0x8D);
    /* the register goes in the ModRM reg field, the base in r/m:
     * lea r64, [rbp+disp8] = mod=01, reg=reg, r/m=rbp                     */
    cb_byte(c, 0x40 | ((reg & 7) << 3) | 0x05);
    cb_byte(c, disp & 0xff);
}

/* ================================================================== */
/* W^X page management                                                 */
/* ================================================================== */

#define DYNALIB_STUB_SLOTS 16

typedef struct {
    void   *base;
    size_t  size;
} DynalibStubRec;

static DynalibStubRec g_stubs[DYNALIB_STUB_SLOTS];
static size_t         g_stub_top = 0;

static void *stub_alloc(size_t size, void **pend)
{
    void *p;
#if defined(_WIN32)
    p = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    p = mmap(NULL, size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) p = NULL;
#endif
    if (!p)
        return NULL;
    if (g_stub_top < DYNALIB_STUB_SLOTS) {
        g_stubs[g_stub_top].base = p;
        g_stubs[g_stub_top].size = size;
        g_stub_top++;
    }
    if (pend) *pend = p;
    return p;
}

static void *stub_exec(void *p)
{
#if defined(_WIN32)
    DWORD old;
    if (!VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old))
        return NULL;
#else
    if (mprotect(p, 4096, PROT_READ | PROT_EXEC) != 0)
        return NULL;
#endif
    return p;
}

void dynalib_trampoline_free(void *stub, void *pend)
{
    size_t i;
    (void)pend;
    for (i = 0; i < g_stub_top; i++) {
        if (g_stubs[i].base == stub) {
#if defined(_WIN32)
            VirtualFree(stub, 0, MEM_RELEASE);
#else
            munmap(stub, g_stubs[i].size);
#endif
            g_stubs[i] = g_stubs[--g_stub_top];
            return;
        }
    }
}

/* ================================================================== */
/* call stub: shuffle an argument buffer into the callee's ABI        */
/* ================================================================== */

/*
 * The emitted stub has ONE canonical native signature, chosen at emit
 * time by the *caller* of this function:
 *
 *   uint64_t stub(const uint64_t args[16]);   -- integer/pointer returns
 *   double   stub(const uint64_t args[16]);   -- f32/f64 returns
 *
 * The 16-slot buffer holds every argument as a raw 64-bit pattern;
 * f32/f64 values are bit-cast into their slot by the caller. Inside the
 * stub the buffer pointer (rcx on MS64 / rdi on SysV) is parked in r10
 * (volatile in both conventions, never an argument register), then each
 * slot is classified and routed:
 *
 *   MS64  : integer slots -> rcx,rdx,r8,r9 (first four integer args),
 *           SSE slots     -> xmm0..xmm3 (first four slots only, positional),
 *           slots 4..     -> outgoing stack area at [rsp+0x20+8k] behind
 *                            a 32-byte shadow space.
 *   SysV  : integer slots -> rdi,rsi,rdx,rcx,r8,r9 (class-counted, NOT
 *           positional), SSE slots -> xmm0..xmm7, overflow -> [rsp+8k].
 *
 * Integer and SSE ordinals are counted independently, which is what
 * makes mixed signatures (`int, double, int, double`) land correctly.
 * Return values pass through in rax / xmm0; an f32 return is widened
 * with cvtss2sd so a caller reading xmm0 as a double sees the right
 * bits.
 */
static void emit_call_stub(CodeBuf *c, const void *target, DynalibAbiKind kind,
                           const DynalibAbiSpec *abi, size_t n)
{
    static const int ms_int_regs[4] = { X_RCX, X_RDX, X_R8, X_R9 };
    static const int sv_int_regs[6] = { X_RDI, X_RSI, X_RDX, X_RCX,
                                        X_R8, X_R9 };
    uintptr_t frame;
    size_t k, kstack = 0;
    int int_idx = 0, sse_idx = 0;
    int32_t out_off;
    int custom = (abi && abi->kind == DYNALIB_ABI_CUSTOM_REG);
    int reg_for[16], xmm_for[16];
    int32_t stk_for[16];
    int stk_next = 0;

    for (k = 0; k < 16; k++) {
        reg_for[k] = -1;
        xmm_for[k] = -1;
        stk_for[k] = -1;
    }

    /* classification pass: decide register / xmm / stack per slot */
    for (k = 0; k < n; k++) {
        int t = abi ? abi->arg_types[k] : 0;
        int is_sse = (t == DYNALIB_T_F32 || t == DYNALIB_T_F64);

        if (custom) {
            /* custom register map: every slot rides a register */
            if (k < 8) {
                int mapped = slot_reg(kind, abi->arg_regs[k]);
                if (mapped >= 0)
                    reg_for[k] = mapped;
            }
            continue;
        }

        if (kind == DYNALIB_ABI_MS64) {
            /* MS64 is POSITIONAL: slot k (k<4) always uses slot k's
             * register — rcx/rdx/r8/r9 for integer, xmm0..xmm3 for
             * floating-point. An integer argument at slot 1 goes to rdx,
             * never "the next free integer register". Slots >= 4 travel
             * on the stack.                                            */
            if (k < 4) {
                if (is_sse)
                    xmm_for[k] = (int)k;
                else
                    reg_for[k] = ms_int_regs[k];
            } else {
                stk_for[k] = stk_next++;
            }
        } else {                              /* SysV: class-counted      */
            if (is_sse) {
                if (sse_idx < 8) xmm_for[k] = sse_idx;
                else stk_for[k] = stk_next++;
                sse_idx++;
            } else {
                if (int_idx < 6) reg_for[k] = sv_int_regs[int_idx];
                else stk_for[k] = stk_next++;
                int_idx++;
            }
        }
    }
    kstack = (size_t)stk_next;

    /* frame: callee shadow (MS64) + outgoing stack arguments */
    frame = 8 * kstack;
    if (kind == DYNALIB_ABI_MS64)
        frame += 32;                          /* callee shadow space       */
    if (frame % 16)
        frame += (16 - (frame % 16));
    out_off = (kind == DYNALIB_ABI_MS64) ? 0x20 : 0x00;

    cb_byte(c, 0x55);                          /* push rbp               */
    /* rbp must be set up, not just pushed: the epilogue rebuilds rsp
     * from it, so a stale caller rbp would steer the return address    */
    cb_byte(c, 0x48); cb_byte(c, 0x89); cb_byte(c, 0xE5);  /* mov rbp, rsp */
    {
        unsigned f32 = (unsigned)frame;
        cb_byte(c, 0x48); cb_byte(c, 0x81); cb_byte(c, 0xEC);
        cb_u32(c, f32);                       /* sub rsp, frame         */
    }

    /* park the argument-buffer pointer: MS64 arg0 arrives in rcx,
     * SysV arg0 in rdi; r10 is volatile scratch in both conventions   */
    enc_mov_rr(c, kind == DYNALIB_ABI_MS64 ? X_RCX : X_RDI, X_R10);

    /* route every slot */
    for (k = 0; k < n; k++) {
        int32_t disp = (int32_t)(8 * k);
        if (reg_for[k] >= 0)
            enc_mem_load_base(c, reg_for[k], X_R10, disp);
        else if (xmm_for[k] >= 0)
            enc_movq_xmm_mem(c, xmm_for[k], X_R10, disp);
        else if (stk_for[k] >= 0) {
            enc_mem_load_base(c, X_R11, X_R10, disp);
            enc_mem_store_base(c, X_R11, X_RSP,
                               out_off + (int32_t)(8 * stk_for[k]));
        }
    }

    enc_reg_imm64(c, X_RAX, (uintptr_t)target);  /* mov rax, target      */
    cb_byte(c, 0xFF); cb_byte(c, 0xD0);          /* call rax             */

    if (abi && abi->ret_type == DYNALIB_T_F32)
        enc_cvtss2sd_xmm0(c);               /* widen a float return      */

    /* after `push rbp; mov rbp, rsp`, rbp already points at the saved
     * frame-pointer slot, so rsp must return there exactly: one slot
     * further down and `ret` pops the saved rbp instead of the address   */
    enc_lea_disp8(c, X_RSP, 0);                   /* lea rsp,[rbp]        */
    cb_byte(c, 0x5D);                            /* pop rbp              */
    cb_byte(c, 0xC3);                            /* ret                  */
}

void *dynalib_trampoline_emit(const void *target, const DynalibAbiSpec *abi,
                              void **pend)
{
    CodeBuf c;
    DynalibAbiKind kind;
    size_t n = 0;
    void *p;
    if (!target)
        return NULL;
    kind = abi ? abi->kind : platform_default();
    if (kind == DYNALIB_ABI_DEFAULT)
        kind = platform_default();
    if (abi)
        n = abi->arg_count;
    if (n > 16)
        n = 16;
    memset(&c, 0, sizeof c);
    emit_call_stub(&c, target, kind, abi, n);
    p = stub_alloc((size_t)4096, pend);
    if (!p)
        return NULL;
    memcpy(p, c.b, c.n);
    return stub_exec(p);
}

/* ================================================================== */
/* callback stub: stamp a slot id, hand the args to the dispatcher    */
/* ================================================================== */

static void emit_cb_stub(CodeBuf *c, uintptr_t slot_id, const void *disp,
                         DynalibAbiKind kind, size_t n)
{
    uintptr_t frame;
    size_t k;
    int s0, s1, s2;
    int arg_regn[8];

    frame = 8 * n;
    if (kind == DYNALIB_ABI_MS64)
        frame += 32;
    if (frame % 16)
        frame += (16 - (frame % 16));

    s0 = slot_reg(kind, 0);
    s1 = slot_reg(kind, 1);
    s2 = slot_reg(kind, 2);
    if (s0 < 0 || s1 < 0 || s2 < 0)
        return;
    for (k = 0; k < n && k < 8; k++)
        arg_regn[k] = slot_reg(kind, k);

    cb_byte(c, 0x55);                          /* push rbp                 */
    cb_byte(c, 0x48); cb_byte(c, 0x89); cb_byte(c, 0xE5); /* mov rbp, rsp    */
    {
        unsigned f32 = (unsigned)frame;
        cb_byte(c, 0x48); cb_byte(c, 0x81); cb_byte(c, 0xEC);
        cb_u32(c, f32);                       /* sub rsp, frame           */
    }

    /* park the native args at [rbp-8n .. rbp) so the dispatcher can read
     * them as a flat array in canonical slot order: slot 0 goes in the
     * LOWEST slot, because the array base is &base[0]                    */
    for (k = 0; k < n && k < 8; k++) {
        if (arg_regn[k] < 0) break;
        enc_mem_store(c, arg_regn[k], (int32_t)(-8 * (int)(n - k)));
    }

    /* then lay them out contiguously at [rbp-8n .. rbp-8) — already so by
     * construction when n is contiguous, so the array base is the disp
     * of the last saved slot                                           */
    enc_reg_imm64(c, s0, slot_id);             /* slot id                 */
    if (n > 0)
        enc_lea_disp8(c, s1, (int8_t)-8 * (int)n);  /* args = &slot1      */
    else
        enc_reg_imm32(c, s1, 0);
    enc_reg_imm32(c, s2, (unsigned)n);        /* argc                    */

    enc_reg_imm64(c, X_RAX, (uintptr_t)disp);
    cb_byte(c, 0xFF); cb_byte(c, 0xD0);       /* call rax                */

    enc_lea_disp8(c, X_RSP, 0);
    cb_byte(c, 0x5D);                        /* pop rbp                 */
    cb_byte(c, 0xC3);                        /* ret                     */
}

void *dynalib_trampoline_cb(uintptr_t slot_id, const void *dispatcher,
                            const DynalibAbiSpec *abi, void **pend)
{
    CodeBuf c;
    DynalibAbiKind kind;
    void *p;
    if (!dispatcher)
        return NULL;
    kind = abi ? abi->kind : platform_default();
    if (kind == DYNALIB_ABI_DEFAULT)
        kind = platform_default();
    memset(&c, 0, sizeof c);
    emit_cb_stub(&c, slot_id, dispatcher, kind, abi ? abi->arg_count : 0);
    p = stub_alloc((size_t)4096, pend);
    if (!p)
        return NULL;
    memcpy(p, c.b, c.n);
    return stub_exec(p);
}
