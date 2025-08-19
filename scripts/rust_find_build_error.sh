#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -e

if [ $# -eq 1 ]; then
    BUILD_DIR="$1"
elif [ $# -eq 0 ] && [ -n "$KBUILD_OUTPUT" ]; then
    BUILD_DIR="$KBUILD_OUTPUT"
else
    echo "Usage: $0 [build output directory]"
    echo
    echo "If no directory is provided, KBUILD_OUTPUT environment variable will be used"
    exit 1
fi

if [ ! -d "$BUILD_DIR" ]; then
    echo "Error: Directory '$BUILD_DIR' does not exist"
    exit 1
fi

find "$BUILD_DIR" -name "*.o" | xargs grep -ahoP "build assertion failed in \K(.+):([0-9]+)" | while read -r line; do
    file=$(echo "$line" | cut -d: -f1)
    line=$(echo "$line" | cut -d: -f2)
    assertion=$(sed -n "${line}s/^[[:space:]]*//p" "$BUILD_DIR/$file")
    echo "$file:$line $assertion"
done
