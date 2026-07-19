#!/bin/sh
rm -f test/sink_test
gcc canp.c ./test/sink.c -o test/sink_test
./test/sink_test
