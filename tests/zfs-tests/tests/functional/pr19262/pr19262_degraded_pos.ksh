#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# DESCRIPTION:
#	After the kernel engine certified parity 2 on a raidz1 pool, the
#	pool imports writable with two members hidden, reads everything,
#	accepts writes, refuses with three members hidden, and resilvers
#	the returned members.
#

. $STF_SUITE/tests/functional/pr19262/pr19262.kshlib

verify_runnable "global"

function cleanup
{
	typeset -i i=0
	while (( i < 3 )); do
		[[ -f $PR19262_DIR/aside/v$i ]] && \
		    mv $PR19262_DIR/aside/v$i $PR19262_DIR/v$i
		(( i += 1 ))
	done
	pr19262_cleanup
}

log_onexit cleanup

log_assert "certified parity 2 survives two hidden members in the kernel"

pr19262_make_members 6
log_must zpool create -f -o ashift=12 -O compression=off \
    $PR19262_POOL raidz1 $(pr19262_members 6)
typeset mnt=$(get_prop mountpoint $PR19262_POOL)
pr19262_write_payload $mnt before
pr19262_param_set zfs_pr19262_engine $PR19262_POOL
pr19262_write_payload $mnt after
pr19262_checksums $mnt $PR19262_DIR/ref.md5
pr19262_param_set zfs_pr19262_convert "$PR19262_POOL 2"
pr19262_wait_state converted
log_must eval "pr19262_status | grep -q 'certified=2 '"
log_must zpool export $PR19262_POOL

mkdir -p $PR19262_DIR/aside
log_must mv $PR19262_DIR/v0 $PR19262_DIR/aside/v0
log_must mv $PR19262_DIR/v1 $PR19262_DIR/aside/v1
log_must zpool import -d $PR19262_DIR $PR19262_POOL
log_must eval "zpool status $PR19262_POOL | grep -q DEGRADED"
pr19262_verify_checksums $mnt $PR19262_DIR/ref.md5
pr19262_write_payload $mnt degraded
pr19262_checksums $mnt $PR19262_DIR/ref2.md5
log_must zpool export $PR19262_POOL

log_must mv $PR19262_DIR/v2 $PR19262_DIR/aside/v2
log_mustnot zpool import -d $PR19262_DIR $PR19262_POOL
log_must mv $PR19262_DIR/aside/v2 $PR19262_DIR/v2

log_must mv $PR19262_DIR/aside/v0 $PR19262_DIR/v0
log_must mv $PR19262_DIR/aside/v1 $PR19262_DIR/v1
log_must zpool import -d $PR19262_DIR $PR19262_POOL
log_must zpool wait -t resilver $PR19262_POOL
pr19262_verify_checksums $mnt $PR19262_DIR/ref2.md5
log_must zpool scrub -w $PR19262_POOL
log_mustnot eval "zpool status -v $PR19262_POOL | grep -q 'errors: [^N]'"
log_must eval "zpool status $PR19262_POOL | grep -q ONLINE"

log_pass "certified parity 2 survives two hidden members in the kernel"
