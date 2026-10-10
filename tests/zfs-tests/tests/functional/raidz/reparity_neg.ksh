#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# CDDL HEADER START
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# https://opensource.org/license/CDDL-1.0.
#
# CDDL HEADER END
#

#
# Copyright (c) 2026 Dmitry R.
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
#	'zpool reparity' fails closed on unsupported inputs, leaving the pool
#	unchanged. This test verifies, on disposable pools, that:
#	  1. reparity of a non-RAIDZ (mirror) pool is refused,
#	  2. reparity with no pool argument reports a usage error,
#	  3. a refused reparity leaves the pool intact (data + clean scrub) and
#	     does NOT activate the raidz_parity_epochs feature.
#
# STRATEGY:
#	1. Create a mirror pool, fill it, and attempt reparity (must fail).
#	2. Confirm the feature stays disabled/enabled (never active) and data
#	   is intact with a clean scrub.
#	3. Confirm 'zpool reparity' with no argument exits non-zero.
#

verify_runnable "global"

typeset TESTPOOL2=${TESTPOOL}_rn
typeset BASEDIR=$TEST_BASE_DIR/reparity_neg.$$
typeset -a VDEVS

function cleanup
{
	poolexists $TESTPOOL2 && destroy_pool $TESTPOOL2
	rm -rf $BASEDIR
}

log_onexit cleanup
log_assert "zpool reparity fails closed on unsupported inputs"

mkdir -p $BASEDIR
typeset i=0
while (( i < 2 )); do
	VDEVS[$i]=$BASEDIR/vdev$i
	log_must truncate -s 512M ${VDEVS[$i]}
	(( i = i + 1 ))
done

log_must zpool create -f $TESTPOOL2 mirror ${VDEVS[0]} ${VDEVS[1]}
log_must dd if=/dev/urandom of=/$TESTPOOL2/payload bs=1M count=8
log_must sync_pool $TESTPOOL2
typeset SUM=$(cksum /$TESTPOOL2/payload)

# 1. reparity of a non-RAIDZ pool must be refused.
log_mustnot zpool reparity $TESTPOOL2

# 2. a refused reparity must not have activated the feature.
log_mustnot eval \
    "zpool get -H -o value feature@raidz_parity_epochs $TESTPOOL2 | grep -q active"

# 3. data intact + clean scrub after the refusal.
log_must eval "test \"$(cksum /$TESTPOOL2/payload)\" = \"$SUM\""
log_must zpool scrub -w $TESTPOOL2
log_must check_pool_status $TESTPOOL2 "errors" "No known data errors"

# 4. missing pool argument is a usage error.
log_mustnot zpool reparity

log_pass "zpool reparity refused unsupported inputs and left the pool intact"
