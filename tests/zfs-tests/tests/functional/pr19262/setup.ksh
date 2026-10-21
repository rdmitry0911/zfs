#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Private pr19262 online-lifetime engine tests: no shared setup, every
# test builds its own file-backed pool.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

log_pass
