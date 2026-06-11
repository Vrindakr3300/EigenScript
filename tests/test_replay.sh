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

# ---- Strict mode: name mismatch is fatal (exit 3) ----
cat > "$TMPDIR/p_strict.eigs" <<'EOF'
r is random of null
print of r
EOF

cat > "$TMPDIR/strict.tape" <<'EOF'
N monotonic_ns=12345
EOF

# Lenient (default): warns on stderr, uses the recorded value anyway.
LEN_OUT=$(EIGS_REPLAY="$TMPDIR/strict.tape" "$EIGS" "$TMPDIR/p_strict.eigs" 2>/dev/null)
if [ "$LEN_OUT" = "12345" ]; then
    ok "lenient replay: mismatched name still serves recorded value"
else
    fail "lenient replay" "out='$LEN_OUT' expected='12345'"
fi

# Strict: same tape aborts with exit 3 and a diagnostic.
STRICT_ERR=$(EIGS_REPLAY="$TMPDIR/strict.tape" EIGS_REPLAY_STRICT=1 "$EIGS" "$TMPDIR/p_strict.eigs" 2>&1 >/dev/null)
STRICT_RC=$?
if [ "$STRICT_RC" = "3" ] && echo "$STRICT_ERR" | grep -q "replay name mismatch"; then
    ok "strict replay: name mismatch aborts with exit 3"
else
    fail "strict replay" "rc=$STRICT_RC err='$STRICT_ERR'"
fi

# ---- Non-replayable boundary (#148): subprocess/concurrency builtins ----
# Each of the seven builtins below must refuse to run under EIGS_REPLAY and
# raise a catchable runtime error rather than silently re-executing real
# side effects against a tape that has no host-side causal structure.
cat > "$TMPDIR/p_block.eigs" <<'EOF'
caught is 0
try:
    r is exec_capture of [["true"]]
catch e:
    caught is caught + 1
try:
    r is proc_spawn of ["true"]
catch e:
    caught is caught + 1
try:
    r is proc_write of [1, "x"]
catch e:
    caught is caught + 1
try:
    r is proc_read_line of 0
catch e:
    caught is caught + 1
try:
    r is proc_read of [0, 16]
catch e:
    caught is caught + 1
try:
    r is proc_close of 0
catch e:
    caught is caught + 1
try:
    r is proc_wait of 1
catch e:
    caught is caught + 1
ch is channel of null
try:
    r is recv of ch
catch e:
    caught is caught + 1
try:
    r is try_recv of ch
catch e:
    caught is caught + 1
try:
    r is recv_timeout of [ch, 1]
catch e:
    caught is caught + 1
print of caught
EOF

# Empty tape so replay is enabled but every builtin's TAKE returns 0 — the
# replay_blocks guard fires before TAKE on these builtins, so no record is
# consumed. The script just counts how many calls raised.
: > "$TMPDIR/block.tape"
BLOCK_OUT=$(EIGS_REPLAY="$TMPDIR/block.tape" "$EIGS" "$TMPDIR/p_block.eigs" 2>/dev/null)
if [ "$BLOCK_OUT" = "10" ]; then
    ok "replay-block: all 10 subprocess/channel builtins refuse under EIGS_REPLAY (#148)"
else
    fail "replay-block" "caught=$BLOCK_OUT (expected 10)"
fi

# And the error text identifies the boundary so the user can find docs/TRACE.md.
cat > "$TMPDIR/p_block_msg.eigs" <<'EOF'
try:
    r is proc_spawn of ["true"]
catch e:
    print of e
EOF
BLOCK_MSG=$(EIGS_REPLAY="$TMPDIR/block.tape" "$EIGS" "$TMPDIR/p_block_msg.eigs" 2>/dev/null)
if echo "$BLOCK_MSG" | grep -q "not replayable under EIGS_REPLAY" \
   && echo "$BLOCK_MSG" | grep -q "subprocess/concurrency"; then
    ok "replay-block: error text names the replay boundary"
else
    fail "replay-block message" "out='$BLOCK_MSG'"
fi

echo
echo "REPLAY: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ] && exit 0 || exit 1
