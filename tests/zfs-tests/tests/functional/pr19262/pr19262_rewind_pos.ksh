#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# DESCRIPTION:
#	After the kernel engine converted a raidz1 pool to parity 2, rewinds
#	obey two engine rules.  A root below the durable floor is refused
#	even read-only (its catalog may name reused backing).  A root above
#	the floor but older than the newest selectable one imports read-only
#	with exactly that txg's data and snapshots, and is refused writable
#	(an older allocation bitmap must not allocate over roots that are
#	still selectable).  The newest root then imports normally with a
#	clean scrub.
#

. $STF_SUITE/tests/functional/pr19262/pr19262.kshlib

verify_runnable "global"

log_onexit pr19262_cleanup

log_assert "rewind imports are bounded by the floor and by newer roots"

typeset -i ROUNDS=4

pr19262_make_members 6
log_must zpool create -f -o ashift=12 -O compression=off \
    $PR19262_POOL raidz1 $(pr19262_members 6)
typeset mnt=$(get_prop mountpoint $PR19262_POOL)

pr19262_write_payload $mnt before
pr19262_param_set zfs_pr19262_engine $PR19262_POOL
pr19262_write_payload $mnt after
pr19262_param_set zfs_pr19262_convert "$PR19262_POOL 2"
pr19262_wait_state converted
log_must eval "pr19262_status | grep -q 'certified=2 '"
typeset floor=$(pr19262_fence $PR19262_POOL)
log_note "durable floor $floor"
log_must test "$floor" -gt 0
log_must test "$(pr19262_floor)" -eq "$floor"

# One file, one snapshot and one reference checksum list per round; the
# pool history gives the txg of each snapshot.
typeset -i k=1
while (( k <= ROUNDS )); do
	log_must dd if=/dev/urandom of=$mnt/round-$k bs=1M count=4 \
	    oflag=sync status=none
	log_must sync
	log_must zfs snapshot $PR19262_POOL@s$k
	pr19262_checksums $mnt $PR19262_DIR/ref$k.md5
	typeset txg=$(pr19262_snapshot_txg $PR19262_POOL $PR19262_POOL@s$k)
	log_note "snapshot s$k txg $txg"
	log_must test "$txg" -gt "$floor"
	eval "typeset -i TXG$k=$txg"
	(( k += 1 ))
done
log_must dd if=/dev/urandom of=$mnt/tail bs=1M count=4 oflag=sync \
    status=none
log_must sync
pr19262_checksums $mnt $PR19262_DIR/ref-newest.md5
log_must zpool export $PR19262_POOL
log_must eval "echo 0 > /proc/spl/kstat/zfs/dbgmsg"

# Below the floor: refused read-only and writable; every older root too.
typeset -i below=floor-1
log_mustnot zpool import -o readonly=on -d $PR19262_DIR -T $below \
    $PR19262_POOL
log_mustnot zpool import -d $PR19262_DIR -T $below $PR19262_POOL
if poolexists $PR19262_POOL; then
	log_fail "pool imported from a root below the floor $floor"
fi
log_must eval "grep -q PR19262_FLOOR_REFUSED /proc/spl/kstat/zfs/dbgmsg"
pr19262_dbgmsg_note "import -T $below"

# Above the floor, older than the newest root: read-only brings back
# exactly that txg; writable is refused while newer roots are selectable.
typeset -i want=TXG2
log_must zpool import -o readonly=on -d $PR19262_DIR -T $want $PR19262_POOL
pr19262_verify_checksums $mnt $PR19262_DIR/ref2.md5
log_must zfs list -H -t snapshot $PR19262_POOL@s1 $PR19262_POOL@s2
log_mustnot zfs list -H -t snapshot $PR19262_POOL@s3
log_must test ! -f $mnt/round-3
log_must test ! -f $mnt/tail
log_must zpool export $PR19262_POOL
log_mustnot zpool import -d $PR19262_DIR -T $want $PR19262_POOL
if poolexists $PR19262_POOL; then
	log_fail "writable import rewound to $want below the newest root"
fi
pr19262_dbgmsg_note "import -T $want"

# The newest root imports writable, with everything written.
log_must zpool import -d $PR19262_DIR $PR19262_POOL
log_must eval "pr19262_status | grep -q 'active=1 '"
pr19262_verify_checksums $mnt $PR19262_DIR/ref-newest.md5
log_must zfs list -H -t snapshot $PR19262_POOL@s4
log_must zpool scrub -w $PR19262_POOL
log_mustnot eval "zpool status -v $PR19262_POOL | grep -q 'errors: [^N]'"
pr19262_write_payload $mnt again
log_must zpool export $PR19262_POOL
log_must zpool import -d $PR19262_DIR $PR19262_POOL
log_must zpool scrub -w $PR19262_POOL
log_mustnot eval "zpool status -v $PR19262_POOL | grep -q 'errors: [^N]'"

log_pass "rewind imports are bounded by the floor and by newer roots"
