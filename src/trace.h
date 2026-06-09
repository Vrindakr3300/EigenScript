/* ================================================================
 * EigenScript Trace — Phase 1: line/assignment event recorder
 * ================================================================
 * Off by default. Enabled by setting EIGS_TRACE=<path> in the env.
 * Output is a text tape — name-keyed assignment deltas + line events —
 * intended to back temporal interrogatives (`what of x at line 42`,
 * `where of y`, etc.) in later phases.
 *
 * Hot-path cost when disabled: one predicted-not-taken branch on the
 * extern int g_trace_enabled at each hook site.
 */
#ifndef EIGENSCRIPT_TRACE_H
#define EIGENSCRIPT_TRACE_H

#include <stdint.h>

/* EigsSlot is fully defined in value_slot.h (which requires Value).
 * For the trace API surface we only need the union shape, so guard
 * with the same sentinel value_slot.h uses to avoid a re-typedef. */
#ifndef EIGENSCRIPT_EIGSSLOT_UNION_DEFINED
#define EIGENSCRIPT_EIGSSLOT_UNION_DEFINED
typedef union { double d; uint64_t u; } EigsSlot;
#endif

/* 1 when EIGS_TRACE was set and a tape was successfully opened.
 * Hook sites in vm.c gate on this directly so the disabled case
 * costs one load + one branch. */
extern int g_trace_enabled;

/* Called once from main() during startup. Reads EIGS_TRACE; if set,
 * opens the path for writing and flips g_trace_enabled. Safe to call
 * multiple times — second and later calls no-op. */
void trace_init(void);

/* Called at process exit to flush and close the tape. */
void trace_shutdown(void);

/* Record a source-line event. Emitted by OP_LINE. */
void trace_line(int line);

/* Record a name-keyed assignment. The slot is captured by value
 * (NaN-boxed union, POD), so this is safe to call from any opcode
 * handler immediately before or after the store. Phase 1 records
 * numbers verbatim; non-numerics get a type marker only. */
void trace_assign(const char *name, EigsSlot value);

/* Forward decl; Value is fully defined in eigenscript.h. */
struct Value;

/* Record a nondeterministic builtin return value (random*, time, env_get,
 * etc.). The order of these records on the tape is the replay-determinism
 * substrate: on Phase 3 replay, each nondet call returns the recorded
 * value instead of re-invoking the underlying source. */
void trace_nondet_value(const char *fn, struct Value *v);

/* Phase 3.0a — `prev of x` query.
 *
 * Returns 1 and fills *out when the name has had at least two distinct
 * assignments recorded. Returns 0 when the name is unknown or has only
 * ever been assigned once. The map is fed by trace_assign and lives
 * independently of g_trace_enabled — `prev` is a language-level
 * interrogative, not a debug-tape feature.
 *
 * `name` must be the process-global interned pointer (matches what the
 * chunk's const_interns slot holds for the same identifier). */
int trace_query_prev(const char *interned_name, EigsSlot *out);

/* Phase 3.1 — `<kw> is x at <line>` and `prev of x at <line>`.
 *
 * Walks the per-name history (line, value) backward and returns the
 * value last bound to `name` at or before `line`. Returns 1 + fills
 * *out on hit, 0 on miss.
 *
 * `kind` mirrors the interrogative encoding:
 *   0 (what), 6 (prev) → return historical slot
 *   2 (when)            → return assignment count up to that line (as immediate num)
 *   1 (who)             → return binding name (string)
 *   others              → unsupported, returns 0
 */
int trace_query_at(int kind, const char *interned_name, int line, EigsSlot *out);

#endif /* EIGENSCRIPT_TRACE_H */
