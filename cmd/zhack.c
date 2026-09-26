// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2011, 2015 by Delphix. All rights reserved.
 * Copyright (c) 2013 Steven Hartland. All rights reserved.
 */

/*
 * zhack is a debugging tool that can write changes to ZFS pool using libzpool
 * for testing purposes. Altering pools with zhack is unsupported and may
 * result in corrupted pools.
 */

#include <zfs_prop.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/dmu.h>
#include <sys/zap.h>
#include <sys/zfs_znode.h>
#include <sys/dsl_synctask.h>
#include <sys/vdev.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_raidz.h>
#include <sys/txg.h>
#include <sys/zfs_sa.h>
#include <sys/zfs_znode.h>
#include <sys/sa_impl.h>
#include <sys/dsl_pool.h>
#include <sys/dmu_traverse.h>
#include <sys/zil.h>
#include <sys/arc.h>
#include <sys/abd.h>
#include <sys/dbuf.h>
#include <sys/dnode.h>
#include <sys/dmu_objset.h>
#include <sys/bpobj.h>
#include <sys/dsl_deadlist.h>
#include <sys/dsl_dir.h>
#include <errno.h>
#include <sys/mmp.h>
#include <sys/fs/zfs.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_pool.h>
#include <sys/zio_checksum.h>
#include <sys/zio_compress.h>
#include <sys/zfeature.h>
#include <sys/dmu_tx.h>
#include <sys/backtrace.h>
#include <zfeature_common.h>
#include <libzutil.h>
#include <sys/metaslab_impl.h>

static importargs_t g_importargs;
static char *g_pool;
static boolean_t g_readonly;
static boolean_t g_dump_dbgmsg;

typedef enum {
	ZHACK_REPAIR_OP_UNKNOWN  = 0,
	ZHACK_REPAIR_OP_CKSUM    = (1 << 0),
	ZHACK_REPAIR_OP_UNDETACH = (1 << 1)
} zhack_repair_op_t;

static __attribute__((noreturn)) void
usage(void)
{
	(void) fprintf(stderr,
	    "Usage: zhack [-o tunable] [-c cachefile] [-d dir] [-G] "
	    "<subcommand> <args> ...\n"
	    "       where <subcommand> <args> is one of the following:\n"
	    "\n");

	(void) fprintf(stderr,
	    "    global options:\n"
	    "    -c <cachefile>   reads config from the given cachefile\n"
	    "    -d <dir>         directory with vdevs for import\n"
	    "    -o var=value...  set global variable to an unsigned "
	    "32-bit integer\n"
	    "    -G               dump zfs_dbgmsg buffer before exiting\n"
	    "\n"
	    "    action idle <pool> [-f] [-t seconds]\n"
	    "        import the pool for a set time then export it\n"
	    "        -t <seconds> sets the time the pool is imported\n"
	    "\n"
	    "    feature stat <pool>\n"
	    "        print information about enabled features\n"
	    "    feature enable [-r] [-d desc] <pool> <feature>\n"
	    "        add a new enabled feature to the pool\n"
	    "        -d <desc> sets the feature's description\n"
	    "        -r set read-only compatible flag for feature\n"
	    "    feature ref [-md] <pool> <feature>\n"
	    "        change the refcount on the given feature\n"
	    "        -d decrease instead of increase the refcount\n"
	    "        -m add the feature to the label if increasing refcount\n"
	    "\n"
	    "    <feature> : should be a feature guid\n"
	    "\n"
	    "    label repair <device>\n"
	    "        repair labels of a specified device according to options\n"
	    "        which may be combined to do their functions in one call\n"
	    "        -c repair corrupted label checksums\n"
	    "        -u restore the label on a detached device\n"
	    "\n"
	    "    <device> : path to vdev\n"
	    "\n"
	    "    mmp reclaim <pool>\n"
	    "        import a pool whose MMP claim cannot reach every mirror\n"
	    "        leg the config still expects, then mark the unreachable\n"
	    "        leaves offline so ordinary imports succeed.  Manual\n"
	    "        recovery: fence the peer first, this cannot see a\n"
	    "        live host whose legs are all invisible from here\n"
	    "\n"
	    "    metaslab leak <pool>\n"
	    "        apply allocation map from zdb to specified pool\n"
	    "\n"
	    "    raidz_epochs <pool> <top-vdev-id> <start:width:parity>...\n"
	    "        DEBUG: write a raidz parity-epoch table to the vdev's\n"
	    "        top-level ZAP verbatim (no validation, so damaged\n"
	    "        tables can be injected) and activate the\n"
	    "        raidz_parity_epochs feature; disposable pools only\n");
	exit(1);
}

static void
dump_debug_buffer(void)
{
	ssize_t ret __attribute__((unused));

	if (!g_dump_dbgmsg)
		return;

	/*
	 * We use write() instead of printf() so that this function
	 * is safe to call from a signal handler.
	 */
	ret = write(STDERR_FILENO, "\n", 1);
	zfs_dbgmsg_print(STDERR_FILENO, "zhack");
}

static void sig_handler(int signo)
{
	struct sigaction action;

	libspl_backtrace(STDERR_FILENO);
	dump_debug_buffer();

	/*
	 * Restore default action and re-raise signal so SIGSEGV and
	 * SIGABRT can trigger a core dump.
	 */
	action.sa_handler = SIG_DFL;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	(void) sigaction(signo, &action, NULL);
	raise(signo);
}

static __attribute__((format(printf, 3, 4))) __attribute__((noreturn)) void
fatal(spa_t *spa, const void *tag, const char *fmt, ...)
{
	va_list ap;

	if (spa != NULL) {
		spa_close(spa, tag);
		(void) spa_export(g_pool, NULL, B_TRUE, B_FALSE);
	}

	va_start(ap, fmt);
	(void) fputs("zhack: ", stderr);
	(void) vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void) fputc('\n', stderr);

	dump_debug_buffer();

	exit(1);
}

static int
space_delta_cb(dmu_object_type_t bonustype, const void *data,
    zfs_file_info_t *zoi)
{
	(void) data, (void) zoi;

	/*
	 * Is it a valid type of object to track?
	 */
	if (bonustype != DMU_OT_ZNODE && bonustype != DMU_OT_SA)
		return (ENOENT);
	(void) fprintf(stderr, "modifying object that needs user accounting");
	abort();
}

/*
 * Target is the dataset whose pool we want to open.
 */
static void
zhack_import(char *target, boolean_t readonly)
{
	nvlist_t *config;
	nvlist_t *props;
	int error;

	kernel_init(readonly ? SPA_MODE_READ :
	    (SPA_MODE_READ | SPA_MODE_WRITE));

	dmu_objset_register_type(DMU_OST_ZFS, space_delta_cb);

	g_readonly = readonly;
	g_importargs.can_be_active = readonly;
	g_pool = strdup(target);

	libpc_handle_t lpch = {
		.lpc_lib_handle = NULL,
		.lpc_ops = &libzpool_config_ops,
		.lpc_printerr = B_TRUE
	};
	error = zpool_find_config(&lpch, target, &config, &g_importargs);
	if (error)
		fatal(NULL, FTAG, "cannot import '%s'", target);

	props = NULL;
	if (readonly) {
		VERIFY0(nvlist_alloc(&props, NV_UNIQUE_NAME, 0));
		VERIFY0(nvlist_add_uint64(props,
		    zpool_prop_to_name(ZPOOL_PROP_READONLY), 1));
	}

	zfeature_checks_disable = B_TRUE;
	error = spa_import(target, config, props,
	    (readonly ? ZFS_IMPORT_SKIP_MMP : ZFS_IMPORT_NORMAL));
	fnvlist_free(config);
	zfeature_checks_disable = B_FALSE;
	if (error == EEXIST)
		error = 0;

	if (error)
		fatal(NULL, FTAG, "can't import '%s': %s", target,
		    strerror(error));
}

static void
zhack_spa_open(char *target, boolean_t readonly, const void *tag, spa_t **spa)
{
	int err;

	zhack_import(target, readonly);

	zfeature_checks_disable = B_TRUE;
	err = spa_open(target, spa, tag);
	zfeature_checks_disable = B_FALSE;

	if (err != 0)
		fatal(*spa, FTAG, "cannot open '%s': %s", target,
		    strerror(err));
	if (spa_version(*spa) < SPA_VERSION_FEATURES) {
		fatal(*spa, FTAG, "'%s' has version %d, features not enabled",
		    target, (int)spa_version(*spa));
	}
}

static void
dump_obj(objset_t *os, uint64_t obj, const char *name)
{
	zap_cursor_t zc;
	zap_attribute_t *za = zap_attribute_long_alloc();

	(void) printf("%s_obj:\n", name);

	for (zap_cursor_init(&zc, os, obj);
	    zap_cursor_retrieve(&zc, za) == 0;
	    zap_cursor_advance(&zc)) {
		if (za->za_integer_length == 8) {
			ASSERT(za->za_num_integers == 1);
			(void) printf("\t%s = %llu\n",
			    za->za_name, (u_longlong_t)za->za_first_integer);
		} else {
			ASSERT(za->za_integer_length == 1);
			char val[1024];
			VERIFY0(zap_lookup(os, obj, za->za_name,
			    1, sizeof (val), val));
			(void) printf("\t%s = %s\n", za->za_name, val);
		}
	}
	zap_cursor_fini(&zc);
	zap_attribute_free(za);
}

static void
dump_mos(spa_t *spa)
{
	nvlist_t *nv = spa->spa_label_features;
	nvpair_t *pair;

	(void) printf("label config:\n");
	for (pair = nvlist_next_nvpair(nv, NULL);
	    pair != NULL;
	    pair = nvlist_next_nvpair(nv, pair)) {
		(void) printf("\t%s\n", nvpair_name(pair));
	}
}

static void
zhack_do_feature_stat(int argc, char **argv)
{
	spa_t *spa;
	objset_t *os;
	char *target;

	argc--;
	argv++;

	if (argc < 1) {
		(void) fprintf(stderr, "error: missing pool name\n");
		usage();
	}
	target = argv[0];

	zhack_spa_open(target, B_TRUE, FTAG, &spa);
	os = spa->spa_meta_objset;

	dump_obj(os, spa->spa_feat_for_read_obj, "for_read");
	dump_obj(os, spa->spa_feat_for_write_obj, "for_write");
	dump_obj(os, spa->spa_feat_desc_obj, "descriptions");
	if (spa_feature_is_active(spa, SPA_FEATURE_ENABLED_TXG)) {
		dump_obj(os, spa->spa_feat_enabled_txg_obj, "enabled_txg");
	}
	dump_mos(spa);

	spa_close(spa, FTAG);
}

static void
zhack_feature_enable_sync(void *arg, dmu_tx_t *tx)
{
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	zfeature_info_t *feature = arg;

	feature_enable_sync(spa, feature, tx);

	spa_history_log_internal(spa, "zhack enable feature", tx,
	    "name=%s flags=%u",
	    feature->fi_guid, feature->fi_flags);
}

static void
zhack_do_feature_enable(int argc, char **argv)
{
	int c;
	char *desc, *target;
	spa_t *spa;
	objset_t *mos;
	zfeature_info_t feature;
	const spa_feature_t nodeps[] = { SPA_FEATURE_NONE };

	/*
	 * Features are not added to the pool's label until their refcounts
	 * are incremented, so fi_mos can just be left as false for now.
	 */
	desc = NULL;
	feature.fi_uname = "zhack";
	feature.fi_flags = 0;
	feature.fi_depends = nodeps;
	feature.fi_feature = SPA_FEATURE_NONE;

	optind = 1;
	while ((c = getopt(argc, argv, "+rd:")) != -1) {
		switch (c) {
		case 'r':
			feature.fi_flags |= ZFEATURE_FLAG_READONLY_COMPAT;
			break;
		case 'd':
			if (desc != NULL)
				free(desc);
			desc = strdup(optarg);
			break;
		default:
			usage();
			break;
		}
	}

	if (desc == NULL)
		desc = strdup("zhack injected");
	feature.fi_desc = desc;

	argc -= optind;
	argv += optind;

	if (argc < 2) {
		(void) fprintf(stderr, "error: missing feature or pool name\n");
		usage();
	}
	target = argv[0];
	feature.fi_guid = argv[1];

	if (!zfeature_is_valid_guid(feature.fi_guid))
		fatal(NULL, FTAG, "invalid feature guid: %s", feature.fi_guid);

	zhack_spa_open(target, B_FALSE, FTAG, &spa);
	mos = spa->spa_meta_objset;

	if (zfeature_is_supported(feature.fi_guid))
		fatal(spa, FTAG, "'%s' is a real feature, will not enable",
		    feature.fi_guid);
	if (0 == zap_contains(mos, spa->spa_feat_desc_obj, feature.fi_guid))
		fatal(spa, FTAG, "feature already enabled: %s",
		    feature.fi_guid);

	VERIFY0(dsl_sync_task(spa_name(spa), NULL,
	    zhack_feature_enable_sync, &feature, 5, ZFS_SPACE_CHECK_NORMAL));

	spa_close(spa, FTAG);

	free(desc);
}

static void
feature_incr_sync(void *arg, dmu_tx_t *tx)
{
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	zfeature_info_t *feature = arg;
	uint64_t refcount;

	mutex_enter(&spa->spa_feat_stats_lock);
	VERIFY0(feature_get_refcount_from_disk(spa, feature, &refcount));
	feature_sync(spa, feature, refcount + 1, tx);
	spa_history_log_internal(spa, "zhack feature incr", tx,
	    "name=%s", feature->fi_guid);
	mutex_exit(&spa->spa_feat_stats_lock);
}

static void
feature_decr_sync(void *arg, dmu_tx_t *tx)
{
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	zfeature_info_t *feature = arg;
	uint64_t refcount;

	mutex_enter(&spa->spa_feat_stats_lock);
	VERIFY0(feature_get_refcount_from_disk(spa, feature, &refcount));
	feature_sync(spa, feature, refcount - 1, tx);
	spa_history_log_internal(spa, "zhack feature decr", tx,
	    "name=%s", feature->fi_guid);
	mutex_exit(&spa->spa_feat_stats_lock);
}

