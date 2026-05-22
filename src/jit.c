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

/* Stage 3a templates implemented in vm.c. Each helper natively executes
 * one opcode at the top-of-frame, advances frame->ip by the opcode size,
 * and returns. */
extern void eigs_jit_tmpl_null(void);
extern void eigs_jit_tmpl_num_zero(void);
extern void eigs_jit_tmpl_num_one(void);
extern void eigs_jit_tmpl_line(int line);
extern void eigs_jit_tmpl_pop(void);

#if defined(__x86_64__)
/* Scan a chunk-start prefix of opcodes we can natively translate.
 * Returns the prefix length in *bytes* (0 = nothing compilable).
 *
 * Profitability filter: the helper-call trampoline pays ~30 cycles per
 * opcode (movabs + indirect call + helper body + ret) vs the
 * interpreter's ~5-cycle computed-goto dispatch. We only compile when
 * the prefix contains at least one non-LINE opcode AND the prefix is
 * at least 3 opcodes long, so the JIT setup amortizes. Stage 3b will
 * replace this with inline-emit which can profitably handle short
 * prefixes including LINE-only.
 */
static int jit_supported_prefix(const struct EigsChunk *chunk) {
    int i = 0, ops = 0, non_line_ops = 0;
    int last_good = 0;
    while (i < chunk->code_len) {
        uint8_t op = chunk->code[i];
        if (op == OP_NULL || op == OP_NUM_ZERO || op == OP_NUM_ONE ||
            op == OP_POP) {
            i += 1; ops++; non_line_ops++;
        } else if (op == OP_LINE) {
            if (i + 3 > chunk->code_len) break;
            i += 3; ops++;
        } else {
            break;
        }
        last_good = i;
    }
    if (non_line_ops == 0 || ops < 3) return 0;
    return last_good;
}

/* Worst-case bytes emitted per opcode in the prefix. */
static size_t jit_estimate_size(const struct EigsChunk *chunk, int prefix) {
    /* prologue (sub $8,%rsp) + epilogue (add $8,%rsp; ret) = 4 + 5 = 9 */
    size_t s = 9;
    int i = 0;
    while (i < prefix) {
        uint8_t op = chunk->code[i];
        if (op == OP_LINE) { s += 17; i += 3; } /* mov imm32,%edi; movabs; call */
        else               { s += 12; i += 1; } /* movabs; call */
    }
    return s;
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

    size_t size = jit_estimate_size(chunk, prefix);
    uint8_t *code = jit_cache_alloc(g_jit_cache, size);
    if (!code) {
        chunk->jit_state = 1;
        chunk->jit_code = NULL;
        jit_cache_seal(g_jit_cache);
        return;
    }

    uint8_t *w = code;

    /* Prologue: sub $8, %rsp  — restores 16-byte stack alignment before
     * inner calls. Thunk entry has %rsp = 16n+8 (caller's `call` pushed
     * 8 bytes). After sub $8: 16n+0; inner call lands at 16n-8+8 = 16n+8
     * which is what callees expect on entry. */
    *w++ = 0x48; *w++ = 0x83; *w++ = 0xEC; *w++ = 0x08;

    int i = 0;
    while (i < prefix) {
        uint8_t op = chunk->code[i];
        void *helper = NULL;
        int op_size = 1;

        if (op == OP_NULL)            helper = (void *)&eigs_jit_tmpl_null;
        else if (op == OP_NUM_ZERO)   helper = (void *)&eigs_jit_tmpl_num_zero;
        else if (op == OP_NUM_ONE)    helper = (void *)&eigs_jit_tmpl_num_one;
        else if (op == OP_POP)        helper = (void *)&eigs_jit_tmpl_pop;
        else if (op == OP_LINE) {
            uint16_t line = (uint16_t)(chunk->code[i + 1] |
                                       ((uint16_t)chunk->code[i + 2] << 8));
            int32_t imm = (int32_t)line;
            /* mov $line, %edi  — argument register for eigs_jit_tmpl_line */
            *w++ = 0xBF;
            memcpy(w, &imm, 4); w += 4;
            helper = (void *)&eigs_jit_tmpl_line;
            op_size = 3;
        }

        /* movabs $helper, %rax */
        *w++ = 0x48; *w++ = 0xB8;
        memcpy(w, &helper, sizeof helper); w += sizeof helper;
        /* call *%rax */
        *w++ = 0xFF; *w++ = 0xD0;

        i += op_size;
    }

    /* Epilogue: add $8, %rsp; ret */
    *w++ = 0x48; *w++ = 0x83; *w++ = 0xC4; *w++ = 0x08;
    *w++ = 0xC3;

    /* Defensive: we may have emitted fewer bytes than estimated if the
     * estimator over-counted. We bumped `used` by `size`; the extra
     * bytes are harmless padding. */
    (void)w;

    if (jit_cache_seal(g_jit_cache) != 0) {
        /* Cache stays RW — we can't safely execute. Abandon this chunk. */
        chunk->jit_state = 1;
        chunk->jit_code = NULL;
        return;
    }

    chunk->jit_state = 2;
    chunk->jit_code = code;
    g_jit_compiled_chunks++;
#endif
}
