/*
 * dynalib_test.c — exercise libdynalib for real.
 *
 * Everything here is executed on this host: the PE path runs against a real
 * Windows DLL (sqlite3.dll and a locally built dbtest.dll), the ELF path
 * against a synthetic image built in memory, and the trampoline/callback
 * paths by actually calling through the generated pages.
 */
#include "dynalib.h"
#include "dynalib_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
#  include <windows.h>
static LONG WINAPI on_fault(EXCEPTION_POINTERS *ep)
{
    BYTE *rip = (BYTE *)(uintptr_t)ep->ContextRecord->Rip;
    BYTE *base = (BYTE *)(uintptr_t)((uintptr_t)rip & ~(uintptr_t)0xFF);
    long i;
    printf("FAULT code=0x%08lx rip=%p rsp=%p rbp=%p rax=%p rcx=%p rdx=%p\n",
           (unsigned long)ep->ExceptionRecord->ExceptionCode, rip,
           (void *)ep->ContextRecord->Rsp, (void *)ep->ContextRecord->Rbp,
           (void *)ep->ContextRecord->Rax, (void *)ep->ContextRecord->Rcx,
           (void *)ep->ContextRecord->Rdx);
    for (i = 0; i < 64; i++) {
        printf("%s%02x", base + i == rip ? " -> " : "    ", base[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");
    fflush(stdout);
    ExitProcess(139);
}
#endif

#if defined(_WIN32)
#  include <windows.h>
#endif

#ifndef SQLITE_DLL
#  define SQLITE_DLL "C:/Python314/DLLs/sqlite3.dll"
#endif
#ifndef DBTEST_DLL
#  define DBTEST_DLL "tests/dbtest.dll"
#endif

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *name, int cond, const char *detail)
{
    if (cond) {
        g_pass++;
        printf("  PASS  %-52s %s\n", name, detail ? detail : "");
    } else {
        g_fail++;
        printf("  FAIL  %-52s %s\n", name, detail ? detail : "");
    }
}

static char *read_file(const char *path, size_t *plen)
{
    FILE *fp = fopen(path, "rb");
    long sz;
    char *buf;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return NULL; }
    buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { free(buf); fclose(fp); return NULL; }
    fclose(fp);
    *plen = (size_t)sz;
    return buf;
}

static int find_entry(DynalibExportEntry *e, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (e[i].name[0] && !strcmp(e[i].name, name))
            return (int)i;
    return -1;
}

/* ================================================================== */
/* T1-T4: PE parse, load, resolve, offset path, live exports           */
/* ================================================================== */

static void t_pe_and_sqlite(void)
{
    char *img;
    size_t len = 0;
    DynalibExportEntry e[512];
    ssize_t n;
    int idx;
    DynalibModule *m;
    DynalibSymbol *s;
    const char *err = NULL;
    uint32_t want_rva = 0, want_rva_num = 0;
    uintptr_t base_rva = 0;

    printf("T1  PE export parsing over a real DLL\n");
    img = read_file(SQLITE_DLL, &len);
    ok("sqlite3.dll readable", img != NULL, img ? "yes" : "NOT FOUND");
    if (!img) return;

    ok("magic sniff says PE",
       dynalib_detect_format(img, len) == DYNALIB_FMT_PE_DLL,
       dynalib_format_name(dynalib_detect_format(img, len)));

    n = dynalib_parse_image(img, len, e, 512, &base_rva, &err);
    ok("export table parsed", n > 100, err);
    printf("        exports found: %ld, first rva 0x%lx\n", (long)n,
           (unsigned long)base_rva);

    idx = find_entry(e, (size_t)n, "sqlite3_libversion");
    ok("sqlite3_libversion named", idx >= 0, "sqlite3_libversion");
    want_rva = (idx >= 0) ? e[idx].rva : 0;
    if (idx >= 0)
        printf("        rva 0x%x  ordinal %u\n", want_rva,
               (unsigned)e[idx].ordinal);
    idx = find_entry(e, (size_t)n, "sqlite3_libversion_number");
    ok("sqlite3_libversion_number named", idx >= 0, "sqlite3_libversion_number");
    want_rva_num = (idx >= 0) ? e[idx].rva : 0;

    {
        size_t i;
        size_t with_ord = 0;
        /* sqlite3.dll exports everything by name, so no slot is ordinal-only.
         * What matters is that the ordinal table was actually walked, i.e.
         * every entry carries the ordinal the loader would use.             */
        for (i = 0; i < (size_t)n; i++)
            if (e[i].ordinal > 0) with_ord++;
        ok("ordinal table walked for every export",
           n > 0 && with_ord == (size_t)n, "stripped-friendly");
    }
    free(img);

    printf("T2  dynalib_open + dynalib_resolve + call\n");
    m = dynalib_open(SQLITE_DLL, DYNALIB_FMT_AUTO, &err);
    ok("module opened", m != NULL, err);
    if (!m) return;
    ok("base_addr non-zero (anti-ASLR base)", m->base_addr != 0, "yes");
    ok("base_size from SizeOfImage", m->base_size > 0, "yes");

    s = dynalib_resolve(m, 0, "sqlite3_libversion", NULL, &err);
    ok("symbol resolved", s != NULL, err);
    if (s) {
        const char *v = ((const char *(*)(void))s->fn_ptr)();
        ok("call through resolved fn", v && v[0] != 0, v ? v : "");
        dynalib_symbol_free(s);
    }

    s = dynalib_resolve(m, 0, "sqlite3_libversion_number", NULL, NULL);
    ok("second symbol resolved", s != NULL, "yes");
    if (s) {
        int num = ((int (*)(void))s->fn_ptr)();
        ok("sqlite3_libversion_number() sane", num > 3000000, "yes");
        dynalib_symbol_free(s);
    }

    printf("T3  offset-only resolution (the stripped path)\n");
    {
        int via_sym = 0, via_off = -1;
        DynalibSymbol *s1, *s2;
        s1 = dynalib_resolve(m, 0, "sqlite3_libversion_number", NULL, NULL);
        if (s1) { via_sym = ((int (*)(void))s1->fn_ptr)(); dynalib_symbol_free(s1); }
        s2 = dynalib_resolve(m, want_rva_num, NULL, NULL, NULL);
        ok("offset resolved to base+rva", s2 != NULL, "yes");
        if (s2) {
            via_off = ((int (*)(void))s2->fn_ptr)();
            dynalib_symbol_free(s2);
        }
        ok("offset path == symbol path", via_sym > 0 && via_sym == via_off, "yes");
    }

    printf("T4  dynalib_exports() over the live mapping\n");
    {
        DynalibExportEntry live[32];
        size_t nx = dynalib_exports(m, live, 32);
        ok("exports enumerated from mapping", nx > 0, "yes");
        if (nx > 0)
            printf("        first live export: %s @ rva 0x%x\n",
                   live[0].name[0] ? live[0].name : "(ordinal-only)", live[0].rva);
    }
    dynalib_close(m);
}

/* ------------------------------------------------------------------ */
/* buffer-call helpers: the emitted stub reads its arguments from a
 * flat 16-slot uint64_t array (floats are bit-cast into their slot)  */

typedef uint64_t (*dl_stub_i)(const uint64_t *args);
typedef double   (*dl_stub_f)(const uint64_t *args);

static void buf_i(uint64_t *b, int i, int64_t v) { b[i] = (uint64_t)v; }
static void buf_d(uint64_t *b, int i, double v)  { memcpy(b + i, &v, 8); }
static void buf_f(uint64_t *b, int i, float v)   { memcpy(b + i, &v, 4); }

/* ================================================================== */
/* T5-T7: call stubs and the callback round-trip                       */
/* ================================================================== */

static uintptr_t g_cb_slot;
static int      g_cb_argc;
static int      g_cb_a, g_cb_b;

static void dispatcher(uintptr_t slot, void *const *args, int argc)
{
    g_cb_slot = slot;
    g_cb_argc = argc;
    g_cb_a = (argc > 0) ? (int)(uintptr_t)args[0] : -1;
    g_cb_b = (argc > 1) ? (int)(uintptr_t)args[1] : -1;
}

static void t_trampolines(const char *dll_name)
{
    DynalibModule *m;
    DynalibSymbol *s;
    DynalibAbiSpec ms, none;
    const char *err = NULL;
    void *stub;

    printf("T5  call stub, MS ABI, two int arguments\n");
    m = dynalib_open(dll_name, DYNALIB_FMT_AUTO, &err);
    ok("dbtest.dll loaded", m != NULL, err);
    if (!m) return;

    s = dynalib_resolve(m, 0, "dbtest_add", NULL, &err);
    ok("dbtest_add resolved", s != NULL, err);
    if (s) {
        int r = ((int (*)(int, int))s->fn_ptr)(3, 4);
        ok("direct call dbtest_add(3,4)", r == 7, "yes");
        dynalib_symbol_free(s);
    }

    dynalib_abi_parse("ms_abi i32(i32, i32)", &ms);
    ok("abi parse ms_abi", ms.kind == DYNALIB_ABI_MS64, "kind=ms64");
    ok("ms_abi contract keeps its type list", ms.arg_count == 2, "args=2");

    s = dynalib_resolve(m, 0, "dbtest_add", NULL, NULL);
    if (s) {
        stub = dynalib_trampoline_emit(s->fn_ptr, &ms, NULL);
        ok("call stub emitted on a W^X page", stub != NULL, "yes");
        if (stub) {
            uint64_t b[16] = {0};
            int r;
            buf_i(b, 0, 3); buf_i(b, 1, 4);
            r = (int)((dl_stub_i)stub)(b);
            ok("stub(3,4) == 7",   r == 7,  "yes");
            buf_i(b, 0, 41); buf_i(b, 1, 1);
            r = (int)((dl_stub_i)stub)(b);
            ok("stub(41,1) == 42", r == 42, "yes");
            buf_i(b, 0, -5); buf_i(b, 1, 5);
            r = (int)((dl_stub_i)stub)(b);
            ok("stub(-5,5) == 0",  r == 0,  "yes");
            dynalib_trampoline_free(stub, NULL);
        }
        dynalib_symbol_free(s);
    }

    printf("T6  zero-argument stub returning a pointer\n");
    s = dynalib_resolve(m, 0, "dbtest_greet", NULL, NULL);
    ok("dbtest_greet resolved", s != NULL, "yes");
    if (s) {
        dynalib_abi_parse("void()", &none);
        ok("abi parse void()",
           none.arg_count == 0 && none.ret_type == DYNALIB_T_VOID, "yes");
        stub = dynalib_trampoline_emit(s->fn_ptr, &none, NULL);
        ok("stub emitted", stub != NULL, "yes");
        if (stub) {
            uint64_t b[16] = {0};
            const char *greet = (const char *)((dl_stub_i)stub)(b);
            ok("stub() returns the greeting",
               greet && !strcmp(greet, "greetings from dbtest"), greet ? greet : "");
            dynalib_trampoline_free(stub, NULL);
        }
        dynalib_symbol_free(s);
    }

    printf("T7  callback stub round-trip (C calling back)\n");
    {
        DynalibAbiSpec abi;
        void *cb;
        DynalibSymbol *inv;

        dynalib_abi_parse("i32(i32, i32)", &abi);
        ok("abi parse i32(i32, i32)",
           abi.arg_count == 2 && abi.ret_type == DYNALIB_T_I32, "yes");

        g_cb_slot = (uintptr_t)-1; g_cb_argc = -1;
        g_cb_a = -1; g_cb_b = -1;
        dynalib_set_callback_dispatch(dispatcher);

        cb = dynalib_create_callback(NULL, &abi, (void *)0x42);
        ok("callback stub emitted", cb != NULL, "yes");
        if (cb) {
            inv = dynalib_resolve(m, 0, "dbtest_invoke_cb2", NULL, NULL);
            ok("dbtest_invoke_cb2 resolved", inv != NULL, "yes");
            if (inv) {
                ((void (*)(int, int, void (*)(int, int)))inv->fn_ptr)
                    (5, 6, (void (*)(int, int))cb);
                dynalib_symbol_free(inv);
            }
            ok("dispatcher saw the slot id", g_cb_slot == 0, "yes");
            ok("dispatcher saw argc", g_cb_argc == 2, "yes");
            ok("dispatcher saw arg a == 5", g_cb_a == 5, "yes");
            ok("dispatcher saw arg b == 6", g_cb_b == 6, "yes");
            dynalib_trampoline_free(cb, NULL);
            dynalib_set_callback_dispatch(NULL);
        }
    }

    dynalib_close(m);
}

/* ================================================================== */
/* T10: stack arguments (5+ args cross into the stack area)            */
/* ================================================================== */

static void t_stack_args(void)
{
    DynalibModule *m;
    DynalibSymbol *s;
    DynalibAbiSpec abi;
    void *stub;
    char detail[64];

    printf("T10  stack arguments through the call stub\n");

    m = dynalib_open(DBTEST_DLL, DYNALIB_FMT_AUTO, NULL);
    ok("dbtest.dll loaded", m != NULL, m ? "yes" : "NOT FOUND");
    if (!m) return;

    /* 5 args: MS64 puts the 5th on the stack, SysV keeps it in a register */
    dynalib_abi_parse("i32(i32, i32, i32, i32, i32)", &abi);
    ok("abi parse i32 x5", abi.arg_count == 5, "yes");
    s = dynalib_resolve(m, 0, "dbtest_sum5", NULL, NULL);
    ok("dbtest_sum5 resolved", s != NULL, "yes");
    if (s) {
        int r;
        stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
        ok("stub emitted for 5 args", stub != NULL, "yes");
        if (stub) {
            uint64_t b[16] = {0}; int j;
            for (j = 0; j < 5; j++) buf_i(b, j, j + 1);
            r = (int)((dl_stub_i)stub)(b);
            snprintf(detail, sizeof detail, "got %d", r);
            ok("stub(1,2,3,4,5) == 15", r == 15, detail);
            dynalib_trampoline_free(stub, NULL);
        }
        dynalib_symbol_free(s);
    }

    /* 8 args: 4 stack slots under MS64, 2 under SysV */
    dynalib_abi_parse("i32(i32, i32, i32, i32, i32, i32, i32, i32)", &abi);
    ok("abi parse i32 x8", abi.arg_count == 8, "yes");
    s = dynalib_resolve(m, 0, "dbtest_sum8", NULL, NULL);
    ok("dbtest_sum8 resolved", s != NULL, "yes");
    if (s) {
        int r;
        stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
        ok("stub emitted for 8 args", stub != NULL, "yes");
        if (stub) {
            uint64_t b[16] = {0}; int j;
            for (j = 0; j < 8; j++) buf_i(b, j, j + 1);
            r = (int)((dl_stub_i)stub)(b);
            snprintf(detail, sizeof detail, "got %d", r);
            ok("stub(1..8) == 36", r == 36, detail);
            dynalib_trampoline_free(stub, NULL);
        }
        dynalib_symbol_free(s);
    }

    /* 12 args: the full stack path */
    dynalib_abi_parse("i32(i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32)",
                      &abi);
    s = dynalib_resolve(m, 0, "dbtest_sum12", NULL, NULL);
    ok("dbtest_sum12 resolved", s != NULL, "yes");
    if (s) {
        int r;
        stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
        ok("stub emitted for 12 args", stub != NULL, "yes");
        if (stub) {
            uint64_t b[16] = {0}; int j;
            for (j = 0; j < 12; j++) buf_i(b, j, j + 1);
            r = (int)((dl_stub_i)stub)(b);
            snprintf(detail, sizeof detail, "got %d", r);
            ok("stub(1..12) == 78", r == 78, detail);
            dynalib_trampoline_free(stub, NULL);
        }
        dynalib_symbol_free(s);
    }

    /* positional check: a mis-forwarded slot shows up as a wrong constant */
    dynalib_abi_parse("i32(i32, i32, i32, i32, i32, i32, i32)", &abi);
    s = dynalib_resolve(m, 0, "dbtest_mix7", NULL, NULL);
    ok("dbtest_mix7 resolved", s != NULL, "yes");
    if (s) {
        int r;
        stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
        ok("stub emitted for 7 args", stub != NULL, "yes");
        if (stub) {
            uint64_t b[16] = {0}; int j;
            for (j = 0; j < 7; j++) buf_i(b, j, j + 1);
            r = (int)((dl_stub_i)stub)(b);
            snprintf(detail, sizeof detail, "got %d (want 1234567)", r);
            ok("stub(1..7) keeps slot order", r == 1234567, detail);
            dynalib_trampoline_free(stub, NULL);
        }
        dynalib_symbol_free(s);
    }

    dynalib_close(m);
}

/* ================================================================== */
/* T11: 64-bit returns                                                  */
/* ================================================================== */

static void t_wide_returns(void)
{
    DynalibModule *m;
    DynalibSymbol *s;
    DynalibAbiSpec abi;
    void *stub;
    char detail[64];

    printf("T11  64-bit return values\n");

    m = dynalib_open(DBTEST_DLL, DYNALIB_FMT_AUTO, NULL);
    ok("dbtest.dll loaded", m != NULL, m ? "yes" : "NOT FOUND");
    if (!m) return;

    dynalib_abi_parse("u64(u64)", &abi);
    ok("abi parse u64(u64)",
       abi.ret_type == DYNALIB_T_U64 && abi.arg_count == 1, "yes");
    s = dynalib_resolve(m, 0, "dbtest_retu64", NULL, NULL);
    ok("dbtest_retu64 resolved", s != NULL, "yes");
    if (s) {
        stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
        ok("stub emitted", stub != NULL, "yes");
        if (stub) {
            uint64_t b[16] = {0}, r;
            buf_i(b, 0, 5);
            r = ((dl_stub_i)stub)(b);
            snprintf(detail, sizeof detail, "got 0x%llx",
                     (unsigned long long)r);
            ok("stub(5) == 0x100000005", r == 0x100000005ULL, detail);
            dynalib_trampoline_free(stub, NULL);
        }
        dynalib_symbol_free(s);
    }

    dynalib_abi_parse("i64(i64)", &abi);
    s = dynalib_resolve(m, 0, "dbtest_reti64", NULL, NULL);
    ok("dbtest_reti64 resolved", s != NULL, "yes");
    if (s) {
        stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
        ok("stub emitted", stub != NULL, "yes");
        if (stub) {
            uint64_t b[16] = {0}; int64_t r;
            buf_i(b, 0, 42);
            r = (int64_t)((dl_stub_i)stub)(b);
            snprintf(detail, sizeof detail, "got %lld", (long long)r);
            ok("stub(42) == -42", r == -42, detail);
            dynalib_trampoline_free(stub, NULL);
        }
        dynalib_symbol_free(s);
    }

    /* 64-bit value crossing the stack boundary */
    dynalib_abi_parse("u64(u64, u64, u64, u64, u64)", &abi);
    s = dynalib_resolve(m, 0, "dbtest_sumu64", NULL, NULL);
    ok("dbtest_sumu64 resolved", s != NULL, "yes");
    if (s) {
        stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
        ok("stub emitted", stub != NULL, "yes");
        if (stub) {
            uint64_t b[16] = {0}, r; int j;
            for (j = 0; j < 5; j++) buf_i(b, j, j + 1);
            r = ((dl_stub_i)stub)(b);
            snprintf(detail, sizeof detail, "got %llu",
                     (unsigned long long)r);
            ok("stub(1..5) == 15", r == 15, detail);
            dynalib_trampoline_free(stub, NULL);
        }
        dynalib_symbol_free(s);
    }

    dynalib_close(m);
}

/* ================================================================== */
/* T8: ABI grammar golden tests                                        */
/* ================================================================== */

static void t_abi_grammar(void)
{
    DynalibAbiSpec a;
    printf("T8  calling-convention grammar\n");

    dynalib_abi_parse("sysv", &a);
    ok("\"sysv\"", a.kind == DYNALIB_ABI_SYSV, "yes");

    dynalib_abi_parse("ms_abi", &a);
    ok("\"ms_abi\" (the vendor_sdk.dll example)", a.kind == DYNALIB_ABI_MS64, "yes");

    dynalib_abi_parse("stdcall", &a);
    ok("\"stdcall\"", a.kind == DYNALIB_ABI_STDCALL, "yes");

    dynalib_abi_parse("u64(ptr, u32, u32)", &a);
    ok("\"u64(ptr, u32, u32)\"",
       a.ret_type == DYNALIB_T_U64 && a.arg_count == 3 &&
       a.arg_types[0] == DYNALIB_T_PTR, "yes");

    dynalib_abi_parse("void(f32, f32)", &a);
    ok("\"void(f32, f32)\" (the onMouseMove example)",
       a.ret_type == DYNALIB_T_VOID && a.arg_count == 2 &&
       a.arg_types[0] == DYNALIB_T_F32, "yes");

    dynalib_abi_parse("i32(str)", &a);
    ok("\"i32(str)\" (the address_ip_has_used example)",
       a.arg_count == 1 && a.arg_types[0] == DYNALIB_T_PTR, "yes");

    {
        char buf[128];
        snprintf(buf, sizeof buf, "count=%u regs=%u,%u kind=%d",
                 a.arg_count, a.arg_regs[0], a.arg_regs[1], a.kind);
        dynalib_abi_parse("ms_abi:rcx,rdx->rax", &a);
        snprintf(buf, sizeof buf, "count=%u regs=%u,%u kind=%d",
                 a.arg_count, a.arg_regs[0], a.arg_regs[1], a.kind);
        ok("\"ms_abi:rcx,rdx->rax\"",
           a.arg_count == 2 && a.arg_regs[0] == DYNALIB_REG_R0 &&
           a.arg_regs[1] == DYNALIB_REG_R1, buf);
    }
    {
        char buf[128];
        dynalib_abi_parse("sysv:rdi,rsi->rax", &a);
        snprintf(buf, sizeof buf, "count=%u regs=%u,%u kind=%d",
                 a.arg_count, a.arg_regs[0], a.arg_regs[1], a.kind);
        ok("\"sysv:rdi,rsi->rax\"",
           a.arg_count == 2 && a.arg_regs[0] == DYNALIB_REG_R0 &&
           a.arg_regs[1] == DYNALIB_REG_R1, buf);
    }

    ok("bad contract rejected", dynalib_abi_parse("nonsense(!!", &a) != 0, "yes");
    ok("bad reg name rejected", dynalib_abi_parse("ms_abi:zzz->rax", &a) != 0, "yes");
}

/* ================================================================== */
/* T9: pinning                                                         */
/* ================================================================== */

static int g_hook_calls = 0;
static void hook(void *c, void *lo, void *hi, bool pinned)
{
    (void)c; (void)lo; (void)hi; (void)pinned;
    g_hook_calls++;
}

static void t_pinning(void)
{
    unsigned char buf[4096];
    void *p;
    printf("T9  GC pinning with a counting hook\n");
    dynalib_set_gc_hook(hook, NULL);
    p = dynalib_pin_buffer(NULL, buf, sizeof buf);
    ok("pin returns the same address (zero-copy)", p == buf, "yes");
    ok("hook fired on pin", g_hook_calls == 1, "yes");
    p = dynalib_pin_buffer(NULL, buf, sizeof buf);
    ok("re-pin is idempotent", p == buf && g_hook_calls == 1, "yes");
    dynalib_unpin_buffer(NULL, buf);
    ok("hook fired on unpin", g_hook_calls == 2, "yes");
    ok("pin capacity reported", dynalib_pin_capacity() == 64, "yes");
    dynalib_set_gc_hook(NULL, NULL);
}

/* ================================================================== */
/* T10: ELF path over a synthetic image                                */
/* ================================================================== */

static void t_elf_synthetic(void)
{
    unsigned char *p;
    size_t eh = 64, phdr = 128, dyn_off = 240, sym_off = 344, str_off = 448;
    DynalibExportEntry e[16];
    ssize_t n;
    uintptr_t base_rva = 0;
    const char *err = NULL;
    size_t i;
    char dbuf[256];

    printf("T10 ELF parser over a synthetic ET_DYN image\n");
    p = (unsigned char *)calloc(1, 1024);
    if (!p) return;

    p[0] = 0x7f; p[1] = 'E'; p[2] = 'L'; p[3] = 'F';
    p[4] = 2;                     /* ELFCLASS64 */
    p[5] = 1;                     /* little-endian */
    p[16] = 2; p[17] = 0;         /* e_type = ET_DYN */
    p[18] = 0x3e; p[19] = 0;      /* e_machine = EM_X86_64 */
    *(uint64_t *)(p + 32) = phdr;          /* e_phoff */
    *(uint32_t *)(p + 52) = (uint32_t)eh;  /* e_ehsize */
    *(uint32_t *)(p + 54) = 56;            /* e_phentsize */
    *(uint32_t *)(p + 56) = 2;             /* e_phnum */

    /* PT_LOAD covering the whole image, then PT_DYNAMIC */
        {
            uint64_t *h0 = (uint64_t *)(p + phdr);
            h0[0] = 1;                       /* p_type  = PT_LOAD */
            h0[1] = 5;                       /* p_flags = PF_R|PF_X */
            h0[2] = 0;                       /* p_offset */
            h0[3] = 0;                       /* p_vaddr */
            h0[4] = 0;                       /* p_paddr */
            h0[5] = 1024;                    /* p_filesz */
            h0[6] = 1024;                    /* p_memsz */
            h0[7] = 0x1000;                  /* p_align  */
        }
        {
            uint64_t *h1 = (uint64_t *)(p + phdr + 56);
            size_t dlen = (sym_off + 72) - dyn_off;
            h1[0] = 2;                       /* p_type  = PT_DYNAMIC */
            h1[1] = 6;                       /* p_flags = PF_R|PF_W */
            h1[2] = dyn_off;                 /* p_offset */
            h1[3] = dyn_off;                 /* p_vaddr  */
            h1[4] = dyn_off;                 /* p_paddr  */
            h1[5] = dlen;                    /* p_filesz */
            h1[6] = dlen;                    /* p_memsz  */
            h1[7] = 8;                       /* p_align  */
        }

    /* DT_SYMTAB, DT_STRTAB, DT_NULL */
    *(uint64_t *)(p + dyn_off)      = 0x6ffffff9;
    *(uint64_t *)(p + dyn_off + 8)  = sym_off;
    *(uint64_t *)(p + dyn_off + 16) = 5;
    *(uint64_t *)(p + dyn_off + 24) = str_off;
    *(uint64_t *)(p + dyn_off + 32) = 0;
    *(uint64_t *)(p + dyn_off + 40) = 0;

    /* .dynsym: null, foo, bar */
    *(uint32_t *)(p + sym_off)       = 0;
    *(uint32_t *)(p + sym_off + 4)   = 0;
    *(uint64_t *)(p + sym_off + 8)   = 0;
    *(uint64_t *)(p + sym_off + 16)  = 0;

    *(uint32_t *)(p + sym_off + 24)  = 1;             /* st_name = "foo" */
    *(uint64_t *)(p + sym_off + 32)  = 0x1000;        /* st_value */
    *(uint32_t *)(p + sym_off + 40)  = 0x12;          /* st_size  */

    *(uint32_t *)(p + sym_off + 48)  = 5;             /* st_name = "bar" */
    *(uint64_t *)(p + sym_off + 56)  = 0x2000;
    *(uint32_t *)(p + sym_off + 64)  = 0x12;

    /* strcpy stops at the leading NUL, so the whole table has to be
     * memcpy'd: "\0foo\0bar\0" is NUL-terminated but NUL-prefixed         */
    memcpy(p + str_off, "\0foo\0bar\0", 9);

    n = dynalib_parse_image(p, 1024, e, 16, &base_rva, &err);
    snprintf(dbuf, sizeof dbuf, "n=%lld base_rva=0x%llx err=%s", (long long)n,
             (unsigned long long)base_rva, err ? err : "(none)");
    ok("synthetic ELF parsed", n == 2, dbuf);
    if (n == 2) {
        ok("first symbol is foo", !strcmp(e[0].name, "foo"), e[0].name);
        ok("foo rva 0x1000", e[0].rva == 0x1000, "yes");
        ok("second symbol is bar", !strcmp(e[1].name, "bar"), e[1].name);
        ok("bar rva 0x2000", e[1].rva == 0x2000, "yes");
        ok("first sym rva reported", base_rva == 0x1000, "yes");
    }
    ok("format sniff says elf",
       dynalib_detect_format(p, 1024) == DYNALIB_FMT_ELF_SO,
       dynalib_format_name(DYNALIB_FMT_ELF_SO));
    for (i = 0; i < (size_t)(n > 0 ? n : 0) && i < 4; i++)
            printf("        sym %-6s rva 0x%x\n", e[i].name, e[i].rva);
    free(p);
}

/* ================================================================== */
/* T11: sniffing + lbrct hook contract                                 */
/* ================================================================== */

static void t_sniff(void)
{
    unsigned char lbrct[16];
    unsigned char other[8] = { 'x' };
    printf("T11 container sniffing\n");
    memset(lbrct, 0, sizeof lbrct);
    lbrct[0] = 0x54; lbrct[1] = 0x52; lbrct[2] = 0x42; lbrct[3] = 0x4c;
    ok("lbrct magic recognised",
       dynalib_detect_format(lbrct, 16) == DYNALIB_FMT_LBRCT,
       dynalib_format_name(DYNALIB_FMT_LBRCT));
    ok("unknown magic is auto",
       dynalib_detect_format(other, 8) == DYNALIB_FMT_AUTO,
       dynalib_format_name(DYNALIB_FMT_AUTO));
    ok("lbrct parser is a documented hook, not a silent pass",
       dynalib_parse_image(lbrct, 16, NULL, 0, NULL, NULL) == -1, "yes");
}

/* ================================================================== */
/* T12: floating-point arguments and returns                           */
/* ================================================================== */

static void t_float_args(void)
{
    DynalibModule *m;
    DynalibSymbol *s;
    DynalibAbiSpec abi;
    void *stub;
    char detail[64];
    uint64_t b[16];
    double r;

    printf("T12  floating-point arguments through the call stub\n");

    m = dynalib_open(DBTEST_DLL, DYNALIB_FMT_AUTO, NULL);
    ok("dbtest.dll loaded", m != NULL, m ? "yes" : "NOT FOUND");
    if (!m) return;

    /* two doubles: xmm0 and xmm1 */
    dynalib_abi_parse("f64(f64, f64)", &abi);
    ok("abi parse f64(f64, f64)",
       abi.ret_type == DYNALIB_T_F64 && abi.arg_count == 2 &&
       abi.arg_types[0] == DYNALIB_T_F64, "yes");
    s = dynalib_resolve(m, 0, "dbtest_fadd", NULL, NULL);
    ok("dbtest_fadd resolved", s != NULL, "yes");
    if (s) {
        stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
        ok("stub emitted for f64 args", stub != NULL, "yes");
        if (stub) {
            memset(b, 0, sizeof b);
            buf_d(b, 0, 2.5); buf_d(b, 1, 4.25);
            r = ((dl_stub_f)stub)(b);
            snprintf(detail, sizeof detail, "got %f", r);
            ok("fadd(2.5, 4.25) == 6.75", r == 6.75, detail);
            dynalib_trampoline_free(stub, NULL);
        }
        dynalib_symbol_free(s);
    }

    /* three doubles: xmm0..xmm2 */
    dynalib_abi_parse("f64(f64, f64, f64)", &abi);
    s = dynalib_resolve(m, 0, "dbtest_fmul3", NULL, NULL);
    ok("dbtest_fmul3 resolved", s != NULL, "yes");
    if (s) {
        stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
        ok("stub emitted for 3 doubles", stub != NULL, "yes");
        if (stub) {
            memset(b, 0, sizeof b);
            buf_d(b, 0, 2.0); buf_d(b, 1, 3.0); buf_d(b, 2, 4.0);
            r = ((dl_stub_f)stub)(b);
            snprintf(detail, sizeof detail, "got %f", r);
            ok("fmul3(2,3,4) == 24", r == 24.0, detail);
            dynalib_trampoline_free(stub, NULL);
        }
        dynalib_symbol_free(s);
    }

    /* mixed classes: int, double, int, double, double */
    dynalib_abi_parse("f64(i32, f64, i32, f64, f64)", &abi);
    s = dynalib_resolve(m, 0, "dbtest_fmix5", NULL, NULL);
    ok("dbtest_fmix5 resolved", s != NULL, "yes");
    if (s) {
        stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
        ok("stub emitted for mixed classes", stub != NULL, "yes");
        if (stub) {
            memset(b, 0, sizeof b);
            buf_i(b, 0, 1); buf_d(b, 1, 2.0); buf_i(b, 2, 3);
            buf_d(b, 3, 4.0); buf_d(b, 4, 5.0);
            r = ((dl_stub_f)stub)(b);
            snprintf(detail, sizeof detail, "got %f (want 1239)", r);
            ok("fmix5 keeps classes apart", r == 1239.0, detail);
            dynalib_trampoline_free(stub, NULL);
        }
        dynalib_symbol_free(s);
    }

    /* nine doubles: the ninth spills past the xmm budget onto the stack */
    dynalib_abi_parse("f64(f64, f64, f64, f64, f64, f64, f64, f64, f64)",
                      &abi);
    s = dynalib_resolve(m, 0, "dbtest_fsum9", NULL, NULL);
    ok("dbtest_fsum9 resolved", s != NULL, "yes");
    if (s) {
        stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
        ok("stub emitted for 9 doubles", stub != NULL, "yes");
        if (stub) {
            int j;
            memset(b, 0, sizeof b);
            for (j = 0; j < 9; j++) buf_d(b, j, (double)(j + 1));
            r = ((dl_stub_f)stub)(b);
            snprintf(detail, sizeof detail, "got %f (want 45)", r);
            ok("fsum9(1..9) == 45", r == 45.0, detail);
            dynalib_trampoline_free(stub, NULL);
        }
        dynalib_symbol_free(s);
    }

    /* a float return has to be widened for a double-reading caller */
    dynalib_abi_parse("f32(f32)", &abi);
    s = dynalib_resolve(m, 0, "dbtest_fhalf", NULL, NULL);
    ok("dbtest_fhalf resolved", s != NULL, "yes");
    if (s) {
        stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
        ok("stub emitted for f32", stub != NULL, "yes");
        if (stub) {
            memset(b, 0, sizeof b);
            buf_f(b, 0, 7.0f);
            r = ((dl_stub_f)stub)(b);
            snprintf(detail, sizeof detail, "got %f (want 3.5)", r);
            ok("fhalf(7.0) == 3.5", r == 3.5, detail);
            dynalib_trampoline_free(stub, NULL);
        }
        dynalib_symbol_free(s);
    }

    /* double first, integer second: the GP budget must not eat the
     * double, and the double must not eat the integer                  */
    dynalib_abi_parse("f64(f64, i32)", &abi);
    s = dynalib_resolve(m, 0, "dbtest_fscale", NULL, NULL);
    ok("dbtest_fscale resolved", s != NULL, "yes");
    if (s) {
        stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
        ok("stub emitted for f64+i32", stub != NULL, "yes");
        if (stub) {
            memset(b, 0, sizeof b);
            buf_d(b, 0, 1.5); buf_i(b, 1, 4);
            r = ((dl_stub_f)stub)(b);
            snprintf(detail, sizeof detail, "got %f (want 6)", r);
            ok("fscale(1.5, 4) == 6", r == 6.0, detail);
            dynalib_trampoline_free(stub, NULL);
        }
        dynalib_symbol_free(s);
    }

    dynalib_close(m);
}

int main(int argc, char **argv)
{
    const char *sqlite = SQLITE_DLL;
    const char *dbtest = DBTEST_DLL;
    if (argc > 1) sqlite = argv[1];
    if (argc > 2) dbtest = argv[2];
    setvbuf(stdout, NULL, _IONBF, 0);   /* a crash must not swallow progress */
#if defined(_WIN32)
    AddVectoredExceptionHandler(1, on_fault);
#endif

    printf("libdynalib test harness\n");
    printf("  sqlite: %s\n  dbtest: %s\n\n", sqlite, dbtest);

    t_sniff();
    t_elf_synthetic();
    t_abi_grammar();
    t_pinning();
    t_pe_and_sqlite();
    t_trampolines(dbtest);
    t_stack_args();
    t_wide_returns();
    t_float_args();

    printf("\n== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
