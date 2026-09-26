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
#	'zpool reparity --online' promotes a raidz1 pool to raidz2 in place
#	while the pool stays mounted and takes concurrent SYNC writes. This
#	test verifies that the promoted pool tolerates the loss of TWO devices
#	*including a subsequent degraded import* -- exercising the ZIL /
#	spa_check_logs read path that the offline test does not. A regression
#	where combrec reconstructs promoted blocks at the base parity instead
#	of the honored (target) parity makes this degraded import fail with
#	"one or more devices is currently unavailable" even though committed
#	data is intact.
#
# STRATEGY:
#	1. Create a 5-wide raidz1 pool of file vdevs and fill it with data.
#	2. Record the baseline data checksums.
#	3. Start a background concurrent SYNC-write workload on the mounted fs.
#	4. Run 'zpool reparity --online' and confirm it COMMITs at parity 2.
#	5. Stop the workload; verify baseline data intact and a clean scrub.
#	6. Export, hide two vdevs, import degraded, and re-verify the data --
#	   this is the step that regresses if combrec uses the base parity.
#

verify_runnable "global"

typeset TESTPOOL2=${TESTPOOL}_ro
typeset BASEDIR=$TEST_BASE_DIR/reparity_online.$$
typeset HIDEDIR=$BASEDIR/hidden
typeset RUNFLAG=$BASEDIR/writer.run
typeset -a VDEVS
typeset -i ndev=5
typeset WPID=""

function cleanup
{
	rm -f $RUNFLAG
	[[ -n $WPID ]] && kill $WPID 2>/dev/null
	wait 2>/dev/null
	poolexists $TESTPOOL2 && destroy_pool $TESTPOOL2
	rm -rf $BASEDIR
}

log_onexit cleanup
log_assert "zpool reparity --online (concurrent writes) survives 2-disk loss + degraded import"

mkdir -p $BASEDIR $HIDEDIR
typeset i=0
while (( i < ndev )); do
	VDEVS[$i]=$BASEDIR/vdev$i
	log_must truncate -s 512M ${VDEVS[$i]}
	(( i = i + 1 ))
done

log_must zpool create -f $TESTPOOL2 -o ashift=12 raidz1 ${VDEVS[@]}
log_must zfs create -o recordsize=1M $TESTPOOL2/data

# Baseline data + checksums (these must survive the whole test).
for f in a b c d e; do
	log_must dd if=/dev/urandom of=/$TESTPOOL2/data/$f bs=1M count=8
done
log_must sync_pool $TESTPOOL2
typeset SUMS=$BASEDIR/sums
log_must eval "cksum /$TESTPOOL2/data/$f > /dev/null" # touch cache
log_must eval "cksum /$TESTPOOL2/data/[a-e] > $SUMS"

# Concurrent SYNC-write workload: churns the ZIL while reparity runs.
touch $RUNFLAG
(
	while [[ -f $RUNFLAG ]]; do
		dd if=/dev/urandom of=/$TESTPOOL2/data/cw_$RANDOM bs=1M \
		    count=2 status=none 2>/dev/null
		sync
	done
) &
WPID=$!
sleep 1

# Promote raidz1 -> raidz2 in place, online, under load.
log_must eval "zpool reparity --online $TESTPOOL2 > $BASEDIR/out 2>&1"
log_must grep -q "COMMITTED at parity 2" $BASEDIR/out
log_must eval "zpool get -H -o value feature@raidz_parity_epochs $TESTPOOL2 | grep -q active"

# Stop the writer and settle.
rm -f $RUNFLAG
kill $WPID 2>/dev/null; wait 2>/dev/null; WPID=""
log_must sync_pool $TESTPOOL2

# Baseline data intact + clean scrub with all disks.
log_must eval "cksum /$TESTPOOL2/data/[a-e] > $BASEDIR/sums.after"
log_must diff $SUMS $BASEDIR/sums.after
log_must zpool scrub -w $TESTPOOL2
log_must check_pool_status $TESTPOOL2 "errors" "No known data errors"

# The promoted pool must now survive the loss of TWO devices AND a degraded
# import (the ZIL/spa_check_logs read path). This is the regression point.
log_must zpool export $TESTPOOL2
log_must mv ${VDEVS[0]} ${VDEVS[1]} $HIDEDIR/
log_must zpool import -d $BASEDIR $TESTPOOL2
log_must eval "cksum /$TESTPOOL2/data/[a-e] > $BASEDIR/sums.degraded"
log_must diff $SUMS $BASEDIR/sums.degraded

log_pass "online raidz1->2 promote under load survived 2-disk loss + degraded import"
