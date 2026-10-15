#!/bin/bash
# compare_logs.sh <expected> <actual>: diff two WM logs, ignoring header comments
# (lines starting with "# " other than the "# step" markers). Exit 0 and print MATCH on equality.
strip() { awk '!/^# / || /^# step /' "$1"; }
if diff -u <(strip "$1") <(strip "$2"); then echo "MATCH: $(strip "$1" | wc -l | tr -d ' ') lines"; else exit 1; fi
