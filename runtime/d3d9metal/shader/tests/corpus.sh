#!/bin/sh
# Corpus run: every unique shader blob in the game's .smt containers (EP1 and base game), read from a local
# shader corpus extracted from the game's packages (SHADER_CORPUS; a development check, see this package's README, "Build and tests"),
# translated by the C library, compared with tools/shaders/msl.py, and compiled with the system Metal
# compiler. Four chunks, each with its own log and a 540 s limit per process. Outputs stay next to the extraction ($W; derived from game data).
#   [SHADER_CORPUS=<dir>] sh runtime/d3d9metal/shader/tests/corpus.sh build/d3d9metal/shader
B=${1:-build/d3d9metal/shader}
CORPUS=${SHADER_CORPUS:-scratch/shaders}
SMT=$CORPUS/smt_ep1/40212004,$CORPUS/smt_base/40212004
W=$CORPUS/d3d9msl
N=${CHUNKS:-4}
mkdir -p $W
k=1
while [ $k -le $N ]; do
    C=$W/chunk$k
    mkdir -p $C
    start=$(date +%s)
    perl -e 'alarm 540; exec @ARGV' python3 -P runtime/d3d9metal/shader/tests/crosscheck.py $B/d3d9msl_tool $C \
        --corpus $SMT --chunk $k/$N > $C/crosscheck.log 2>&1
    echo "chunk $k/$N crosscheck exit $? ($(( $(date +%s) - start )) s): $(grep -E '^corpus|translations compared' $C/crosscheck.log | tr '\n' ' ')"
    start=$(date +%s)
    perl -e 'alarm 540; exec @ARGV' $B/mslcheck -q $C/c_out/*.metal > $C/compile.log 2>&1
    echo "chunk $k/$N compile exit $? ($(( $(date +%s) - start )) s): $(tail -1 $C/compile.log)"
    k=$((k + 1))
done
