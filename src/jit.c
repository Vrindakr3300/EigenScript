#include "jit.h"
#include "eigenscript.h"
#include "vm.h"

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

void jit_module_init(void) {
    /* Defer cache creation until the first compile request. */
}

void jit_module_shutdown(void) {
    if (g_jit_cache) {
        jit_cache_free(g_jit_cache);
        g_jit_cache = NULL;
    }
}

void jit_try_compile_chunk(struct EigsChunk *chunk) {
    if (!chunk) return;
    /* Stage 2: no opcode templates yet. Mark as failed so we never
     * retry. Stage 3 will scan chunk->code and emit native templates
     * for the supported opcode prefix, then set jit_state = 2. */
    chunk->jit_state = 1;
    chunk->jit_code = NULL;
    /* Reference the cache var so a future compiler doesn't warn —
     * Stage 3 will replace this with real lazy allocation. */
    (void)g_jit_cache;
}
