#!/bin/sh
# Builds the sink against photon-server/canp and runs it. Works from any directory.
here=$(cd "$(dirname "$0")" && pwd)
canp="$here/../../photon-server/canp"
rm -f "$here/sink_test"
gcc "$canp/canp.c" "$here/sink.c" -I "$canp" -o "$here/sink_test"
"$here/sink_test"
