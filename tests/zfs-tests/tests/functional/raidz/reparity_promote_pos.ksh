#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright (c) 2026 Dmitry R.
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
#	'zpool reparity' promotes a raidz1 pool to raidz2 in place, without
#	adding devices, preserving all data. This test verifies, on a
#	disposable pool, that:
#	  1. reparity COMMITs at parity 2 and activates raidz_parity_epochs,
#	  2. all file data is byte-for-byte intact afterward,
#	  3. the pool scrubs with no errors,
#	  4. the promoted pool now tolerates the loss of TWO devices.
#
# STRATEGY:
#	1. Create a 5-wide raidz1 pool of file vdevs and fill it with data.
#	2. Record the data checksums.
#	3. Run 'zpool reparity' and confirm it COMMITs at parity 2.
#	4. Verify feature state, data integrity, and a clean scrub.
#	5. Export, hide two vdevs, import degraded, and re-verify the data.
#

verify_runnable "global"

typeset TESTPOOL2=${TESTPOOL}_rp
typeset BASEDIR=$TEST_BASE_DIR/reparity_promote.$$
typeset HIDEDIR=$BASEDIR/hidden
typeset -a VDEVS
typeset -i ndev=5

function cleanup
{
	poolexists $TESTPOOL2 && destroy_pool $TESTPOOL2
	rm -rf $BASEDIR
}

log_onexit cleanup
log_assert "zpool reparity promotes raidz1->2 in place, preserving data"

mkdir -p $BASEDIR $HIDEDIR
typeset i=0
while (( i < ndev )); do
	VDEVS[$i]=$BASEDIR/vdev$i
	log_must truncate -s 512M ${VDEVS[$i]}
	(( i = i + 1 ))
done

log_must zpool create -f $TESTPOOL2 raidz1 ${VDEVS[@]}
log_must zfs create -o recordsize=1M $TESTPOOL2/data

# Fill with random data and record checksums.
for f in a b c d e; do
	log_must dd if=/dev/urandom of=/$TESTPOOL2/data/$f bs=1M count=8
done
log_must sync_pool $TESTPOOL2
typeset SUMS=$BASEDIR/sums
log_must eval "cksum /$TESTPOOL2/data/* > $SUMS"

# Promote raidz1 -> raidz2 in place.
log_must eval "zpool reparity $TESTPOOL2 > $BASEDIR/out 2>&1"
log_must grep -q "COMMITTED at parity 2" $BASEDIR/out
log_must eval "zpool get -H -o value feature@raidz_parity_epochs $TESTPOOL2 | grep -q active"

# Data intact + clean scrub.
log_must eval "cksum /$TESTPOOL2/data/* > $BASEDIR/sums.after"
log_must diff $SUMS $BASEDIR/sums.after
log_must zpool scrub -w $TESTPOOL2
log_must check_pool_status $TESTPOOL2 "errors" "No known data errors"

# The pool must now survive the loss of two devices (raidz2 redundancy).
log_must zpool export $TESTPOOL2
log_must mv ${VDEVS[0]} ${VDEVS[1]} $HIDEDIR/
log_must zpool import -d $BASEDIR $TESTPOOL2
log_must eval "cksum /$TESTPOOL2/data/* > $BASEDIR/sums.degraded"
log_must diff $SUMS $BASEDIR/sums.degraded

log_pass "zpool reparity promoted raidz1->2 in place; data survived 2-disk loss"
