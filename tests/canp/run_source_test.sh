#!/bin/sh
# Builds the source against photon-server/canp and runs it. Works from any directory.
here=$(cd "$(dirname "$0")" && pwd)
canp="$here/../../photon-server/canp"
rm -f "$here/source_test"
gcc "$canp/canp.c" "$here/source.c" -I "$canp" -o "$here/source_test" -lm
"$here/source_test"
