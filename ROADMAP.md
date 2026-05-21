# Roadmap

Current version: **0.11.0**

## Completed Sprint: Language Completeness (0.11.0)

Goal: fix all remaining test failures, make the bytecode VM fully
compatible with the tree-walker's behavior, improve error handling.

### Week 1: Error handling & try/catch

- [ ] Fix try/catch type error and nested re-throw (2 failures)
- [ ] Fix error propagation across function calls (test_error_propagation)
- [ ] Fix error message content matching (test_error_extra)
- [ ] Fix import error handling (test_import_errors, 2 failures)
- [ ] Fix GC error-path behavioral differences (test_gc, 14/34)
- [ ] Fix /dev/null stdin handling (test_file_io, 1 failure)

### Week 2: Segfaults & deep recursion

- [ ] Fix test_coverage_v2 crash (large test suite, likely deep recursion or stack overflow in computation)
- [ ] Fix test_geometry crash (geometry library, likely recursive algorithm)
- [ ] Investigate and fix the recursion_guard test
- [ ] Chunk reference counting (closures hold refs to compiled chunks that are currently leaked)

### Week 3: Observer & correctness

- [ ] Fix observer interaction: observation age tracking (test_observer_interactions)
- [ ] Fix tensor observer edge case (test_coverage_gaps CG48)
- [ ] Fix softmax length check (test_softmax_guard)
- [ ] Fix closure with break escaping caller loop (test_control_flow_interactions CF20)
- [ ] Fix STEM library failures (test_lab, test_linalg — 1 each)

### Week 4: Polish & release

- [ ] Run full test suite through the test runner (not just individual files)
- [ ] Verify EigenMiniSat, DMG, EigenGauntlet all pass
- [ ] Update test count in docs (was 1030, now should reflect bytecode VM)
- [ ] Performance comparison: bytecode VM vs tree-walker baseline
- [ ] Release 0.11.0

## After 0.11.0

### Performance (0.12.0)

- [ ] Dict field inline caching (OP_DOT_GET with cached slot offset)
- [ ] Extend GET_LOCAL/SET_LOCAL to all local variables (not just params)
- [ ] In-place numeric mutation in SET_LOCAL/SET_NAME for refcount-1 values
- [ ] Benchmark DMG target: 1+ MHz (currently 0.16 MHz)

### Language features (0.13.0)

- [ ] Destructuring assignment (`[a, b] is [1, 2]`)
- [ ] Streaming subprocess I/O (stdin pipe, unbuffered stdout)
- [ ] String slicing (`s[1:3]`)
- [ ] Default parameter values

### Ecosystem

- [ ] Package manager / module registry
- [ ] More STEM modules (graph theory, regression, numerical PDEs)
- [ ] WASM compilation target
- [ ] Foreign function interface (FFI) for calling C libraries

### Long-term

- [ ] Self-hosting compiler (EigenScript written in EigenScript)
- [ ] GitHub Linguist submission (requires 2K+ .eigs files across repos)
- [ ] Public release

## Completed

### 0.10.0 (2026-05-21)

- [x] Bytecode VM — replaced AST tree-walker with compiled bytecode + computed-goto dispatch
- [x] Non-recursive function calls — no C stack recursion, 4096 frame depth
- [x] Stack-local optimization — GET_LOCAL/SET_LOCAL for function params
- [x] Observer stall detection in VM loops (OP_LOOP_STALL_CHECK)
- [x] list_truncate, list_remove_at, sort_by builtins

### 0.9.3

- [x] Computational geometry library — 60+ functions, convex hull, transforms
- [x] Lab data collection framework
- [x] 49 stdlib modules, 14 STEM
- [x] 15 STEM simulation examples

### 0.9.2

- [x] 12 STEM standard library modules
- [x] SDL2 audio extension
- [x] Code formatter and linter
- [x] Tidepool game near-parity

### 0.9.1

- [x] Language server protocol (LSP) — eigenlsp, VS Code extension
- [x] Hashing builtins — SHA-256, MD5, HMAC-SHA256

### 0.9.0

- [x] UI toolkit — 44 widgets, 3 themes, flex layout, animation
- [x] Real concurrency — spawn/thread_join/channels
- [x] EigenStore embedded database
- [x] Graphical debugger with observer-aware inspection
- [x] 817 tests

### 0.8.0

- [x] Reference counting GC, unobserved blocks, SDL2 graphics
- [x] Bitwise operations, terminal I/O, arena allocator
- [x] Fuzz testing, security hardening

### 0.7.0

- [x] Pattern matching, pipe operator, lambdas, break/continue
- [x] Regex, import system, EBNF grammar

### 0.6.0

- [x] REPL, dictionaries, closures, f-strings, try/catch, eval

### 0.5.0

- [x] Observer semantics, 121 builtins, 25 stdlib modules
- [x] HTTP, PostgreSQL, transformer extensions
