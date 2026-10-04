#!/usr/bin/env bash
# Verify the C client and the Python sync agent can share one state directory
# without truncating each other: C writes device leaves, Python writes synced
# leaves; each must see the other's data after re-open.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMPD="$(mktemp -d)"
trap 'rm -rf "$TMPD"' EXIT

BUILD="$TMPD/interop"
gcc -std=c11 -Wall -Wextra -Werror -I"$ROOT/src" "$ROOT/tests/interop_writer.c" \
    "$ROOT/src/jsonutil.c" "$ROOT/src/geometry.c" "$ROOT/src/config.c" \
    "$ROOT/src/migrate.c" -o "$BUILD"

"$BUILD" "$TMPD/state" c-init 2>/dev/null
PYTHONPATH="$ROOT/sync" python3 - "$TMPD/state" <<'PY'
import sys
from synckit.store import StateStore
d = sys.argv[1]
s = StateStore(d, device_id="dev-1")
assert s.get("window.x") == 2100, s.get("window.x")
assert s.get("displays.fingerprint") == "1920x1080@0,0|1920x1080@1920,0"
s.set_synced("theme.accent", "#cafe00")
s.set_synced("ui.language", "de")
assert "window.x" not in s.pending_changes()
assert s.pending_changes()["theme.accent"]["value"] == "#cafe00"
print("python saw C device leaves; wrote synced leaves")
PY
"$BUILD" "$TMPD/state" c-verify 2>/dev/null

echo "C<->Python state interop OK"
