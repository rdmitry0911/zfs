#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# DESCRIPTION:
#	The private online-lifetime engine converts a live raidz1 pool to
#	parity 2 in the kernel: data written before and after the engine
#	started (ordinary and synchronous) stays readable, the census ends
#	with no legacy and no weaker backing, the certificate is 2, a scrub
#	is clean, and the pool survives export/import.
#

. $STF_SUITE/tests/functional/pr19262/pr19262.kshlib

verify_runnable "global"

log_onexit pr19262_cleanup

log_assert "online raidz1 -> parity 2 conversion by the kernel engine"

pr19262_make_members 6
log_must zpool create -f -o ashift=12 -O compression=off \
    $PR19262_POOL raidz1 $(pr19262_members 6)
log_must eval "zpool get -H -o value feature@raidz_parity_epochs " \
    "$PR19262_POOL | grep -q enabled"
typeset mnt=$(get_prop mountpoint $PR19262_POOL)

pr19262_write_payload $mnt before
pr19262_param_set zfs_pr19262_engine $PR19262_POOL
log_must eval "pr19262_status | grep -q 'pool=$PR19262_POOL'"
pr19262_write_payload $mnt after
pr19262_checksums $mnt $PR19262_DIR/ref.md5

# The imported engine remains active while the driver's pool name is stale.
# Reattach without initializing its durable catalog a second time.
typeset original_pool=$PR19262_POOL
log_must zpool export $PR19262_POOL
PR19262_POOL=${PR19262_POOL}_resumed
log_must zpool import -d $PR19262_DIR $original_pool $PR19262_POOL
mnt=$(get_prop mountpoint $PR19262_POOL)
pr19262_param_set zfs_pr19262_engine $PR19262_POOL
pr19262_param_set zfs_pr19262_engine $PR19262_POOL
pr19262_verify_checksums $mnt $PR19262_DIR/ref.md5

pr19262_param_set zfs_pr19262_convert "$PR19262_POOL 2"
pr19262_wait_state converted
typeset s=$(pr19262_status)
log_must eval "echo '$s' | grep -q 'legacy=0 '"
log_must eval "echo '$s' | grep -q 'weak=0 '"
log_must eval "echo '$s' | grep -q 'certified=2 '"

pr19262_verify_checksums $mnt $PR19262_DIR/ref.md5
pr19262_write_payload $mnt converted
pr19262_checksums $mnt $PR19262_DIR/ref2.md5
log_must zpool scrub -w $PR19262_POOL
log_mustnot eval "zpool status -v $PR19262_POOL | grep -q 'errors: [^N]'"

log_must zpool export $PR19262_POOL
log_must zpool import -o readonly=on -d $PR19262_DIR $PR19262_POOL
log_mustnot eval "echo '$PR19262_POOL' > " \
    "$PR19262_PARAMS/zfs_pr19262_engine"
pr19262_verify_checksums $mnt $PR19262_DIR/ref2.md5
log_must zpool export $PR19262_POOL
log_must zpool import -d $PR19262_DIR $PR19262_POOL
pr19262_param_set zfs_pr19262_engine $PR19262_POOL
pr19262_verify_checksums $mnt $PR19262_DIR/ref2.md5
log_must zpool scrub -w $PR19262_POOL
log_mustnot eval "zpool status -v $PR19262_POOL | grep -q 'errors: [^N]'"

log_pass "online raidz1 -> parity 2 conversion by the kernel engine"
