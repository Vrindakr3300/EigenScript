/*
 * EigenScript JIT — Stage 1 scaffolding.
 *
 * Code cache built on a bump-allocated mmap region that starts RW, then
 * flips to RX once a batch of thunks has been emitted. No external
 * dependencies; raw machine-code emission per target ISA.
 *
 * Stage 1 exposes only the cache primitives plus a smoke emitter that
 * produces a "return int64 constant" thunk. Stage 2+ will add real
 * opcode templates (ARITH_FAST, LOCAL_GET/SET, JUMP) and a chunk-level
 * compile entry hook into vm_run.
 *
 * Currently x86-64 SysV only. aarch64 is a small port: 4-byte fixed-
 * width insns + an explicit __builtin___clear_cache call.
 */
#ifndef EIGS_JIT_H
#define EIGS_JIT_H

#include <stddef.h>
#include <stdint.h>

typedef struct EigsJitCache EigsJitCache;

/* Allocate a code cache covering `page_count` system pages. Memory is
 * initially RW. Returns NULL on mmap failure. */
EigsJitCache *jit_cache_new(size_t page_count);

/* Unmap and free the cache. Safe on NULL. */
void jit_cache_free(EigsJitCache *jc);

/* Bump-allocate `bytes` from the cache. Returns NULL if the cache is
 * sealed or out of room. The returned pointer is writable until seal. */
void *jit_cache_alloc(EigsJitCache *jc, size_t bytes);

/* Flip the entire cache to RX and clear the instruction cache. Returns
 * 0 on success, -1 if mprotect failed. Idempotent. */
int jit_cache_seal(EigsJitCache *jc);

/* Bytes currently allocated from the cache. */
size_t jit_cache_used(const EigsJitCache *jc);

/* Smoke: emit a thunk with signature `int64_t fn(void)` that returns
 * `value`. Returns a callable pointer or NULL. Caller must seal the
 * cache before invoking the returned function. */
typedef int64_t (*JitConstFn)(void);
JitConstFn jit_emit_const_return(EigsJitCache *jc, int64_t value);

/* ---- In-VM integration ----
 *
 * Forward declare the chunk type so vm.c can hand chunks to the JIT
 * without dragging the full vm.h into jit.c.  The full layout is
 * defined in vm.h. */
struct EigsChunk;

/* Signature of a chunk-level JITed entry point. The thunk reads the
 * thread-local VM state, executes the prefix of `chunk`'s opcodes it
 * supports, points `g_vm.frames[frame_count-1].ip` at the next byte
 * the interpreter should resume from, and returns. */
typedef void (*JitChunkFn)(struct EigsChunk *chunk);

/* Try to compile `chunk` into native code. On success, sets
 * chunk->jit_state = 2 and chunk->jit_code to a JitChunkFn pointer.
 * On any unsupported pattern, sets jit_state = 1 and leaves jit_code
 * NULL. Idempotent — subsequent calls on the same chunk return
 * immediately if jit_state != 0. */
void jit_try_compile_chunk(struct EigsChunk *chunk);

/* Lifetime hooks for the per-thread JIT cache. jit_module_shutdown is
 * optional (process exit reclaims pages anyway) but useful in tests. */
void jit_module_init(void);
void jit_module_shutdown(void);

#endif /* EIGS_JIT_H */
