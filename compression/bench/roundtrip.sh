#!/bin/sh
# Round-trip correctness: every file must decompress bit-identically, under
# every feature combination, at several memory levels.
set -e
PRISM=${1:-./prism}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0; n=0

# A spread of awkward inputs plus whatever the caller passes in.
: > "$TMP/empty"
printf 'a' > "$TMP/one"
head -c 100000 /dev/urandom > "$TMP/random"
python3 -c "import sys;sys.stdout.write('ab'*50000)" > "$TMP/periodic"
python3 -c "
import sys,struct
for i in range(20000): sys.stdout.buffer.write(struct.pack('<IHHi', i, i%7, 300-i%11, -i))
" > "$TMP/records"
head -c 300000 "$0" > "$TMP/self" 2>/dev/null || cp "$0" "$TMP/self"

FILES="$TMP/empty $TMP/one $TMP/random $TMP/periodic $TMP/records $TMP/self"
shift 2>/dev/null || true
FILES="$FILES $*"

for f in $FILES; do
  [ -f "$f" ] || continue
  for opt in "-3" "-6" "-3 --no-stride" "-3 --no-regime" "-3 --no-line" "-3 --no-match" "-3 --baseline"; do
    n=$((n+1))
    $PRISM c $opt -q "$f" "$TMP/c.prz" 2>/dev/null
    $PRISM d -q "$TMP/c.prz" "$TMP/d.out" 2>/dev/null
    if cmp -s "$f" "$TMP/d.out"; then
      printf '  ok   %-28s %-18s %8s -> %s\n' "$(basename "$f")" "$opt" "$(wc -c < "$f")" "$(wc -c < "$TMP/c.prz")"
    else
      printf '  FAIL %-28s %-18s\n' "$(basename "$f")" "$opt"; fail=$((fail+1))
    fi
  done
done
echo
if [ "$fail" -eq 0 ]; then echo "roundtrip: $n/$n cases passed"; else echo "roundtrip: $fail of $n cases FAILED"; exit 1; fi
