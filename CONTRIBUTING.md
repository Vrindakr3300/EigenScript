# Contributing to EigenScript

Thanks for your interest in contributing to EigenScript.

## Getting Started

```bash
git clone https://github.com/InauguralSystems/EigenScript.git
cd EigenScript
./build.sh
cd tests && bash run_all_tests.sh
```

Requires only `gcc` — no external dependencies.

## Making Changes

1. Fork the repository
2. Create a branch from `main`
3. Make your changes
4. Run the test suite: `cd tests && bash run_all_tests.sh`
5. Open a pull request

Two gates worth knowing before you push:

- **The suite must also pass under sanitizers** (CI enforces it):
  `make asan && cd tests && ASAN_OPTIONS=detect_leaks=1 bash run_all_tests.sh`.
  The final summary prints a tolerated-leak tally (known closure-cycle
  leaks, see `docs/CLOSURE_CYCLE_GC.md`) — if your change makes that
  number jump, you've introduced a leak.
- **The spec is executable.** Every example in `docs/SPEC.md` and
  `docs/COMPARISON.md` runs in CI and must match its output block
  byte-for-byte. If you change language semantics, update the spec in
  the same PR or CI fails — that's by design. Same for the expected
  messages in `examples/errors/`.

## Code Style

- **C source** (`src/`): 4-space indent, no tabs. Keep functions short. Every builtin gets a signature comment.
- **EigenScript libraries** (`lib/`): Follow the conventions in [docs/STDLIB.md](docs/STDLIB.md) — header block, signature comments, snake_case naming.
- **Tests**: One `.eigs` file per feature area. Tests should print clear pass/fail output.

## What to Contribute

- **Bug fixes** — always welcome.
- **New builtins** — open an issue first to discuss the API.
- **Standard library modules** — see `lib/` for the pattern. New modules should include docs in `docs/STDLIB.md`.
- **Examples** — add to `examples/` with a comment header explaining what the example demonstrates.
- **Documentation** — improvements to `docs/` or `README.md`.

## Reporting Bugs

Use the [bug report template](https://github.com/InauguralSystems/EigenScript/issues/new?template=bug_report.md) and include a minimal `.eigs` reproducer.

For private or non-issue contact, email contact@inauguralsystems.com with one
of these subject prefixes:

- `[SECURITY]` Vulnerabilities or suspected security issues. Do not file public
  GitHub issues for suspected vulnerabilities.
- `[BUG]` Reproducible bugs that are not appropriate for a public issue.
- `[SUPPORT]` Installation, usage, or release questions.
- `[PRESS]` Media, interviews, or public inquiries.
- `[LEGAL]` Licensing or trademark questions.

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
