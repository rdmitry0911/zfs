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
#	The offline reshape toolkit ('zhack snap_bpr') re-encodes
#	snapshot-referenced blocks at the new parity level, so a raidz1->2
#	reparity can COMMIT while snapshots are retained. This test verifies,
#	on a disposable pool, that:
#	  1. after raidz_epochs + snap_bpr, reparity COMMITs at parity 2 with
#	     a snapshot present,
#	  2. both live and snapshotted data are byte-for-byte intact,
#	  3. the pool (with the snapshot) survives the loss of two devices,
#	  4. destroying the snapshot succeeds and leaves 'freeing' at 0.
#
# STRATEGY:
#	1. Create a raidz1 pool, fill a dataset, snapshot it.
#	2. Export; write a parity-epoch table (parity 2 from txg+2) and run
#	   snap_bpr to rewrite the snapshot's block pointers.
#	3. Import and run 'zpool reparity'; confirm COMMIT at parity 2.
#	4. Verify data + snapshot integrity and 2-disk survival.
#	5. Destroy the snapshot and confirm freeing settles to 0.
#

verify_runnable "global"

typeset TESTPOOL2=${TESTPOOL}_rs
typeset BASEDIR=$TEST_BASE_DIR/reparity_snap.$$
typeset HIDEDIR=$BASEDIR/hidden
typeset -a VDEVS
typeset -i ndev=5

function cleanup
{
	poolexists $TESTPOOL2 && destroy_pool $TESTPOOL2
	rm -rf $BASEDIR
}

log_onexit cleanup
log_assert "snap_bpr enables snapshot-preserving raidz1->2 reparity"

mkdir -p $BASEDIR $HIDEDIR
typeset i=0
while (( i < ndev )); do
	VDEVS[$i]=$BASEDIR/vdev$i
	log_must truncate -s 512M ${VDEVS[$i]}
	(( i = i + 1 ))
done

log_must zpool create -f $TESTPOOL2 raidz1 ${VDEVS[@]}
log_must zfs create -o recordsize=1M $TESTPOOL2/data
for f in a b c d; do
	log_must dd if=/dev/urandom of=/$TESTPOOL2/data/$f bs=1M count=8
done
log_must sync_pool $TESTPOOL2
typeset SUMS=$BASEDIR/sums
log_must eval "cksum /$TESTPOOL2/data/* > $SUMS"
log_must zfs snapshot $TESTPOOL2/data@s

# Offline: install a parity-epoch table (parity 2 starting txg+2) and rewrite
# the snapshot-referenced block pointers to the new parity.
log_must zpool export $TESTPOOL2
typeset TXG=$(zdb -l ${VDEVS[0]} | grep -m1 -oE 'txg: [0-9]+' | grep -oE '[0-9]+')
typeset T=$(( TXG + 2 ))
log_must zhack -d $BASEDIR raidz_epochs $TESTPOOL2 0 0:5:1 $T:5:2
log_must eval "zhack -d $BASEDIR snap_bpr $TESTPOOL2 > $BASEDIR/bpr 2>&1"
log_must grep -qE "rewritten=[1-9][0-9]* " $BASEDIR/bpr
log_must zpool import -d $BASEDIR $TESTPOOL2

# Commit the reshape (snapshot is retained).
log_must eval "zpool reparity $TESTPOOL2 > $BASEDIR/out 2>&1"
log_must grep -q "COMMITTED at parity 2" $BASEDIR/out

# Live + snapshot data intact, clean scrub.
log_must eval "cksum /$TESTPOOL2/data/* > $BASEDIR/sums.after"
log_must diff $SUMS $BASEDIR/sums.after
log_must zpool scrub -w $TESTPOOL2
log_must check_pool_status $TESTPOOL2 "errors" "No known data errors"

# Survive the loss of two devices with the snapshot present.
log_must zpool export $TESTPOOL2
log_must mv ${VDEVS[0]} ${VDEVS[1]} $HIDEDIR/
log_must zpool import -d $BASEDIR $TESTPOOL2
log_must eval "cksum /$TESTPOOL2/data/* > $BASEDIR/sums.degraded"
log_must diff $SUMS $BASEDIR/sums.degraded
log_must mv $HIDEDIR/vdev0 $HIDEDIR/vdev1 $BASEDIR/

# Destroy the snapshot and confirm freeing settles to 0.
log_must zfs destroy $TESTPOOL2/data@s
log_must sync_pool $TESTPOOL2
log_must eval "test \"$(zpool list -Hp -o freeing $TESTPOOL2)\" = \"0\""

log_pass "snap_bpr enabled snapshot-preserving reparity; snapshot destroyed cleanly"
