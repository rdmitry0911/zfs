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
#	AVAILABLE and statfs estimate future payload after an epoch change
#	without changing the stable space charged to quotas and reservations.
#
# STRATEGY:
#	Change parity or width, fill a quota/refquota, and compare the actual
#	payload with the estimate. Preserve the data, USED and REFERENCED across
#	export/import. Exercise ancestor quota and both reservation variants.
#

verify_runnable "global"

typeset -r BASEDIR=$TEST_BASE_DIR/raidz_pe_available.$$
typeset -r POOL=${TESTPOOL}_pe_available
typeset -a disks

function cleanup
{
	poolexists $POOL && destroy_pool $POOL
	rm -rf $BASEDIR
}

function available_case
{
	typeset label=$1 base=$2 width=$3 parity=$4 limit=$5 reservation=$6
	typeset copies=${7:-1} special=${8:-no} recordsize=${9:-128K}
	typeset quota=${10:-32M} small=0
	typeset dir=$BASEDIR/$label mnt=$BASEDIR/$label/mnt
	typeset ds=$POOL/data before_used before_ref checksum
	typeset -i estimate actual dfavail diff used ref

	log_must mkdir -p $mnt
	for i in 0 1 2 3 4; do
		disks[i]=$dir/v$i
		log_must truncate -s 128M ${disks[i]}
	done
	if [[ $special == yes ]]; then
		log_must truncate -s 128M $dir/s0 $dir/s1
		log_must zpool create -f -o cachefile=none -O mountpoint=none \
		    -O compression=off $POOL raidz$base ${disks[@]} \
		    special mirror $dir/s0 $dir/s1
		small=$recordsize
	else
		log_must zpool create -f -o cachefile=none -O mountpoint=none \
		    -O compression=off $POOL raidz$base ${disks[@]}
	fi
	log_must zpool export $POOL
	log_must zhack -d $dir raidz_epochs $POOL 0 \
	    0:5:$base next:$width:$parity
	log_must zpool import -d $dir $POOL
	log_must zfs create -o mountpoint=$mnt -o recordsize=$recordsize \
	    -o copies=$copies -o special_small_blocks=$small $ds
	if [[ $limit == ancestor ]]; then
		log_must zfs set quota=$quota $POOL
	else
		log_must zfs set $limit=$quota $ds
	fi
	if [[ $reservation != none ]]; then
		log_must zfs set $reservation=16M $ds
	fi
	sync_pool $POOL
	estimate=$(get_prop available $ds)
	dfavail=$(df -Pk $mnt | awk 'NR == 2 {print $4 * 1024}')
	(( estimate > 0 )) || log_fail "$label: no available capacity"
	diff=$(( estimate - dfavail ))
	(( diff >= -1048576 && diff <= 1048576 )) || \
	    log_fail "$label: available=$estimate statfs=$dfavail"

	# The error must be quota exhaustion, not an unrelated dd failure.
	log_mustnot eval "LC_ALL=C dd if=/dev/urandom of=$mnt/payload \
	    bs=128k count=1536 conv=fsync 2> $dir/dd.err"
	log_must grep -Ei 'quota exceeded' $dir/dd.err
	sync_pool $POOL
	actual=$(wc -c < $mnt/payload)
	(( actual > 0 )) || log_fail "$label: no payload written"
	diff=$(( estimate - actual ))
	(( diff < 0 )) && diff=$(( -diff ))
	log_note "$label: estimated=$estimate written=$actual error=$diff"
	(( diff * 100 <= actual * 3 )) || \
	    log_fail "$label: available estimate differs by more than 3%"

	before_used=$(get_prop used $ds)
	before_ref=$(get_prop referenced $ds)
	checksum=$(cksum $mnt/payload)
	log_must zpool export $POOL
	log_must zpool import -d $dir $POOL
	used=$(get_prop used $ds)
	ref=$(get_prop referenced $ds)
	[[ $used == $before_used && $ref == $before_ref ]] || \
	    log_fail "$label: accounting changed after import"
	[[ $(cksum $mnt/payload) == $checksum ]] || \
	    log_fail "$label: data changed after import"
	log_must zpool scrub -w $POOL
	log_must check_pool_status $POOL "errors" "No known data errors"
	log_must zpool destroy $POOL
	log_must rm -rf $dir
}

log_onexit cleanup
log_assert "available payload follows the current allocation epoch"
available_case raise 1 5 2 quota none
available_case lower 2 5 1 quota none
available_case narrow 1 4 1 quota none
available_case refquota 1 5 2 refquota none
available_case ancestor 1 5 2 ancestor none
available_case reservation 1 5 2 quota reservation
available_case refreservation 1 5 2 refquota refreservation
available_case copies 1 5 2 quota none 2
available_case small_records 1 5 2 quota none 1 no 16K
available_case special_small 1 5 2 quota none 1 yes
available_case special_fallback 1 5 2 quota none 1 yes 128K 128M
log_pass "epoch-aware available capacity preserves stable accounting"
