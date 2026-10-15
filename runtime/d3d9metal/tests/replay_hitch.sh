#!/bin/sh
# tests/replay_hitch.sh -- the shader-compile hitch replay, four cases (see tests/replay_hitch.m):
#   before cold, before warm (compile at the first draw), after cold, after warm (asynchronous
#   batches at creation and the pipeline archive). A cold case uses a new salt (code the macOS Metal cache has
#   never seen); the warm case repeats it, as a second launch does. Usage: replay_hitch.sh OUT [scale] [extra]
set -e
OUT=${1:?out dir}; SCALE=${2:-0.25}; EXTRA=${3:-1707}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
W=$OUT/hitch; rm -rf "$W"; mkdir -p "$W/cache"
cp "$ROOT"/build/d3d9metal/shadercache/*.d3d9msl "$W/cache/"
# keep each entry's write time order: cp gives new birth times, so copy in the original order with a pause
python3 -P - "$ROOT/build/d3d9metal/shadercache" "$W/cache" <<'PY'
import os, sys, shutil, time
src, dst = sys.argv[1], sys.argv[2]
for f in os.listdir(dst): os.unlink(os.path.join(dst, f))
names = sorted((os.stat(os.path.join(src, f)).st_birthtime, f) for f in os.listdir(src) if f.endswith('.d3d9msl'))
for _, f in names:
    shutil.copyfile(os.path.join(src, f), os.path.join(dst, f)); time.sleep(0.002)
PY
export SPORE_D3D9MSL_CACHE="$W/cache"
A=$(( $(date +%s) * 7 + $$ )); B=$(( A + 1 ))
run() { echo "== $1"; shift; perl -e 'alarm 590; exec @ARGV' "$OUT/replay_hitch" "$@"; }
run "before, cold" --mode before --salt $A --scale "$SCALE" --extra "$EXTRA"
run "before, warm (the same code again: a second launch)" --mode before --salt $A --scale "$SCALE" --extra "$EXTRA"
run "after, cold" --mode after --salt $B --archive "$W/pso.metalar" --scale "$SCALE" --extra "$EXTRA"
# the stand-ins the cold run added to the translation cache are gone again at a real second launch
# (they were never drawn, so the game would not have written them): remove them
python3 -P - "$W/cache" "$ROOT/build/d3d9metal/shadercache" <<'PY'
import os, sys
d, ref = sys.argv[1], set(os.listdir(sys.argv[2]))
for f in os.listdir(d):
    if f not in ref: os.unlink(os.path.join(d, f))
PY
run "after, warm (the same code again: a second launch, archive from the cold run)" --mode after --salt $B --archive "$W/pso.metalar" --scale "$SCALE" --extra "$EXTRA"
