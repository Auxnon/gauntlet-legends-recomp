#!/usr/bin/env bash
# Run N64Recomp repeatedly, adding each function it can't translate to the toml's
# ignored list, until it succeeds. Functions in ignored_manual.txt are always ignored. Prints each excluded function with its reason.
set -euo pipefail
cd "$(dirname "$0")/.."

TOML=gauntlet.us.toml
LOG=generated/recomp.log
FAILED=generated/failed.txt
: > "$FAILED"
: > generated/failed_reasons.txt

Write_Ignored() {
    python3 - "$TOML" "$FAILED" <<'EOF'
import re, sys
toml, failed = sys.argv[1], sys.argv[2]
names = [l.split("#")[0].strip() for l in open("ignored_manual.txt")]
names = [n for n in names if n] + [l.strip() for l in open(failed) if l.strip()]
s = open(toml).read()
s = re.sub(r"ignored = \[.*?\]", "ignored = [\n" + "".join(f'    "{n}",\n' for n in names) + "]", s, flags=re.S)
open(toml, "w").write(s)
EOF
}

for _ in $(seq 1 300); do
    Write_Ignored
    if ./tools/N64Recomp/build/N64Recomp "$TOML" > "$LOG" 2>&1; then
        echo "recompiled cleanly with $(wc -l < "$FAILED") ignored functions"
        cat generated/failed_reasons.txt
        exit 0
    fi
    func=$(grep -oE "^Error recompiling [A-Za-z0-9_]+" "$LOG" | awk '{print $3}' | head -1 || true)
    if [ -z "$func" ]; then
        echo "N64Recomp failed outside a function:" >&2
        grep -viE "^(Function|Recompil|Working|\[Info)" "$LOG" | head >&2
        exit 1
    fi
    echo "$func | $(grep -E "Unhandled|No function found" "$LOG" | head -1 || true)" >> generated/failed_reasons.txt
    echo "$func" >> "$FAILED"
done
echo "gave up after 300 runs" >&2
exit 1
