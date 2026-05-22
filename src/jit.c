#include "jit.h"
#include "eigenscript.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct EigsJitCache {
    uint8_t *base;
    size_t   capacity;
    size_t   used;
    int      sealed;
};

EigsJitCache *jit_cache_new(size_t page_count) {
    if (page_count == 0) page_count = 1;
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    size_t cap = page_count * (size_t)ps;
    void *p = mmap(NULL, cap, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    EigsJitCache *jc = calloc(1, sizeof *jc);
    if (!jc) {
        munmap(p, cap);
        return NULL;
    }
    jc->base = (uint8_t *)p;
    jc->capacity = cap;
    return jc;
}

void jit_cache_free(EigsJitCache *jc) {
    if (!jc) return;
    if (jc->base) munmap(jc->base, jc->capacity);
    free(jc);
}

void *jit_cache_alloc(EigsJitCache *jc, size_t bytes) {
    if (!jc || jc->sealed) return NULL;
    if (jc->used + bytes > jc->capacity) return NULL;
    void *p = jc->base + jc->used;
    jc->used += bytes;
    return p;
}

int jit_cache_seal(EigsJitCache *jc) {
    if (!jc) return -1;
    if (jc->sealed) return 0;
    if (mprotect(jc->base, jc->capacity, PROT_READ | PROT_EXEC) != 0) return -1;
    __builtin___clear_cache((char *)jc->base, (char *)jc->base + jc->capacity);
    jc->sealed = 1;
    return 0;
}

int jit_cache_unseal(EigsJitCache *jc) {
    if (!jc) return -1;
    if (!jc->sealed) return 0;
    if (mprotect(jc->base, jc->capacity, PROT_READ | PROT_WRITE) != 0) return -1;
    jc->sealed = 0;
    return 0;
}

size_t jit_cache_used(const EigsJitCache *jc) {
    return jc ? jc->used : 0;
}

/* x86-64 SysV: int64 return goes in %rax.
 *
 *   48 B8 <imm64>   movabs $imm64, %rax
 *   C3              ret
 *
 * 11 bytes total. */
JitConstFn jit_emit_const_return(EigsJitCache *jc, int64_t value) {
#if defined(__x86_64__)
    uint8_t *code = jit_cache_alloc(jc, 11);
    if (!code) return NULL;
    code[0] = 0x48;
    code[1] = 0xB8;
    memcpy(&code[2], &value, 8);
    code[10] = 0xC3;
    return (JitConstFn)(void *)code;
#else
    (void)jc;
    (void)value;
    return NULL;
#endif
}

/* ---- In-VM integration ---- */

/* Per-thread cache so each interpreter thread compiles into its own
 * pages. Lazily initialized on first compile attempt. */
static __thread EigsJitCache *g_jit_cache = NULL;
static __thread int g_jit_compiled_chunks = 0;
static __thread int g_jit_scanned_chunks = 0;

void jit_module_init(void) {
    /* Defer cache creation until the first compile request. */
}

__attribute__((constructor))
static void jit_register_atexit(void) {
    atexit(jit_module_shutdown);
}

void jit_module_shutdown(void) {
    if (getenv("EIGS_JIT_STATS")) {
        fprintf(stderr, "[jit] scanned=%d compiled=%d cache_used=%zu\n",
                g_jit_scanned_chunks, g_jit_compiled_chunks,
                g_jit_cache ? jit_cache_used(g_jit_cache) : 0);
    }
    if (g_jit_cache) {
        jit_cache_free(g_jit_cache);
        g_jit_cache = NULL;
    }
}

#if defined(__x86_64__)
/* Stage 3b: layout descriptor captured once via the vm.c-owned probe. */
static int g_layout_inited = 0;
static EigsJitLayout g_layout;

static void ensure_layout(void) {
    if (g_layout_inited) return;
    eigs_jit_get_layout(&g_layout);
    g_layout_inited = 1;
}

/* Scan a chunk-start prefix of opcodes we can natively translate.
 * Returns the prefix length in *bytes* (0 = nothing compilable).
 *
 * Stage 4 supported set:
 *   OP_NULL, OP_NUM_ZERO, OP_NUM_ONE — inline immediate push.
 *   OP_LINE                          — inline 32-bit store to current_line.
 *   OP_CONST [idx:16]                — inline push of pre-encoded slot;
 *                                      heap constants emit a one-shot
 *                                      atomic incref (skipped statically
 *                                      when v->arena == 1).
 *   OP_POP                           — peephole `dec %ecx`, valid only
 *                                      when the most recent push was an
 *                                      immediate (number/null) whose
 *                                      slot_decref is a no-op.
 *
 * Requires ≥3 ops AND ≥1 non-LINE op so the prologue/epilogue fixed
 * cost is recovered. */
static int jit_supported_prefix(const struct EigsChunk *chunk) {
    int i = 0, ops = 0, non_line_ops = 0;
    int last_good = 0;
    int last_push_immediate = 0;  /* most recent push was a non-refcounted slot */
    while (i < chunk->code_len) {
        uint8_t op = chunk->code[i];
        if (op == OP_NULL || op == OP_NUM_ZERO || op == OP_NUM_ONE) {
            i += 1; ops++; non_line_ops++;
            last_push_immediate = 1;
        } else if (op == OP_CONST) {
            if (i + 3 > chunk->code_len) break;
            uint16_t idx = (uint16_t)(chunk->code[i + 1] |
                                      ((uint16_t)chunk->code[i + 2] << 8));
            if (idx >= chunk->const_count) break;
            Value *v = chunk->constants[idx];
            if (!v) break;
            i += 3; ops++; non_line_ops++;
            /* Numeric constants flow as immediates; heap constants need
             * a runtime incref but still don't grow the stack with a
             * refcounted slot (the bits are the same — heap slot). */
            last_push_immediate = (v->type == VAL_NUM && v->obs_age == 0);
        } else if (op == OP_POP) {
            if (!last_push_immediate) break;
            i += 1; ops++; non_line_ops++;
            last_push_immediate = 0;
        } else if (op == OP_LINE) {
            if (i + 3 > chunk->code_len) break;
            i += 3; ops++;
            /* OP_LINE doesn't touch the stack — preserve last_push_immediate */
        } else {
            break;
        }
        last_good = i;
    }
    if (non_line_ops == 0 || ops < 3) return 0;
    return last_good;
}

/* Upper bound on emitted bytes:
 *   prologue 23 + epilogue 10 + 40 bytes/op worst case (OP_CONST heap
 *   with incref: movabs+store+inc + movabs %rdi + lock addl).
 *   The caller advances frame->ip by chunk->jit_advance, so the thunk
 *   only writes %ecx back to g_vm.sp before returning. */
static size_t jit_estimate_size(int prefix) {
    return 23 + 10 + 40 * (size_t)prefix;
}

/* ---- x86-64 encoding helpers ---- */

static uint8_t *emit_u8(uint8_t *w, uint8_t b)         { *w++ = b; return w; }
static uint8_t *emit_u32(uint8_t *w, uint32_t v)       { memcpy(w, &v, 4); return w + 4; }
static uint8_t *emit_u64(uint8_t *w, uint64_t v)       { memcpy(w, &v, 8); return w + 8; }

/* mov %fs:0, %rbx  — load TLS base into %rbx (9 bytes) */
static uint8_t *emit_load_fs_zero_rbx(uint8_t *w) {
    *w++ = 0x64; *w++ = 0x48; *w++ = 0x8B;
    *w++ = 0x1C; *w++ = 0x25;
    return emit_u32(w, 0);
}

/* lea disp32(%rbx), %rbx  — %rbx += disp32 (7 bytes) */
static uint8_t *emit_lea_rbx_disp32_rbx(uint8_t *w, int32_t disp) {
    *w++ = 0x48; *w++ = 0x8D; *w++ = 0x9B;
    return emit_u32(w, (uint32_t)disp);
}

/* mov disp32(%rbx), %ecx  — load 32-bit (6 bytes) */
static uint8_t *emit_mov_disp32_rbx_to_ecx(uint8_t *w, int32_t disp) {
    *w++ = 0x8B; *w++ = 0x8B;
    return emit_u32(w, (uint32_t)disp);
}

/* mov %ecx, disp32(%rbx)  — store 32-bit (6 bytes) */
static uint8_t *emit_mov_ecx_to_disp32_rbx(uint8_t *w, int32_t disp) {
    *w++ = 0x89; *w++ = 0x8B;
    return emit_u32(w, (uint32_t)disp);
}

/* movabs $imm64, %rax  (10 bytes) */
static uint8_t *emit_movabs_rax(uint8_t *w, uint64_t imm) {
    *w++ = 0x48; *w++ = 0xB8;
    return emit_u64(w, imm);
}

/* mov %rax, disp32(%rbx, %rcx, 8)  — store 64-bit at stack[sp] (8 bytes) */
static uint8_t *emit_store_rax_at_stack(uint8_t *w, int32_t off_stack) {
    *w++ = 0x48; *w++ = 0x89; *w++ = 0x84; *w++ = 0xCB;
    return emit_u32(w, (uint32_t)off_stack);
}

/* movl $imm32, disp32(%rbx)  (10 bytes) */
static uint8_t *emit_movl_imm_at_disp32_rbx(uint8_t *w, int32_t disp,
                                            int32_t value) {
    *w++ = 0xC7; *w++ = 0x83;
    w = emit_u32(w, (uint32_t)disp);
    return emit_u32(w, (uint32_t)value);
}

/* inc %ecx (2 bytes) */
static uint8_t *emit_inc_ecx(uint8_t *w) { *w++ = 0xFF; *w++ = 0xC1; return w; }
/* dec %ecx (2 bytes) */
static uint8_t *emit_dec_ecx(uint8_t *w) { *w++ = 0xFF; *w++ = 0xC9; return w; }

/* movabs $imm64, %rdi  (10 bytes) */
static uint8_t *emit_movabs_rdi(uint8_t *w, uint64_t imm) {
    *w++ = 0x48; *w++ = 0xBF;
    return emit_u64(w, imm);
}

/* lock addl $1, disp32(%rdi)  — atomic refcount increment (8 bytes) */
static uint8_t *emit_lock_addl_1_disp32_rdi(uint8_t *w, int32_t disp) {
    *w++ = 0xF0; *w++ = 0x83; *w++ = 0x87;
    w = emit_u32(w, (uint32_t)disp);
    *w++ = 0x01;
    return w;
}
#endif

void jit_try_compile_chunk(struct EigsChunk *chunk) {
    if (!chunk) return;
    if (chunk->jit_state != 0) return;
    g_jit_scanned_chunks++;
#if !defined(__x86_64__)
    chunk->jit_state = 1;
    chunk->jit_code = NULL;
    return;
#else
    ensure_layout();

    int prefix = jit_supported_prefix(chunk);
    if (prefix == 0) {
        chunk->jit_state = 1;
        chunk->jit_code = NULL;
        return;
    }

    if (!g_jit_cache) {
        g_jit_cache = jit_cache_new(64); /* 64 pages = 256 KB */
        if (!g_jit_cache) {
            chunk->jit_state = 1;
            chunk->jit_code = NULL;
            return;
        }
    }
    if (jit_cache_unseal(g_jit_cache) != 0) {
        chunk->jit_state = 1;
        chunk->jit_code = NULL;
        return;
    }

    size_t size = jit_estimate_size(prefix);
    uint8_t *code = jit_cache_alloc(g_jit_cache, size);
    if (!code) {
        chunk->jit_state = 1;
        chunk->jit_code = NULL;
        jit_cache_seal(g_jit_cache);
        return;
    }

    uint8_t *w = code;

    /* Prologue (23 bytes): preserve %rbx, point it at &g_vm, cache sp. */
    w = emit_u8(w, 0x53);                                       /* push %rbx */
    w = emit_load_fs_zero_rbx(w);                               /* mov %fs:0, %rbx */
    w = emit_lea_rbx_disp32_rbx(w, (int32_t)g_layout.g_vm_tpoff);
    w = emit_mov_disp32_rbx_to_ecx(w, g_layout.off_sp);         /* mov sp, %ecx */

    /* Body: inline native code per opcode. */
    int i = 0;
    while (i < prefix) {
        uint8_t op = chunk->code[i];
        if (op == OP_NULL || op == OP_NUM_ZERO || op == OP_NUM_ONE) {
            uint64_t bits;
            if (op == OP_NULL) {
                bits = 0xFFF8000000000000ULL;       /* SLOT_NULL_BITS */
            } else if (op == OP_NUM_ZERO) {
                bits = 0;                            /* IEEE-754 +0.0 */
            } else {
                double d = 1.0;
                memcpy(&bits, &d, 8);                /* IEEE-754 1.0 */
            }
            w = emit_movabs_rax(w, bits);
            w = emit_store_rax_at_stack(w, g_layout.off_stack);
            w = emit_inc_ecx(w);
            i += 1;
        } else if (op == OP_CONST) {
            uint16_t idx = (uint16_t)(chunk->code[i + 1] |
                                      ((uint16_t)chunk->code[i + 2] << 8));
            Value *v = chunk->constants[idx];
            uint64_t bits;
            int needs_incref = 0;
            if (v->type == VAL_NUM && v->obs_age == 0) {
                /* Immediate-num: same shape as NUM_ZERO. */
                memcpy(&bits, &v->data.num, 8);
            } else {
                /* Heap slot: TAG_HEAP | payload. Incref unless arena. */
                bits = 0xFFFB000000000000ULL |
                       ((uint64_t)(uintptr_t)v & 0x0000FFFFFFFFFFFFULL);
                needs_incref = !v->arena;
            }
            w = emit_movabs_rax(w, bits);
            w = emit_store_rax_at_stack(w, g_layout.off_stack);
            w = emit_inc_ecx(w);
            if (needs_incref) {
                w = emit_movabs_rdi(w, (uint64_t)(uintptr_t)v);
                w = emit_lock_addl_1_disp32_rdi(
                        w, (int32_t)offsetof(Value, refcount));
            }
            i += 3;
        } else if (op == OP_POP) {
            /* Peephole: prior op pushed an immediate, whose slot_decref
             * is a no-op. Just dec the cached sp. */
            w = emit_dec_ecx(w);
            i += 1;
        } else { /* OP_LINE */
            uint16_t line = (uint16_t)(chunk->code[i + 1] |
                                       ((uint16_t)chunk->code[i + 2] << 8));
            w = emit_movl_imm_at_disp32_rbx(w, g_layout.off_current_line,
                                            (int32_t)line);
            i += 3;
        }
    }

    /* Epilogue (8 bytes): write sp back to g_vm, restore %rbx, return.
     * frame->ip advance is the caller's responsibility (see jit.h). */
    w = emit_mov_ecx_to_disp32_rbx(w, g_layout.off_sp);
    w = emit_u8(w, 0x5B);                                       /* pop %rbx */
    w = emit_u8(w, 0xC3);                                       /* ret */

    /* Sanity: did we stay within the allocation? */
    if ((size_t)(w - code) > size) {
        /* Catastrophic — we wrote past `used`. Abandon. */
        chunk->jit_state = 1;
        chunk->jit_code = NULL;
        jit_cache_seal(g_jit_cache);
        return;
    }

    if (jit_cache_seal(g_jit_cache) != 0) {
        chunk->jit_state = 1;
        chunk->jit_code = NULL;
        return;
    }

    chunk->jit_state = 2;
    chunk->jit_code = code;
    chunk->jit_advance = prefix;
    g_jit_compiled_chunks++;
    if (getenv("EIGS_JIT_DEBUG")) {
        fprintf(stderr, "[jit] compiled %s: prefix=%d bytes (",
                chunk->name ? chunk->name : "?", prefix);
        for (int j = 0; j < prefix; j++)
            fprintf(stderr, " %02x", chunk->code[j]);
        fprintf(stderr, " ) -> %zu bytes native\n", (size_t)(w - code));
    }
#endif
}
