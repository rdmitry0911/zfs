#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# DESCRIPTION:
#	The kernel engine converts the first raidz1 vdev of a pool that also
#	holds a native raidz1, a log, a special and a dedup vdev.  An
#	encrypted dataset (the engine needs no key), a dedup dataset with
#	repeated data and small-record files on the special class are
#	written before and after the engine starts; the conversion to
#	parity 2 keeps every checksum, the census ends with no legacy and no
#	weaker backing, a scrub is clean, and after export/import the
#	encrypted dataset reads again once its key is loaded.
#

. $STF_SUITE/tests/functional/pr19262/pr19262.kshlib

verify_runnable "global"

log_onexit pr19262_cleanup

log_assert "online conversion beside native raidz, log, special and dedup vdevs"

typeset PASSPHRASE="pr19262 classes passphrase"

pr19262_make_members 6
typeset native=""
typeset -i i=0
while (( i < 6 )); do
	log_must truncate -s 256M $PR19262_DIR/w$i
	native="$native $PR19262_DIR/w$i"
	(( i += 1 ))
done
for v in log0 special0 dedup0; do
	log_must truncate -s 64M $PR19262_DIR/$v
done
log_must zpool create -f -o ashift=12 -O compression=off $PR19262_POOL \
    raidz1 $(pr19262_members 6) raidz1 $native \
    log $PR19262_DIR/log0 special $PR19262_DIR/special0 \
    dedup $PR19262_DIR/dedup0
typeset mnt=$(get_prop mountpoint $PR19262_POOL)

log_must eval "echo '$PASSPHRASE' | zfs create -o encryption=on" \
    "-o keyformat=passphrase -o keylocation=prompt $PR19262_POOL/enc"
log_must zfs create -o dedup=on $PR19262_POOL/dd
log_must zfs create -o recordsize=16k -o special_small_blocks=16k \
    $PR19262_POOL/small

# Ordinary and synchronous data on the root, encrypted data, a repeated
# chunk in the dedup dataset (16 MiB logical, 1 MiB unique) and 16 KiB
# records that the special class takes.
function classes_write
{
	typeset tag=$1
	log_must dd if=/dev/urandom of=$mnt/async-$tag bs=1M count=16 \
	    status=none
	log_must dd if=/dev/urandom of=$mnt/sync-$tag bs=128k count=32 \
	    oflag=sync status=none
	log_must dd if=/dev/urandom of=$mnt/enc/enc-$tag bs=1M count=8 \
	    status=none
	log_must dd if=/dev/urandom of=$PR19262_DIR/chunk-$tag bs=1M count=1 \
	    status=none
	typeset -i j=0
	while (( j < 16 )); do
		log_must eval "cat $PR19262_DIR/chunk-$tag >> $mnt/dd/rep-$tag"
		(( j += 1 ))
	done
	log_must dd if=/dev/urandom of=$mnt/small/small-$tag bs=16k \
	    count=256 oflag=sync status=none
	log_must sync
}

function classes_check_placement
{
	typeset ratio=$(zpool get -H -o value dedupratio $PR19262_POOL)
	ratio=${ratio%x}
	log_note "dedupratio $ratio"
	log_must test $(echo "$ratio >= 4" | bc) -eq 1
	typeset list=$(zpool list -Hp -v $PR19262_POOL)
	for v in special0 dedup0; do
		typeset alloc=$(echo "$list" | awk -v v="$v" \
		    '$1 ~ v"$" { print $3 }')
		log_note "$v alloc=$alloc"
		log_must test -n "$alloc"
		log_must test "$alloc" -gt 0
	done
}

classes_write before
pr19262_param_set zfs_pr19262_engine $PR19262_POOL
log_must eval "pr19262_status | grep -q 'pool=$PR19262_POOL'"
classes_write after
pr19262_checksums $mnt $PR19262_DIR/ref.md5
classes_check_placement

pr19262_param_set zfs_pr19262_convert "$PR19262_POOL 2"
pr19262_wait_state converted
typeset s=$(pr19262_status)
log_must eval "echo '$s' | grep -q 'legacy=0 '"
log_must eval "echo '$s' | grep -q 'weak=0 '"
log_must eval "echo '$s' | grep -q 'certified=2 '"

pr19262_verify_checksums $mnt $PR19262_DIR/ref.md5
classes_write converted
pr19262_checksums $mnt $PR19262_DIR/ref2.md5
classes_check_placement
log_must zpool scrub -w $PR19262_POOL
log_mustnot eval "zpool status -v $PR19262_POOL | grep -q 'errors: [^N]'"

log_must zpool export $PR19262_POOL
log_must zpool import -d $PR19262_DIR $PR19262_POOL
log_must eval "pr19262_status | grep -q 'active=1 '"
log_mustnot zfs mount $PR19262_POOL/enc
log_must eval "echo '$PASSPHRASE' | zfs load-key $PR19262_POOL/enc"
log_must zfs mount $PR19262_POOL/enc
pr19262_verify_checksums $mnt $PR19262_DIR/ref2.md5
classes_check_placement
log_must zpool scrub -w $PR19262_POOL
log_mustnot eval "zpool status -v $PR19262_POOL | grep -q 'errors: [^N]'"

log_pass "online conversion beside native raidz, log, special and dedup vdevs"
