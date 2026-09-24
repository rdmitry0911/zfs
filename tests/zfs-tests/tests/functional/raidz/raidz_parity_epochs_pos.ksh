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
# or https://opensource.org/licenses/CDDL-1.0.
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
# Copyright (c) 2026 RAIDZ Reshape MVP.
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
#	The org.openzfs:raidz_parity_epochs feature persists a parity-epoch
#	table on a top-level RAIDZ vdev. This test verifies, on a disposable
#	pool, that:
#	  1. a new RAIDZ pool enables the feature,
#	  2. the debug writer stores a table and activates the feature,
#	  3. zdb prints the stored table,
#	  4. a structurally damaged table fails import closed,
#	  5. the raidz_ignore_parity_epochs rescue tunable imports it and scrubs clean
#	     with intact data, and
#	  6. restoring a valid table imports cleanly.
#

verify_runnable "global"

typeset TESTPOOL2=${TESTPOOL}_pe
typeset BASEDIR=$TEST_BASE_DIR/raidz_pe.$$
typeset -a VDEVS

function cleanup
{
	set_tunable32 RAIDZ_IGNORE_PARITY_EPOCHS 0
	poolexists $TESTPOOL2 && destroy_pool $TESTPOOL2
	rm -rf $BASEDIR
}

log_onexit cleanup
log_assert "raidz_parity_epochs persists, is inspectable, and is recoverable"

mkdir -p $BASEDIR
for i in 0 1 2; do
	VDEVS[$i]=$BASEDIR/vdev$i
	log_must truncate -s 512M ${VDEVS[$i]}
done

log_must zpool create -f $TESTPOOL2 raidz1 ${VDEVS[0]} ${VDEVS[1]} ${VDEVS[2]}
log_must eval "zpool get -H -o value feature@raidz_parity_epochs $TESTPOOL2 | grep -q enabled"

log_must dd if=/dev/urandom of=/$TESTPOOL2/payload bs=1M count=8
log_must zpool export $TESTPOOL2

# Store a geometry-identical table and activate the feature.
log_must zhack -d $BASEDIR raidz_epochs $TESTPOOL2 0 0:3:1
log_must eval "zdb -e -p $BASEDIR -m $TESTPOOL2 2>/dev/null | grep -q raidz_parity_epochs"

log_must zpool import -d $BASEDIR $TESTPOOL2
log_must eval "zpool get -H -o value feature@raidz_parity_epochs $TESTPOOL2 | grep -q active"
log_must zpool export $TESTPOOL2

# A structurally damaged table (first entry does not cover txg 0) is refused.
log_must zhack -d $BASEDIR raidz_epochs $TESTPOOL2 0 5:3:1
log_mustnot zpool import -d $BASEDIR $TESTPOOL2

# The rescue tunable skips the table so the pool imports and scrubs clean.
log_must set_tunable32 RAIDZ_IGNORE_PARITY_EPOCHS 1
log_must zpool import -d $BASEDIR $TESTPOOL2
log_must zpool scrub -w $TESTPOOL2
log_must check_pool_status $TESTPOOL2 "errors" "No known data errors"
log_must zpool export $TESTPOOL2
log_must set_tunable32 RAIDZ_IGNORE_PARITY_EPOCHS 0

# Repair the damaged table: zhack opens the still-bricked pool with the
# rescue tunable set for libzpool, then overwrites it with a valid table.
log_must zhack -o raidz_ignore_parity_epochs=1 -d $BASEDIR \
    raidz_epochs $TESTPOOL2 0 0:3:1
log_must zpool import -d $BASEDIR $TESTPOOL2

log_pass "raidz_parity_epochs persists, is inspectable, and is recoverable"
