#!/bin/sh

set -e
set -u

cp -a debian/tests/test-cmake-variables/ "$AUTOPKGTEST_TMP/"

cd "$AUTOPKGTEST_TMP/test-cmake-variables"

# Run simple cmake code to check that the cmake config is importable
# and print all the variables that these imports set for manual checking

cmake -S . -B build
