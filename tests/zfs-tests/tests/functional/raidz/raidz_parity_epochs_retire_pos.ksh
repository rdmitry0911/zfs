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

# Copyright (c) 2026, Dmitry R.
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
#	Only a complete strong-allocation census may raise claimed tolerance.
#	Retained weak roots, checkpoints and future weaker epochs prevent it.
#
# STRATEGY:
#	Create a nonallocating RAIDZ target beside an existing mirror, switch
#	it to parity 2 before allowing any allocation, and write real data.
#	Keep a snapshot and clone, prove full coverage and certify parity 2.
#	Read all retained data after a cold import with two target children gone.
#	A later weaker epoch must lower tolerance. A separate pool with weak
#	snapshots and clones must refuse certification after rewriting the head.
#

verify_runnable "global"

typeset -r BASEDIR=$TEST_BASE_DIR/raidz_pe_retirement.$$
typeset -r POOL=${TESTPOOL}_pe_retirement
typeset -r DATA=$POOL/data
typeset -r DIR=$BASEDIR/vdevs
typeset -r MNT=$BASEDIR/mnt
typeset -a disks
typeset start out

function cleanup
{
	if poolexists $POOL; then
		zpool export $POOL
	fi
	for i in 0 1; do
		[[ -f $BASEDIR/away/v$i ]] && mv $BASEDIR/away/v$i $DIR/
	done
	if [[ -d $DIR ]]; then
		zpool import -N -d $DIR $POOL 2>/dev/null
		poolexists $POOL && destroy_pool $POOL
	fi
	! poolexists $POOL && rm -rf $BASEDIR
}

function parity_is
{
	typeset value
	value=$(zpool get -Hp -o value parity $POOL $1) || \
	    log_fail "cannot get target parity"
	[[ $value == $2 ]] || log_fail "target parity $value, expected $2"
}

log_onexit cleanup
log_assert "a retirement certificate requires complete strong coverage"
log_must mkdir -p $DIR $MNT $BASEDIR/away
log_must dd if=/dev/urandom of=$BASEDIR/expected bs=128k count=64

# The helper fails if the target has made any pre-epoch allocation.
log_must raidz_epoch_test retirement-create $DIR $POOL
log_must eval "zhack -d $DIR raidz_epochs $POOL 1 \
    0:5:1 next:5:2 > $BASEDIR/table.out"
start=$(awk '{for (i=1; i<=NF; i++) if ($i ~ /^[0-9]+:5:2$/) \
    {split($i, a, ":"); print a[1]}}' $BASEDIR/table.out)
[[ $start == +([0-9]) ]] && (( start > 0 )) || log_fail "no epoch boundary"
log_must zpool import -N -d $DIR $POOL
# libzpool creates a pool root without a ZPL filesystem. Use a native child.
log_must zfs set mountpoint=none $POOL
log_must zfs create -o mountpoint=$MNT -o atime=off $DATA
log_must ismounted $DATA
log_must zpool set allocating=on $POOL raidz1-1
log_must zpool set allocating=off $POOL mirror-0
log_must cp $BASEDIR/expected $MNT/payload
log_must zfs snapshot $DATA@strong
log_must zfs clone -o mountpoint=$BASEDIR/clone $DATA@strong $POOL/clone
sync_pool $POOL
parity_is raidz1-1 1

# The census must not run against a kernel-imported pool.
log_mustnot zhack -d $DIR raidz_retire -w $POOL 1 2
log_must zpool checkpoint $POOL
log_must zpool export $POOL
log_mustnot zhack -d $DIR raidz_retire -w $POOL 1 2
log_must zpool import -d $DIR $POOL
log_must zpool checkpoint -d $POOL
log_must zpool wait -t discard $POOL
# Flush deferred metadata frees before demanding complete allocated coverage.
for i in 1 2 3; do
	log_must zpool sync -f $POOL
done
log_must zpool export $POOL
log_must zpool import -N -o readonly=on -d $DIR $POOL
log_mustnot zhack -d $DIR raidz_retire -w $POOL 1 2
log_must zpool export $POOL

# A dry run must leave both the claim and feature activation unchanged.
out=$(zhack -d $DIR raidz_retire $POOL 1 2 2>&1) || \
	log_fail "dry census failed: $out"
log_note "$out"
[[ $out == *"weak=0 uncovered=0"* ]] || log_fail "incomplete census: $out"
log_must zpool import -d $DIR $POOL
parity_is raidz1-1 1
[[ $(get_pool_prop feature@raidz_epoch_retirement $POOL) == enabled ]] || \
	log_fail "dry run activated retirement"
log_must zpool export $POOL

