#!/bin/sh
rm -f test/source_test
gcc canp.c ./test/source.c -o test/source_test -lm
./test/source_test
