#!/bin/bash
# Replay tape tests for the eigenscript binary (Item 1: parse_value
# containers). Records a tape under EIGS_TRACE, then re-runs the script
# under EIGS_REPLAY with the underlying nondet source mutated — replayed
# output must match the recording.
#
# Run directly or from run_all_tests.sh. Prints a summary line:
#   REPLAY: N passed, M failed
# Exit code: 0 if all pass, 1 if any fail.

set -u
TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$TESTS_DIR/.." && pwd)/src"
EIGS="$SRC_DIR/eigenscript"

PASS=0
FAIL=0
TMPDIR=$(mktemp -d -t eigs_replay.XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

ok()   { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1${2:+ ($2)}"; FAIL=$((FAIL+1)); }

if [ ! -x "$EIGS" ]; then
    echo "  FAIL: eigenscript binary not found at $EIGS"
    echo "REPLAY: 0 passed, 1 failed"
    exit 1
fi

# ---- List replay (read_bytes returns VAL_LIST of nums) ----
INPUT="$TMPDIR/in.bin"
printf 'ABC' > "$INPUT"

cat > "$TMPDIR/p_list.eigs" <<EOF
print of (read_bytes of "$INPUT")
EOF

TAPE_L="$TMPDIR/list.tape"
REC_L=$(EIGS_TRACE="$TAPE_L" "$EIGS" "$TMPDIR/p_list.eigs" 2>&1)

# Mutate the underlying source — replay must still return the recorded list.
printf 'XYZ' > "$INPUT"
REP_L=$(EIGS_REPLAY="$TAPE_L" "$EIGS" "$TMPDIR/p_list.eigs" 2>&1)

if [ "$REC_L" = "[65, 66, 67]" ] && [ "$REP_L" = "[65, 66, 67]" ]; then
    ok "list replay: read_bytes returns recorded list under EIGS_REPLAY"
else
    fail "list replay" "rec='$REC_L' rep='$REP_L'"
fi

# ---- Buffer replay (read_bytes_buf returns VAL_BUFFER) ----
printf '\x01\x02\x03' > "$INPUT"

cat > "$TMPDIR/p_buf.eigs" <<EOF
b is read_bytes_buf of "$INPUT"
print of b[0]
print of b[1]
print of b[2]
EOF

TAPE_B="$TMPDIR/buf.tape"
REC_B=$(EIGS_TRACE="$TAPE_B" "$EIGS" "$TMPDIR/p_buf.eigs" 2>&1)

printf '\xff\xfe\xfd' > "$INPUT"
REP_B=$(EIGS_REPLAY="$TAPE_B" "$EIGS" "$TMPDIR/p_buf.eigs" 2>&1)

EXPECTED_B=$'1\n2\n3'
if [ "$REC_B" = "$EXPECTED_B" ] && [ "$REP_B" = "$EXPECTED_B" ]; then
    ok "buffer replay: read_bytes_buf restored as VAL_BUFFER (b[…] disambiguator)"
else
    fail "buffer replay" "rec='$REC_B' rep='$REP_B'"
fi

# ---- Dict replay (handcrafted tape — no nondet builtin returns dicts) ----
# trace_replay_take is lenient on name mismatch (warns, uses anyway),
# so we craft the N record under any nondet name and bind it.
cat > "$TMPDIR/p_dict.eigs" <<'EOF'
v is env_get of "EIGS_REPLAY_TEST_KEY_DOES_NOT_EXIST"
print of v
EOF

cat > "$TMPDIR/dict.tape" <<'EOF'
N env_get={"a": 1, "b": "two", "c": null}
EOF

REP_D=$(EIGS_REPLAY="$TMPDIR/dict.tape" "$EIGS" "$TMPDIR/p_dict.eigs" 2>/dev/null)

if [ "$REP_D" = '{"a": 1, "b": "two", "c": null}' ]; then
    ok "dict replay: handcrafted N record materializes as VAL_DICT"
else
    fail "dict replay" "out='$REP_D'"
fi

# ---- Nested replay (list containing dict and buffer) ----
cat > "$TMPDIR/p_nested.eigs" <<'EOF'
v is env_get of "EIGS_REPLAY_TEST_KEY_NESTED"
print of v[0]
print of v[1]["k"]
print of v[2][1]
EOF

# Outer list: [{"k": 42}, {"k": "ok"}, b[10, 20, 30]]
cat > "$TMPDIR/nested.tape" <<'EOF'
N env_get=[{"k": 42}, {"k": "ok"}, b[10, 20, 30]]
EOF

REP_N=$(EIGS_REPLAY="$TMPDIR/nested.tape" "$EIGS" "$TMPDIR/p_nested.eigs" 2>/dev/null)
EXPECTED_N=$'{"k": 42}\nok\n20'

if [ "$REP_N" = "$EXPECTED_N" ]; then
    ok "nested replay: list[dict, dict, buffer] round-trip"
else
    fail "nested replay" "out='$REP_N' expected='$EXPECTED_N'"
fi

echo
echo "REPLAY: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ] && exit 0 || exit 1
