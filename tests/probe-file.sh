#!/bin/bash
# tests/probe-file.sh - "Diagnose by probing" saved files read back.
#
#   tests/probe-file.sh            check
#   tests/probe-file.sh --update   make the sample's report the one printed now
#
# tests/probe-sample.txt was saved by the window's Save... button (the
# lines up to "----" are read back, the report after them is what the
# window ranked). --probe-load must read it and print that same report:
# then Save and Load agree. A changed atlas or ranking changes the report:
# look at the difference, and when it is meant, run with --update.
# Then the files Load must refuse, and a pin that isn't on the board.
#
# Seconds. AI-generated (Claude Code).
cd "$(dirname "$0")/.."
EXE=./c64-hw-sim.exe
[ -x "$EXE" ] || EXE=./c64-hw-sim
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
SAMPLE=tests/probe-sample.txt

# 1. The sample: its saved report against the one --probe-load prints.
"$EXE" --probe-load "$SAMPLE" 2> "$T/err" | tr -d '\r' | sed -n '/^Board:/,$p' > "$T/now"
sed -n '/^Board:/,$p' "$SAMPLE" | tr -d '\r' > "$T/saved"
if [ "$1" = "--update" ]; then
    { sed -n '1,/^----/p' "$SAMPLE"; echo; sed -n '/^Repair records/p' "$SAMPLE"; cat "$T/now"; } > "$T/new"
    mv "$T/new" "$SAMPLE"
    echo "updated $SAMPLE's report ($(wc -l < "$T/now") lines)"
elif ! [ -s "$T/now" ]; then
    echo "FAIL sample: --probe-load printed no report"; cat "$T/err"; fail=1
elif ! diff "$T/saved" "$T/now" > "$T/diff"; then
    echo "FAIL sample: the report isn't the one saved (< saved, > now):"; head -40 "$T/diff"; fail=1
else
    echo "ok   sample: the saved report read back ($(wc -l < "$T/now") lines)"
fi

# 2. Files Load must refuse, each with its reason.
refuse() {     # NAME TEXT-THE-ERROR-HOLDS  (the file in $T/NAME.txt)
    "$EXE" --probe-load "$T/$1.txt" > /dev/null 2> "$T/$1.err"
    rc=$?
    if [ $rc -ne 0 ] && grep -q "$2" "$T/$1.err"; then echo "ok   $1: refused ($(head -1 "$T/$1.err"))"
    else echo "FAIL $1: rc $rc, wanted an error with \"$2\":"; cat "$T/$1.err"; fail=1; fi
}
cat > "$T/no-board.txt" <<'EOF'
# no board line
reading: IC2.26 leg high
EOF
refuse no-board 'no "board:" line'
cat > "$T/bad-reading.txt" <<'EOF'
board: zx48i2
reading: IC2.26 leg high
reading: IC2.6 leg wobbly
EOF
refuse bad-reading 'line 3: "wobbly" isn'
cat > "$T/bad-board.txt" <<'EOF'
board: zx48i9
reading: IC2.26 leg high
EOF
refuse bad-board 'no board "zx48i9"'

# 3. A pin not on the board: left out, said so, the rest still ranked.
cat > "$T/no-pin.txt" <<'EOF'
board: zx48i2
reading: IC2.26 leg high
reading: IC99.1 pad low
EOF
"$EXE" --probe-load "$T/no-pin.txt" > "$T/no-pin.out" 2> "$T/no-pin.err"
rc=$?
if [ $rc -ne 0 ] && grep -q "no pin IC99.1" "$T/no-pin.err" && grep -q "^Readings (1):" "$T/no-pin.out"; then
    echo "ok   no-pin: IC99.1 left out, 1 reading ranked"
else echo "FAIL no-pin: rc $rc"; cat "$T/no-pin.err"; head -12 "$T/no-pin.out"; fail=1; fi

[ $fail -eq 0 ] && echo "All as expected." || echo "Some FAILED."
exit $fail
