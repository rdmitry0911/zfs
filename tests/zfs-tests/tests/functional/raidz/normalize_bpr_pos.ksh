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
#	'zhack normalize_bpr' (P20/RC) normalizes the CODEC (checksum +
#	compressor) of snapshot-pinned level-0 file-DATA blocks that
#	`zfs rewrite -r` cannot touch (immutable in the snapshot), via the
#	shared-block block-pointer-rewrite. This test verifies, on a disposable
#	pool, that:
#	  1. before, normalize_verify FAILS closed (the snapshot's blocks are
#	     the old codec),
#	  2. normalize_bpr rewrites them,
#	  3. after, normalize_verify conforms (all file blocks at the target),
#	  4. the snapshot's data is byte-for-byte intact, scrub is clean, and
#	     the snapshot destroys cleanly.
#
# STRATEGY:
#	1. Create a raidz1 pool, an old-codec (fletcher4/off) dataset, fill it
#	   with random data, snapshot, then remove the head files so the
#	   old-codec blocks are snapshot-private.
#	2. Set the dataset to the target codec (sha256/lz4).
#	3. Export; run 'zhack normalize_verify' (must fail closed) then
#	   'zhack normalize_bpr' then 'zhack normalize_verify' (must pass).
#	4. Import, verify the snapshot data, scrub, and destroy the snapshot.
#

verify_runnable "global"

typeset TESTPOOL2=${TESTPOOL}_nb
typeset BASEDIR=$TEST_BASE_DIR/normalize_bpr.$$
typeset -a VDEVS
typeset -i ndev=5

function cleanup
{
	poolexists $TESTPOOL2 && destroy_pool $TESTPOOL2
	rm -rf $BASEDIR
}

log_onexit cleanup
log_assert "zhack normalize_bpr normalizes snapshot-pinned blocks' codec"

mkdir -p $BASEDIR
typeset -i i=0
while (( i < ndev )); do
	VDEVS[$i]=$BASEDIR/vdev$i
	log_must truncate -s 512M ${VDEVS[$i]}
	(( i = i + 1 ))
done

log_must zpool create -f -o ashift=12 $TESTPOOL2 raidz1 ${VDEVS[@]}
log_must zfs create -o recordsize=128k -o checksum=fletcher4 \
    -o compression=off $TESTPOOL2/data

# Corpus of random data. Record path-less checksums so they compare against the
# snapshot copies later. (fletcher4->sha256 is a real checksum re-encode;
# incompressible data stays OFF under lz4 via the raw fallback, which
# normalize_verify accepts -- so the property under test is the checksum.)
typeset SUMS=$BASEDIR/sums
typeset f
for f in a b c d e; do
	log_must dd if=/dev/urandom of=/$TESTPOOL2/data/$f bs=128k count=16
done
log_must sync_pool $TESTPOOL2
for f in a b c d e; do
	log_must eval "cksum < /$TESTPOOL2/data/$f >> $SUMS"
done

# Snapshot, then drop the head copies so the old-codec blocks are snap-private.
log_must zfs snapshot $TESTPOOL2/data@s1
log_must sync_pool $TESTPOOL2
log_must rm -f /$TESTPOOL2/data/a /$TESTPOOL2/data/b /$TESTPOOL2/data/c \
    /$TESTPOOL2/data/d /$TESTPOOL2/data/e
log_must sync_pool $TESTPOOL2
log_must zfs set checksum=sha256 compression=lz4 $TESTPOOL2/data
log_must sync_pool $TESTPOOL2

# Offline codec relocation via the toolkit.
log_must zfs unmount $TESTPOOL2/data
log_must zpool export $TESTPOOL2
# Before: the snapshot's file blocks are the old codec -> verify fails closed.
log_mustnot zhack -d $BASEDIR normalize_verify $TESTPOOL2 sha256 lz4
log_must eval "zhack -d $BASEDIR normalize_bpr $TESTPOOL2 sha256 lz4 > $BASEDIR/nb 2>&1"
log_must grep -qE "rewritten=[1-9][0-9]* " $BASEDIR/nb
# After: every file-data block is at the target codec.
log_must zhack -d $BASEDIR normalize_verify $TESTPOOL2 sha256 lz4

# Import and verify the snapshot data is byte-for-byte intact.
log_must zpool import -d $BASEDIR $TESTPOOL2
typeset SNAPSUMS=$BASEDIR/snapsums
for f in a b c d e; do
	log_must eval "cksum < /$TESTPOOL2/data/.zfs/snapshot/s1/$f >> $SNAPSUMS"
done
log_must diff $SUMS $SNAPSUMS

log_must zpool scrub -w $TESTPOOL2
log_must check_pool_status $TESTPOOL2 "errors" "No known data errors"
log_must zfs destroy $TESTPOOL2/data@s1
log_must sync_pool $TESTPOOL2

log_pass "zhack normalize_bpr normalized snapshot-pinned blocks; data + pool intact"
