# Test Coverage Analysis (0.13.0, post #148–#159)

Method: `make coverage` (gcov-instrumented minimal build, full
`run_all_tests.sh`), then per-function `gcov -f` and uncovered-block
analysis on the big files, cross-checked against what CI
(`.github/workflows/ci.yml`) actually executes.

## Headline numbers (lines executed, minimal build)

| File | Coverage | Lines | Notes |
|---|---|---|---|
| strbuf.c | 100% | 51 | |
| hash.c | 96.9% | 196 | |
| builtins_tensor.c | 92.5% | 684 | |
| compiler.c | 86.5% | 1329 | |
| trace.c | 81.6% | 517 | |
| main.c | 81.2% | 165 | |
| ext_store.c | 78.2% | 771 | header/array parse paths weak |
| builtins.c | 77.2% | 2875 | see "shadowed builtins" below |
| parser.c | 77.6% | 1186 | `clone_ast` (161 lines) at 0% |
| eigenscript.c | 74.7% | 977 | |
| vm.c | 73.5% | 2359 | fused DMG opcodes at 0% |
| chunk.c | 73.0% | 152 | disassembler at 0% (debug-only, fine) |
| jit.c | 70.2% | 1930 | fused-op emission + stats dump at 0% |
| lexer.c | 69.7% | 423 | detokenizer (87 lines) at 0% |
| **lint.c** | **0%** | 509 | test exists, never runs |
| **fmt.c** | **0%** | 243 | test exists, never runs |

Not measured at all (never compiled into the CI-tested binary):
`ext_http.c` (925), `model_io/infer/train.c` (3163), `ext_gfx.c`
(1212), `ext_db.c` (163), `eigenlsp.c` (1211) — **~6,700 lines of C
with zero CI execution**.

## Proposed areas, in priority order

### 1. Wire in the orphaned fmt/lint suites (cheap, +750 covered lines)

`tests/test_fmt.sh` and `tests/test_lint.sh` exist but are referenced
by neither `run_all_tests.sh` nor CI — `fmt.c` and `lint.c` are the
only two core files at literally 0%.

- `test_fmt.sh` passes 14/14 standalone today.
- `test_lint.sh` is bitrotted: one check fails on a wrong path
  (`Error: cannot read file 'examples/hello.eigs'` — run from the
  wrong cwd), **and the script exits 0 even with failures**, so wiring
  it in as-is would report green forever.

Action: fix the path + exit-code propagation in `test_lint.sh`, then
add both as numbered sections in `run_all_tests.sh`.

### 2. Get the extension builds into CI

CI builds only the minimal variant, so every probe-gated suite
section ([44]–[45] HTTP, [46] DB, [47] model roundtrip/overflow, [62]
audio) **silently skips on every CI run**. The HTTP/model code paths
have real suites — they just never execute where it counts.

