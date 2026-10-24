#!/usr/bin/env bash
# Builds the remote log test binary for this machine: harness/remotelog/build/remotelog-test
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$here/build"
${CC:-cc} -std=gnu11 -g -O0 -fno-omit-frame-pointer -Wall -Wextra -D_GNU_SOURCE -DSG_IPLOG=1 \
  -I"$here/../../tweak/Sources" "$here/main.c" "$here/../../tweak/Sources/Core/SGRemoteLogCore.c" \
  -lpthread -o "$here/build/remotelog-test"
echo "$here/build/remotelog-test"