log_must mkdir -p $BASEDIR/before-cert
for disk in $DIR/v* $DIR/m*; do
	log_must cp -p $disk $BASEDIR/before-cert/
done
log_must eval "zhack -d $DIR raidz_retire -w $POOL 1 2 \
    > $BASEDIR/commit.out"
log_must grep 'retirement committed' $BASEDIR/commit.out
log_must zpool import -d $DIR $POOL
parity_is raidz1-1 2
[[ $(get_pool_prop feature@raidz_epoch_retirement $POOL) == active ]] || \
	log_fail "certificate did not activate the write compatibility gate"
log_must cmp $BASEDIR/expected $MNT/payload
log_must zpool export $POOL
out=$(raidz_epoch_test crash $DIR $POOL) || log_fail "crash import: $out"
log_note "$out"
[[ $out == *" first="[1-9]* ]] || log_fail "no crash point: $out"

log_must mv $DIR/v0 $DIR/v1 $BASEDIR/away/
log_must zpool import -o readonly=on -d $DIR $POOL
parity_is raidz1-1 2
log_must cmp $BASEDIR/expected $MNT/payload
log_must cmp $BASEDIR/expected $MNT/.zfs/snapshot/strong/payload
log_must cmp $BASEDIR/expected $BASEDIR/clone/payload
log_must_busy zpool export $POOL
log_must zpool import -d $DIR $POOL
parity_is raidz1-1 2
log_must cmp $BASEDIR/expected $MNT/payload
log_must zpool export $POOL
log_must mv $BASEDIR/away/v0 $BASEDIR/away/v1 $DIR/

# Appending a weaker epoch must override the earlier certificate immediately.
log_must zhack -d $DIR raidz_epochs $POOL 1 \
    0:5:1 $start:5:2 next:5:1
log_must zpool import -d $DIR $POOL
parity_is raidz1-1 1
log_must zpool export $POOL
log_mustnot zhack -d $DIR raidz_retire -w $POOL 1 2

# A selected MOS predating the atomic commit must retain the old claim.
# Restore a complete, exported pre-certificate image, including its labels.
for disk in $BASEDIR/before-cert/*; do
	log_must cp -p $disk $DIR/
done
log_must mv $DIR/v0 $DIR/v1 $BASEDIR/away/
log_must eval "cksum $DIR/[vm]* > $BASEDIR/before-refusal.cksum"
log_mustnot zpool import -N -d $DIR $POOL
log_mustnot zpool import -N -o readonly=on -d $DIR $POOL
log_must eval "cksum $DIR/[vm]* > $BASEDIR/after-refusal.cksum"
log_must cmp $BASEDIR/before-refusal.cksum $BASEDIR/after-refusal.cksum
log_must mv $BASEDIR/away/v0 $BASEDIR/away/v1 $DIR/
log_must zpool import -d $DIR $POOL
parity_is raidz1-1 1
[[ $(get_pool_prop feature@raidz_epoch_retirement $POOL) == enabled ]] || \
	log_fail "pre-certificate MOS retained feature activation"
log_must cmp $BASEDIR/expected $MNT/payload
log_must zpool export $POOL
log_must zpool import -N -d $DIR $POOL
log_must zpool destroy $POOL
log_must rm -rf $DIR

# Rewriting the head cannot retire weak snapshot, clone or MOS allocations.
log_must mkdir -p $DIR
for i in 0 1 2 3 4; do
	disks[i]=$DIR/v$i
	log_must truncate -s 128M ${disks[i]}
done
log_must zpool create -f -o cachefile=none -O mountpoint=$MNT \
    -O compression=off $POOL raidz1 ${disks[@]}
log_must cp $BASEDIR/expected $MNT/payload
log_must zfs snapshot $POOL@weak
log_must zfs clone -o mountpoint=$BASEDIR/clone $POOL@weak $POOL/clone
log_must zpool export $POOL
log_must zhack -d $DIR raidz_epochs $POOL 0 0:5:1 next:5:2
log_must zpool import -d $DIR $POOL
log_must zfs rewrite -P $MNT/payload
log_must zpool scrub -w $POOL
parity_is raidz1-0 1
log_must cmp $BASEDIR/expected $MNT/.zfs/snapshot/weak/payload
log_must cmp $BASEDIR/expected $BASEDIR/clone/payload
log_must zpool export $POOL
log_mustnot eval "zhack -d $DIR raidz_retire -w $POOL 0 2 \
    > $BASEDIR/refused.out 2>&1"
log_must grep -E 'weak=[1-9][0-9]*' $BASEDIR/refused.out
log_must zpool import -N -d $DIR $POOL
parity_is raidz1-0 1
log_must zpool destroy $POOL
log_pass "only complete strong coverage raises the claimed failure tolerance"
