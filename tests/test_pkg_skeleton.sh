#!/usr/bin/env bash
# Phase 1a smoke test for the `--pkg` tool: dispatcher, help, add (manifest
# write only — git is Phase 1b), and list. Each in a fresh tmp dir so the
# tool's cwd-relative I/O is hermetic.
set -euo pipefail

EIGS="${EIGENSCRIPT:-./eigenscript}"
# Make EIGS an absolute path so we can cd around freely.
EIGS=$(realpath "$EIGS")

TMP=$(mktemp -d)
trap "rm -rf '$TMP'" EXIT

cd "$TMP"

# ---- help ----
HELP_OUT=$("$EIGS" --pkg help 2>&1)
if ! echo "$HELP_OUT" | grep -q "Subcommands:"; then
    echo "  FAIL: --pkg help missing 'Subcommands:'"
    echo "$HELP_OUT" | head -10
    exit 1
fi
echo "  PASS: --pkg help prints usage"

# ---- list on empty dir ----
LIST_EMPTY=$("$EIGS" --pkg list 2>&1)
if ! echo "$LIST_EMPTY" | grep -q "No dependencies"; then
    echo "  FAIL: --pkg list on empty dir didn't say 'No dependencies'"
    echo "$LIST_EMPTY" | head -5
    exit 1
fi
echo "  PASS: --pkg list reports no deps on a fresh dir"

# ---- add writes a manifest, list reflects it ----
"$EIGS" --pkg add vecmath https://github.com/alice/vecmath v1.0.0 >/dev/null 2>&1
if [ ! -f eigs.json ]; then
    echo "  FAIL: --pkg add did not write eigs.json"
    exit 1
fi
if ! grep -q '"vecmath"' eigs.json; then
    echo "  FAIL: eigs.json missing vecmath entry"
    cat eigs.json
    exit 1
fi
if ! grep -q '"v1.0.0"' eigs.json; then
    echo "  FAIL: eigs.json missing v1.0.0 tag"
    cat eigs.json
    exit 1
fi
echo "  PASS: --pkg add writes manifest entry"

LIST_AFTER=$("$EIGS" --pkg list 2>&1)
if ! echo "$LIST_AFTER" | grep -q "1 dependency"; then
    echo "  FAIL: --pkg list count wrong"
    echo "$LIST_AFTER"
    exit 1
fi
if ! echo "$LIST_AFTER" | grep -q "vecmath  https://github.com/alice/vecmath  v1.0.0"; then
    echo "  FAIL: --pkg list missing dep line"
    echo "$LIST_AFTER"
    exit 1
fi
echo "  PASS: --pkg list shows the added dep"

# ---- second add → 2 deps ----
"$EIGS" --pkg add greeting https://example.com/greeting v0.2.0 >/dev/null 2>&1
LIST_TWO=$("$EIGS" --pkg list 2>&1)
if ! echo "$LIST_TWO" | grep -q "2 dependencies"; then
    echo "  FAIL: --pkg list count not pluralized after second add"
    echo "$LIST_TWO"
    exit 1
fi
echo "  PASS: --pkg list pluralizes after second add"

# ---- bogus subcommand → nonzero exit ----
if "$EIGS" --pkg bogus_subcmd >/dev/null 2>&1; then
    echo "  FAIL: --pkg with bad subcommand should have exited nonzero"
    exit 1
fi
echo "  PASS: --pkg bogus subcommand exits nonzero"
