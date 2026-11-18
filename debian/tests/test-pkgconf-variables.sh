#!/bin/bash

set -e
set -u

cd "$AUTOPKGTEST_TMP"

error=0

for pkg in pyside6 shiboken6; do
    echo "========== $pkg =========="
    echo "path       = $(pkg-config --path "$pkg")"
    echo "modversion = $(pkg-config --modversion "$pkg")"
    echo

    echo "-- variables --"
    for v in $(pkg-config --print-variables "$pkg"); do
        val=$(pkg-config --variable="$v" "$pkg")
        echo -n "$v = $val"
        if [ ! -d "$val" -a ! -f "$val" ]; then
            echo "  <== ERROR: doesn't exist"
            error=1
        else
            echo
        fi
    done
    echo

    echo "-- cflags --"
    pkg-config --cflags "$pkg"
    echo

    echo "-- libs --"
    pkg-config --libs "$pkg"
    echo
done

if [ $error -gt 0 ]; then
  echo "Exiting with error: errors detected above"
  exit 1
fi