Action: add a CI job that does `make http` and runs the suite (the
probe-gating then turns those sections on), plus
`tests/test_http_server.sh`. At minimum compile-check `make gfx` and
the LSP so they can't rot silently. Also: `make jit-smoke` — the only
direct test of the JIT emitter — is not in CI; add it (it's seconds).

### 3. JIT fast paths and bailout helpers: the perf-critical surface is untested

All of these are 0% under the full suite:

- `jit_helper_index_set` (55 lines), `jit_helper_local_idx_dot_get`
  (30), `jit_helper_iter_next` (27), `jit_helper_set_fn_name_local`
  (26), `jit_helper_dot_set` (25), `jit_helper_dot_get` (23),
  `jit_helper_local_dot_set` (20) in vm.c.
- The fused interpreter opcodes `OP_LOCAL_IDX_DOT_GET` /
  `OP_LOCAL_IDX_DOT_SET` (vm.c ~3250–3306) — the DMG hot path.
- Their compiler fusion site (compiler.c ~1640) and JIT emission site
  (jit.c ~2580).

Root cause: these only fire on benchmark-shaped workloads
(`bench_dmg_shape.eigs`, `bench_idxset.eigs`), and the benches are
not part of the suite. Today, Stage-5 correctness (inline ICs, fused
dot ops, OSR, env recycling) is verified only by manually running
benchmarks — exactly the "inline-vs-measure trap" CLAUDE.md warns
about, but for correctness instead of perf.

Action: add a `test_jit_paths.eigs` that exercises the bench shapes
with *checksummed results* and small iteration counts past the JIT
hot threshold: fn-local list-of-dicts with `ctx[0].field` get/set,
buffer index writes in a hot loop, `for`-in loops hot enough to OSR,
fn-name-local set, and shapes that force each helper's slow path
(non-dict element, out-of-range index, rc>1 operands). Consider a
suite mode that runs the three bench files once with tiny N and
verifies output values, and a check that thunks actually compiled
(`EIGS_JIT_STATS=1` grep) so the test can't silently pass interpreted.

### 4. Compiler pre-pass walkers — the #156 family is still half-uncovered

`collect_referenced_names`'s cases for `AST_FUNC`, `AST_LAMBDA`,
`AST_INTERROGATE`, `AST_PREDICATE` (compiler.c ~535–580) never
execute. Issue #156 was precisely a walker that didn't know a node
kind, silently breaking closure capture. The defense is a mechanical
matrix test: for each AST node kind, a closure that captures a name
reachable *only* through that node (inside a match arm, a lambda
body, an interrogate's at-expr, a slice bound, a dict literal value,
…) and asserts the capture works. Cheap to write, directly pins the
known footgun, and any future node kind that misses a walker fails
the matrix instead of shipping.

### 5. Builtin fallbacks shadowed by compiler lowerings

The compiler lowers `dispatch of [t, k, ctx]` (literal 3-list) to
`OP_DISPATCH` (compiler.c:1494), so `builtin_dispatch` — the path
taken for *indirect* calls (`d is dispatch` … `d of [...]`, pipes,
spawn targets) — is at 0%. Any opcode/builtin semantic divergence
(the exact class of bug #155/#158 were) is invisible. Same shadow
pattern for the buffer builtins: `builtin_fill`, `buf_from_list`,
`buf_get`, `buf_len`, `ord`, `sign_extend` are all 0% (the benches
use them; the suite doesn't).

Action: a small test that calls each lowered builtin both directly
and through an indirect reference and asserts identical results,
including error shapes.

### 6. Triage the ~30 orphaned test files

These exist in `tests/` but are never referenced by
`run_all_tests.sh` (so editing them does nothing — actively
misleading for contributors):

`test_lab`, `test_observer_interactions` (would cover the 0%
`set/get_observer_thresholds`), `test_control_flow_interactions`,
`test_scope_semantics`, `test_error_propagation`, `test_deep_nest`,
`test_break_scope`, `test_json_hard`, `test_json_roundtrip`,
`test_split_empty`, `test_split_hard`, `test_copy_into_neg`,
`test_tensor_overflow`, `test_handle_forge`, `test_numerics`,
`test_optimize`, `test_simulation`, `test_experiment`, `test_data`,
`test_linalg`, `test_probability`, and the per-domain STEM files
(`test_biology/chemistry/physics/geometry/calculus/engineering/
earth_science` — likely superseded by `test_stem_accuracy.eigs`).

Action: run each once; wire in the ones that pass and add unique
coverage, delete the superseded ones. Anything kept must be in the
runner — the directory should contain no silent no-ops.

### 7. Smaller targeted gaps

- `clone_ast` (parser.c, 161 lines, 0%): reachable via VAL_FN body
  cloning (eigenscript.c:569) yet never runs — determine whether it's
  dead (delete) or an untested live path like eval-defined functions
  (test it). 161 lines of untested deep-copy logic is leak/UAF
  surface.
- `builtin_build_corpus` (163 lines) + the `tok_base_string`
  detokenizer (lexer.c 117–203): the corpus/tokenizer surface has no
  tests in the minimal build. Belongs with the `make http` CI job
  (item 2) or a dedicated check.
- `builtin_nearest_in_range` / `_all` (172 lines combined, 0%).
- ext_store: `store_read_header` 51%, `store_json_parse_array` 0% —
  no test reopens a store containing JSON arrays or exercises
  corrupt/truncated-header recovery.
- `eigs_json_parse_string` 51% — JSON escape-sequence edges
  (`\u`, invalid escapes, truncation mid-escape).
- Exit-code blindness: most suite sections grep output and ignore
  `rc` (a crash *after* correct output passes — already bit once,
  check [71]). Converting `check`-style sections to also assert
  `rc=0` (where nonzero isn't expected) is a one-shell-function
  change that hardens every section at once.

## What's in good shape

builtins_tensor (92%), hash (97%), strbuf (100%), the 0.13.0 feature
suites (destructuring, slicing, proc streaming, defaults, channels
each have dedicated sections), the error-message section ([16] with
line-number assertions), and the ASan+leaks CI job are genuinely
strong. The temporal/replay layer has dedicated coverage and its
known nondet holes are documented (#148) and fail loudly.
