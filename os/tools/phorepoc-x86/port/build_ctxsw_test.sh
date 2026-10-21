#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

# build_ctxsw_test.sh -- IN-CONTAINER: build + run the amd64 context-switch unit
# test (the core of TScheduler::Reschedule). Hosted Linux binary, so it actually
# runs cooperatively without a board/IDT. Run inside the phorepoc-x86 image:
#   docker run --rm -v <repo>:/repo phorepoc-x86 \
#     bash /repo/tools/phorepoc-x86/port/build_ctxsw_test.sh
set -eu
cd /repo/tools/phorepoc-x86/port
gcc -m64 -O0 -g -Wall -o /tmp/ctxsw_test ctxsw_test.c x64_ctxsw.S
/tmp/ctxsw_test
