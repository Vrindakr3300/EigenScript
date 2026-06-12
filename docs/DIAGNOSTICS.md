# EigenScript Diagnostics

EigenScript reports errors with source line numbers and consistent
formatting. This document defines the error contract. The behaviors
here are enforced by the suite (`run_all_tests.sh` EM1–EM23 and
`examples/errors/`).

## Error Categories

| Category | Format | Exit Code | Behavior |
|----------|--------|-----------|----------|
| Syntax error | `Syntax error line N: ...` | 1 | Tokenizer problem. Accumulates all errors, aborts before execution. |
| Parse error | `Parse error line N: ...` | 1 | Parser problem. Accumulates all errors, aborts before execution. |
| Runtime error (uncaught) | `Error line N: ...` + stack trace | 1 | Type mismatch, undefined variable, index out of bounds, non-callable, uncaught `throw`. Prints to stderr and **halts** — no statement after the error runs. |
| Runtime error (caught) | bound to the `catch` variable | — | Recoverable with `try`/`catch`; execution continues in the handler. |
| Warning | `Warning line N: ...` | 0 | Recoverable issue (division by zero yields `0`). Prints to stderr, continues execution. |
| Assertion | `ASSERT FAIL: ...` | 1 | Intentional termination via `assert`. |

Parse errors prevent execution entirely — the program never runs if
parsing fails. Warnings allow execution to continue; **uncaught runtime
errors do not** — they halt with exit 1 so scripts fail loudly for
callers, Makefiles, and CI.

## Stack traces

An uncaught runtime error (or uncaught `throw`) prints the call stack
to stderr after the error line — every frame between the failure and
the top level, innermost first:

```
Error line 6: index 99 out of range (list length 2)
  at inner (line 6)
  at middle (line 8)
  at <module> (line 9)
```

Caught errors print nothing; the handler decides.

## What `catch` binds

- A **runtime error** binds its message string
  (`"Error line 2: cannot apply '-' to list and num"`).
- A **thrown value** binds unchanged: `throw of {"kind": "validation"}`
  gives the catch variable that dict — match on fields instead of
  substring-searching a message. Thrown strings bind as strings.

## Exit Codes

| Situation | Exit Code |
|-----------|-----------|
| Successful execution | 0 |
| File not found | 1 |
| Syntax / parse errors | 1 |
| Uncaught runtime error or `throw` | 1 |
| Assertion failure | 1 |
| Errors caught by `try`/`catch` | 0 |
| Warnings only | 0 |

## Examples

### Parse error — aborts before execution
```
$ cat bad.eigs
if x > 0
    print of x

$ eigenscript bad.eigs
Parse error line 1: expected ':', got newline
1 parse error(s) — aborting
$ echo $?
1
```

### Uncaught runtime error — halts with a trace
```
$ cat type_err.eigs
x is [1, 2, 3]
y is x - 5
print of "reached"

$ eigenscript type_err.eigs
Error line 2: cannot apply '-' to list and num
  at <module> (line 2)
$ echo $?
1
```
(`"reached"` does **not** print.)

### Caught runtime error — execution continues
```
$ cat caught.eigs
try:
    y is [1] - 5
catch e:
    print of "recovered"
print of "reached"

$ eigenscript caught.eigs
recovered
reached
$ echo $?
0
```

### Warning — returns a fallback value and continues
```
$ cat div_zero.eigs
result is 10 / 0
print of result

$ eigenscript div_zero.eigs
Warning line 1: division by zero
0
$ echo $?
0
```

## Runtime Error Types

| Error | Trigger | Example |
|-------|---------|---------|
| `cannot apply 'OP' to T1 and T2` | Binary operator on incompatible types | `[1,2] - 5` |
| `undefined variable 'NAME'` | Using a name that hasn't been assigned | `print of x` (never defined) |
| `index N out of range (list length M)` | List/string/buffer index outside bounds | `items[10]` on a 3-element list |
| `index must be an integer, got X` | Fractional index (use `floor of`) | `items[2.5]` |
| `'for' requires a list, got T` | For loop over non-list value | `for i in 42:` |
| `cannot call T (not a function)` | Using `of` with a non-function | `5 of 10` |
| `cannot negate T` | Unary minus on non-number | `-"hello"` |
| `cannot index T` | Indexing a non-list/non-string | `42[0]` |
| `import: module 'M' not found (tried lib/M.eigs and M.eigs)` | Missing stdlib/user module | `import nope` |
| `division by zero` | Dividing or modulo by zero | `10 / 0` (warning, returns 0) |

Numeric overflow and invalid math domains are not diagnostics. They are
handled by the finite-number invariant: `NaN` -> `0`, overflow/Inf ->
`+/-1e308`, and selected functions clamp their domains.

## Builtin Errors

Builtin functions report errors without line numbers (the error is in the
builtin's domain, not at a specific source location):

```
Type error: json_decode requires a string, got num
load_file: cannot read 'missing.eigs'
```

## Informational Messages

Diagnostic messages use bracketed prefixes and are not errors:

```
[load_file] Loading lib/math.eigs (1301 bytes)
```

## Editor diagnostics

The LSP server (`make lsp` → `src/eigenlsp`) surfaces the first syntax
or parse error of each open document as a
`textDocument/publishDiagnostics` squiggle at the failing line
(`tests/test_lsp.py` pins the protocol behavior). For batch static
checks, `eigenscript --lint file.eigs` runs the linter and
`eigenscript --fmt` the formatter.
