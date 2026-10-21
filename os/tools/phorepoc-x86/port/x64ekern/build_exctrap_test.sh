#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

# build_exctrap_test.sh -- HOST driver: build + run the hosted TExcTrap save/resume test in the
# phorepoc-x86 image (both -O0 and -O2: register allocation differs, both must pass).
set -eu
cd "$(dirname "$0")/../../../.."
MSYS_NO_PATHCONV=1 "${DOCKER:-docker.exe}" run --rm -v "$(pwd -W 2>/dev/null || pwd)":/repo phorepoc-x86 bash -c '
  cd /repo/tools/phorepoc-x86/port/x64ekern
  for o in -O0 -O2; do
    echo "== $o"; gcc -m64 $o -masm=intel -fno-pie -no-pie -Wall -o /tmp/exctrap_test exctrap_test.c && /tmp/exctrap_test
  done'