static void
zhack_do_feature_ref(int argc, char **argv)
{
	int c;
	char *target;
	boolean_t decr = B_FALSE;
	spa_t *spa;
	objset_t *mos;
	zfeature_info_t feature;
	const spa_feature_t nodeps[] = { SPA_FEATURE_NONE };

	/*
	 * fi_desc does not matter here because it was written to disk
	 * when the feature was enabled, but we need to properly set the
	 * feature for read or write based on the information we read off
	 * disk later.
	 */
	feature.fi_uname = "zhack";
	feature.fi_flags = 0;
	feature.fi_desc = NULL;
	feature.fi_depends = nodeps;
	feature.fi_feature = SPA_FEATURE_NONE;

	optind = 1;
	while ((c = getopt(argc, argv, "+md")) != -1) {
		switch (c) {
		case 'm':
			feature.fi_flags |= ZFEATURE_FLAG_MOS;
			break;
		case 'd':
			decr = B_TRUE;
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 2) {
		(void) fprintf(stderr, "error: missing feature or pool name\n");
		usage();
	}
	target = argv[0];
	feature.fi_guid = argv[1];

	if (!zfeature_is_valid_guid(feature.fi_guid))
		fatal(NULL, FTAG, "invalid feature guid: %s", feature.fi_guid);

	zhack_spa_open(target, B_FALSE, FTAG, &spa);
	mos = spa->spa_meta_objset;

	if (zfeature_is_supported(feature.fi_guid)) {
		fatal(spa, FTAG,
		    "'%s' is a real feature, will not change refcount",
		    feature.fi_guid);
	}

	if (0 == zap_contains(mos, spa->spa_feat_for_read_obj,
	    feature.fi_guid)) {
		feature.fi_flags &= ~ZFEATURE_FLAG_READONLY_COMPAT;
	} else if (0 == zap_contains(mos, spa->spa_feat_for_write_obj,
	    feature.fi_guid)) {
		feature.fi_flags |= ZFEATURE_FLAG_READONLY_COMPAT;
	} else {
		fatal(spa, FTAG, "feature is not enabled: %s", feature.fi_guid);
	}

	if (decr) {
		uint64_t count;
		if (feature_get_refcount_from_disk(spa, &feature,
		    &count) == 0 && count == 0) {
			fatal(spa, FTAG, "feature refcount already 0: %s",
			    feature.fi_guid);
		}
	}

	VERIFY0(dsl_sync_task(spa_name(spa), NULL,
	    decr ? feature_decr_sync : feature_incr_sync, &feature,
	    5, ZFS_SPACE_CHECK_NORMAL));

	spa_close(spa, FTAG);
}

static int
zhack_do_feature(int argc, char **argv)
{
	char *subcommand;

	argc--;
	argv++;
	if (argc == 0) {
		(void) fprintf(stderr,
		    "error: no feature operation specified\n");
		usage();
	}

	subcommand = argv[0];
	if (strcmp(subcommand, "stat") == 0) {
		zhack_do_feature_stat(argc, argv);
	} else if (strcmp(subcommand, "enable") == 0) {
		zhack_do_feature_enable(argc, argv);
	} else if (strcmp(subcommand, "ref") == 0) {
		zhack_do_feature_ref(argc, argv);
	} else {
		(void) fprintf(stderr, "error: unknown subcommand: %s\n",
		    subcommand);
		usage();
	}

	return (0);
}

static void
zhack_do_action_idle(int argc, char **argv)
{
	spa_t *spa;
	char *target, *tmp;
	int idle_time = 0;
	int c;

	optind = 1;
	while ((c = getopt(argc, argv, "+t:")) != -1) {
		switch (c) {
		case 't':
			idle_time = strtol(optarg, &tmp, 0);
			if (*tmp) {
				(void) fprintf(stderr, "error: time must "
				    "be an integer in seconds: %s\n", tmp);
				usage();
			}
			if (idle_time < 0) {
				(void) fprintf(stderr, "error: time must "
				    "not be negative: %d\n", idle_time);
				usage();
			}
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		(void) fprintf(stderr, "error: missing pool name\n");
		usage();
	}
	target = argv[0];

	zhack_spa_open(target, B_FALSE, FTAG, &spa);

	fprintf(stdout, "Imported pool %s, idle for %d seconds\n",
	    target, idle_time);
	sleep(idle_time);

	spa_close(spa, FTAG);
}

/*
 * Collect the mirror legs this host could not open.  vdev_not_present is set
 * during import for any leaf whose open failed (vdev.c), and a leg in that
 * state is what the relaxed claim forgives, so it is also what has to be
 * marked offline for the ordinary imports which follow to succeed.
 */
static void
zhack_collect_absent(vdev_t *vd, uint64_t *guids, uint_t *n, uint_t max)
{
	/*
	 * Skip the same subtrees mmp_claim_uberblock_sync() skips.  The claim
	 * never counts these, so offlining them buys nothing, and offlining a
	 * log leg would drag in spa_reset_logs().  Pruned at the interior node
	 * because the log flag lives on the top-level vdev.
	 */
	if (vd->vdev_islog || vd->vdev_isspare || vd->vdev_isl2cache ||
	    vd->vdev_ishole || vd->vdev_ops == &vdev_indirect_ops)
		return;

	for (uint64_t c = 0; c < vd->vdev_children; c++)
		zhack_collect_absent(vd->vdev_child[c], guids, n, max);

	if (!vd->vdev_ops->vdev_op_leaf || !vd->vdev_not_present)
		return;

	/*
	 * Take only the legs the relaxed claim can forgive, which are the
	 * direct children of a top-level mirror: the relaxation lives in the
	 * nparity == 0 branch and walks that vdev's children.  A raidz or
	 * draid member is required as parity+1 in aggregate and never demanded
	 * individually, so an absent one does not raise the requirement and
	 * offlining it would be a persistent change that buys nothing.
	 */
	if (vd->vdev_parent != vd->vdev_top ||
	    vdev_get_nparity(vd->vdev_top) != 0)
		return;

	VERIFY3U(*n, <, max);
	guids[(*n)++] = vd->vdev_guid;
}

static int
zhack_do_mmp_reclaim(int argc, char **argv)
{
	spa_t *spa;
	char *target;
	uint64_t *guids;
	uint_t nguids = 0, max;
	int c, failed = 0;

	optind = 1;
	while ((c = getopt(argc, argv, "+")) != -1) {
		switch (c) {
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		(void) fprintf(stderr, "error: missing pool name\n");
		usage();
	}
	target = argv[0];

	/*
	 * Relax the claim for this import only.  The write, the wait and the
	 * re-read are untouched, so a competing importer which shares any
	 * visibility with us is still refused.
	 */
	mmp_claim_relaxed = B_TRUE;
	zhack_spa_open(target, B_FALSE, FTAG, &spa);
	mmp_claim_relaxed = B_FALSE;

	/*
	 * Nothing to recover on a pool without multihost: no claim runs, so an
	 * ordinary import already succeeds with the absent leaves simply
	 * missing.  Offlining them here would be a permanent change to a pool
	 * that never needed this tool.
	 */
	if (!spa_multihost(spa)) {
		(void) fprintf(stderr, "%s: multihost is off, so no uberblock "
		    "claim runs and an ordinary import will succeed; refusing "
		    "to offline anything\n", target);
		spa_close(spa, FTAG);
		return (1);
	}

	/* Takes SCL_VDEV itself, so size the array before we hold it. */
	max = MAX(vdev_count_leaves(spa), 1);
	guids = umem_zalloc(max * sizeof (uint64_t), UMEM_NOFAIL);

	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	zhack_collect_absent(spa->spa_root_vdev, guids, &nguids, max);
	spa_config_exit(spa, SCL_VDEV, FTAG);

	if (nguids == 0) {
		(void) fprintf(stdout, "%s: imported, no absent leaves to "
		    "mark offline\n", target);
	}

	for (uint_t i = 0; i < nguids; i++) {
		int error = vdev_offline(spa, guids[i], 0);

		if (error == 0) {
			(void) fprintf(stdout, "%s: marked absent leaf %llu "
			    "offline\n", target, (u_longlong_t)guids[i]);
			continue;
		}

		failed++;
		if (error == EBUSY) {
			(void) fprintf(stderr, "%s: leaf %llu holds data no "
			    "other leaf has, left online\n", target,
			    (u_longlong_t)guids[i]);
		} else {
			(void) fprintf(stderr, "%s: could not offline leaf "
			    "%llu: %s\n", target, (u_longlong_t)guids[i],
			    strerror(error));
		}
	}

	if (failed != 0) {
		/*
		 * Not "the pool still needs zhack": this run exports cleanly
		 * under our own hostid, so the next import here skips the
		 * activity check entirely.  The cost lands on the next host
		 * to take the pool, whose claim will count the leaves left
		 * online and refuse.
		 */
		(void) fprintf(stderr, "%s: %d absent leaves are still "
		    "online; a later import from another host will count "
		    "them and be refused\n", target, failed);
	}

	umem_free(guids, max * sizeof (uint64_t));
	spa_close(spa, FTAG);

	return (failed == 0 ? 0 : 1);
}

static int
zhack_do_mmp(int argc, char **argv)
{
	char *subcommand;

	argc--;
	argv++;
	if (argc == 0) {
		(void) fprintf(stderr,
		    "error: no mmp operation specified\n");
		usage();
	}

	subcommand = argv[0];
	if (strcmp(subcommand, "reclaim") == 0) {
		return (zhack_do_mmp_reclaim(argc, argv));
	} else {
		(void) fprintf(stderr, "error: unknown subcommand: %s\n",
		    subcommand);
		usage();
	}

	return (0);
}

static int
zhack_do_action(int argc, char **argv)
{
	char *subcommand;

	argc--;
	argv++;
	if (argc == 0) {
		(void) fprintf(stderr,
		    "error: no import operation specified\n");
		usage();
	}

	subcommand = argv[0];
	if (strcmp(subcommand, "idle") == 0) {
		zhack_do_action_idle(argc, argv);
	} else {
		(void) fprintf(stderr, "error: unknown subcommand: %s\n",
		    subcommand);
		usage();
	}

	return (0);
}


static boolean_t
strstarts(const char *a, const char *b)
{
	return (strncmp(a, b, strlen(b)) == 0);
}

static void
metaslab_force_alloc(metaslab_t *msp, uint64_t start, uint64_t size,
    dmu_tx_t *tx)
{
	ASSERT(msp->ms_disabled);
	ASSERT(MUTEX_HELD(&msp->ms_lock));
	uint64_t txg = dmu_tx_get_txg(tx);

	uint64_t off = start;
	while (off < start + size) {
		uint64_t ostart, osize;
		boolean_t found = zfs_range_tree_find_in(msp->ms_allocatable,
		    off, start + size - off, &ostart, &osize);
		if (!found)
			break;
		zfs_range_tree_remove(msp->ms_allocatable, ostart, osize);

		if (zfs_range_tree_is_empty(msp->ms_allocating[txg & TXG_MASK]))
			vdev_dirty(msp->ms_group->mg_vd, VDD_METASLAB, msp,
			    txg);

		zfs_range_tree_add(msp->ms_allocating[txg & TXG_MASK], ostart,
		    osize);
		msp->ms_allocating_total += osize;
		off = ostart + osize;
	}
}

static void
zhack_do_metaslab_leak(int argc, char **argv)
{
	int c;
	char *target;
	spa_t *spa;

	optind = 1;
	boolean_t force = B_FALSE;
	while ((c = getopt(argc, argv, "f")) != -1) {
		switch (c) {
		case 'f':
			force = B_TRUE;
			break;
		default:
			usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		(void) fprintf(stderr, "error: missing pool name\n");
		usage();
	}
	target = argv[0];

	zhack_spa_open(target, B_FALSE, FTAG, &spa);
	spa_config_enter(spa, SCL_VDEV | SCL_ALLOC, FTAG, RW_READER);

	char *line = NULL;
	size_t cap = 0;

	vdev_t *vd = NULL;
	metaslab_t *prev = NULL;
	dmu_tx_t *tx = NULL;
	while (getline(&line, &cap, stdin) > 0) {
		if (strstarts(line, "\tvdev ")) {
			uint64_t vdev_id, ms_shift;
			if (sscanf(line,
			    "\tvdev %10"PRIu64"\t%*s  metaslab shift %4"PRIu64,
			    &vdev_id, &ms_shift) == 1) {
				VERIFY3U(sscanf(line, "\tvdev %"PRIu64
				    "\t  metaslab shift %4"PRIu64,
				    &vdev_id, &ms_shift), ==, 2);
			}
			vd = vdev_lookup_top(spa, vdev_id);
			if (vd == NULL) {
				fprintf(stderr, "error: no such vdev with "
				    "id %"PRIu64"\n", vdev_id);
				break;
			}
			if (tx) {
				dmu_tx_commit(tx);
				mutex_exit(&prev->ms_lock);
				metaslab_enable(prev, B_FALSE, B_FALSE);
				tx = NULL;
				prev = NULL;
			}
			if (vd->vdev_ms_shift != ms_shift) {
				fprintf(stderr, "error: ms_shift mismatch: %"
				    PRIu64" != %"PRIu64"\n", vd->vdev_ms_shift,
				    ms_shift);
				break;
			}
		} else if (strstarts(line, "\tmetaslabs ")) {
			uint64_t ms_count;
			VERIFY3U(sscanf(line, "\tmetaslabs %"PRIu64, &ms_count),
			    ==, 1);
			ASSERT(vd);
			if (!force && vd->vdev_ms_count != ms_count) {
				fprintf(stderr, "error: ms_count mismatch: %"
				    PRIu64" != %"PRIu64"\n", vd->vdev_ms_count,
				    ms_count);
				break;
			}
		} else if (strstarts(line, "ALLOC:")) {
			uint64_t start, size;
			VERIFY3U(sscanf(line, "ALLOC: %"PRIu64" %"PRIu64"\n",
			    &start, &size), ==, 2);

			ASSERT(vd);
			metaslab_t *cur =
			    vd->vdev_ms[start >> vd->vdev_ms_shift];
			if (prev != cur) {
				if (prev) {
					dmu_tx_commit(tx);
					mutex_exit(&prev->ms_lock);
					metaslab_enable(prev, B_FALSE, B_FALSE);
				}
				ASSERT(cur);
				metaslab_disable(cur);
				mutex_enter(&cur->ms_lock);
				metaslab_load(cur);
				prev = cur;
				tx = dmu_tx_create_dd(
				    spa_get_dsl(vd->vdev_spa)->dp_root_dir);
				dmu_tx_assign(tx, DMU_TX_WAIT);
			}

			metaslab_force_alloc(cur, start, size, tx);
		} else {
			continue;
		}
	}
	if (tx) {
		dmu_tx_commit(tx);
		mutex_exit(&prev->ms_lock);
		metaslab_enable(prev, B_FALSE, B_FALSE);
		tx = NULL;
		prev = NULL;
	}
	if (line)
		free(line);

	spa_config_exit(spa, SCL_VDEV | SCL_ALLOC, FTAG);
	spa_close(spa, FTAG);
}

typedef struct zhack_repochs {
	uint64_t re_vdev_id;
	uint64_t re_entries;
	uint64_t *re_table;
} zhack_repochs_t;

static void
zhack_raidz_epochs_sync(void *arg, dmu_tx_t *tx)
{
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	zhack_repochs_t *ra = arg;
	vdev_t *vd = vdev_lookup_top(spa, ra->re_vdev_id);

	if (vd == NULL || vd->vdev_ops != &vdev_raidz_ops)
		fatal(spa, FTAG, "vdev %llu is not a raidz top-level vdev",
		    (u_longlong_t)ra->re_vdev_id);
	if (vd->vdev_top_zap == 0)
		fatal(spa, FTAG, "vdev %llu has no top-level ZAP",
		    (u_longlong_t)ra->re_vdev_id);
	VERIFY0(zap_update(spa->spa_meta_objset, vd->vdev_top_zap,
	    VDEV_TOP_ZAP_RAIDZ_PARITY_EPOCHS, sizeof (uint64_t),
	    ra->re_entries * 3, ra->re_table, tx));
	if (!spa_feature_is_active(spa, SPA_FEATURE_RAIDZ_PARITY_EPOCHS))
		spa_feature_incr(spa, SPA_FEATURE_RAIDZ_PARITY_EPOCHS, tx);
	{
		vdev_raidz_t *vdrz2 = vd->vdev_tsd;
		if (vdrz2->vd_parity_epochs != NULL)
			kmem_free(vdrz2->vd_parity_epochs,
			    vdrz2->vd_parity_epoch_count * 3 *
			    sizeof (uint64_t));
		vdrz2->vd_parity_epochs = kmem_alloc(
		    ra->re_entries * 3 * sizeof (uint64_t), KM_SLEEP);
		for (uint64_t i = 0; i < ra->re_entries * 3; i++)
			vdrz2->vd_parity_epochs[i] = ra->re_table[i];
		vdrz2->vd_parity_epoch_count = ra->re_entries;
		vdev_config_dirty(vd);
	}
	spa_history_log_internal(spa, "zhack raidz_epochs", tx,
	    "vdev=%llu entries=%llu", (u_longlong_t)ra->re_vdev_id,
	    (u_longlong_t)ra->re_entries);
}

/* F5: bounded, resumable, error-surfacing MOS sweep. */
static uint64_t zhack_mos_cursor;
static uint64_t zhack_mos_budget = 256;
static uint64_t zhack_mos_dirtied;
static uint64_t zhack_mos_errors;
static boolean_t zhack_mos_more;

static void
zhack_reparity_mos_sync(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	objset_t *mos = spa->spa_meta_objset;
	uint64_t obj = zhack_mos_cursor;
	uint64_t n = 0;
	zhack_mos_more = B_FALSE;
	while (dmu_object_next(mos, &obj, B_FALSE, 0) == 0) {
		dmu_object_info_t doi;
		if (dmu_object_info(mos, obj, &doi) != 0)
			continue;
		if (doi.doi_type == DMU_OT_SPACE_MAP ||
		    doi.doi_type == DMU_OT_OBJECT_ARRAY)
			continue;
		dmu_buf_t *db;
		if (dmu_bonus_hold(mos, obj, FTAG, &db) == 0) {
			dmu_buf_will_dirty(db, tx);
			dmu_buf_rele(db, FTAG);
		}
		uint64_t bs = doi.doi_data_block_size, cap = doi.doi_max_offset;
		if (cap > (16ULL << 20))
			cap = (16ULL << 20);
		if (bs > 0) {
			for (uint64_t off = 0; off < cap; off += bs) {
				dmu_buf_t *ddb;
				if (dmu_buf_hold(mos, obj, off, FTAG, &ddb,
				    0) == 0) {
					dmu_buf_will_dirty(ddb, tx);
					dmu_buf_rele(ddb, FTAG);
				} else {
					zhack_mos_errors++;
				}
			}
		}
		zhack_mos_dirtied++;
		zhack_mos_cursor = obj;
		if (++n >= zhack_mos_budget) {
			zhack_mos_more = B_TRUE;
			break;
		}
	}
}

static uint64_t
zhack_reparity_objset(objset_t *os)
{
	uint64_t obj = 0, n = 0;
	while (dmu_object_next(os, &obj, B_FALSE, 0) == 0) {
		dmu_object_info_t doi;
		if (dmu_object_info(os, obj, &doi) != 0)
			continue;
		uint64_t bs = doi.doi_data_block_size;
		uint64_t cap = doi.doi_max_offset;
		if (bs == 0 || cap == 0) {
			n++;
			continue;
		}
		/*
		 * Cover EVERY object -- including ZPL-accounted files and
		 * directories -- with dmu_buf_will_dirty (normal write path so
		 * the epoch table selects block at the new parity while
		 * PRESERVING its logical birth, so it does not trip user
		 * accounting (unlike will_dirty). Bounded 4 MiB tx chunks cover
		 * the whole object.
		 */
		uint64_t chunk = 4ULL << 20;
		for (uint64_t base = 0; base < cap; base += chunk) {
			uint64_t end = base + chunk;
			if (end > cap)
				end = cap;
			dmu_tx_t *tx = dmu_tx_create(os);
			dmu_tx_hold_write(tx, obj, base, (int)(end - base));
			if (dmu_tx_assign(tx, DMU_TX_WAIT) != 0) {
				dmu_tx_abort(tx);
				break;
			}
			for (uint64_t off = base; off < end; off += bs) {
				dmu_buf_t *ddb;
				if (dmu_buf_hold(os, obj, off, FTAG, &ddb,
				    0) == 0) {
					dmu_buf_will_dirty(ddb, tx);
					dmu_buf_rele(ddb, FTAG);
				}
			}
			dmu_tx_commit(tx);
		}
		n++;
	}
	return (n);
}

/*
 * Replica of the kernel's zpl_get_file_info (module/zfs/zfs_quota.c): parse
 * uid/gid/gen/project straight from the znode/SA bonus, no mounted ZPL. The
 * symbol is not exported from userland libzpool, so we carry a copy; it lets
 * the sweep re-emit ZPL files/directories with CORRECT user accounting as the
 * added parity column grows each object's used space.
 */
static int
zhack_get_file_info(dmu_object_type_t bonustype, const void *data,
    zfs_file_info_t *zoi)
{
	if (bonustype != DMU_OT_ZNODE && bonustype != DMU_OT_SA)
		return (SET_ERROR(ENOENT));
	zoi->zfi_project = ZFS_DEFAULT_PROJID;
	if (data == NULL)
		return (SET_ERROR(EEXIST));
	if (bonustype == DMU_OT_ZNODE) {
		const znode_phys_t *znp = data;
		zoi->zfi_user = znp->zp_uid;
		zoi->zfi_group = znp->zp_gid;
		zoi->zfi_generation = znp->zp_gen;
		return (0);
	}
	const sa_hdr_phys_t *sap = data;
	if (sap->sa_magic == 0) {
		zoi->zfi_user = 0;
		zoi->zfi_group = 0;
		zoi->zfi_generation = 0;
		return (0);
	}
	sa_hdr_phys_t sa = *sap;
	boolean_t swap = B_FALSE;
	if (sa.sa_magic == BSWAP_32(SA_MAGIC)) {
		sa.sa_magic = SA_MAGIC;
		sa.sa_layout_info = BSWAP_16(sa.sa_layout_info);
		swap = B_TRUE;
	}
	if (sa.sa_magic != SA_MAGIC)
		return (SET_ERROR(EINVAL));
	int hdrsize = sa_hdrsize(&sa);
	if (hdrsize < (int)sizeof (sa_hdr_phys_t))
		return (SET_ERROR(EINVAL));
	uintptr_t data_after_hdr = (uintptr_t)data + hdrsize;
	zoi->zfi_user = *((uint64_t *)(data_after_hdr + SA_UID_OFFSET));
	zoi->zfi_group = *((uint64_t *)(data_after_hdr + SA_GID_OFFSET));
	zoi->zfi_generation = *((uint64_t *)(data_after_hdr + SA_GEN_OFFSET));
	uint64_t flags = *((uint64_t *)(data_after_hdr + SA_FLAGS_OFFSET));
	if (swap)
		flags = BSWAP_64(flags);
	if (flags & ZFS_PROJID)
		zoi->zfi_project =
		    *((uint64_t *)(data_after_hdr + SA_PROJID_OFFSET));
	if (swap) {
		zoi->zfi_user = BSWAP_64(zoi->zfi_user);
		zoi->zfi_group = BSWAP_64(zoi->zfi_group);
		zoi->zfi_project = BSWAP_64(zoi->zfi_project);
		zoi->zfi_generation = BSWAP_64(zoi->zfi_generation);
	}
	return (0);
}

static int
zhack_do_reparity(int argc, char **argv)
{
	char *target;
	spa_t *spa;
	argc--; argv++;
	if (argc < 1)
		usage();
	target = argv[0];
	zhack_spa_open(target, B_FALSE, FTAG, &spa);
	zhack_mos_cursor = 0;
	zhack_mos_dirtied = 0;
	zhack_mos_errors = 0;
	uint64_t zhack_mos_batches = 0;
	do {
		VERIFY0(dsl_sync_task(spa_name(spa), NULL,
		    zhack_reparity_mos_sync, NULL, 128,
		    ZFS_SPACE_CHECK_NORMAL));
		zhack_mos_batches++;
	} while (zhack_mos_more);
	(void) fprintf(stderr, "reparity MOS: dirtied=%llu errors=%llu "
	    "batches=%llu (bounded budget=%llu/txg)\n",
	    (u_longlong_t)zhack_mos_dirtied, (u_longlong_t)zhack_mos_errors,
	    (u_longlong_t)zhack_mos_batches, (u_longlong_t)zhack_mos_budget);
	if (zhack_mos_errors != 0) {
		(void) fprintf(stderr,
		    "reparity: MOS sweep errors; aborting\n");
		spa_close(spa, FTAG);
		return (1);
	}
	/*
	 * The sweep re-emits ZPL objects (files AND directories) with
	 * dmu_buf_will_rewrite. zhack's default ZPL stub aborts on any
	 * ZPL-object dirty; register a REAL file-info callback
	 * (zhack_get_file_info, which parses uid/gid/gen from the znode/SA
	 * bonus with no mounted ZPL) so user accounting stays correct as the
	 * extra parity space is attributed to each object's real owner.
	 */
	dmu_objset_register_type(DMU_OST_ZFS, zhack_get_file_info);
	for (int i = 1; i < argc; i++) {
		objset_t *os;
		if (dmu_objset_own(argv[i], DMU_OST_ZFS, B_FALSE, B_FALSE,
		    FTAG, &os) != 0)
			continue;
		(void) zhack_reparity_objset(os);
		/*
		 * Free any stale intent-log chain (parity-1 ZIL blocks born
		 * before the promotion). The pool is exported/quiesced, so the
		 * ZIL is already committed -- destroying it loses nothing and
		 * leaves an empty header, so no old-parity ZIL block remains in
		 * the tree.
		 */
		zil_destroy(dmu_objset_zil(os), B_FALSE);
		txg_wait_synced(dmu_objset_pool(os), 0);
		dmu_objset_disown(os, B_FALSE, FTAG);
	}
	spa_close(spa, FTAG);
	return (0);
}

typedef struct { uint64_t vdev_guid, parity; } zhack_commit_t;

/*
 * Authoritative epoch state (from the vdev, by GUID) — never operator input.
 */
static const uint64_t *zhack_epochs;
static uint64_t zhack_epoch_count;
static uint64_t zhack_target_parity;
static uint64_t zhack_reparity_residual;
static uint64_t zhack_reparity_visited;
static uint64_t zhack_reparity_embedded;

/* F1: strict base-10 parse; rejects "banana"/""/"0x.."/trailing junk. */
static boolean_t
zhack_strict_u64(const char *s, uint64_t *out)
{
	if (s == NULL || *s == '\0')
		return (B_FALSE);
	char *end = NULL;
	errno = 0;
	unsigned long long v = strtoull(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0')
		return (B_FALSE);
	*out = (uint64_t)v;
	return (B_TRUE);
}

/* F1: per-block parity selected by the AUTHORITATIVE epoch table. */
static uint64_t
zhack_epoch_parity_for(uint64_t birth)
{
	uint64_t parity = zhack_epochs[2];
	for (uint64_t i = 0; i < zhack_epoch_count; i++) {
		if (zhack_epochs[3 * i] > birth)
			break;
		parity = zhack_epochs[3 * i + 2];
	}
	return (parity);
}

static int
zhack_reparity_verify_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	(void) spa; (void) zilog; (void) dnp; (void) arg;
	if (bp == NULL || BP_IS_HOLE(bp))
		return (0);
	if (BP_IS_EMBEDDED(bp)) {
		zhack_reparity_embedded++;
		return (0);
	}
	zhack_reparity_visited++;
	if (zhack_epoch_parity_for(BP_GET_PHYSICAL_BIRTH(bp)) <
	    zhack_target_parity) {
		zhack_reparity_residual++;
		if (zhack_reparity_residual <= 8 && zb != NULL)
			(void) fprintf(stderr,
			    "  residual: objset=%llu obj=%llu "
			    "lvl=%lld blkid=%llu otype=%d birth=%llu\n",
			    (u_longlong_t)zb->zb_objset,
			    (u_longlong_t)zb->zb_object,
			    (longlong_t)zb->zb_level,
			    (u_longlong_t)zb->zb_blkid,
			    (int)BP_GET_TYPE(bp),
			    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(bp));
	}
	return (0);
}

static uint64_t
zhack_epoch_digest(const uint64_t *tbl, uint64_t nent, uint64_t max_parity)
{
	uint64_t h = 1469598103934665603ULL;
	for (uint64_t i = 0; i < nent; i++) {
		if (tbl[3 * i + 2] > max_parity)
			continue;
		for (int k = 0; k < 3; k++) {
			h ^= tbl[3 * i + k];
			h *= 1099511628211ULL;
		}
	}
	return (h);
}

static void
zhack_reparity_commit_sync(void *arg, dmu_tx_t *tx)
{
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	zhack_commit_t *c = arg;
	vdev_t *vd = vdev_lookup_by_guid(spa->spa_root_vdev, c->vdev_guid);
	if (vd == NULL || vd->vdev_ops != &vdev_raidz_ops)
		fatal(spa, FTAG, "vdev guid %llu is not raidz",
		    (u_longlong_t)c->vdev_guid);
	vdev_raidz_t *vdrz = vd->vdev_tsd;
	vdrz->vd_reparity_parity = c->parity;
	vdrz->vd_reparity_completion_txg = dmu_tx_get_txg(tx);
	vdrz->vd_reparity_epoch_digest = zhack_epoch_digest(
	    vdrz->vd_parity_epochs, vdrz->vd_parity_epoch_count, c->parity);
	vdev_config_dirty(vd);
	spa_history_log_internal(spa, "zhack reparity_commit", tx,
	    "vdev_guid=%llu parity=%llu", (u_longlong_t)c->vdev_guid,
	    (u_longlong_t)c->parity);
}

/*
 * usage: zhack reparity_commit <pool> <vdev_guid>
 * Completion is derived from AUTHORITATIVE state: vdev resolved by GUID; target
 * = max parity in its epoch table; census uses each block's table-selected
 * parity; TRAVERSE_PRE (no HARD) so errors are not hidden. No operator
 * target/promo_txg can raise tolerance.
 */
static int
zhack_do_reparity_commit(int argc, char **argv)
{
	spa_t *spa;
	zhack_commit_t c;
	uint64_t vguid;
	argc--; argv++;
	if (argc < 2)
		usage();
	if (!zhack_strict_u64(argv[1], &vguid))
		fatal(NULL, FTAG, "invalid vdev guid: %s", argv[1]);
	zhack_spa_open(argv[0], B_FALSE, FTAG, &spa);
	vdev_t *vd = vdev_lookup_by_guid(spa->spa_root_vdev, vguid);
	if (vd == NULL || vd->vdev_ops != &vdev_raidz_ops)
		fatal(spa, FTAG, "vdev guid %llu is not a raidz vdev",
		    (u_longlong_t)vguid);
	vdev_raidz_t *vdrz = vd->vdev_tsd;
	if (vdrz->vd_parity_epochs == NULL || vdrz->vd_parity_epoch_count == 0)
		fatal(spa, FTAG, "vdev has no parity-epoch table");
	zhack_epochs = vdrz->vd_parity_epochs;
	zhack_epoch_count = vdrz->vd_parity_epoch_count;
	zhack_target_parity = 0;
	for (uint64_t i = 0; i < zhack_epoch_count; i++) {
		uint64_t pp = vdrz->vd_parity_epochs[3 * i + 2];
		if (pp > zhack_target_parity)
			zhack_target_parity = pp;
	}
	if (zhack_target_parity <= vdrz->vd_nparity)
		fatal(spa, FTAG,
		    "epoch table has no parity promotion to commit");
	zhack_reparity_residual = 0;
	zhack_reparity_visited = 0;
	zhack_reparity_embedded = 0;
	int err = traverse_pool(spa, 0, TRAVERSE_PRE,
	    zhack_reparity_verify_cb, NULL);
	(void) fprintf(stderr, "reparity census: visited=%llu embedded=%llu "
	    "residual=%llu traverse_err=%d target_parity=%llu\n",
	    (u_longlong_t)zhack_reparity_visited,
	    (u_longlong_t)zhack_reparity_embedded,
	    (u_longlong_t)zhack_reparity_residual, err,
	    (u_longlong_t)zhack_target_parity);
	if (err != 0 || zhack_reparity_residual != 0 ||
	    zhack_reparity_visited == 0) {
		(void) fprintf(stderr,
		    "reparity_commit REFUSED: traverse_err=%d "
		    "residual=%llu visited=%llu (a skipped/errored subtree "
		    "or an "
		    "empty census cannot certify)\n", err,
		    (u_longlong_t)zhack_reparity_residual,
		    (u_longlong_t)zhack_reparity_visited);
		spa_close(spa, FTAG);
		return (1);
	}
	c.vdev_guid = vd->vdev_guid;
	c.parity = zhack_target_parity;
	VERIFY0(dsl_sync_task(spa_name(spa), NULL, zhack_reparity_commit_sync,
	    &c, 5, ZFS_SPACE_CHECK_NORMAL));
	(void) fprintf(stderr, "reparity_commit: COMMITTED parity=%llu "
	    "(authoritative)\n", (u_longlong_t)zhack_target_parity);
	spa_close(spa, FTAG);
	return (0);
}

/*
 * ADVERSARIAL tool (Blocker 4 / EPOCH-10..14): write a FORGED reparity marker
 * with attacker-chosen parity/completion/digest -- bypassing the self-verifying
 * derivation -- so the label carries an untrusted certificate. Used to prove
 * vdev_raidz_honored_parity() refuses it (a pool with a forged parity-2 marker
 * over parity-1 data must NOT import with 2 disks missing). RESEARCH/test only.
 * usage: reparity_commit_raw <pool> <vdev_guid> <parity> <completion> <digest>
 */
typedef struct { uint64_t guid, parity, completion, digest; } zhack_craw_t;
static void
zhack_commit_raw_sync(void *arg, dmu_tx_t *tx)
{
	zhack_craw_t *c = arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	vdev_t *vd = vdev_lookup_by_guid(spa->spa_root_vdev, c->guid);
	if (vd == NULL || vd->vdev_ops != &vdev_raidz_ops)
		fatal(spa, FTAG, "vdev guid %llu is not raidz",
		    (u_longlong_t)c->guid);
	vdev_raidz_t *vdrz = vd->vdev_tsd;
	vdrz->vd_reparity_parity = c->parity;	/* forged, no derivation */
	vdrz->vd_reparity_completion_txg = c->completion;	/* forged */
	vdrz->vd_reparity_epoch_digest = c->digest;	/* forged */
	vdev_config_dirty(vd);
}
static int
zhack_do_reparity_commit_raw(int argc, char **argv)
{
	spa_t *spa;
	zhack_craw_t c;
	argc--; argv++;
	if (argc < 5)
		usage();
	if (!zhack_strict_u64(argv[1], &c.guid) ||
	    !zhack_strict_u64(argv[2], &c.parity) ||
	    !zhack_strict_u64(argv[3], &c.completion) ||
	    !zhack_strict_u64(argv[4], &c.digest))
		fatal(NULL, FTAG, "invalid numeric argument");
	zhack_spa_open(argv[0], B_FALSE, FTAG, &spa);
	VERIFY0(dsl_sync_task(spa_name(spa), NULL, zhack_commit_raw_sync, &c, 3,
	    ZFS_SPACE_CHECK_NORMAL));
	(void) fprintf(stderr, "reparity_commit_raw: FORGED marker written "
	    "parity=%llu completion=%llu digest=%llu\n",
	    (u_longlong_t)c.parity, (u_longlong_t)c.completion,
	    (u_longlong_t)c.digest);
	spa_close(spa, FTAG);
	return (0);
}

/*
 * ================= RC1: compression/checksum normalization verifier ========
 * Roadmap v4 P20/RC. The auditor's directive: reuse the standard `zfs rewrite`
 * to re-emit the corpus at the dataset's current compression/checksum, and add
 * the NEW product -- a strict, scoped, fail-closed PROOF that the corpus is
 * actually normalized. This is that proof: an independent traverse census that
 * classifies every user file-DATA block by its stored checksum and compressor.
 * It never mutates the pool. Scope is user file data (the checksum/compression
 * property governs it); pool metadata follows a separate policy and is
 * excluded.
 */
static uint64_t zhack_norm_target_checksum;
static uint64_t zhack_norm_target_compress;
static uint64_t zhack_norm_visited;
static uint64_t zhack_norm_conforming;
static uint64_t zhack_norm_residual;

static int
zhack_normalize_verify_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	(void) spa; (void) zilog; (void) dnp; (void) arg;
	if (bp == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return (0);
	/* RC1 scope: level-0 plain file contents (the normalized corpus). */
	if (BP_GET_LEVEL(bp) != 0 ||
	    BP_GET_TYPE(bp) != DMU_OT_PLAIN_FILE_CONTENTS)
		return (0);
	zhack_norm_visited++;
	uint64_t cksum = BP_GET_CHECKSUM(bp);
	uint64_t comp = BP_GET_COMPRESS(bp);
	boolean_t ck_ok = (cksum == zhack_norm_target_checksum);
	/*
	 * Explicit raw fallback: an incompressible block legitimately stored
	 * OFF conforms to any target compressor -- policy=lz4 must not force us
	 * to falsely tag incompressible bytes as lz4 (auditor RC0). Any OTHER
	 * compressor is the un-normalized old codec.
	 */
	boolean_t cp_ok = (comp == zhack_norm_target_compress ||
	    comp == ZIO_COMPRESS_OFF);
	if (ck_ok && cp_ok) {
		zhack_norm_conforming++;
	} else {
		zhack_norm_residual++;
		if (zhack_norm_residual <= 8 && zb != NULL)
			(void) fprintf(stderr,
			    "  non-normalized: objset=%llu obj=%llu "
			    "blkid=%llu checksum=%llu(want %llu) "
			    "compress=%llu(want %llu)\n",
			    (u_longlong_t)zb->zb_objset,
			    (u_longlong_t)zb->zb_object,
			    (u_longlong_t)zb->zb_blkid, (u_longlong_t)cksum,
			    (u_longlong_t)zhack_norm_target_checksum,
			    (u_longlong_t)comp,
			    (u_longlong_t)zhack_norm_target_compress);
	}
	return (0);
}

static boolean_t
zhack_checksum_by_name(const char *s, uint64_t *out)
{
	if (strcmp(s, "fletcher2") == 0) *out = ZIO_CHECKSUM_FLETCHER_2;
	else if (strcmp(s, "fletcher4") == 0) *out = ZIO_CHECKSUM_FLETCHER_4;
	else if (strcmp(s, "sha256") == 0) *out = ZIO_CHECKSUM_SHA256;
	else if (strcmp(s, "sha512") == 0) *out = ZIO_CHECKSUM_SHA512;
	else if (strcmp(s, "skein") == 0) *out = ZIO_CHECKSUM_SKEIN;
	else return (B_FALSE);
	return (B_TRUE);
}

static boolean_t
zhack_compress_by_name(const char *s, uint64_t *out)
{
	if (strcmp(s, "off") == 0) *out = ZIO_COMPRESS_OFF;
	else if (strcmp(s, "lz4") == 0) *out = ZIO_COMPRESS_LZ4;
	else if (strcmp(s, "lzjb") == 0) *out = ZIO_COMPRESS_LZJB;
	else if (strcmp(s, "zle") == 0) *out = ZIO_COMPRESS_ZLE;
	else if (strcmp(s, "gzip") == 0) *out = ZIO_COMPRESS_GZIP_6;
	else if (strcmp(s, "zstd") == 0) *out = ZIO_COMPRESS_ZSTD;
	else return (B_FALSE);
	return (B_TRUE);
}

/*
 * usage: zhack normalize_verify <pool> <checksum-name> <compress-name>
 * Fail-closed: exit nonzero on any non-conforming file-data block, a traverse
 * error, OR an empty data census (visited==0 is never a silent success).
 */
static int
zhack_do_normalize_verify(int argc, char **argv)
{
	spa_t *spa;
	argc--; argv++;
	if (argc < 3)
		usage();
	if (!zhack_checksum_by_name(argv[1], &zhack_norm_target_checksum))
		fatal(NULL, FTAG, "unsupported checksum name: %s", argv[1]);
	if (!zhack_compress_by_name(argv[2], &zhack_norm_target_compress))
		fatal(NULL, FTAG, "unsupported compress name: %s", argv[2]);
	zhack_spa_open(argv[0], B_FALSE, FTAG, &spa);
	zhack_norm_visited = 0;
	zhack_norm_conforming = 0;
	zhack_norm_residual = 0;
	int err = traverse_pool(spa, 0, TRAVERSE_PRE,
	    zhack_normalize_verify_cb, NULL);
	(void) fprintf(stderr,
	    "normalize census: data_blocks=%llu conforming=%llu "
	    "non_normalized=%llu traverse_err=%d target_checksum=%llu "
	    "target_compress=%llu\n", (u_longlong_t)zhack_norm_visited,
	    (u_longlong_t)zhack_norm_conforming,
	    (u_longlong_t)zhack_norm_residual,
	    err, (u_longlong_t)zhack_norm_target_checksum,
	    (u_longlong_t)zhack_norm_target_compress);
	int rc = 0;
	if (err != 0 || zhack_norm_residual != 0 || zhack_norm_visited == 0) {
		(void) fprintf(stderr,
		    "normalize_verify FAILED-CLOSED: traverse_err=%d "
		    "non_normalized=%llu data_blocks=%llu\n", err,
		    (u_longlong_t)zhack_norm_residual,
		    (u_longlong_t)zhack_norm_visited);
		rc = 1;
	} else {
		(void) fprintf(stderr,
		    "normalize_verify OK: all %llu file-data blocks "
		    "at target codec\n", (u_longlong_t)zhack_norm_visited);
	}
	spa_close(spa, FTAG);
	return (rc);
}

/*
 * ================= PL1: placement / class-migration verifier ===============
 * Roadmap v4 P15/PL. After migrating data off a vdev (or an allocation class)
 * -- e.g. `zfs set special_small_blocks=0` + `zfs rewrite` to evacuate the
 * special class -- prove NO user file-DATA block still has a DVA on the
 * evacuated top-level vdev. Read-only census; fail-closed on any residual DVA,
 * a traverse error, or an empty census. This is the strict placement PROOF the
 * standard tools lack (the auditor's "result contains real placements + the
 * remainder", not a silent property change).
 */
static uint64_t zhack_pl_excl_vdev;
static uint64_t zhack_pl_visited;
static uint64_t zhack_pl_residual;

static int
zhack_placement_verify_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	(void) spa; (void) zilog; (void) dnp; (void) arg;
	if (bp == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return (0);
	if (BP_GET_LEVEL(bp) != 0 ||
	    BP_GET_TYPE(bp) != DMU_OT_PLAIN_FILE_CONTENTS)
		return (0);
	zhack_pl_visited++;
	for (int d = 0; d < SPA_DVAS_PER_BP; d++) {
		const dva_t *dva = &bp->blk_dva[d];
		if (DVA_GET_ASIZE(dva) == 0)
			continue;
		if (DVA_GET_VDEV(dva) == zhack_pl_excl_vdev) {
			zhack_pl_residual++;
			if (zhack_pl_residual <= 8 && zb != NULL)
				(void) fprintf(stderr,
				    "  mis-placed: objset=%llu obj=%llu "
				    "blkid=%llu still on vdev=%llu\n",
				    (u_longlong_t)zb->zb_objset,
				    (u_longlong_t)zb->zb_object,
				    (u_longlong_t)zb->zb_blkid,
				    (u_longlong_t)zhack_pl_excl_vdev);
			break;
		}
	}
	return (0);
}

/*
 * usage: zhack placement_verify <pool> <excluded_vdev_id>
 * Fail-closed: nonzero on any file-data DVA still on the excluded vdev, a
 * traverse error, or an empty data census.
 */
static int
zhack_do_placement_verify(int argc, char **argv)
{
	spa_t *spa;
	argc--; argv++;
	if (argc < 2)
		usage();
	if (!zhack_strict_u64(argv[1], &zhack_pl_excl_vdev))
		fatal(NULL, FTAG, "invalid vdev id: %s", argv[1]);
	zhack_spa_open(argv[0], B_FALSE, FTAG, &spa);
	zhack_pl_visited = 0; zhack_pl_residual = 0;
	int err = traverse_pool(spa, 0, TRAVERSE_PRE,
	    zhack_placement_verify_cb, NULL);
	(void) fprintf(stderr,
	    "placement census: data_blocks=%llu mis_placed=%llu "
	    "traverse_err=%d excluded_vdev=%llu\n",
	    (u_longlong_t)zhack_pl_visited,
	    (u_longlong_t)zhack_pl_residual, err,
	    (u_longlong_t)zhack_pl_excl_vdev);
	int rc = 0;
	if (err != 0 || zhack_pl_residual != 0 || zhack_pl_visited == 0) {
		(void) fprintf(stderr,
		    "placement_verify FAILED-CLOSED: traverse_err=%d "
		    "mis_placed=%llu data_blocks=%llu\n", err,
		    (u_longlong_t)zhack_pl_residual,
		    (u_longlong_t)zhack_pl_visited);
		rc = 1;
	} else {
		(void) fprintf(stderr,
		    "placement_verify OK: no file-data on vdev %llu "
		    "(%llu blocks checked)\n", (u_longlong_t)zhack_pl_excl_vdev,
		    (u_longlong_t)zhack_pl_visited);
	}
	spa_close(spa, FTAG);
	return (rc);
}

/*
 * S3 in-place PROBE -- UNSAFE, RESEARCH ONLY, NEVER a shipped capability.
 * Repoints a snapshot's ds_bp at another dataset's objset root. The only
 * mutable handle on a snapshot is its DSL dataset MOS object; its DATA tree is
 * immutable, so this is the only way to make a snapshot's blocks reach a
 * promoted (parity-2) tree without a general block-pointer-rewrite primitive.
 *
 * FINDING (validated in VM): after clone->full-sweep->repoint, the promoted
 * snapshot reads its correct HISTORICAL content, scrubs clean, survives P+1
 * disk loss, and `zfs send` works -- DATA and REDUNDANCY are correct. BUT it
 * PERMANENTLY corrupts the DSL block-accounting: `zdb -bb` reports persistent
 * "block claim error 2" on every shared parity-2 block (65 in the probe),
 * because the deadlist/livelist/refcount surgery is NOT done (the snapshot and
 * the clone both reference the blocks with no accounting of the sharing).
 * Destroying the clone does NOT resolve it. This is latent corruption
 * (double-free/leak on a future free or spacemap condense) -- so this primitive
 * MUST NOT be used on real data. Making it safe requires the DSL deadlist
 * surgery specified by rrmvp/shared_blocks.py + shared_rewrite.py (BPR-class,
 * unshipped by OpenZFS). See S3_SHARED_GRAPH_KERNEL_BOUNDARY.md.
 * usage: zhack snap_repoint <pool> <snap fullname> <src dataset fullname>
 */
typedef struct { uint64_t snap_obj; blkptr_t bp; } zhack_repoint_t;

static void
zhack_snap_repoint_sync(void *arg, dmu_tx_t *tx)
{
	dsl_pool_t *dp = dmu_tx_pool(tx);
	zhack_repoint_t *r = arg;
	dsl_dataset_t *snap;
	VERIFY0(dsl_dataset_hold_obj(dp, r->snap_obj, FTAG, &snap));
	dmu_buf_will_dirty(snap->ds_dbuf, tx);
	dsl_dataset_phys(snap)->ds_bp = r->bp;
	dsl_dataset_rele(snap, FTAG);
}

static int
zhack_do_snap_repoint(int argc, char **argv)
{
	spa_t *spa;
	zhack_repoint_t r;
	argc--; argv++;
	if (argc < 3)
		usage();
	zhack_spa_open(argv[0], B_FALSE, FTAG, &spa);
	dsl_pool_t *dp;
	VERIFY0(dsl_pool_hold(argv[0], FTAG, &dp));
	dsl_dataset_t *snap, *src;
	VERIFY0(dsl_dataset_hold(dp, argv[1], FTAG, &snap));
	VERIFY0(dsl_dataset_hold(dp, argv[2], FTAG, &src));
	r.snap_obj = snap->ds_object;
	rrw_enter(&src->ds_bp_rwlock, RW_READER, FTAG);
	r.bp = dsl_dataset_phys(src)->ds_bp;
	rrw_exit(&src->ds_bp_rwlock, FTAG);
	dsl_dataset_rele(src, FTAG);
	dsl_dataset_rele(snap, FTAG);
	dsl_pool_rele(dp, FTAG);
	VERIFY0(dsl_sync_task(spa_name(spa), NULL, zhack_snap_repoint_sync, &r,
	    3, ZFS_SPACE_CHECK_NONE));
	(void) fprintf(stderr, "snap_repoint: %s ds_bp <- %s ds_bp\n",
	    argv[1], argv[2]);
	spa_close(spa, FTAG);
	return (0);
}

/*
 * S3 DSL-surgery PROBE -- FATAL, PROVEN INVALID, NEVER usable.
 * Attempts to re-anchor a snapshot's birth-txg window (ds_creation_txg /
 * ds_prev_snap_txg) so it can "own" blocks born at the reparity txg T, to fix
 * the snap_repoint block-claim errors.
 *
 * RESULT (validated in VM): this CORRUPTS the pool -- a subsequent `zpool
 * import` hangs in D-state at spl_panic() inside
 * dsl_dataset_space_written_impl() (dataset_list_next -> objset_stats ->
 * dsl_get_written): the "written" property computation VERIFYs birth-txg
 * ordering, and a re-anchored creation_txg makes it panic. Recovery required
 * `reboot -f` + deleting the pool images without importing. This proves ZFS's
 * birth-txg is a LOAD-BEARING invariant enforced by kernel assertions, not a
 * soft field: a snapshot cannot be made to own blocks born after its creation
 * by any txg-field surgery. See
 * S3_SHARED_GRAPH_KERNEL_BOUNDARY.md. Retained only as evidence; DO NOT RUN.
 * usage: zhack snap_reanchor <pool> <snap> <creation_txg> [prev_snap_txg]
 */
typedef struct {
	uint64_t snap_obj, creation_txg, prev_snap_txg;
	boolean_t set_prev;
} zhack_reanchor_t;

static void
zhack_snap_reanchor_sync(void *arg, dmu_tx_t *tx)
{
	dsl_pool_t *dp = dmu_tx_pool(tx);
	zhack_reanchor_t *r = arg;
	dsl_dataset_t *snap;
	VERIFY0(dsl_dataset_hold_obj(dp, r->snap_obj, FTAG, &snap));
	dmu_buf_will_dirty(snap->ds_dbuf, tx);
	dsl_dataset_phys(snap)->ds_creation_txg = r->creation_txg;
	if (r->set_prev)
		dsl_dataset_phys(snap)->ds_prev_snap_txg = r->prev_snap_txg;
	dsl_dataset_rele(snap, FTAG);
}

static int
zhack_do_snap_reanchor(int argc, char **argv)
{
	spa_t *spa;
	zhack_reanchor_t r;
	memset(&r, 0, sizeof (r));
	argc--; argv++;
	if (argc < 3)
		usage();
	if (!zhack_strict_u64(argv[2], &r.creation_txg))
		fatal(NULL, FTAG, "invalid creation_txg: %s", argv[2]);
	if (argc >= 4) {
		if (!zhack_strict_u64(argv[3], &r.prev_snap_txg))
			fatal(NULL, FTAG, "invalid prev_snap_txg: %s", argv[3]);
		r.set_prev = B_TRUE;
	}
	zhack_spa_open(argv[0], B_FALSE, FTAG, &spa);
	dsl_pool_t *dp;
	VERIFY0(dsl_pool_hold(argv[0], FTAG, &dp));
	dsl_dataset_t *snap;
	VERIFY0(dsl_dataset_hold(dp, argv[1], FTAG, &snap));
	r.snap_obj = snap->ds_object;
	dsl_dataset_rele(snap, FTAG);
	dsl_pool_rele(dp, FTAG);
	VERIFY0(dsl_sync_task(spa_name(spa), NULL, zhack_snap_reanchor_sync, &r,
	    3, ZFS_SPACE_CHECK_NONE));
	(void) fprintf(stderr, "snap_reanchor: %s creation_txg=%llu%s\n",
	    argv[1], (u_longlong_t)r.creation_txg,
	    r.set_prev ? " (prev set)" : "");
	spa_close(spa, FTAG);
	return (0);
}

/*
 * ==== snap_bpr: in-place snapshot block-pointer rewrite (parity promotion)
 * ==== RESEARCH PROTOTYPE -- proves the concept (data + redundancy) but is NOT
 * production-safe. DO NOT run on real data, and DO NOT `zfs destroy` a promoted
 * snapshot (it HANGS the kernel -- the deadlist is not updated).
 *
 * Rewrites a snapshot's whole block tree bottom-up. Each block is relocated +
 * re-encoded at the promoted parity (physical birth = txg drives the epoch
 * parity selection) with its LOGICAL birth PRESERVED. Handles objset /
 * dnode-array / indirect / leaf blocks (mirrors traverse). VALIDATED: content
 * preserved, zdb block-claim clean, scrub clean, survives P+1 disk loss.
 * KNOWN-BROKEN (both from bypassing the DSL accounting/deadlist layer): old
 * blocks are not reclaimed (space leak), and destroy hangs. Full correctness
 * needs the alloc/free wired through dsl_dataset_block_born/kill + a deadlist
 * rebuild (research-grade S3/S4 DSL surgery). See BPR_ENGINE.md.
 */
static uint64_t zhack_bpr_rewritten;
static uint64_t zhack_bpr_errors;
static uint64_t zhack_bpr_alloc_bytes;
static uint64_t zhack_bpr_free_bytes;
static uint64_t zhack_bpr_dlremapped;
static uint64_t zhack_bpr_freed;
/*
 * per-snapshot growth in physical (asize) space, applied to ds_referenced_bytes
 */
static int64_t zhack_bpr_ref_delta;
/*
 * S4: the parity-growth delta of ONLY the blocks THIS dataset relocates itself
 * (memo MISS), i.e. its PRIVATE blocks -- distinct from ref_delta, which also
 * counts blocks shared with (already relocated by) an older dataset. For a
 * clone processed after its origin, memo-miss == clone-private, so this is
 * exactly the amount by which the clone's dsl_dir dd_used_bytes must grow (the
 * BPR enlarged the private blocks but block_born/kill are skipped, so dd_used
 * is otherwise stale -> destroying the clone transfers a too-small dd_used to
 * dp_free_dir while the async drain credits the real (larger) size,
 * underflowing `freeing`).
 */
static int64_t zhack_bpr_private_delta;
/* P8b: count of DDT (dedup) entries repointed old-DVA -> relocated new-DVA. */
static uint64_t zhack_bpr_ddt_remapped;
/*
 * P8b: count of BRT (block-cloning) entries migrated old-DVA -> relocated
 * new-DVA.
 */
static uint64_t zhack_bpr_brt_remapped;
static void zhack_bpr_bp(spa_t *, dmu_tx_t *, blkptr_t *,
    const zbookmark_phys_t *);

/*
 * Rewrite-once memo (old DVA -> new BP): shared snapshot/clone subtrees are
 * relocated exactly once; every other referrer reuses the new BP. Without this,
 * a shared block would be read/freed twice across snapshots (double-free
 * abort).
 */
typedef struct {
	uint64_t bm_vdev, bm_offset;
	blkptr_t bm_oldbp; /* original block, for the deferred free pass */
	blkptr_t bm_newbp;
	avl_node_t bm_link;
} zhack_bpr_memo_t;
static avl_tree_t zhack_bpr_memo;

static int
zhack_bpr_memo_cmp(const void *a, const void *b)
{
	const zhack_bpr_memo_t *x = a, *y = b;
	if (x->bm_vdev != y->bm_vdev)
		return (x->bm_vdev < y->bm_vdev ? -1 : 1);
	if (x->bm_offset != y->bm_offset)
		return (x->bm_offset < y->bm_offset ? -1 : 1);
	return (0);
}

static int
zhack_bpr_collect(const char *name, void *arg)
{
	fnvlist_add_boolean((nvlist_t *)arg, name);
	return (0);
}

/*
 * S4 Part A: collect a live dataset ONLY if it is a CLONE (dd_origin_obj != 0).
 * A clone's tree still points at its origin snapshot's ORIGINAL blocks; adding
 * it to the snap_bpr passes (processed oldest-first, after its origin)
 * relocates the clone's tree through the SAME rewrite-once memo, so it points
 * at the relocated blocks -- otherwise reparity fail-closes on the un-promoted
 * shared blocks.
 */
static int
zhack_bpr_collect_clone(const char *name, void *arg)
{
	dsl_pool_t *dp;
	if (dsl_pool_hold(name, FTAG, &dp) != 0)
		return (0);
	dsl_dataset_t *ds;
	if (dsl_dataset_hold(dp, name, FTAG, &ds) == 0) {
		uint64_t org = dsl_dir_phys(ds->ds_dir)->dd_origin_obj;
		/*
		 * A REAL clone's origin is a user snapshot; a normally-created
		 * dataset's dd_origin_obj points at the pool's hidden $ORIGIN
		 * snapshot (shared by the root + all plain filesystems). Only
		 * add real clones.
		 */
		uint64_t poolorigin = (dp->dp_origin_snap != NULL) ?
		    dp->dp_origin_snap->ds_object : 0;
		if (org != 0 && org != poolorigin)
			fnvlist_add_boolean((nvlist_t *)arg, name);
		dsl_dataset_rele(ds, FTAG);
	}
	dsl_pool_rele(dp, FTAG);
	return (0);
}

/* gate (defined in dsl_dataset.c) + the snapshot currently being rewritten */
extern int reparity_allow_snap_dirty;
extern int zfs_free_bpobj_enabled;
static dsl_dataset_t *zhack_bpr_ds;


static void
zhack_bpr_getbuf(zio_t *zio, const zbookmark_phys_t *zb, const blkptr_t *bp,
    arc_buf_t *buf, void *arg)
{
	(void) zio; (void) zb; (void) bp;
	*(arc_buf_t **)arg = buf;
}

static void
zhack_bpr_ready(zio_t *zio, arc_buf_t *buf, void *arg)
{
	(void) buf; (void) arg;
	/*
	 * keep a nonzero fill so the new BP isn't taken for a hole; the
	 * caller restores the exact fill/births after the write completes.
	 */
	if (zio->io_bp != NULL && BP_GET_LEVEL(zio->io_bp) == 0)
		zio->io_bp->blk_fill = 1;
}

static void
zhack_bpr_done(zio_t *zio, arc_buf_t *buf, void *arg)
{
	(void) zio; (void) buf; (void) arg;
}

static void
zhack_bpr_dnode(spa_t *spa, dmu_tx_t *tx, dnode_phys_t *dnp, uint64_t objset,
    uint64_t object)
{
	for (int j = 0; j < dnp->dn_nblkptr; j++) {
		zbookmark_phys_t czb;
		SET_BOOKMARK(&czb, objset, object,
		    dnp->dn_nlevels - 1, j);
		zhack_bpr_bp(spa, tx, &dnp->dn_blkptr[j], &czb);
	}
	if (dnp->dn_flags & DNODE_FLAG_SPILL_BLKPTR) {
		zbookmark_phys_t czb;
		SET_BOOKMARK(&czb, objset, object, 0, DMU_SPILL_BLKID);
		zhack_bpr_bp(spa, tx, DN_SPILL_BLKPTR(dnp), &czb);
	}
}

static void
zhack_bpr_bp(spa_t *spa, dmu_tx_t *tx, blkptr_t *bp, const zbookmark_phys_t *zb)
{
	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) || BP_IS_REDACTED(bp))
		return;
	uint64_t txg = dmu_tx_get_txg(tx);
	int level = BP_GET_LEVEL(bp);
	dmu_object_type_t type = BP_GET_TYPE(bp);
	uint64_t logical = BP_GET_LOGICAL_BIRTH(bp);
	uint64_t fill = bp->blk_fill;
	uint64_t lsize = BP_GET_LSIZE(bp);
	blkptr_t old = *bp;

	/* rewrite-once: reuse an already-relocated shared subtree */
	uint64_t mvdev = DVA_GET_VDEV(&old.blk_dva[0]);
	uint64_t moffset = DVA_GET_OFFSET(&old.blk_dva[0]);
	zhack_bpr_memo_t mkey;
	mkey.bm_vdev = mvdev;
	mkey.bm_offset = moffset;
	zhack_bpr_memo_t *mfound = avl_find(&zhack_bpr_memo, &mkey, NULL);
	if (mfound != NULL) {
		/*
		 * this snapshot references a block relocated for an earlier
		 * one; its physical footprint still grows, so account the
		 * delta.
		 */
		zhack_bpr_ref_delta +=
		    (int64_t)bp_get_dsize_sync(spa, &mfound->bm_newbp) -
		    (int64_t)bp_get_dsize_sync(spa, &old);
		*bp = mfound->bm_newbp;
		return;
	}

	arc_flags_t aflags = ARC_FLAG_WAIT;
	arc_buf_t *abuf = NULL;
	if (arc_read(NULL, spa, bp, zhack_bpr_getbuf, &abuf,
	    ZIO_PRIORITY_SYNC_READ, ZIO_FLAG_CANFAIL, &aflags, zb) != 0 ||
	    abuf == NULL) {
		zhack_bpr_errors++;
		return;
	}
	void *priv = kmem_alloc(lsize, KM_SLEEP);
	memcpy(priv, abuf->b_data, lsize);
	arc_buf_destroy(abuf, &abuf);

	if (level > 0) {
		int epb = lsize >> SPA_BLKPTRSHIFT;
		blkptr_t *cbps = priv;
		for (int i = 0; i < epb; i++) {
			zbookmark_phys_t czb;
			SET_BOOKMARK(&czb, zb->zb_objset, zb->zb_object,
			    level - 1, zb->zb_blkid * epb + i);
			zhack_bpr_bp(spa, tx, &cbps[i], &czb);
		}
	} else if (type == DMU_OT_DNODE) {
		int epb = lsize >> DNODE_SHIFT;
		dnode_phys_t *dnp = priv;
		for (int i = 0; i < epb; i += dnp[i].dn_extra_slots + 1)
			zhack_bpr_dnode(spa, tx, &dnp[i], zb->zb_objset,
			    zb->zb_blkid * epb + i);
	} else if (type == DMU_OT_OBJSET) {
		objset_phys_t *osp = priv;
		zhack_bpr_dnode(spa, tx, &osp->os_meta_dnode, zb->zb_objset,
		    DMU_META_DNODE_OBJECT);
		if (osp->os_flags & OBJSET_FLAG_USERACCOUNTING_COMPLETE) {
			zhack_bpr_dnode(spa, tx, &osp->os_userused_dnode,
			    zb->zb_objset, DMU_USERUSED_OBJECT);
			zhack_bpr_dnode(spa, tx, &osp->os_groupused_dnode,
			    zb->zb_objset, DMU_GROUPUSED_OBJECT);
		}
	}

	zio_prop_t zp;
	memset(&zp, 0, sizeof (zp));
	zp.zp_checksum = BP_GET_CHECKSUM(&old);
	zp.zp_compress = BP_GET_COMPRESS(&old);
	zp.zp_complevel = ZIO_COMPLEVEL_DEFAULT;
	zp.zp_type = type;
	zp.zp_level = level;
	zp.zp_copies = BP_GET_NDVAS(&old);
	zp.zp_gang_copies = zp.zp_copies;
	zp.zp_dedup = B_FALSE;
	zp.zp_dedup_verify = B_FALSE;
	zp.zp_nopwrite = B_FALSE;
	zp.zp_encrypt = B_FALSE;
	zp.zp_byteorder = ZFS_HOST_BYTEORDER;

	arc_buf_contents_t bufc = (level > 0 || DMU_OT_IS_METADATA(type)) ?
	    ARC_BUFC_METADATA : ARC_BUFC_DATA;
	arc_buf_t *wbuf = arc_alloc_buf(spa, FTAG, bufc, lsize);
	memcpy(wbuf->b_data, priv, lsize);
	kmem_free(priv, lsize);

	blkptr_t newbp;
	BP_ZERO(&newbp);
	zio_t *pio = zio_root(spa, NULL, NULL, ZIO_FLAG_CANFAIL);
	zio_nowait(arc_write(pio, spa, txg, &newbp, wbuf, B_FALSE, B_FALSE,
	    &zp, zhack_bpr_ready, NULL, zhack_bpr_done, NULL,
	    ZIO_PRIORITY_ASYNC_WRITE, ZIO_FLAG_CANFAIL, zb));
	int werr = zio_wait(pio);
	arc_buf_destroy(wbuf, FTAG);
	if (werr != 0) {
		zhack_bpr_errors++;
		return;
	}

	/*
	 * Mirror the physical_rewrite finalize (zio.c zio_dva_allocate):
	 * preserve the LOGICAL birth, stamp PHYSICAL birth = this txg, and set
	 * the REWRITE flag. The flag is what makes BP_GET_BIRTH() return the
	 * physical birth for this block (allocation/scan/born see "born now"),
	 * while BP_GET_LOGICAL_BIRTH() still returns the original
	 * (deadlist/destroy ownership, re-keyed to logical, see the true
	 * snapshot birth).
	 */
	BP_SET_BIRTH(&newbp, logical, txg);
	BP_SET_REWRITE(&newbp, 1);
	/*
	 * P8b: preserve the DEDUP bit. The write above used zp_dedup=B_FALSE so
	 * arc_write allocated a FRESH block (not deduped back onto the old
	 * DVA), but the relocated block must stay DDT-managed: its DDT entry is
	 * repointed to this new DVA (zhack_bpr_ddt_remap_sync) and its N
	 * logical references (all resolving to this one new BP via the
	 * rewrite-once memo) must free through ddt_phys_decref so the block is
	 * freed only when the last reference drops. With DEDUP=0 the first free
	 * would release the shared block under the other references ->
	 * corruption. Non-dedup blocks keep DEDUP=0 (no-op).
	 */
	BP_SET_DEDUP(&newbp, BP_GET_DEDUP(&old));
	newbp.blk_fill = fill;
	{
		int64_t _pd = (int64_t)bp_get_dsize_sync(spa, &newbp)
		    - (int64_t)bp_get_dsize_sync(spa, &old);
		zhack_bpr_ref_delta += _pd;
		/* memo MISS: this dataset's own block */
		zhack_bpr_private_delta += _pd;
	}
	*bp = newbp;
	zhack_bpr_alloc_bytes += BP_GET_ASIZE(&newbp);
	zhack_bpr_free_bytes += BP_GET_ASIZE(&old);
	/*
	 * A parity promotion changes only each block's PHYSICAL encoding and
	 * location -- the LOGICAL structure (which object/snapshot references
	 * which block, at which logical birth) is invariant. So we do NOT touch
	 * ds_referenced/unique_bytes or call block_kill/block_born (those model
	 * live-dataset CoW, whose deadlist direction is wrong for a snapshot
	 * rewrite). We repoint the BP, record the relocation in the shared
	 * memo, and -- crucially -- a later pass remaps EVERY other structure
	 * that also holds this old BP (the per-snapshot deadlists) through the
	 * same memo, so nothing is left pointing at the old DVA. The old
	 * block's space is reclaimed by the deferred-free pass. The memo
	 * relocates a shared block exactly once (oldest owner); every other
	 * reference resolves via memo.
	 */
	zhack_bpr_rewritten++;

	zhack_bpr_memo_t *mnode = kmem_alloc(sizeof (*mnode), KM_SLEEP);
	mnode->bm_vdev = mvdev;
	mnode->bm_offset = moffset;
	mnode->bm_oldbp = old;
	mnode->bm_newbp = newbp;
	avl_add(&zhack_bpr_memo, mnode);
}

typedef struct { uint64_t snap_obj; } zhack_bpr_arg_t;

static void
zhack_snap_bpr_sync(void *arg, dmu_tx_t *tx)
{
	zhack_bpr_arg_t *b = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dataset_t *snap;
	VERIFY0(dsl_dataset_hold_obj(dp, b->snap_obj, FTAG, &snap));
	dmu_buf_will_dirty(snap->ds_dbuf, tx);
	/*
	 * The re-encoded blocks carry the REWRITE flag, so the physical_rewrite
	 * feature must be active on this snapshot's dataset (same activation
	 * the live rewrite path does in dbuf_sync_leaf). Do it once per
	 * snapshot.
	 */
	if (!dsl_dataset_feature_is_active(snap, SPA_FEATURE_PHYSICAL_REWRITE))
		dsl_dataset_activate_feature(snap->ds_object,
		    SPA_FEATURE_PHYSICAL_REWRITE, (void *)B_TRUE, tx);
	zhack_bpr_ds = snap;
	zhack_bpr_ref_delta = 0;
	zhack_bpr_private_delta = 0;
	blkptr_t *rootbp = &dsl_dataset_phys(snap)->ds_bp;
	zbookmark_phys_t zb;
	SET_BOOKMARK(&zb, snap->ds_object, ZB_ROOT_OBJECT, ZB_ROOT_LEVEL,
	    ZB_ROOT_BLKID);
	zhack_bpr_bp(dp->dp_spa, tx, rootbp, &zb);
	/*
	 * Every block this snapshot references grew by its parity delta; grow
	 * ds_referenced_bytes by the total so the deadlist/uniqueness invariant
	 * (dsl_dataset_recalc_head_uniq: head dl_used <= most-recent-snapshot's
	 * ds_referenced) holds. ds_unique is left to the recalc / next-snap
	 * freeze (bumping it by the full delta would over-count blocks shared
	 * with a neighbour); referenced-only keeps the invariant without
	 * over-freeing.
	 */
	dsl_dataset_phys(snap)->ds_referenced_bytes += zhack_bpr_ref_delta;
	/*
	 * Grow the dsl_dir dd_used_bytes for the parity-delta of this dataset's
	 * blocks. block_born/kill are skipped by the BPR, so dd_used is
	 * otherwise stale at the pre-promotion size; on `zfs destroy` that
	 * stale (too-small) dd_used is transferred to dp_free_dir while the
	 * async free drain (dsl_scan_free_block_cb) credits the real, larger
	 * block sizes PER BLOCK POINTER -- so a dedup'd block referenced N
	 * times is credited N times. The grow amount therefore differs by
	 * dataset kind:
	 *   - HEAD filesystem (dd_origin is 0 or the pool's hidden $ORIGIN):
	 *     use ref_delta, which counts EVERY reference (memo hit + miss).
	 *     For a plain head ref_delta == private_delta (one ref per block);
	 *     for a deduped head ref_delta is N x the block delta, matching the
	 *     N per-BP credits at destroy -- this is what fixes the dedup
	 *     destroy `freeing` underflow.
	 *   - real CLONE (dd_origin is a user snapshot): use private_delta
	 *     (memo miss = clone-PRIVATE blocks only); blocks shared with the
	 *     origin are the origin's and are not freed with the clone, so must
	 *     not be charged.
	 * Only physical (used) grows; logical comp/uncomp are unchanged by an
	 * in-place parity promotion. Snapshots fold into their fs via the
	 * deadlist and must NOT touch dd_used here.
	 */
	if (!snap->ds_is_snapshot) {
		uint64_t org = dsl_dir_phys(snap->ds_dir)->dd_origin_obj;
		uint64_t poolorigin = (dp->dp_origin_snap != NULL) ?
		    dp->dp_origin_snap->ds_object : 0;
		boolean_t is_clone = (org != 0 && org != poolorigin);
		int64_t grow = is_clone ? zhack_bpr_private_delta :
		    zhack_bpr_ref_delta;
		if (grow != 0)
			dsl_dir_diduse_space(snap->ds_dir, DD_USED_HEAD,
			    grow, 0, 0, tx);
	}
	zhack_bpr_ds = NULL;
	dsl_dataset_rele(snap, FTAG);
}

/*
 * Deadlist remap. After every snapshot tree is relocated (memo fully
 * populated), rewrite every deadlist so no BP points at a relocated old block.
 * A snapshot's deadlist is a ZAP (ds_deadlist_obj) mapping mintxg -> sub-bpobj;
 * each sub-bpobj is an append-only BP list. A relocated block keeps its LOGICAL
 * birth, so it stays in the same mintxg bucket -- we rebuild each sub-bpobj
 * with every BP resolved through the memo, then swap it into the ZAP. This is
 * what makes `zfs destroy` of a promoted snapshot safe: destroy walks the
 * deadlist and every block it frees now exists at the promoted parity.
 */
typedef struct {
	bpobj_t *dr_newbpo;
	dmu_tx_t *dr_tx;
	spa_t *dr_spa;
	/* growth in physical (asize) space from promotion */
	int64_t dr_used_delta;
} zhack_dlremap_ctx_t;

static int
zhack_dlremap_itor(void *arg, const blkptr_t *bp, boolean_t bp_freed,
    dmu_tx_t *tx)
{
	(void) tx;
	zhack_dlremap_ctx_t *ctx = arg;
	blkptr_t nbp = *bp;
	if (!BP_IS_HOLE(bp) && !BP_IS_EMBEDDED(bp)) {
		zhack_bpr_memo_t mkey;
		mkey.bm_vdev = DVA_GET_VDEV(&bp->blk_dva[0]);
		mkey.bm_offset = DVA_GET_OFFSET(&bp->blk_dva[0]);
		zhack_bpr_memo_t *m = avl_find(&zhack_bpr_memo, &mkey, NULL);
		if (m != NULL) {
			/*
			 * The deadlist header (dl_phys->dl_used) sums each BP's
			 * PHYSICAL size (bp_get_dsize_sync). Promotion grows
			 * that (extra parity column), so track the delta and
			 * apply it to dl_used. dl_comp/dl_uncomp are logical ->
			 * unchanged.
			 */
			ctx->dr_used_delta +=
			    (int64_t)bp_get_dsize_sync(ctx->dr_spa,
			    &m->bm_newbp) -
			    (int64_t)bp_get_dsize_sync(ctx->dr_spa, bp);
			nbp = m->bm_newbp;
		}
	}
	bpobj_enqueue(ctx->dr_newbpo, &nbp, bp_freed, ctx->dr_tx);
	return (0);
}

static void
zhack_bpr_remap_dlobj(objset_t *mos, uint64_t dlobj, dmu_tx_t *tx)
{
	if (dlobj == 0)
		return;
	dmu_object_info_t doi;
	if (dmu_object_info(mos, dlobj, &doi) != 0)
		return;
	if (doi.doi_type == DMU_OT_BPOBJ) /* old single-bpobj format: skip */
		return;
	spa_t *spa = dmu_objset_spa(mos);
	uint64_t empty = dmu_objset_pool(mos)->dp_empty_bpobj;
	/*
	 * hold the deadlist header (bonus) to correct dl_used for the asize
	 * growth
	 */
	dmu_buf_t *hdr;
	VERIFY0(dmu_bonus_hold(mos, dlobj, FTAG, &hdr));
	dmu_buf_will_dirty(hdr, tx);
	dsl_deadlist_phys_t *dlp = hdr->db_data;
	/* snapshot the (mintxg, oldobj) pairs before mutating the ZAP */
	uint64_t mtxgs[2048], oldobjs[2048];
	int n = 0;
	zap_cursor_t zc;
	zap_attribute_t *za = zap_attribute_alloc();
	for (zap_cursor_init(&zc, mos, dlobj);
	    n < 2048 && zap_cursor_retrieve(&zc, za) == 0;
	    zap_cursor_advance(&zc)) {
		mtxgs[n] = zfs_strtonum(za->za_name, NULL);
		oldobjs[n] = za->za_first_integer;
		n++;
	}
	zap_cursor_fini(&zc);
	zap_attribute_free(za);
	int64_t used_delta = 0;
	for (int i = 0; i < n; i++) {
		if (oldobjs[i] == empty || oldobjs[i] == 0)
			continue;
		uint64_t newobj = bpobj_alloc(mos, SPA_OLD_MAXBLOCKSIZE, tx);
		bpobj_t oldbpo, newbpo;
		if (bpobj_open(&oldbpo, mos, oldobjs[i]) != 0) {
			bpobj_free(mos, newobj, tx);
			continue;
		}
		VERIFY0(bpobj_open(&newbpo, mos, newobj));
		zhack_dlremap_ctx_t ctx = { &newbpo, tx, spa, 0 };
		(void) bpobj_iterate_nofree(&oldbpo, zhack_dlremap_itor, &ctx,
		    NULL);
		used_delta += ctx.dr_used_delta;
		bpobj_close(&newbpo);
		bpobj_close(&oldbpo);
		VERIFY0(zap_update_int_key(mos, dlobj, mtxgs[i], newobj, tx));
		bpobj_free(mos, oldobjs[i], tx);
		zhack_bpr_dlremapped++;
	}
	dlp->dl_used += used_delta;
	dmu_buf_rele(hdr, FTAG);
}

static void
zhack_snap_dlremap_sync(void *arg, dmu_tx_t *tx)
{
	zhack_bpr_arg_t *b = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	objset_t *mos = dp->dp_meta_objset;
	dsl_dataset_t *snap;
	VERIFY0(dsl_dataset_hold_obj(dp, b->snap_obj, FTAG, &snap));
	uint64_t dlobj = dsl_dataset_phys(snap)->ds_deadlist_obj;
	zhack_bpr_remap_dlobj(mos, dlobj, tx);
	/*
	 * S4 Part B: a CLONE is a LIVE dataset (dd_origin_obj != 0) whose
	 * dd_livelist (the ALLOC/FREE log of blocks born in the clone vs
	 * inherited from its origin) is SEPARATE from ds_deadlist_obj and still
	 * points at the pre-BPR block pointers. Pass 3 frees those old blocks,
	 * so destroying the clone would FREE THEM AGAIN through the livelist
	 * and underflow dp_free_dir: the pool's `freeing` counter wraps to
	 * ~2^64, which poisons dsl_pool_adjustedsize() so every later
	 * space-checked operation (notably destroying the clone-origin
	 * snapshot) fails ENOSPC -- surfaced to the user as the zcp wrapper
	 * error ECHRNG ("Channel number out of range"). Remap the livelist
	 * through the SAME rewrite-once memo so every ALLOC/FREE entry points
	 * at the relocated block, exactly like the deadlist remap above
	 * (zhack_dlremap_itor preserves each entry's bp_freed flag). The
	 * livelist object is a deadlist-format object stored in the clone's
	 * dsl_dir ZAP under DD_FIELD_LIVELIST.
	 */
	if (dsl_dir_phys(snap->ds_dir)->dd_origin_obj != 0) {
		uint64_t llobj = 0;
		if (zap_lookup(mos, snap->ds_dir->dd_object, DD_FIELD_LIVELIST,
		    sizeof (uint64_t), 1, &llobj) == 0 && llobj != 0)
			zhack_bpr_remap_dlobj(mos, llobj, tx);
	}
	/*
	 * RE-DERIVE ds_unique_bytes from the now-consistent referenced +
	 * deadlist, using the standard identity (dsl_dataset_recalc_head_uniq
	 * generalized to any dataset): unique = referenced -
	 * prev_snap.referenced + dl_used. ds_referenced was grown by the
	 * tree-BPR pass and dl_used by the deadlist remap, so this makes
	 * ds_unique consistent -- without it, block_kill's
	 * ASSERT(ds_unique_bytes >= used) underflows and panics on a promoted
	 * block.
	 */
	/*
	 * CLONE-ORIGIN GUARD (S4). The chain identity below (referenced - prev
	 * + dl_used) is only correct for a snapshot in a plain CHAIN, where
	 * every referenced-beyond-prev block is unique to this snapshot. A
	 * snapshot that is a CLONE ORIGIN (ds_next_clones_obj != 0) also shares
	 * blocks with its clone(s); those shared blocks are NOT unique and
	 * would be freed with the clone, not with this snapshot. The chain
	 * formula does not subtract them, so it OVER-counts ds_unique
	 * (empirically 0 -> full-referenced for a fully shared origin), which
	 * then makes `zfs destroy <origin-snap>` mis-compute its space
	 * reservation and fail ENOSPC. Pass 1 grew ds_referenced but left
	 * ds_unique_bytes untouched, so the value sitting here is still the
	 * ORIGINAL one ZFS derived correctly (accounting for the clone). For a
	 * clone origin we therefore PRESERVE it rather than re-derive from the
	 * chain identity.
	 */
	if (dsl_dataset_phys(snap)->ds_next_clones_obj != 0) {
		dsl_dataset_rele(snap, FTAG);
		return;
	}
	uint64_t dlused = 0;
	dmu_buf_t *dh;
	if (dmu_bonus_hold(mos, dlobj, FTAG, &dh) == 0) {
		dsl_deadlist_phys_t *dlp = dh->db_data;
		dlused = dlp->dl_used;
		dmu_buf_rele(dh, FTAG);
	}
	uint64_t dsref = dsl_dataset_phys(snap)->ds_referenced_bytes;
	uint64_t prevref = 0;
	if (dsl_dataset_phys(snap)->ds_prev_snap_obj != 0) {
		dsl_dataset_t *prev;
		if (dsl_dataset_hold_obj(dp,
		    dsl_dataset_phys(snap)->ds_prev_snap_obj, FTAG,
		    &prev) == 0) {
			prevref = dsl_dataset_phys(prev)->ds_referenced_bytes;
			dsl_dataset_rele(prev, FTAG);
		}
	}
	int64_t uniq = (int64_t)dsref - (int64_t)prevref + (int64_t)dlused;
	dmu_buf_will_dirty(snap->ds_dbuf, tx);
	dsl_dataset_phys(snap)->ds_unique_bytes =
	    (uniq < 0) ? 0 : (uint64_t)uniq;
	dsl_dataset_phys(snap)->ds_flags |= DS_FLAG_UNIQUE_ACCURATE;
	dsl_dataset_rele(snap, FTAG);
}

/*
 * P8b Pass 2.5: remap the DEDUP table (DDT). A deduped block has ONE physical
 * copy referenced N times; the DDT (keyed by checksum) holds the authoritative
 * physical DVA + refcount. The rewrite-once memo already relocated the unique
 * block ONCE and repointed all N tree/deadlist references to the new BP, so
 * PHYSICAL dedup sharing is preserved. But the DDT entry still points at the
 * OLD DVA. For each relocated deduped block, look its entry up by the
 * (unchanged) checksum and repoint the entry's phys DVAs + phys_birth to the
 * new block, preserving the refcount. ddt_lookup loads the entry into ddt_tree
 * so ddt_sync persists the change. After this the DDT references the new
 * blocks; the old blocks are then freed as PLAIN (dedup bit cleared, Pass 3) so
 * the drain does NOT decref the (now-migrated) entry.
 */
static void
zhack_bpr_ddt_remap_sync(void *arg, dmu_tx_t *tx)
{
	(void) tx;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	spa_t *spa = dp->dp_spa;
	for (zhack_bpr_memo_t *m = avl_first(&zhack_bpr_memo); m != NULL;
	    m = AVL_NEXT(&zhack_bpr_memo, m)) {
		if (!BP_GET_DEDUP(&m->bm_oldbp))
			continue;
		if (BP_IS_HOLE(&m->bm_oldbp) || BP_IS_EMBEDDED(&m->bm_oldbp))
			continue;
		ddt_t *ddt = ddt_select(spa, &m->bm_oldbp);
		if (ddt == NULL)
			continue;
		ddt_enter(ddt);
		ddt_entry_t *dde = ddt_lookup(ddt, &m->bm_oldbp, B_FALSE);
		if (dde != NULL && (dde->dde_flags & DDE_FLAG_LOADED) &&
		    !(dde->dde_flags & DDE_FLAG_OVERQUOTA)) {
			ddt_phys_variant_t v = ddt_phys_select(ddt, dde,
			    &m->bm_oldbp);
			if (v != DDT_PHYS_NONE) {
				boolean_t flat =
				    (ddt->ddt_flags & DDT_FLAG_FLAT);
				dva_t *dvas = flat ?
				    dde->dde_phys->ddp_flat.ddp_dva :
				    dde->dde_phys->ddp_trad[v].ddp_dva;
				for (int d = 0; d < SPA_DVAS_PER_BP; d++)
					dvas[d] = m->bm_newbp.blk_dva[d];
				if (flat)
					dde->dde_phys->ddp_flat.ddp_phys_birth =
					    BP_GET_BIRTH(&m->bm_newbp);
				else
					dde->dde_phys->
					    ddp_trad[v].ddp_phys_birth =
					    BP_GET_BIRTH(&m->bm_newbp);
				zhack_bpr_ddt_remapped++;
			}
		}
		ddt_exit(ddt);
	}
}

/*
 * P8b Pass 2.6: migrate the BRT (block-cloning reference table). BRT is keyed
 * by PHYSICAL DVA (vdev,offset), unlike the DDT (keyed by checksum). A cloned
 * block has one physical copy, M block pointers, and BRT bre_count = M-1 at its
 * DVA (the base reference is not counted). The rewrite-once memo repoints all M
 * tree BPs to the new DVA, but the new DVA is NOT in the BRT -- so on destroy
 * each of the M frees would physically free the SAME new block (zio_brt_free
 * sees no entry -> frees), a double-free that PANICS the metaslab. And the OLD
 * DVA keeps a stale bre_count. Migrate: establish the new DVA's bre_count = M-1
 * (brt_pending_add x (M-1)) and clear the old DVA's (brt_entry_decref x (M-1)).
 * Then on destroy the new block's M frees do M-1 decrefs + one physical free;
 * and Pass 3's single free of the OLD block (now bre_count 0) frees it exactly
 * once. BRT only tracks L0 data blocks (zio_brt_free skips level>0 / metadata),
 * so restrict to those.
 */
static void
zhack_bpr_brt_remap_sync(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	if (!spa_feature_is_active(spa, SPA_FEATURE_BLOCK_CLONING))
		return;
	for (zhack_bpr_memo_t *m = avl_first(&zhack_bpr_memo); m != NULL;
	    m = AVL_NEXT(&zhack_bpr_memo, m)) {
		if (BP_IS_HOLE(&m->bm_oldbp) || BP_IS_EMBEDDED(&m->bm_oldbp))
			continue;
		if (BP_GET_LEVEL(&m->bm_oldbp) > 0 ||
		    BP_IS_METADATA(&m->bm_oldbp))
			continue;
		if (!brt_maybe_exists(spa, &m->bm_oldbp))
			continue;
		uint64_t c = brt_entry_get_refcount(spa, &m->bm_oldbp);
		for (uint64_t i = 0; i < c; i++) {
			brt_pending_add(spa, &m->bm_newbp, tx);
			(void) brt_entry_decref(spa, &m->bm_oldbp);
		}
		if (c > 0)
			zhack_bpr_brt_remapped++;
	}
}

/*
 * Pass 3: reclaim the old (pre-promotion) blocks. Every tree BP and every
 * deadlist BP now points at the relocated copy, so each original block in the
 * memo is fully unreferenced and safe to free. We enqueue them on the pool's
 * deferred-free bpobj (dp_free_bpobj) so the block is freed through the
 * KERNEL's normal syncing free on the next import -- that path persists the
 * free to the on-disk spacemap (an offline zio_free from libzpool only updates
 * the in-core ms_allocatable, which is why earlier offline frees leaked).
 */
static void
zhack_bpr_free_sync(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	spa_t *spa = dp->dp_spa;
	for (zhack_bpr_memo_t *m = avl_first(&zhack_bpr_memo); m != NULL;
	    m = AVL_NEXT(&zhack_bpr_memo, m)) {
		if (BP_IS_HOLE(&m->bm_oldbp) || BP_IS_EMBEDDED(&m->bm_oldbp))
			continue;
		/*
		 * P8b: free the relocated OLD block as PLAIN. If it was a
		 * deduped block its DDT entry was already repointed to the new
		 * DVA (zhack_bpr_ddt_remap_sync), so freeing the old DVA must
		 * NOT go through ddt_phys_decref (that would decrement the
		 * migrated entry and could free the NEW block). Clearing the
		 * dedup bit routes the free through the plain metaslab path on
		 * the old DVA. Byte-neutral for non-dedup blocks (bit already
		 * 0).
		 */
		blkptr_t freebp = m->bm_oldbp;
		BP_SET_DEDUP(&freebp, 0);
		bpobj_enqueue(&dp->dp_free_bpobj, &freebp, B_FALSE, tx);
		/*
		 * The kernel's deferred-free drain (dsl_scan_free_block_cb)
		 * CREDITS dp_free_dir by the block's size when it frees it;
		 * charge dp_free_dir the same amount here so that credit
		 * balances (exactly how dsl_destroy charges dp_free_dir before
		 * enqueueing a block for async free).
		 */
		if (dp->dp_free_dir != NULL)
			dsl_dir_diduse_space(dp->dp_free_dir, DD_USED_HEAD,
			    bp_get_dsize_sync(spa, &m->bm_oldbp),
			    BP_GET_PSIZE(&m->bm_oldbp),
			    BP_GET_UCSIZE(&m->bm_oldbp), tx);
		zhack_bpr_freed++;
	}
}

/*
 * usage: zhack snap_bpr <pool> [snap]
 * Rewrite every snapshot's tree (or just <snap>) with a SHARED rewrite-once
 * memo, so blocks shared across the snapshot chain are relocated exactly once.
 */
static int
zhack_do_snap_bpr(int argc, char **argv)
{
	spa_t *spa;
	argc--; argv++;
	if (argc < 1)
		usage();
	zhack_spa_open(argv[0], B_FALSE, FTAG, &spa);
	avl_create(&zhack_bpr_memo, zhack_bpr_memo_cmp,
	    sizeof (zhack_bpr_memo_t), offsetof(zhack_bpr_memo_t, bm_link));
	zhack_bpr_rewritten = 0;
	zhack_bpr_errors = 0;
	zhack_bpr_alloc_bytes = 0;
	zhack_bpr_free_bytes = 0;
	nvlist_t *snaps = fnvlist_alloc();
	if (argc >= 2) {
		fnvlist_add_boolean(snaps, argv[1]);
	} else {
		(void) dmu_objset_find(argv[0], zhack_bpr_collect, snaps,
		    DS_FIND_SNAPSHOTS | DS_FIND_CHILDREN);
		/*
		 * S4 Part A: also process clones (live datasets with an
		 * origin).
		 */
		(void) dmu_objset_find(argv[0], zhack_bpr_collect_clone, snaps,
		    DS_FIND_CHILDREN);
	}
	/*
	 * Collect (obj, creation_txg) and process OLDEST-first: with the
	 * rewrite-once memo, the oldest snapshot referencing a shared block
	 * owns it, so dsl_dataset_block_kill's free-vs-deadlist decision (keyed
	 * on the OLDER neighbour) is correct.
	 */
	uint64_t objs[1024], crtxgs[1024];
	int nent = 0;
	for (nvpair_t *pair = nvlist_next_nvpair(snaps, NULL);
	    pair != NULL && nent < 1024;
	    pair = nvlist_next_nvpair(snaps, pair)) {
		dsl_pool_t *dp;
		if (dsl_pool_hold(argv[0], FTAG, &dp) != 0)
			continue;
		dsl_dataset_t *snap;
		if (dsl_dataset_hold(dp, nvpair_name(pair), FTAG, &snap) != 0) {
			dsl_pool_rele(dp, FTAG);
			continue;
		}
		objs[nent] = snap->ds_object;
		crtxgs[nent] = dsl_dataset_phys(snap)->ds_creation_txg;
		nent++;
		dsl_dataset_rele(snap, FTAG);
		dsl_pool_rele(dp, FTAG);
	}
	fnvlist_free(snaps);
	for (int i = 1; i < nent; i++) {
		uint64_t ko = objs[i], kc = crtxgs[i];
		int j = i - 1;
		while (j >= 0 && crtxgs[j] > kc) {
			objs[j + 1] = objs[j];
			crtxgs[j + 1] = crtxgs[j];
			j--;
		}
		objs[j + 1] = ko;
		crtxgs[j + 1] = kc;
	}
	zhack_bpr_dlremapped = 0;
	zhack_bpr_freed = 0;
	reparity_allow_snap_dirty = 1;
	/*
	 * Pass 1: relocate + re-encode every snapshot tree (populates the
	 * memo).
	 */
	for (int i = 0; i < nent; i++) {
		zhack_bpr_arg_t b;
		b.snap_obj = objs[i];
		(void) dsl_sync_task(spa_name(spa), NULL, zhack_snap_bpr_sync,
		    &b, 0, ZFS_SPACE_CHECK_NONE);
	}
	for (int i = 0; i < 4; i++)
		txg_wait_synced(spa_get_dsl(spa), 0);
	/*
	 * Pass 2: remap every snapshot's deadlist through the (now complete)
	 * memo, so no deadlist BP points at a relocated old block -- this is
	 * what makes `zfs destroy` of a promoted snapshot safe.
	 */
	for (int i = 0; i < nent; i++) {
		zhack_bpr_arg_t b;
		b.snap_obj = objs[i];
		(void) dsl_sync_task(spa_name(spa), NULL,
		    zhack_snap_dlremap_sync, &b, 0, ZFS_SPACE_CHECK_NONE);
	}
	for (int i = 0; i < 4; i++)
		txg_wait_synced(spa_get_dsl(spa), 0);
	/*
	 * Pass 2.5 (P8b): repoint the DDT so deduped blocks' dedup-table
	 * entries reference the relocated new DVAs (with the refcount
	 * preserved) BEFORE the old blocks are freed. No-op on non-deduped
	 * pools (the memo has no dedup-flagged old blocks).
	 */
	(void) dsl_sync_task(spa_name(spa), NULL, zhack_bpr_ddt_remap_sync,
	    NULL, 0, ZFS_SPACE_CHECK_NONE);
	for (int i = 0; i < 4; i++)
		txg_wait_synced(spa_get_dsl(spa), 0);
	/*
	 * Pass 2.6 (P8b): migrate the BRT (block-cloning) entries old-DVA ->
	 * relocated new-DVA, so cloned blocks stay refcounted at their new
	 * location (no double-free metaslab panic on destroy) and the old
	 * blocks free once. No-op if the block_cloning feature is inactive.
	 */
	(void) dsl_sync_task(spa_name(spa), NULL, zhack_bpr_brt_remap_sync,
	    NULL, 0, ZFS_SPACE_CHECK_NONE);
	for (int i = 0; i < 4; i++)
		txg_wait_synced(spa_get_dsl(spa), 0);
	/*
	 * Pass 3: enqueue every relocated old block on the deferred-free queue
	 * and let it drain HERE. We charge dp_free_dir on enqueue (so the
	 * drain's credit balances) and MUST let the drain run in this same
	 * context, otherwise dsl_process_async_destroys'
	 * VERIFY0(dp_free_dir->dd_used_bytes) aborts the sync thread. The drain
	 * frees each old block through zio_free; the metaslab frees are
	 * persisted by the txg_wait_synced sync that follows.
	 */
	(void) dsl_sync_task(spa_name(spa), NULL, zhack_bpr_free_sync,
	    NULL, 0, ZFS_SPACE_CHECK_NONE);
	for (int i = 0; i < 8; i++)
		txg_wait_synced(spa_get_dsl(spa), 0);
	reparity_allow_snap_dirty = 0;
	void *cookie = NULL;
	zhack_bpr_memo_t *mn;
	while ((mn = avl_destroy_nodes(&zhack_bpr_memo, &cookie)) != NULL)
		kmem_free(mn, sizeof (*mn));
	avl_destroy(&zhack_bpr_memo);
	(void) fprintf(stderr,
	    "snap_bpr: rewritten=%llu dlremapped=%llu freed=%llu "
	    "errors=%llu alloc=%lluK free=%lluK\n",
	    (u_longlong_t)zhack_bpr_rewritten,
	    (u_longlong_t)zhack_bpr_dlremapped,
	    (u_longlong_t)zhack_bpr_freed,
	    (u_longlong_t)zhack_bpr_errors,
	    (u_longlong_t)(zhack_bpr_alloc_bytes>>10),
	    (u_longlong_t)(zhack_bpr_free_bytes>>10));
	spa_close(spa, FTAG);
	return (0);
}

/*
 * ======== P7 part C: atomic in-libzpool raidz width contraction sweep ========
 */
extern int raidz_contracting;
extern uint64_t raidz_contract_floor;
extern uint64_t raidz_contract_ceiling;
extern uint8_t *raidz_contract_occmap;
extern uint64_t raidz_contract_occ_nsectors;
extern uint64_t raidz_contract_occ_nwidth;
extern uint64_t raidz_contract_occ_ashift;

static uint64_t zhack_contract_vdevid;
static uint8_t *zhack_occ;
static uint64_t zhack_occ_nsec, zhack_occ_owidth, zhack_occ_ashift;

/*
 * pre-pass: mark every original N-wide block's physical (child, depth) cells in
 * the occupancy bitmap, so the picker can later place re-encoded width-W blocks
 * in the physical gaps between them.
 */
static int
zhack_contract_mark_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	(void) spa; (void) zilog; (void) zb; (void) dnp; (void) arg;
	if (bp == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return (0);
	for (int di = 0; di < BP_GET_NDVAS(bp); di++) {
		const dva_t *dva = &bp->blk_dva[di];
		if (DVA_GET_VDEV(dva) != zhack_contract_vdevid)
			continue;
		uint64_t b = DVA_GET_OFFSET(dva) >> zhack_occ_ashift;
		uint64_t s = DVA_GET_ASIZE(dva) >> zhack_occ_ashift;
		uint64_t f = b % zhack_occ_owidth;
		uint64_t base_depth = b / zhack_occ_owidth;
		for (uint64_t c = 0; c < s; c++) {
			uint64_t col = f + c;
			uint64_t child = col % zhack_occ_owidth;
			uint64_t depth = base_depth + col / zhack_occ_owidth;
			if (depth >= zhack_occ_nsec)
				continue;
			uint64_t bit = child * zhack_occ_nsec + depth;
			zhack_occ[bit >> 3] |= (1 << (bit & 7));
		}
	}
	return (0);
}

/*
 * usage: zhack raidz_contract <pool> <vdev_guid> <target_width> <dataset>...
 * ATOMIC in-place raidz width contraction. In one libzpool session (no import
 * boundary -> no expanded/simple map race): (1) pre-pass to find OFF_HW, the
 * old-width offset high-water; (2) arm the routing (raidz_contracting) + the
 * allocation window [OFF_HW, W*child_asize) so every re-encoded narrow block
 * lands physically ABOVE all wide blocks (disjoint by construction) and
 * on-device; (3) append a width epoch (births >= T use target_width); (4) sweep
 * every MOS + dataset block via dmu_buf_will_rewrite -> re-encoded to width W.
 * physical_width stays N; a later `zpool detach` of the now-empty last child
 * (part D) completes the shrink. Import the result with raidz_contracting=1.
 */
static int
zhack_do_raidz_contract(int argc, char **argv)
{
	spa_t *spa;
	uint64_t vguid, target_width;
	argc--; argv++;
	if (argc < 3)
		fatal(NULL, FTAG, "usage: raidz_contract <pool> <vdev_guid> "
		    "<target_width> <dataset>...");
	if (!zhack_strict_u64(argv[1], &vguid))
		fatal(NULL, FTAG, "invalid vdev guid: %s", argv[1]);
	if (!zhack_strict_u64(argv[2], &target_width))
		fatal(NULL, FTAG, "invalid target width: %s", argv[2]);
	zhack_spa_open(argv[0], B_FALSE, FTAG, &spa);
	vdev_t *vd = vdev_lookup_by_guid(spa->spa_root_vdev, vguid);
	if (vd == NULL || vd->vdev_ops != &vdev_raidz_ops)
		fatal(spa, FTAG, "vdev guid %llu is not a raidz vdev",
		    (u_longlong_t)vguid);
	vdev_raidz_t *vdrz = vd->vdev_tsd;
	if ((int)target_width < vdrz->vd_nparity + 1 ||
	    (int)target_width >= vdrz->vd_physical_width)
		fatal(spa, FTAG, "target_width %llu must be in [nparity+1, "
		    "physical_width=%d)", (u_longlong_t)target_width,
		    vdrz->vd_physical_width);

	uint64_t ashift = vd->vdev_top->vdev_ashift;
	uint64_t child_asize = vd->vdev_child[0]->vdev_asize;
	uint64_t nsec = child_asize >> ashift;
	uint64_t N = (uint64_t)vdrz->vd_physical_width;
	uint64_t occbytes = (N * nsec + 7) / 8;
	uint8_t *occ = malloc(occbytes);
	if (occ == NULL)
		fatal(spa, FTAG, "out of memory for occmap (%llu bytes)",
		    (u_longlong_t)occbytes);
	memset(occ, 0, occbytes);

	/* pre-pass: build the physical occupancy bitmap of all N-wide blocks */
	zhack_contract_vdevid = vd->vdev_id;
	zhack_occ = occ; zhack_occ_nsec = nsec;
	zhack_occ_owidth = N; zhack_occ_ashift = ashift;
	int perr = traverse_pool(spa, 0, TRAVERSE_PRE,
	    zhack_contract_mark_cb, NULL);
	if (perr != 0) {
		free(occ);
		fatal(spa, FTAG, "pre-pass traverse error %d", perr);
	}
	(void) fprintf(stderr, "raidz_contract: occmap built (nsec/child=%llu "
	    "N=%llu W=%llu ceiling=%llu bytes=%llu)\n", (u_longlong_t)nsec,
	    (u_longlong_t)N, (u_longlong_t)target_width,
	    (u_longlong_t)(target_width * child_asize),
	    (u_longlong_t)occbytes);

	/*
	 * Set the width epoch FIRST, while contracting/occmap are still OFF, so
	 * the epoch-set txg's own writes are clean width-N (the width-W gap
	 * check would mis-place them). T = last_synced + 2: that epoch-set txg
	 * (< T) stays width N, and every later sweep txg (>= T) is width W with
	 * the epoch already committed -- no within-txg width ambiguity (write
	 * width == read width for every block, both keyed on physical birth).
	 */
	uint64_t T = spa_last_synced_txg(spa) + 2;
	uint64_t tbl[6] = { 0, (uint64_t)vdrz->vd_physical_width,
	    (uint64_t)vdrz->vd_nparity, T, target_width,
	    (uint64_t)vdrz->vd_nparity };
	zhack_repochs_t ra;
	ra.re_vdev_id = vd->vdev_id;
	ra.re_entries = 2;
	ra.re_table = tbl;
	VERIFY0(dsl_sync_task(spa_name(spa), NULL, zhack_raidz_epochs_sync,
	    &ra, 5, ZFS_SPACE_CHECK_NORMAL));
	txg_wait_synced(spa_get_dsl(spa), 0);

	/* NOW arm the physical-gap allocator + routing for the width-W sweep */
	raidz_contract_occ_nsectors = nsec;
	raidz_contract_occ_nwidth = target_width;
	raidz_contract_occ_ashift = ashift;
	raidz_contract_occmap = occ;
	/*
	 * Align the ceiling DOWN to a metaslab boundary so ALL re-encoded data
	 * sits below the last FULL metaslab. A later shrink then drops only
	 * whole-empty high metaslabs [W*child>>ms_shift, N); dropping a
	 * metaslab that straddles the ceiling (still holding valid data) panics
	 * metaslab_check_free_impl on the reimport free path.
	 */
	{
		uint64_t ceil = target_width * child_asize;
		if (vd->vdev_ms_shift != 0)
			ceil = (ceil >> vd->vdev_ms_shift) << vd->vdev_ms_shift;
		raidz_contract_ceiling = ceil;
		(void) fprintf(stderr, "raidz_contract: ceiling_aligned=%llu\n",
		    (u_longlong_t)ceil);
	}
	raidz_contracting = 1;

	zhack_mos_cursor = 0; zhack_mos_dirtied = 0; zhack_mos_errors = 0;
	do {
		VERIFY0(dsl_sync_task(spa_name(spa), NULL,
		    zhack_reparity_mos_sync, NULL, 128,
		    ZFS_SPACE_CHECK_NORMAL));
	} while (zhack_mos_more);
	if (zhack_mos_errors != 0) {
		raidz_contracting = 0;
		raidz_contract_occmap = NULL;
		free(occ);
		fatal(spa, FTAG, "MOS sweep errors=%llu",
		    (u_longlong_t)zhack_mos_errors);
	}
	dmu_objset_register_type(DMU_OST_ZFS, zhack_get_file_info);
	for (int i = 3; i < argc; i++) {
		objset_t *os;
		if (dmu_objset_own(argv[i], DMU_OST_ZFS, B_FALSE, B_FALSE,
		    FTAG, &os) != 0)
			continue;
		(void) zhack_reparity_objset(os);
		zil_destroy(dmu_objset_zil(os), B_FALSE);
		txg_wait_synced(dmu_objset_pool(os), 0);
		dmu_objset_disown(os, B_FALSE, FTAG);
	}
	txg_wait_synced(spa_get_dsl(spa), 0);
	(void) fprintf(stderr, "raidz_contract: DONE T=%llu MOS dirtied=%llu "
	    "(import with raidz_contracting=1 + ceiling, then detach last "
	    "child)\n",
	    (u_longlong_t)T, (u_longlong_t)zhack_mos_dirtied);
	spa_close(spa, FTAG);
	/*
	 * Export NOW, while routing + the gap allocator are still armed, so the
	 * final export sync writes the config/uberblock/objset-root as width-W
	 * and gap-placed. Otherwise libzpool's process-exit auto-export syncs
	 * them with contracting OFF -> the objset root is written width-W but
	 * with the EXPANDED map at an unconstrained (5-wide) offset, then
	 * re-read with the simple 4-wide map off-device -> import EIO
	 * (dsl_pool_init error=5).
	 */
	(void) spa_export(argv[0], NULL, B_TRUE, B_FALSE);
	raidz_contracting = 0;
	raidz_contract_occmap = NULL;
	raidz_contract_occ_nsectors = 0;
	raidz_contract_ceiling = 0;
	free(occ);
	return (0);
}

static int
zhack_do_raidz_epochs(int argc, char **argv)
{
	char *target;
	spa_t *spa;
	zhack_repochs_t ra;

	argc--;
	argv++;

	if (argc < 3) {
		(void) fprintf(stderr,
		    "error: raidz_epochs needs <pool> <top-vdev-id> "
		    "<start:width:parity>...\n");
		usage();
	}
	target = argv[0];
	ra.re_vdev_id = strtoull(argv[1], NULL, 10);
	ra.re_entries = argc - 2;
	ra.re_table = malloc(ra.re_entries * 3 * sizeof (uint64_t));
	if (ra.re_table == NULL)
		fatal(NULL, FTAG, "out of memory");
	for (uint64_t i = 0; i < ra.re_entries; i++) {
		u_longlong_t start, width, parity;

		if (sscanf(argv[2 + i], "%llu:%llu:%llu",
		    &start, &width, &parity) != 3)
			fatal(NULL, FTAG, "bad triplet: %s", argv[2 + i]);
		ra.re_table[3 * i] = start;
		ra.re_table[3 * i + 1] = width;
		ra.re_table[3 * i + 2] = parity;
	}

	zhack_spa_open(target, B_FALSE, FTAG, &spa);
	if (!spa_feature_is_enabled(spa, SPA_FEATURE_RAIDZ_PARITY_EPOCHS))
		fatal(spa, FTAG,
		    "feature@raidz_parity_epochs is not enabled on %s",
		    target);
	VERIFY0(dsl_sync_task(spa_name(spa), NULL, zhack_raidz_epochs_sync,
	    &ra, 5, ZFS_SPACE_CHECK_NORMAL));
	spa_close(spa, FTAG);
	free(ra.re_table);
	return (0);
}

static int
zhack_do_metaslab(int argc, char **argv)
{
	char *subcommand;

	argc--;
	argv++;
	if (argc == 0) {
		(void) fprintf(stderr,
		    "error: no metaslab operation specified\n");
		usage();
	}

	subcommand = argv[0];
	if (strcmp(subcommand, "leak") == 0) {
		zhack_do_metaslab_leak(argc, argv);
	} else {
		(void) fprintf(stderr, "error: unknown subcommand: %s\n",
		    subcommand);
		usage();
	}

	return (0);
}

#define	ASHIFT_UBERBLOCK_SHIFT(ashift)	\
	MIN(MAX(ashift, UBERBLOCK_SHIFT), \
	MAX_UBERBLOCK_SHIFT)
#define	ASHIFT_UBERBLOCK_SIZE(ashift) \
	(1ULL << ASHIFT_UBERBLOCK_SHIFT(ashift))

#define	REPAIR_LABEL_STATUS_CKSUM (1 << 0)
#define	REPAIR_LABEL_STATUS_UB    (1 << 1)

static int
zhack_repair_read_label(const int fd, vdev_label_t *vl,
    const uint64_t label_offset, const int l)
{
	const int err = pread64(fd, vl, sizeof (vdev_label_t), label_offset);

	if (err == -1) {
		(void) fprintf(stderr,
		    "error: cannot read label %d: %s\n",
		    l, strerror(errno));
		return (err);
	} else if (err != sizeof (vdev_label_t)) {
		(void) fprintf(stderr,
		    "error: bad label %d read size\n", l);
		return (err);
	}

	return (0);
}

static int
zhack_repair_get_byteswap(const zio_eck_t *vdev_eck, const int l, int *byteswap)
{
	if (vdev_eck->zec_magic == ZEC_MAGIC) {
		*byteswap = B_FALSE;
	} else if (vdev_eck->zec_magic == BSWAP_64((uint64_t)ZEC_MAGIC)) {
		*byteswap = B_TRUE;
	} else {
		(void) fprintf(stderr, "error: label %d: "
		    "Expected the nvlist checksum magic number but instead got "
		    "0x%" PRIx64 "\n",
		    l, vdev_eck->zec_magic);
		return (1);
	}
	return (0);
}

static void
zhack_repair_calc_cksum(const int byteswap, void *data, const uint64_t offset,
    const uint64_t abdsize, zio_eck_t *eck, zio_cksum_t *cksum)
{
	zio_cksum_t verifier;
	zio_cksum_t current_cksum;
	zio_checksum_info_t *ci;
	abd_t *abd;

	ZIO_SET_CHECKSUM(&verifier, offset, 0, 0, 0);

	if (byteswap)
		byteswap_uint64_array(&verifier, sizeof (zio_cksum_t));

	current_cksum = eck->zec_cksum;
	eck->zec_cksum = verifier;

	ci = &zio_checksum_table[ZIO_CHECKSUM_LABEL];
	abd = abd_get_from_buf(data, abdsize);
	ci->ci_func[byteswap](abd, abdsize, NULL, cksum);
	abd_free(abd);

	eck->zec_cksum = current_cksum;
}

static int
zhack_repair_get_ashift(nvlist_t *cfg, const int l, uint64_t *ashift)
{
	int err;
	nvlist_t *vdev_tree_cfg;

	err = nvlist_lookup_nvlist(cfg,
	    ZPOOL_CONFIG_VDEV_TREE, &vdev_tree_cfg);
	if (err) {
		(void) fprintf(stderr,
		    "error: label %d: cannot find nvlist key %s\n",
		    l, ZPOOL_CONFIG_VDEV_TREE);
		return (err);
	}

	err = nvlist_lookup_uint64(vdev_tree_cfg,
	    ZPOOL_CONFIG_ASHIFT, ashift);
	if (err) {
		(void) fprintf(stderr,
		    "error: label %d: cannot find nvlist key %s\n",
		    l, ZPOOL_CONFIG_ASHIFT);
		return (err);
	}

	if (*ashift == 0) {
		(void) fprintf(stderr,
		    "error: label %d: nvlist key %s is zero\n",
		    l, ZPOOL_CONFIG_ASHIFT);
		return (1);
	}

	return (0);
}

static int
zhack_repair_undetach(uberblock_t *ub, nvlist_t *cfg, const int l)
{
	/*
	 * Uberblock root block pointer has valid birth TXG.
	 * Copying it to the label NVlist
	 */
	if (BP_GET_LOGICAL_BIRTH(&ub->ub_rootbp) != 0) {
		const uint64_t txg = BP_GET_LOGICAL_BIRTH(&ub->ub_rootbp);
		int err;

		ub->ub_txg = txg;

		err = nvlist_remove_all(cfg, ZPOOL_CONFIG_CREATE_TXG);
		if (err) {
			(void) fprintf(stderr,
			    "error: label %d: "
			    "Failed to remove pool creation TXG\n",
			    l);
			return (err);
		}

		err = nvlist_remove_all(cfg, ZPOOL_CONFIG_POOL_TXG);
		if (err) {
			(void) fprintf(stderr,
			    "error: label %d: Failed to remove pool TXG to "
			    "be replaced.\n",
			    l);
			return (err);
		}

		err = nvlist_add_uint64(cfg, ZPOOL_CONFIG_POOL_TXG, txg);
		if (err) {
			(void) fprintf(stderr,
			    "error: label %d: "
			    "Failed to add pool TXG of %" PRIu64 "\n",
			    l, txg);
			return (err);
		}
	}

	return (0);
}

static boolean_t
zhack_repair_write_label(const int l, const int fd, const int byteswap,
    void *data, zio_eck_t *eck, const uint64_t offset, const uint64_t abdsize)
{
	zio_cksum_t actual_cksum;
	zhack_repair_calc_cksum(byteswap, data, offset, abdsize, eck,
	    &actual_cksum);
	zio_cksum_t expected_cksum = eck->zec_cksum;
	ssize_t err;

	if (ZIO_CHECKSUM_EQUAL(actual_cksum, expected_cksum))
		return (B_FALSE);

	eck->zec_cksum = actual_cksum;

	err = pwrite64(fd, data, abdsize, offset);
	if (err == -1) {
		(void) fprintf(stderr, "error: cannot write label %d: %s\n",
		    l, strerror(errno));
		return (B_FALSE);
	} else if (err != abdsize) {
		(void) fprintf(stderr, "error: bad write size label %d\n", l);
		return (B_FALSE);
	} else {
		(void) fprintf(stderr,
		    "label %d: wrote %" PRIu64 " bytes at offset %" PRIu64 "\n",
		    l, abdsize, offset);
	}

	return (B_TRUE);
}

static void
zhack_repair_write_uberblock(vdev_label_t *vl, const int l,
    const uint64_t ashift, const int fd, const int byteswap,
    const uint64_t label_offset, uint32_t *labels_repaired)
{
	void *ub_data =
	    (char *)vl + offsetof(vdev_label_t, vl_uberblock);
	zio_eck_t *ub_eck =
	    (zio_eck_t *)
	    ((char *)(ub_data) + (ASHIFT_UBERBLOCK_SIZE(ashift))) - 1;

	if (ub_eck->zec_magic != 0) {
		(void) fprintf(stderr,
		    "error: label %d: "
		    "Expected Uberblock checksum magic number to "
		    "be 0, but got %" PRIu64 "\n",
		    l, ub_eck->zec_magic);
		(void) fprintf(stderr, "It would appear there's already "
		    "a checksum for the uberblock.\n");
		return;
	}


	ub_eck->zec_magic = byteswap ? BSWAP_64(ZEC_MAGIC) : ZEC_MAGIC;

	if (zhack_repair_write_label(l, fd, byteswap,
	    ub_data, ub_eck,
	    label_offset + offsetof(vdev_label_t, vl_uberblock),
	    ASHIFT_UBERBLOCK_SIZE(ashift)))
			labels_repaired[l] |= REPAIR_LABEL_STATUS_UB;
}

static void
zhack_repair_print_cksum(FILE *stream, const zio_cksum_t *cksum)
{
	(void) fprintf(stream,
	    "%016llx:%016llx:%016llx:%016llx",
	    (u_longlong_t)cksum->zc_word[0],
	    (u_longlong_t)cksum->zc_word[1],
	    (u_longlong_t)cksum->zc_word[2],
	    (u_longlong_t)cksum->zc_word[3]);
}

static int
zhack_repair_test_cksum(const int byteswap, void *vdev_data,
    zio_eck_t *vdev_eck, const uint64_t vdev_phys_offset, const int l)
{
	const zio_cksum_t expected_cksum = vdev_eck->zec_cksum;
	zio_cksum_t actual_cksum;
	zhack_repair_calc_cksum(byteswap, vdev_data, vdev_phys_offset,
	    VDEV_PHYS_SIZE, vdev_eck, &actual_cksum);
	const uint64_t expected_magic = byteswap ?
	    BSWAP_64(ZEC_MAGIC) : ZEC_MAGIC;
	const uint64_t actual_magic = vdev_eck->zec_magic;
	int err = 0;

	if (actual_magic != expected_magic) {
		(void) fprintf(stderr, "error: label %d: "
		    "Expected "
		    "the nvlist checksum magic number to not be %"
		    PRIu64 " not %" PRIu64 "\n",
		    l, expected_magic, actual_magic);
		err = ECKSUM;
	}
	if (!ZIO_CHECKSUM_EQUAL(actual_cksum, expected_cksum)) {
		(void) fprintf(stderr, "error: label %d: "
		    "Expected the nvlist checksum to be ", l);
		(void) zhack_repair_print_cksum(stderr,
		    &expected_cksum);
		(void) fprintf(stderr, " not ");
		zhack_repair_print_cksum(stderr, &actual_cksum);
		(void) fprintf(stderr, "\n");
		err = ECKSUM;
	}
	return (err);
}

static int
zhack_repair_unpack_cfg(vdev_label_t *vl, const int l, nvlist_t **cfg)
{
	const char *cfg_keys[] = { ZPOOL_CONFIG_VERSION,
	    ZPOOL_CONFIG_POOL_STATE, ZPOOL_CONFIG_GUID };
	int err;

	err = nvlist_unpack(vl->vl_vdev_phys.vp_nvlist,
	    VDEV_PHYS_SIZE - sizeof (zio_eck_t), cfg, 0);
	if (err) {
		(void) fprintf(stderr,
		    "error: cannot unpack nvlist label %d\n", l);
		return (err);
	}

	for (int i = 0; i < ARRAY_SIZE(cfg_keys); i++) {
		uint64_t val;
		err = nvlist_lookup_uint64(*cfg, cfg_keys[i], &val);
		if (err) {
			(void) fprintf(stderr,
			    "error: label %d, %d: "
			    "cannot find nvlist key %s\n",
			    l, i, cfg_keys[i]);
			return (err);
		}
	}

	return (0);
}

static void
zhack_repair_one_label(const zhack_repair_op_t op, const int fd,
    vdev_label_t *vl, const uint64_t label_offset, const int l,
    uint32_t *labels_repaired)
{
	ssize_t err;
	uberblock_t *ub = (uberblock_t *)vl->vl_uberblock;
	void *vdev_data =
	    (char *)vl + offsetof(vdev_label_t, vl_vdev_phys);
	zio_eck_t *vdev_eck =
	    (zio_eck_t *)((char *)(vdev_data) + VDEV_PHYS_SIZE) - 1;
	const uint64_t vdev_phys_offset =
	    label_offset + offsetof(vdev_label_t, vl_vdev_phys);
	nvlist_t *cfg;
	uint64_t ashift;
	int byteswap;

	err = zhack_repair_read_label(fd, vl, label_offset, l);
	if (err)
		return;

	err = zhack_repair_get_byteswap(vdev_eck, l, &byteswap);
	if (err)
		return;

	if (byteswap) {
		byteswap_uint64_array(&vdev_eck->zec_cksum,
		    sizeof (zio_cksum_t));
		vdev_eck->zec_magic = BSWAP_64(vdev_eck->zec_magic);
	}

	if ((op & ZHACK_REPAIR_OP_CKSUM) == 0 &&
	    zhack_repair_test_cksum(byteswap, vdev_data, vdev_eck,
	    vdev_phys_offset, l) != 0) {
		(void) fprintf(stderr, "It would appear checksums are "
		    "corrupted. Try zhack repair label -c <device>\n");
		return;
	}

	err = zhack_repair_unpack_cfg(vl, l, &cfg);
	if (err)
		return;

	if ((op & ZHACK_REPAIR_OP_UNDETACH) != 0) {
		char *buf;
		size_t buflen;

		if (ub->ub_txg != 0) {
			(void) fprintf(stderr,
			    "error: label %d: UB TXG of 0 expected, but got %"
			    PRIu64 "\n", l, ub->ub_txg);
			(void) fprintf(stderr, "It would appear the device was "
			    "not properly detached.\n");
			return;
		}

		err = zhack_repair_get_ashift(cfg, l, &ashift);
		if (err)
			return;

		err = zhack_repair_undetach(ub, cfg, l);
		if (err)
			return;

		buf = vl->vl_vdev_phys.vp_nvlist;
		buflen = VDEV_PHYS_SIZE - sizeof (zio_eck_t);
		if (nvlist_pack(cfg, &buf, &buflen, NV_ENCODE_XDR, 0) != 0) {
			(void) fprintf(stderr,
			    "error: label %d: Failed to pack nvlist\n", l);
			return;
		}

		zhack_repair_write_uberblock(vl,
		    l, ashift, fd, byteswap, label_offset, labels_repaired);
	}

	if (zhack_repair_write_label(l, fd, byteswap, vdev_data, vdev_eck,
	    vdev_phys_offset, VDEV_PHYS_SIZE))
			labels_repaired[l] |= REPAIR_LABEL_STATUS_CKSUM;

	fsync(fd);
}

static const char *
zhack_repair_label_status(const uint32_t label_status,
    const uint32_t to_check)
{
	return ((label_status & to_check) != 0 ? "repaired" : "skipped");
}

static int
zhack_label_repair(const zhack_repair_op_t op, const int argc, char **argv)
{
	uint32_t labels_repaired[VDEV_LABELS] = {0};
	vdev_label_t labels[VDEV_LABELS] = {{{0}}};
	struct stat64 st;
	int fd;
	off_t filesize;
	uint32_t repaired = 0;

	abd_init();

	if (argc < 1) {
		(void) fprintf(stderr, "error: missing device\n");
		usage();
	}

	if ((fd = open(argv[0], O_RDWR)) == -1)
		fatal(NULL, FTAG, "cannot open '%s': %s", argv[0],
		    strerror(errno));

	if (fstat64_blk(fd, &st) != 0)
		fatal(NULL, FTAG, "cannot stat '%s': %s", argv[0],
		    strerror(errno));

	filesize = st.st_size;
	(void) fprintf(stderr, "Calculated filesize to be %jd\n",
	    (intmax_t)filesize);

	if (filesize % sizeof (vdev_label_t) != 0)
		filesize =
		    (filesize / sizeof (vdev_label_t)) * sizeof (vdev_label_t);

	for (int l = 0; l < VDEV_LABELS; l++) {
		zhack_repair_one_label(op, fd, &labels[l],
		    vdev_label_offset(filesize, l, 0), l, labels_repaired);
	}

	close(fd);

	abd_fini();

	for (int l = 0; l < VDEV_LABELS; l++) {
		const uint32_t lr = labels_repaired[l];
		(void) printf("label %d: ", l);
		(void) printf("uberblock: %s ",
		    zhack_repair_label_status(lr, REPAIR_LABEL_STATUS_UB));
		(void) printf("checksum: %s\n",
		    zhack_repair_label_status(lr, REPAIR_LABEL_STATUS_CKSUM));
		repaired |= lr;
	}

	if (repaired > 0)
		return (0);

	return (1);
}

static int
zhack_do_label_repair(int argc, char **argv)
{
	zhack_repair_op_t op = ZHACK_REPAIR_OP_UNKNOWN;
	int c;

	optind = 1;
	while ((c = getopt(argc, argv, "+cu")) != -1) {
		switch (c) {
		case 'c':
			op |= ZHACK_REPAIR_OP_CKSUM;
			break;
		case 'u':
			op |= ZHACK_REPAIR_OP_UNDETACH;
			break;
		default:
			usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (op == ZHACK_REPAIR_OP_UNKNOWN)
		op = ZHACK_REPAIR_OP_CKSUM;

	return (zhack_label_repair(op, argc, argv));
}

static int
zhack_do_label(int argc, char **argv)
{
	char *subcommand;
	int err;

	argc--;
	argv++;
	if (argc == 0) {
		(void) fprintf(stderr,
		    "error: no label operation specified\n");
		usage();
	}

	subcommand = argv[0];
	if (strcmp(subcommand, "repair") == 0) {
		err = zhack_do_label_repair(argc, argv);
	} else {
		(void) fprintf(stderr, "error: unknown subcommand: %s\n",
		    subcommand);
		usage();
	}

	return (err);
}

#define	MAX_NUM_PATHS 1024

int
main(int argc, char **argv)
{
	struct sigaction action;
	char *path[MAX_NUM_PATHS];
	const char *subcommand;
	int rv = 0;
	int c;

	/*
	 * Set up signal handlers, so if we crash due to bad on-disk data we
	 * can get more info. Unlike ztest, we don't bail out if we can't set
	 * up signal handlers, because zhack is very useful without them.
	 */
	action.sa_handler = sig_handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	if (sigaction(SIGSEGV, &action, NULL) < 0) {
		(void) fprintf(stderr, "zhack: cannot catch SIGSEGV: %s\n",
		    strerror(errno));
	}
	if (sigaction(SIGABRT, &action, NULL) < 0) {
		(void) fprintf(stderr, "zhack: cannot catch SIGABRT: %s\n",
		    strerror(errno));
	}

	g_importargs.path = path;

	dprintf_setup(&argc, argv);
	zfs_prop_init();

	while ((c = getopt(argc, argv, "+c:d:Go:")) != -1) {
		switch (c) {
		case 'c':
			g_importargs.cachefile = optarg;
			break;
		case 'd':
			assert(g_importargs.paths < MAX_NUM_PATHS);
			g_importargs.path[g_importargs.paths++] = optarg;
			break;
		case 'G':
			g_dump_dbgmsg = B_TRUE;
			break;
		case 'o':
			if (handle_tunable_option(optarg, B_FALSE) != 0)
				exit(1);
			break;
		default:
			usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;
	optind = 1;

	if (argc == 0) {
		(void) fprintf(stderr, "error: no command specified\n");
		usage();
	}

	subcommand = argv[0];

	if (strcmp(subcommand, "action") == 0) {
		rv = zhack_do_action(argc, argv);
	} else if (strcmp(subcommand, "feature") == 0) {
		rv = zhack_do_feature(argc, argv);
	} else if (strcmp(subcommand, "mmp") == 0) {
		rv = zhack_do_mmp(argc, argv);
	} else if (strcmp(subcommand, "label") == 0) {
		return (zhack_do_label(argc, argv));
	} else if (strcmp(subcommand, "metaslab") == 0) {
		rv = zhack_do_metaslab(argc, argv);
	} else if (strcmp(subcommand, "reparity") == 0) {
		rv = zhack_do_reparity(argc, argv);
	} else if (strcmp(subcommand, "reparity_commit") == 0) {
		rv = zhack_do_reparity_commit(argc, argv);
	} else if (strcmp(subcommand, "reparity_commit_raw") == 0) {
		rv = zhack_do_reparity_commit_raw(argc, argv);
	} else if (strcmp(subcommand, "snap_repoint") == 0) {
		rv = zhack_do_snap_repoint(argc, argv);
	} else if (strcmp(subcommand, "snap_reanchor") == 0) {
		rv = zhack_do_snap_reanchor(argc, argv);
	} else if (strcmp(subcommand, "snap_bpr") == 0) {
		rv = zhack_do_snap_bpr(argc, argv);
	} else if (strcmp(subcommand, "raidz_contract") == 0) {
		rv = zhack_do_raidz_contract(argc, argv);
	} else if (strcmp(subcommand, "normalize_verify") == 0) {
		rv = zhack_do_normalize_verify(argc, argv);
	} else if (strcmp(subcommand, "placement_verify") == 0) {
		rv = zhack_do_placement_verify(argc, argv);
	} else if (strcmp(subcommand, "raidz_epochs") == 0) {
		rv = zhack_do_raidz_epochs(argc, argv);
	} else {
		(void) fprintf(stderr, "error: unknown subcommand: %s\n",
		    subcommand);
		usage();
	}

	if (!g_readonly && spa_export(g_pool, NULL, B_TRUE, B_FALSE) != 0) {
		fatal(NULL, FTAG, "pool export failed; "
		    "changes may not be committed to disk\n");
	}

	if (g_dump_dbgmsg)
		dump_debug_buffer();

	kernel_fini();

	return (rv);
}
