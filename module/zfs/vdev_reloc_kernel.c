/* SPDX-License-Identifier: CDDL-1.0 */
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */
/* Private kernel driver: each SPA owns its conversion job and progress. */
#ifdef _KERNEL
#include <sys/zfs_context.h>
#include <sys/dsl_scan.h>
#include <sys/dmu.h>
#include <sys/dmu_traverse.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_pool.h>

#define	ENGINE_KERNEL_STATE 16
#define	ENGINE_KERNEL_R02_MANIFEST_STATE 512

/*
 * A worker retains this object after its final spa_close().  The engine owns
 * one reference while its SPA exists; every worker owns one more.  Thus an
 * unload waiter can observe completion only after the worker's SPA reference
 * is gone, without dereferencing the per-SPA engine after that close.
 */
typedef struct engine_kernel_join {
	kmutex_t lock;
	kcondvar_t cv;
	uint64_t refs;
	uint64_t workers;
} engine_kernel_join_t;

typedef struct engine_kernel {
	kmutex_t lock;
	char state[ENGINE_KERNEL_STATE];
	uint64_t parity;
	uint64_t batch;
	uint64_t birth;
	int error;
	boolean_t running;
	pr19262_engine_convert_stats_t stats;
	uint64_t floor;
	uint64_t reclaimed;
	uint64_t released;
	engine_kernel_join_t *join;
	boolean_t unloading;
	boolean_t exporting;
	uint_t zil_teardowns;
} engine_kernel_t;

/* A legacy parameter selects a pool to observe, never a worker to own. */
static struct {
	kmutex_t lock;
	char pool[ZFS_MAX_DATASET_NAME_LEN];
	char cached_status[PAGE_SIZE];
	char r02_manifest[ENGINE_KERNEL_R02_MANIFEST_STATE];
	spa_t *r01_spa;
	uint64_t r01_users;
	boolean_t r01_detaching;
	kcondvar_t r01_cv;
	char denominator[PAGE_SIZE];
} engine_selector;

static engine_kernel_join_t *
engine_kernel_join_alloc(void)
{
	engine_kernel_join_t *join = kmem_zalloc(sizeof (*join), KM_SLEEP);

	mutex_init(&join->lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&join->cv, NULL, CV_DEFAULT, NULL);
	join->refs = 1;
	return (join);
}

static void
engine_kernel_join_rele(engine_kernel_join_t *join)
{
	boolean_t free = B_FALSE;

	mutex_enter(&join->lock);
	VERIFY3U(join->refs, >, 0);
	if (--join->refs == 0) {
		VERIFY0(join->workers);
		free = B_TRUE;
	}
	mutex_exit(&join->lock);
	if (free) {
		cv_destroy(&join->cv);
		mutex_destroy(&join->lock);
		kmem_free(join, sizeof (*join));
	}
}

static void
engine_kernel_join_worker_hold(engine_kernel_join_t *join)
{
	mutex_enter(&join->lock);
	join->refs++;
	join->workers++;
	mutex_exit(&join->lock);
}

static void
engine_kernel_join_worker_rele(engine_kernel_join_t *join)
{
	mutex_enter(&join->lock);
	VERIFY3U(join->workers, >, 0);
	join->workers--;
	cv_broadcast(&join->cv);
	mutex_exit(&join->lock);
	engine_kernel_join_rele(join);
}

/* Caller owns the SPA and may therefore dereference engine->kernel. */
static void
engine_kernel_join_wait(engine_kernel_t *job)
{
	engine_kernel_join_t *join = job->join;

	mutex_enter(&join->lock);
	while (join->workers != 0)
		cv_wait(&join->cv, &join->lock);
	mutex_exit(&join->lock);
}

/* Only spa_unload sets this permanent-for-this-instance admission gate. */
static void
engine_kernel_unload_begin(engine_kernel_t *job)
{
	mutex_enter(&job->lock);
	job->unloading = B_TRUE;
	mutex_exit(&job->lock);
	engine_kernel_join_wait(job);
}

/* Export temporarily closes R-01 admission until async work is suspended. */
static void
engine_kernel_export_begin(spa_t *spa)
{
	engine_kernel_t *job = spa->spa_reloc->kernel;

	mutex_enter(&job->lock);
	job->exporting = B_TRUE;
	mutex_exit(&job->lock);
}

/* An unsuccessful export leaves this per-SPA job available for R-01 again. */
static void
engine_kernel_export_abort(spa_t *spa)
{
	engine_kernel_t *job = spa->spa_reloc->kernel;

	mutex_enter(&job->lock);
	if (!job->unloading)
		job->exporting = B_FALSE;
	mutex_exit(&job->lock);
}

/* A zfsvfs teardown closes this short R-01 admission window around ZIL. */
static void
engine_kernel_zil_begin(spa_t *spa)
{
	engine_kernel_t *job = spa->spa_reloc->kernel;

	mutex_enter(&job->lock);
	VERIFY3U(job->zil_teardowns, <, UINT_MAX);
	job->zil_teardowns++;
	mutex_exit(&job->lock);
}

static void
engine_kernel_zil_finish(spa_t *spa)
{
	engine_kernel_t *job = spa->spa_reloc->kernel;

	mutex_enter(&job->lock);
	VERIFY3U(job->zil_teardowns, >, 0);
	job->zil_teardowns--;
	mutex_exit(&job->lock);
}

/* Arm serializes admission with actual unload; it releases before scan I/O. */
static int
engine_kernel_r01_arm_hold(spa_t *spa)
{
	engine_kernel_t *job = spa->spa_reloc->kernel;

	mutex_enter(&job->lock);
	if (job->running || job->unloading || job->exporting ||
	    job->zil_teardowns != 0) {
		mutex_exit(&job->lock);
		return (EBUSY);
	}
	return (0);
}

static void
engine_kernel_r01_arm_rele(spa_t *spa)
{
	mutex_exit(&spa->spa_reloc->kernel->lock);
}

static void
engine_kernel_init(void)
{
	mutex_init(&engine_selector.lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&engine_selector.r01_cv, NULL, CV_DEFAULT, NULL);
	(void) strlcpy(engine_selector.cached_status,
	    "state=idle pool= active=0\n",
	    sizeof (engine_selector.cached_status));
	(void) strlcpy(engine_selector.r02_manifest,
	    "r02_manifest_v=2 state=idle\n",
	    sizeof (engine_selector.r02_manifest));
	(void) strlcpy(engine_selector.denominator,
	    "denom_v=1 denom_complete=0 denom_error=not_sampled\n",
	    sizeof (engine_selector.denominator));
}

/*
 * A selector user borrows the selector-owned SPA reference.  Detach first
 * removes that reference from publication, then waits these users before it
 * can close it.  No dereference of a borrowed SPA is allowed after user_exit.
 */
static spa_t *
engine_selector_r01_user_enter(const char *name, boolean_t *other)
{
	spa_t *spa = NULL;

	if (other != NULL)
		*other = B_FALSE;
	mutex_enter(&engine_selector.lock);
	if (engine_selector.r01_detaching) {
		if (other != NULL)
			*other = B_TRUE;
	} else if (engine_selector.r01_spa != NULL) {
		if (name != NULL && strcmp(spa_name(engine_selector.r01_spa),
		    name) != 0) {
			if (other != NULL)
				*other = B_TRUE;
		} else {
			spa = engine_selector.r01_spa;
			engine_selector.r01_users++;
		}
	}
	mutex_exit(&engine_selector.lock);
	return (spa);
}

/*
 * Transfer the caller's spa_open() reference to the selector and make the
 * caller a joined user before it drops the selector lock.
 */
static int
engine_selector_r01_install(spa_t *spa)
{
	int error = 0;

	mutex_enter(&engine_selector.lock);
	if (engine_selector.r01_spa != NULL || engine_selector.r01_detaching) {
		error = EBUSY;
	} else {
		engine_selector.r01_spa = spa;
		engine_selector.r01_users++;
	}
	mutex_exit(&engine_selector.lock);
	return (error);
}

/*
 * End a joined user.  If drop is set, atomically remove the selector
 * reference after this user has stopped using it, wait all older users, and
 * close that sole selector reference.  This avoids a post-unlock SPA use.
 */
static void
engine_selector_r01_user_exit(spa_t *spa, boolean_t drop)
{
	boolean_t close = B_FALSE;

	mutex_enter(&engine_selector.lock);
	VERIFY3U(engine_selector.r01_users, >, 0);
	engine_selector.r01_users--;
	if (engine_selector.r01_users == 0)
		cv_broadcast(&engine_selector.r01_cv);
	if (drop && engine_selector.r01_spa == spa) {
		engine_selector.r01_spa = NULL;
		engine_selector.r01_detaching = B_TRUE;
		while (engine_selector.r01_users != 0)
			cv_wait(&engine_selector.r01_cv, &engine_selector.lock);
		close = B_TRUE;
	}
	mutex_exit(&engine_selector.lock);
	if (close) {
		spa_close(spa, FTAG);
		mutex_enter(&engine_selector.lock);
		engine_selector.r01_detaching = B_FALSE;
		cv_broadcast(&engine_selector.r01_cv);
		mutex_exit(&engine_selector.lock);
	}
}

/*
 * Stop new selector users and transfer the selector reference to this caller.
 * The caller must later use put_taken() exactly once, after it no longer needs
 * the SPA to cancel/wait its R-01 or worker state.
 */
static boolean_t
engine_selector_r01_take(spa_t *spa)
{
	boolean_t taken = B_FALSE;

	mutex_enter(&engine_selector.lock);
	if (engine_selector.r01_spa == spa) {
		engine_selector.r01_spa = NULL;
		engine_selector.r01_detaching = B_TRUE;
		while (engine_selector.r01_users != 0)
			cv_wait(&engine_selector.r01_cv, &engine_selector.lock);
		taken = B_TRUE;
	}
	mutex_exit(&engine_selector.lock);
	return (taken);
}

/* Module teardown has no SPA argument, but uses the same reader drain. */
static spa_t *
engine_selector_r01_take_any(void)
{
	spa_t *spa;

	mutex_enter(&engine_selector.lock);
	while (engine_selector.r01_detaching)
		cv_wait(&engine_selector.r01_cv, &engine_selector.lock);
	spa = engine_selector.r01_spa;
	if (spa != NULL) {
		engine_selector.r01_spa = NULL;
		engine_selector.r01_detaching = B_TRUE;
		while (engine_selector.r01_users != 0)
			cv_wait(&engine_selector.r01_cv, &engine_selector.lock);
	}
	mutex_exit(&engine_selector.lock);
	return (spa);
}

static void
engine_selector_r01_put_taken(spa_t *spa)
{
	spa_close(spa, FTAG);
	mutex_enter(&engine_selector.lock);
	VERIFY(engine_selector.r01_detaching);
	engine_selector.r01_detaching = B_FALSE;
	cv_broadcast(&engine_selector.r01_cv);
	mutex_exit(&engine_selector.lock);
}

static void
engine_kernel_fini(void)
{
	spa_t *r01_spa = engine_selector_r01_take_any();

	if (r01_spa != NULL)
		spa_close(r01_spa, FTAG);
	mutex_enter(&engine_selector.lock);
	VERIFY0(engine_selector.r01_users);
	engine_selector.r01_detaching = B_FALSE;
	mutex_exit(&engine_selector.lock);
	cv_destroy(&engine_selector.r01_cv);
	mutex_destroy(&engine_selector.lock);
}

/* One word from a module parameter write, without its trailing newline. */
static void
engine_kernel_word(const char *val, char *out, size_t size)
{
	size_t n = 0;
	while (n + 1 < size && val[n] != '\0' && val[n] != '\n' &&
	    val[n] != ' ')
		n++;
	(void) memcpy(out, val, n);
	out[n] = '\0';
}

static void
engine_kernel_select(const char *name)
{
	mutex_enter(&engine_selector.lock);
	if (strcmp(engine_selector.pool, name) != 0) {
		(void) strlcpy(engine_selector.pool, name,
		    sizeof (engine_selector.pool));
		(void) strlcpy(engine_selector.cached_status,
		    "state=idle pool= active=0\n",
		    sizeof (engine_selector.cached_status));
	}
	mutex_exit(&engine_selector.lock);
}

static int engine_kernel_snapshot(spa_t *, char *);

/* L18: the pool is being exported or unloaded: a pause stops waiting. */
static boolean_t
engine_kernel_leaving(spa_t *spa)
{
	engine_kernel_t *job = spa->spa_reloc->kernel;
	if (job == NULL)
		return (B_FALSE);
	mutex_enter(&job->lock);
	boolean_t leaving = job->exporting || job->unloading;
	mutex_exit(&job->lock);
	return (leaving);
}

/* The unload path already owns the SPA; it must never reopen it by name. */
static void
engine_kernel_cache(spa_t *spa)
{
	char *buf = kmem_alloc(PAGE_SIZE, KM_SLEEP);
	if (engine_kernel_snapshot(spa, buf) < 0)
		(void) strlcpy(buf, "ram_v=1 ram_complete=0 "
		    "ram_error=status_truncated\n", PAGE_SIZE);
	mutex_enter(&engine_selector.lock);
	if (strcmp(engine_selector.pool, spa_name(spa)) == 0)
		(void) strlcpy(engine_selector.cached_status, buf, PAGE_SIZE);
	mutex_exit(&engine_selector.lock);
	kmem_free(buf, PAGE_SIZE);
}

static void
engine_kernel_worker(void *arg)
{
	spa_t *spa = arg;
	engine_kernel_t *job = spa->spa_reloc->kernel;
	engine_kernel_join_t *join = job->join;
	uint64_t birth = 0, floor = 0, reclaimed = 0, released = 0;
	pr19262_engine_convert_stats_t st = { 0 };
	/* convert_set transfers its open SPA reference to this worker. */
	uint64_t parity = job->parity, batch = job->batch;
	/* Final-free is legal only on the already-prepared idempotent epoch. */
	int error = pr19262_engine_r01_epoch_wait(spa, parity);
	/*
	 * L18: a conversion that does not fit is refused before its epoch,
	 * changing nothing (the room of the plan asked for is computed
	 * without setting it).
	 */
	if (error == 0)
		error = engine_convert_room_check(spa, parity, batch, "epoch",
		    NULL);
	if (error == 0)
		error = pr19262_engine_epoch(spa, parity, &birth);
	if (error == 0)
		error = pr19262_engine_r01_epoch_check(spa, parity, birth);
	if (error == 0)
		error = pr19262_engine_convert(spa, birth, batch, 4096, &st);
	if (error == 0)
		error = pr19262_engine_fence(spa, &floor, &reclaimed,
		    &released, NULL);
	if (error == 0) {
		int certify = pr19262_engine_certify(spa, parity);
		/*
		 * A healthy-start store may keep running while degraded and
		 * un-certified.
		 */
		if (certify != 0 && certify != ENXIO)
			error = certify;
	}
	if (error != 0)
		pr19262_engine_r01_epoch_abort(spa);
	/* A zero-batch run and every error releases observer on exit. */
	pr19262_engine_r01_epoch_finish(spa);
	mutex_enter(&job->lock);
	job->stats = st;
	job->birth = birth;
	job->floor = floor;
	job->reclaimed = reclaimed;
	job->released = released;
	job->error = error;
	(void) strlcpy(job->state, "finalizing", ENGINE_KERNEL_STATE);
	mutex_exit(&job->lock);
	zfs_dbgmsg("pr19262 kernel convert pool=%s parity=%llu birth=%llu "
	    "copied=%llu error=%d", spa_name(spa), (u_longlong_t)parity,
	    (u_longlong_t)birth, (u_longlong_t)st.copied, error);
	engine_kernel_cache(spa);
	/* The cache is the worker's last use of the per-SPA engine. */
	mutex_enter(&job->lock);
	job->running = B_FALSE;
	(void) strlcpy(job->state, error == 0 ? "converted" : "error",
	    ENGINE_KERNEL_STATE);
	mutex_exit(&job->lock);
	spa_close(spa, FTAG);
	/* Completion is visible only after the transferred SPA ref releases. */
	engine_kernel_join_worker_rele(join);
	thread_exit();
}

/* Start a durable engine, or select an already imported one. */
static int
engine_kernel_engine_set(const char *val, zfs_kernel_param_t *kp)
{
	(void) kp;
	char name[ZFS_MAX_DATASET_NAME_LEN];
	engine_kernel_word(val, name, sizeof (name));
	if (name[0] == '\0')
		return (-EINVAL);
	spa_t *spa;
	int error = spa_open(name, &spa, FTAG);
	if (error != 0)
		return (-error);
	engine_kernel_t *job = spa->spa_reloc->kernel;
	mutex_enter(&job->lock);
	if (job->running || job->unloading || job->exporting) {
		error = EBUSY;
		goto out;
	}
	if (!spa_writeable(spa) || !spa->spa_trust_config ||
	    engine_target_vdev(spa, 0) == NULL) {
		error = ENOTSUP;
		goto out;
	}
	if (pr19262_engine_active(spa)) {
		if (!pr19262_engine_persistent(spa))
			error = ENOTSUP;
	} else {
		error = pr19262_engine_start_persistent(spa);
		if (error == 0)
			txg_wait_synced(spa_get_dsl(spa), 0);
	}
out:
	mutex_exit(&job->lock);
	if (error == 0) {
		engine_kernel_select(name);
		zfs_dbgmsg("pr19262 kernel engine ready on %s", name);
	}
	spa_close(spa, FTAG);
	return (-error);
}

static int
engine_kernel_engine_get(char *buf, zfs_kernel_param_t *kp)
{
	(void) kp;
	mutex_enter(&engine_selector.lock);
	int n = scnprintf(buf, PAGE_SIZE, "%s\n", engine_selector.pool);
	mutex_exit(&engine_selector.lock);
	return (n);
}

/* zfs_pr19262_convert: "pool parity [batch_mib]" starts that SPA's worker. */
static int
engine_kernel_convert_set(const char *val, zfs_kernel_param_t *kp)
{
	(void) kp;
	char name[ZFS_MAX_DATASET_NAME_LEN];
	engine_kernel_word(val, name, sizeof (name));
	const char *rest = val + strlen(name);
	unsigned long long parity = 0, batch = 8;
	if (name[0] == '\0' || sscanf(rest, "%llu %llu", &parity, &batch) < 1 ||
	    parity < 1 || parity > 3 || batch == 0 || batch > 1024)
		return (-EINVAL);
	spa_t *spa;
	int error = spa_open(name, &spa, FTAG);
	if (error != 0)
		return (-error);
	if (!pr19262_engine_active(spa) || !pr19262_engine_persistent(spa) ||
	    !spa_writeable(spa)) {
		spa_close(spa, FTAG);
		return (-ENOTSUP);
	}
	engine_kernel_t *job = spa->spa_reloc->kernel;
	mutex_enter(&job->lock);
	if (job->running || job->unloading || job->exporting) {
		mutex_exit(&job->lock);
		spa_close(spa, FTAG);
		return (-EBUSY);
	}
	job->running = B_TRUE;
	job->parity = parity;
	job->batch = batch << 20;
	job->error = 0;
	(void) strlcpy(job->state, "running", ENGINE_KERNEL_STATE);
	engine_kernel_join_worker_hold(job->join);
	mutex_exit(&job->lock);
	engine_kernel_select(name);
	(void) thread_create(NULL, 0, engine_kernel_worker, spa, 0, &p0,
	    TS_RUN, minclsyspri);
	return (0);
}

static int
engine_kernel_selected(char *name, spa_t **spa)
{
	mutex_enter(&engine_selector.lock);
	(void) strlcpy(name, engine_selector.pool, ZFS_MAX_DATASET_NAME_LEN);
	mutex_exit(&engine_selector.lock);
	return (name[0] == '\0' ? ENOENT : spa_open(name, spa, FTAG));
}

static int
engine_kernel_convert_get(char *buf, zfs_kernel_param_t *kp)
{
	(void) kp;
	char name[ZFS_MAX_DATASET_NAME_LEN];
	spa_t *spa;
	if (engine_kernel_selected(name, &spa) != 0)
		return (scnprintf(buf, PAGE_SIZE, "%s 0 0\n", name));
	engine_kernel_t *job = spa->spa_reloc->kernel;
	mutex_enter(&job->lock);
	int n = scnprintf(buf, PAGE_SIZE, "%s %llu %llu\n", name,
	    (u_longlong_t)job->parity, (u_longlong_t)(job->batch >> 20));
	mutex_exit(&job->lock);
	spa_close(spa, FTAG);
	return (n);
}

static int
engine_kernel_fence_set(const char *val, zfs_kernel_param_t *kp)
{
	(void) kp;
	char name[ZFS_MAX_DATASET_NAME_LEN];
	engine_kernel_word(val, name, sizeof (name));
	spa_t *spa;
	int error = spa_open(name, &spa, FTAG);
	if (error != 0)
		return (-error);
	uint64_t floor, reclaimed, released;
	error = pr19262_engine_fence(spa, &floor, &reclaimed, &released, NULL);
	if (error == 0) {
		engine_kernel_t *job = spa->spa_reloc->kernel;
		mutex_enter(&job->lock);
		job->floor = floor;
		job->reclaimed = reclaimed;
		job->released = released;
		mutex_exit(&job->lock);
		engine_kernel_select(name);
	}
	spa_close(spa, FTAG);
	return (-error);
}

static int
engine_kernel_fence_get(char *buf, zfs_kernel_param_t *kp)
{
	(void) kp;
	char name[ZFS_MAX_DATASET_NAME_LEN];
	spa_t *spa;
	if (engine_kernel_selected(name, &spa) != 0)
		return (scnprintf(buf,
		    PAGE_SIZE, "floor=0 reclaimed=0 released=0\n"));
	engine_kernel_t *job = spa->spa_reloc->kernel;
	mutex_enter(&job->lock);
	int n = scnprintf(buf, PAGE_SIZE, "floor=%llu reclaimed=%llu "
	    "released=%llu\n", (u_longlong_t)job->floor,
	    (u_longlong_t)job->reclaimed, (u_longlong_t)job->released);
	mutex_exit(&job->lock);
	spa_close(spa, FTAG);
	return (n);
}

/* Selecting a pool changes observation only; jobs stay owned by their SPA. */
static int
engine_kernel_status_set(const char *val, zfs_kernel_param_t *kp)
{
	(void) kp;
	char name[ZFS_MAX_DATASET_NAME_LEN];
	engine_kernel_word(val, name, sizeof (name));
	spa_t *spa;
	int error = spa_open(name, &spa, FTAG);
	if (error != 0)
		return (-error);
	engine_kernel_select(name);
	spa_close(spa, FTAG);
	return (0);
}

static int
engine_kernel_status_get(char *buf, zfs_kernel_param_t *kp)
{
	(void) kp;
	char name[ZFS_MAX_DATASET_NAME_LEN];
	spa_t *spa;
	if (engine_kernel_selected(name, &spa) == 0) {
		int n = engine_kernel_snapshot(spa, buf);
		spa_close(spa, FTAG);
		return (n);
	}
	mutex_enter(&engine_selector.lock);
	int n = scnprintf(buf, PAGE_SIZE, "%s", engine_selector.cached_status);
	mutex_exit(&engine_selector.lock);
	return (n);
}

/* One selected SPA, explicit arm/release/cancel word, no implicit worker. */
static int
engine_kernel_r01_set(const char *val, zfs_kernel_param_t *kp)
{
	(void) kp;
	char name[ZFS_MAX_DATASET_NAME_LEN];
	char command[16];
	char boundary[16];
	engine_kernel_word(val, name, sizeof (name));
	if (name[0] == '\0')
		return (-EINVAL);
	const char *p = val + strlen(name);
	if (*p != ' ')
		return (-EINVAL);
	while (*p == ' ')
		p++;
	engine_kernel_word(p, command, sizeof (command));
	if (command[0] == '\0')
		return (-EINVAL);
	p += strlen(command);
	while (*p == ' ')
		p++;
	engine_kernel_word(p, boundary, sizeof (boundary));
	p += strlen(boundary);
	if (*p != '\0' && *p != '\n')
		return (-EINVAL);
	uint_t point = PR19262_R01_SELECTED;
	uint64_t parity = 0;
	if (strcmp(command, "arm") == 0 && boundary[0] != '\0') {
		if (strcmp(boundary, "selected") == 0)
			point = PR19262_R01_SELECTED;
		else if (strcmp(boundary, "copying") == 0)
			point = PR19262_R01_COPYING;
		else if (strcmp(boundary, "published") == 0)
			point = PR19262_R01_PUBLISHED;
		else if (strcmp(boundary, "final-free") == 0)
			point = PR19262_R01_FINAL_FREE;
		else
			return (-EINVAL);
	}
	if (strcmp(command, "prepare") == 0 &&
	    (sscanf(boundary, "%llu", &parity) != 1 || parity < 1 ||
	    parity > VDEV_RAIDZ_MAXPARITY))
		return (-EINVAL);
	uint_t test_mode = PR19262_R01_TEST_NONE;
	if (strcmp(command, "fault-arm") == 0) {
		if (strcmp(boundary, "epoch-stale") == 0)
			test_mode = PR19262_R01_TEST_EPOCH_STALE;
		else if (strcmp(boundary, "convert-eio") == 0)
			test_mode = PR19262_R01_TEST_CONVERT_EIO;
		else
			return (-EINVAL);
	}
	if (strcmp(command, "arm") != 0 && strcmp(command, "prepare") != 0 &&
	    strcmp(command, "fault-arm") != 0 && boundary[0] != '\0')
		return (-EINVAL);
	spa_t *spa;
	boolean_t joined = B_FALSE;
	boolean_t other = B_FALSE;
	boolean_t installed = B_FALSE;
	boolean_t local = B_FALSE;
	boolean_t terminal = B_FALSE;

	spa = engine_selector_r01_user_enter(name, &other);
	if (other)
		return (-EBUSY);
	int error = 0;
	if (spa == NULL) {
		error = spa_open(name, &spa, FTAG);
		if (error != 0)
			return (-error);
		local = B_TRUE;
		if (strcmp(command, "arm") == 0) {
			error = engine_selector_r01_install(spa);
			if (error != 0) {
				spa_close(spa, FTAG);
				return (-error);
			}
			/* install() consumed this spa_open() reference. */
			joined = B_TRUE;
			installed = B_TRUE;
			local = B_FALSE;
		}
	} else {
		joined = B_TRUE;
	}
	if (strcmp(command, "arm") == 0)
		error = pr19262_engine_r01_arm(spa, point);
	else if (strcmp(command, "release") == 0)
		error = pr19262_engine_r01_release(spa);
	else if (strcmp(command, "cancel") == 0)
		error = pr19262_engine_r01_cancel(spa);
	else if (strcmp(command, "scan-cancel") == 0) {
		error = pr19262_engine_r01_scan_cancel_begin(spa);
		if (error == 0) {
			error = dsl_scan_cancel_resilver_suspended(
			    spa_get_dsl(spa));
			pr19262_engine_r01_scan_cancel_finish(spa, error);
		}
	} else if (strcmp(command, "advance") == 0)
		error = pr19262_engine_r01_advance(spa);
	else if (strcmp(command, "copy") == 0)
		error = pr19262_engine_r01_copy(spa);
	else if (strcmp(command, "prepare") == 0)
		error = pr19262_engine_r01_prepare(spa, parity);
	else if (strcmp(command, "fault-arm") == 0)
		error = pr19262_engine_r01_test_arm(spa, test_mode);
	else if (strcmp(command, "fault-fire") == 0)
		error = pr19262_engine_r01_test_fire(spa);
	else
		error = EINVAL;
	if (error == 0 && joined && strcmp(command, "cancel") == 0)
		terminal = pr19262_engine_r01_terminal(spa);
	if (error == 0)
		engine_kernel_select(name);
	if (joined) {
		/* No SPA dereference is permitted after this joined exit. */
		engine_selector_r01_user_exit(spa,
		    (installed && error != 0) || terminal);
	} else if (local) {
		spa_close(spa, FTAG);
	}
	return (-error);
}

static int
engine_kernel_r01_get(char *buf, zfs_kernel_param_t *kp)
{
	(void) kp;
	spa_t *held = engine_selector_r01_user_enter(NULL, NULL);
	if (held != NULL) {
		int n = pr19262_engine_r01_status(held, buf, PAGE_SIZE);
		/*
		 * Ask the engine, as the cancel command does: matching the
		 * status text missed a cancelled+drained R-01 once fields
		 * followed drained=, and text alone cannot see a root still
		 * in flight.
		 */
		boolean_t terminal = n >= 0 &&
		    pr19262_engine_r01_terminal(held);
		/* user_exit() detaches terminal state before any SPA free. */
		engine_selector_r01_user_exit(held, terminal);
		return (n);
	}
	char name[ZFS_MAX_DATASET_NAME_LEN];
	spa_t *spa;
	if (engine_kernel_selected(name, &spa) != 0)
		return (scnprintf(buf, PAGE_SIZE,
		    "state=idle pool=%s generation=0\n", name));
	int n = pr19262_engine_r01_status(spa, buf, PAGE_SIZE);
	spa_close(spa, FTAG);
	return (n);
}

/* One selected SPA, explicit arm only; the observer never controls a scan. */
static int
engine_kernel_r02_set(const char *val, zfs_kernel_param_t *kp)
{
	(void) kp;
	char name[ZFS_MAX_DATASET_NAME_LEN];
	char command[16];
	engine_kernel_word(val, name, sizeof (name));
	if (name[0] == '\0')
		return (-EINVAL);
	const char *p = val + strlen(name);
	if (*p != ' ')
		return (-EINVAL);
	while (*p == ' ')
		p++;
	engine_kernel_word(p, command, sizeof (command));
	if (strcmp(command, "arm") != 0)
		return (-EINVAL);
	p += strlen(command);
	if (*p != '\0' && *p != '\n')
		return (-EINVAL);
	spa_t *spa;
	int error = spa_open(name, &spa, FTAG);
	if (error != 0)
		return (-error);
	error = pr19262_engine_r02_arm(spa);
	if (error == 0)
		engine_kernel_select(name);
	spa_close(spa, FTAG);
	return (-error);
}

static int
engine_kernel_r02_get(char *buf, zfs_kernel_param_t *kp)
{
	(void) kp;
	char name[ZFS_MAX_DATASET_NAME_LEN];
	spa_t *spa;
	if (engine_kernel_selected(name, &spa) != 0)
		return (scnprintf(buf, PAGE_SIZE,
		    "state=idle pool=%s generation=0\n", name));
	int n = pr19262_engine_r02_status(spa, buf, PAGE_SIZE);
	spa_close(spa, FTAG);
	return (n);
}

/*
 * The walk collector owns its scratch manifest entries and invokes this
 * callback only after releasing its SPA and engine locks.  Keep the kernel
 * aperture's exact count local so the collector-facing manifest ABI stays
 * unchanged.
 */
typedef struct engine_kernel_r02_count {
	uint64_t entries;
} engine_kernel_r02_count_t;

static int
engine_kernel_r02_count(void *arg,
    const pr19262_r02_manifest_entry_t *entry)
{
	engine_kernel_r02_count_t *count = arg;

	if (entry == NULL || count->entries == UINT64_MAX)
		return (entry == NULL ? EINVAL : EOVERFLOW);
	count->entries++;
	return (0);
}

/*
 * A test-only, read-only R02 witness.  Its sole command is a pool name; it
 * refuses writable imports and never arms, starts, synchronizes, or changes
 * the selected SPA.  The manifest collector owns its own reader locks.
 */
static int
engine_kernel_r02_manifest_set(const char *val, zfs_kernel_param_t *kp)
{
	(void) kp;
	char name[ZFS_MAX_DATASET_NAME_LEN];
	pr19262_r02_manifest_t manifest = { 0 };
	engine_kernel_r02_count_t count = { 0 };
	engine_kernel_word(val, name, sizeof (name));
	if (name[0] == '\0')
		return (-EINVAL);
	const char *p = val + strlen(name);
	if (*p != '\0' && *p != '\n')
		return (-EINVAL);

	spa_t *spa;
	int error = spa_open(name, &spa, FTAG);
	if (error != 0)
		return (-error);
	if (spa_writeable(spa)) {
		mutex_enter(&engine_selector.lock);
		(void) scnprintf(engine_selector.r02_manifest,
		    sizeof (engine_selector.r02_manifest),
		    "r02_manifest_v=2 state=refused pool=%s reason=writable "
		    "error=%d\n", name, EROFS);
		mutex_exit(&engine_selector.lock);
		spa_close(spa, FTAG);
		return (-EROFS);
	}
	error = pr19262_engine_r02_manifest_walk(spa,
	    engine_kernel_r02_count, &count, &manifest);
	mutex_enter(&engine_selector.lock);
	(void) scnprintf(engine_selector.r02_manifest,
	    sizeof (engine_selector.r02_manifest),
	    "r02_manifest_v=2 state=%s pool=%s error=%d entries=%llu "
	    "roots=%llu catalog_pages=%llu tree_pages=%llu control_pages=%llu "
	    "directory_pages=%llu active_arenas=%llu pending_arenas=%llu "
	    "journal_heads=%llu journal_binds=%llu journal_forwards=%llu "
	    "journal_pending=%llu\n", error == 0 ? "observed" : "error",
	    name, error, (u_longlong_t)count.entries,
	    (u_longlong_t)manifest.roots,
	    (u_longlong_t)manifest.catalog_pages,
	    (u_longlong_t)manifest.tree_pages,
	    (u_longlong_t)manifest.control_pages,
	    (u_longlong_t)manifest.directory_pages,
	    (u_longlong_t)manifest.active_arenas,
	    (u_longlong_t)manifest.pending_arenas,
	    (u_longlong_t)manifest.journal_heads,
	    (u_longlong_t)manifest.journal_binds,
	    (u_longlong_t)manifest.journal_forwards,
	    (u_longlong_t)manifest.journal_pending);
	mutex_exit(&engine_selector.lock);
	spa_close(spa, FTAG);
	return (-error);
}

static int
engine_kernel_r02_manifest_get(char *buf, zfs_kernel_param_t *kp)
{
	(void) kp;
	mutex_enter(&engine_selector.lock);
	int n = scnprintf(buf, PAGE_SIZE, "%s", engine_selector.r02_manifest);
	mutex_exit(&engine_selector.lock);
	return (n);
}

/*
 * This is deliberately a first denominator slice, not the final RAM
 * denominator.  The sync task gives traverse_pool() its required stable
 * on-disk view and the callback counts logical BP references, never unique
 * physical DVAs.  Version two adds a logical partition by traversal objset:
 * MOS, head, snapshot and clone.  This is still not a unique-DVA, DDT, BRT,
 * gang-member or allocator-RAM denominator.
 */
typedef enum engine_denominator_partition {
	ENGINE_DENOM_PART_HEAD,
	ENGINE_DENOM_PART_SNAPSHOT,
	ENGINE_DENOM_PART_CLONE
} engine_denominator_partition_t;

typedef struct engine_denominator_dataset {
	avl_node_t node;
	uint64_t object;
	engine_denominator_partition_t partition;
} engine_denominator_dataset_t;

typedef struct engine_denominator_walk {
	uint64_t spa;
	uint64_t txg;
	avl_tree_t datasets;
	uint64_t dataset_objsets;
	uint64_t callbacks;
	uint64_t synthetic_dnodes;
	uint64_t holes;
	uint64_t zil_refs;
	uint64_t logical_refs;
	uint64_t logical_lsize;
	uint64_t metadata_refs;
	uint64_t data_refs;
	uint64_t embedded_refs;
	uint64_t redacted_refs;
	uint64_t gang_headers;
	uint64_t dva_refs_nonunique;
	uint64_t dva_asize_nonunique;
	uint64_t mos_refs;
	uint64_t mos_lsize;
	uint64_t head_refs;
	uint64_t head_lsize;
	uint64_t snapshot_refs;
	uint64_t snapshot_lsize;
	uint64_t clone_refs;
	uint64_t clone_lsize;
	int error;
} engine_denominator_walk_t;

static int
engine_denominator_add(uint64_t *value, uint64_t add)
{
	if (*value > UINT64_MAX - add)
		return (SET_ERROR(EOVERFLOW));
	*value += add;
	return (0);
}

static int
engine_denominator_dataset_compare(const void *x, const void *y)
{
	const engine_denominator_dataset_t *a = x;
	const engine_denominator_dataset_t *b = y;

	return (a->object < b->object ? -1 : a->object > b->object);
}

static void
engine_denominator_datasets_fini(engine_denominator_walk_t *w)
{
	void *cookie = NULL;
	engine_denominator_dataset_t *entry;

	while ((entry = avl_destroy_nodes(&w->datasets, &cookie)) != NULL)
		kmem_free(entry, sizeof (*entry));
	avl_destroy(&w->datasets);
}

static int
engine_denominator_dataset_insert(engine_denominator_walk_t *w,
    uint64_t object, engine_denominator_partition_t partition)
{
	engine_denominator_dataset_t search = { .object = object };
	avl_index_t where;

	if (avl_find(&w->datasets, &search, &where) != NULL)
		return (SET_ERROR(EEXIST));
	engine_denominator_dataset_t *entry = kmem_zalloc(sizeof (*entry),
	    KM_SLEEP);
	entry->object = object;
	entry->partition = partition;
	avl_insert(&w->datasets, entry, where);
	return (engine_denominator_add(&w->dataset_objsets, 1));
}

/*
 * traverse_dataset() stamps callbacks with ds->ds_object, not an opened
 * objset.  Build the classification from those stable dataset objects while
 * the sync task owns dp_config_rwlock writer.  Opening an objset may wait on
 * txg work; it is neither needed for the traversal ID nor safe in this sync
 * callback.
 */
static int
engine_denominator_datasets_build(spa_t *spa, engine_denominator_walk_t *w)
{
	dsl_pool_t *dp = spa_get_dsl(spa);
	objset_t *mos = dp->dp_meta_objset;
	int error = 0;
	uint64_t object = 1;

	ASSERT(RRW_WRITE_HELD(&dp->dp_config_rwlock));
	while (error == 0) {
		dmu_object_info_t doi;
		dsl_dataset_t *ds;
		engine_denominator_partition_t partition;

		error = dmu_object_info(mos, object, &doi);
		if (error != 0)
			break;
		if (doi.doi_bonus_type == DMU_OT_DSL_DATASET) {
			error = dsl_dataset_hold_obj(dp, object, FTAG, &ds);
			if (error != 0)
				break;
			if (dsl_dataset_is_snapshot(ds))
				partition = ENGINE_DENOM_PART_SNAPSHOT;
			else if (dsl_dir_is_clone(ds->ds_dir))
				partition = ENGINE_DENOM_PART_CLONE;
			else
				partition = ENGINE_DENOM_PART_HEAD;
			dsl_dataset_rele(ds, FTAG);
			error = engine_denominator_dataset_insert(w, object,
			    partition);
			if (error != 0)
				break;
		}
		error = dmu_object_next(mos, &object, B_FALSE, 0);
	}
	return (error == ESRCH ? 0 : error);
}

static int
engine_denominator_partition_add(engine_denominator_walk_t *w,
    const zbookmark_phys_t *zb, const blkptr_t *bp)
{
	uint64_t *refs;
	uint64_t *lsize;

	if (zb->zb_objset == DMU_META_OBJSET) {
		refs = &w->mos_refs;
		lsize = &w->mos_lsize;
	} else {
		engine_denominator_dataset_t search = {
			.object = zb->zb_objset
		};
		engine_denominator_dataset_t *entry = avl_find(&w->datasets,
		    &search, NULL);
		if (entry == NULL)
			return (SET_ERROR(ENOENT));
		switch (entry->partition) {
		case ENGINE_DENOM_PART_HEAD:
			refs = &w->head_refs;
			lsize = &w->head_lsize;
			break;
		case ENGINE_DENOM_PART_SNAPSHOT:
			refs = &w->snapshot_refs;
			lsize = &w->snapshot_lsize;
			break;
		case ENGINE_DENOM_PART_CLONE:
			refs = &w->clone_refs;
			lsize = &w->clone_lsize;
			break;
		default:
			return (SET_ERROR(EINVAL));
		}
	}
	int error = engine_denominator_add(refs, 1);
	if (error == 0)
		error = engine_denominator_add(lsize, BP_GET_LSIZE(bp));
	return (error);
}

static int
engine_denominator_partition_check(const engine_denominator_walk_t *w)
{
	uint64_t refs = 0;
	uint64_t lsize = 0;
	int error;

	if ((error = engine_denominator_add(&refs, w->mos_refs)) != 0 ||
	    (error = engine_denominator_add(&refs, w->head_refs)) != 0 ||
	    (error = engine_denominator_add(&refs, w->snapshot_refs)) != 0 ||
	    (error = engine_denominator_add(&refs, w->clone_refs)) != 0 ||
	    (error = engine_denominator_add(&lsize, w->mos_lsize)) != 0 ||
	    (error = engine_denominator_add(&lsize, w->head_lsize)) != 0 ||
	    (error = engine_denominator_add(&lsize, w->snapshot_lsize)) != 0 ||
	    (error = engine_denominator_add(&lsize, w->clone_lsize)) != 0)
		return (error);
	if (refs != w->logical_refs || lsize != w->logical_lsize)
		return (SET_ERROR(EINVAL));
	return (0);
}

static int
engine_kernel_denominator_cb(spa_t *spa, zilog_t *zilog,
    const blkptr_t *bp, const zbookmark_phys_t *zb,
    const dnode_phys_t *dnp, void *arg)
{
	(void) spa, (void) dnp;
	engine_denominator_walk_t *w = arg;
	int error = engine_denominator_add(&w->callbacks, 1);
	if (error != 0)
		return (error);
	/* traverse_dnode() reports the parent BP as a structural callback. */
	if (zb->zb_level == ZB_DNODE_LEVEL)
		return (engine_denominator_add(&w->synthetic_dnodes, 1));
	if (BP_IS_HOLE(bp))
		return (engine_denominator_add(&w->holes, 1));
	/* ZIL is a transient side bin, not an active-data denominator entry. */
	if (zilog != NULL)
		return (engine_denominator_add(&w->zil_refs, 1));
	if ((error = engine_denominator_add(&w->logical_refs, 1)) != 0 ||
	    (error = engine_denominator_add(&w->logical_lsize,
	    BP_GET_LSIZE(bp))) != 0)
		return (error);
	if ((error = engine_denominator_partition_add(w, zb, bp)) != 0)
		return (error);
	if (BP_IS_REDACTED(bp)) {
		if ((error = engine_denominator_add(&w->redacted_refs, 1)) != 0)
			return (error);
	} else if (BP_IS_EMBEDDED(bp)) {
		if ((error = engine_denominator_add(&w->embedded_refs, 1)) != 0)
			return (error);
	} else {
		uint64_t n = BP_GET_NDVAS(bp);
		if ((error = engine_denominator_add(&w->dva_refs_nonunique,
		    n)) != 0 || (error = engine_denominator_add(
		    &w->dva_asize_nonunique, BP_GET_ASIZE(bp))) != 0)
			return (error);
	}
	if (BP_IS_GANG(bp) &&
	    (error = engine_denominator_add(&w->gang_headers, 1)) != 0)
		return (error);
	if (BP_GET_LEVEL(bp) != 0 || DMU_OT_IS_METADATA(BP_GET_TYPE(bp)))
		return (engine_denominator_add(&w->metadata_refs, 1));
	return (engine_denominator_add(&w->data_refs, 1));
}

static void
engine_kernel_denominator_sync(void *arg, dmu_tx_t *tx)
{
	engine_denominator_walk_t *w = arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	w->spa = spa_load_guid(spa);
	w->txg = dmu_tx_get_txg(tx);
	w->error = engine_denominator_datasets_build(spa, w);
	if (w->error == 0) {
		w->error = traverse_pool_sync(spa, 0,
		    TRAVERSE_PRE | TRAVERSE_NO_DECRYPT,
		    engine_kernel_denominator_cb, w);
	}
}

static int
engine_kernel_denominator_set(const char *val, zfs_kernel_param_t *kp)
{
	(void) kp;
	char name[ZFS_MAX_DATASET_NAME_LEN];
	char command[16];
	engine_kernel_word(val, name, sizeof (name));
	if (name[0] == '\0')
		return (-EINVAL);
	const char *p = val + strlen(name);
	if (*p != ' ')
		return (-EINVAL);
	while (*p == ' ')
		p++;
	engine_kernel_word(p, command, sizeof (command));
	p += strlen(command);
	if (strcmp(command, "snapshot") != 0 ||
	    (*p != '\0' && *p != '\n'))
		return (-EINVAL);
	spa_t *spa;
	int error = spa_open(name, &spa, FTAG);
	if (error != 0)
		return (-error);
	if (!spa_writeable(spa)) {
		spa_close(spa, FTAG);
		return (-EROFS);
	}
	spa_close(spa, FTAG);
	engine_denominator_walk_t walk = { 0 };
	avl_create(&walk.datasets, engine_denominator_dataset_compare,
	    sizeof (engine_denominator_dataset_t),
	    offsetof(engine_denominator_dataset_t, node));
	error = dsl_sync_task(name, NULL, engine_kernel_denominator_sync,
	    &walk, 0, ZFS_SPACE_CHECK_NONE);
	if (error == 0)
		error = walk.error;
	if (error == 0)
		error = engine_denominator_partition_check(&walk);
	engine_denominator_datasets_fini(&walk);
	if (error != 0)
		return (-error);
	char *line = kmem_alloc(PAGE_SIZE, KM_SLEEP);
	int n = snprintf(line, PAGE_SIZE,
	    "denom_v=2 denom_complete=0 denom_scope=bp_ref_partitioned "
	    "denom_pool=%s denom_spa_load_guid=%llu denom_txg=%llu "
	    "denom_logical_refs_all_datasets=%llu denom_logical_lsize_B=%llu "
	    "denom_metadata_refs=%llu denom_data_refs=%llu "
	    "denom_embedded_refs=%llu denom_redacted_refs=%llu "
	    "denom_gang_headers=%llu denom_dva_refs_nonunique=%llu "
	    "denom_dva_asize_nonunique_B=%llu denom_zil_refs=%llu "
	    "denom_holes=%llu denom_synthetic_dnodes=%llu "
	    "denom_callbacks=%llu denom_dataset_objsets=%llu "
	    "denom_part_mos_refs=%llu denom_part_mos_lsize_B=%llu "
	    "denom_part_head_refs=%llu denom_part_head_lsize_B=%llu "
	    "denom_part_snapshot_refs=%llu denom_part_snapshot_lsize_B=%llu "
	    "denom_part_clone_refs=%llu denom_part_clone_lsize_B=%llu "
	    "denom_partition_invariant=1 "
	    "denom_unique_dva=unavailable denom_ddt=unavailable "
	    "denom_gang_members=unavailable "
	    "denom_engine_native_ledgers=unavailable\n",
	    name, (u_longlong_t)walk.spa, (u_longlong_t)walk.txg,
	    (u_longlong_t)walk.logical_refs,
	    (u_longlong_t)walk.logical_lsize,
	    (u_longlong_t)walk.metadata_refs, (u_longlong_t)walk.data_refs,
	    (u_longlong_t)walk.embedded_refs,
	    (u_longlong_t)walk.redacted_refs,
	    (u_longlong_t)walk.gang_headers,
	    (u_longlong_t)walk.dva_refs_nonunique,
	    (u_longlong_t)walk.dva_asize_nonunique,
	    (u_longlong_t)walk.zil_refs, (u_longlong_t)walk.holes,
	    (u_longlong_t)walk.synthetic_dnodes,
	    (u_longlong_t)walk.callbacks,
	    (u_longlong_t)walk.dataset_objsets,
	    (u_longlong_t)walk.mos_refs, (u_longlong_t)walk.mos_lsize,
	    (u_longlong_t)walk.head_refs, (u_longlong_t)walk.head_lsize,
	    (u_longlong_t)walk.snapshot_refs,
	    (u_longlong_t)walk.snapshot_lsize,
	    (u_longlong_t)walk.clone_refs, (u_longlong_t)walk.clone_lsize);
	if (n < 0 || n >= PAGE_SIZE) {
		kmem_free(line, PAGE_SIZE);
		return (-EOVERFLOW);
	}
	mutex_enter(&engine_selector.lock);
	(void) strlcpy(engine_selector.denominator, line,
	    sizeof (engine_selector.denominator));
	mutex_exit(&engine_selector.lock);
	kmem_free(line, PAGE_SIZE);
	engine_kernel_select(name);
	return (0);
}

static int
engine_kernel_denominator_get(char *buf, zfs_kernel_param_t *kp)
{
	(void) kp;
	mutex_enter(&engine_selector.lock);
	int n = scnprintf(buf, PAGE_SIZE, "%s", engine_selector.denominator);
	mutex_exit(&engine_selector.lock);
	return (n);
}

/* Request bytes only; no allocator charge, scratch or shared helper costs. */
typedef struct engine_ram_snapshot {
	uint64_t owners;
	uint64_t versions;
	uint64_t tombs;
	uint64_t pending;
	uint64_t gone;
	uint64_t cached;
	uint64_t capacity;
	uint64_t arenas;
	uint64_t arena_pages;
	uint64_t bitmap;
	uint64_t mirror;
	uint64_t jlive;
	uint64_t jretired;
	uint64_t jpending;
	uint64_t vcur;
	uint64_t vretired;
	uint64_t vother;
	uint64_t pending_owner;
	uint64_t pending_parked;
} engine_ram_snapshot_t;

/* Each row is count:unit_request_B:subtotal_request_B:source_tag. */
static int
engine_kernel_ram_class(char *buf, size_t size, size_t *used,
    const char *name, const char *source, uint64_t count, uint64_t unit,
    uint64_t *total)
{
	if (count > (UINT64_MAX - *total) / unit)
		return (-EOVERFLOW);
	uint64_t bytes = count * unit;
	int n = snprintf(buf + *used, size - *used,
	    " ram_%s=%llu:%llu:%llu:%s", name, (u_longlong_t)count,
	    (u_longlong_t)unit, (u_longlong_t)bytes, source);
	if (n < 0 || (size_t)n >= size - *used)
		return (-ENOSPC);
	*used += n;
	*total += bytes;
	return (0);
}

/*
 * store_lock pins cache nodes, arrays, arenas and the optional row mirror.
 * engine->lock pins owners, every linked version, pending nodes, tombs,
 * gone capacity and journal list topology (not journal payload fields).
 * SPA lifetime pins engine/pages/job; never follow cur/tail/current aliases
 * a second time, or pending ZIOs, or cached page data/headers here.
 * Source tags and exclusions are defined by retained-v1's private verifier.
 */
static int
engine_kernel_ram(spa_t *spa, char *buf, size_t size)
{
	engine_t *engine = spa->spa_reloc;
	engine_pages_t *pages = engine->pages;
	engine_ram_snapshot_t r = { 0 };
	ASSERT(MUTEX_HELD(&engine->store_lock));
	ASSERT(MUTEX_HELD(&engine->lock));
	hrtime_t when = gethrtime();
	for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
	    e = AVL_NEXT(&engine->owners, e)) {
		r.owners++;
		for (engine_version_t *v = e->versions; v != NULL;
		    v = v->next) {
			r.versions++;
			if (v->retired)
				r.vretired++;
			else if (v == e->cur)
				r.vcur++;
			else
				r.vother++;
		}
		for (engine_pending_t *p = e->pending; p != NULL;
		    p = p->next)
			r.pending_owner++;
	}
	for (engine_pending_t *p = engine->parked; p != NULL; p = p->next)
		r.pending_parked++;
	r.pending = r.pending_owner + r.pending_parked;
	r.tombs = avl_numnodes(&engine->tombstones);
	r.gone = engine->gone_capacity;
	r.capacity = pages->capacity;
	for (uint64_t i = 0; i < r.capacity; i++)
		r.cached += pages->cache[i] != NULL;
	for (engine_marena_t *a = pages->arenas; a != NULL; a = a->next) {
		r.arenas++;
		r.arena_pages += a->pages;
	}
	r.bitmap = engine->bitmap != NULL;
	/* A retained empty mirror still owns MAX(1, count) rows. */
	r.mirror = pages->rows != NULL ? MAX(1, pages->count) : 0;
	for (engine_j_segment_t *s = engine->journal_segments; s != NULL;
	    s = s->next)
		r.jlive++;
	for (engine_j_segment_t *s = engine->journal_retired; s != NULL;
	    s = s->next)
		r.jretired++;
	/* Pending is disjoint; publication links and clears it under lock. */
	r.jpending = engine->journal_pending != NULL;
	int n = snprintf(buf, size, "ram_v=1 ram_complete=1 "
	    "ram_scope=partial_retained_owned ram_unit=request_B "
	    "ram_source=retained-v1 ram_den=resident_owner_records "
	    "ram_den_n=%llu ram_spa=%llu ram_vdev=%llu ram_hrtime=%llu "
	    "ram_active=%u ram_vcur=%llu ram_vretired=%llu ram_vother=%llu "
	    "ram_pending_owner=%llu ram_pending_parked=%llu "
	    "ram_gone_used=%u ram_mirror_rows=%u ram_mirror_present=%u "
	    "ram_diag_stack_B=%llu ram_diag_selector_B=%llu",
	    (u_longlong_t)r.owners, (u_longlong_t)spa_load_guid(spa),
	    (u_longlong_t)(engine->spa == spa ? engine->vd_guid : 0),
	    (u_longlong_t)when, engine->spa == spa,
	    (u_longlong_t)r.vcur, (u_longlong_t)r.vretired,
	    (u_longlong_t)r.vother, (u_longlong_t)r.pending_owner,
	    (u_longlong_t)r.pending_parked, engine->gone_count, pages->count,
	    pages->rows != NULL, (u_longlong_t)sizeof (r),
	    (u_longlong_t)sizeof (engine_selector));
	if (n < 0 || (size_t)n >= size)
		return (-ENOSPC);
	size_t used = n;
	uint64_t total = 0;
	int error;
#define	ENGINE_RAM_CLASS(name, source, count, unit) \
	do { \
		error = engine_kernel_ram_class(buf, size, &used, name, \
		    source, count, unit, &total); \
		if (error != 0) \
			return (error); \
	} while (0)
	ENGINE_RAM_CLASS("owners", "Eo", r.owners, sizeof (engine_owner_t));
	ENGINE_RAM_CLASS("versions", "Ev", r.versions,
	    sizeof (engine_version_t));
	ENGINE_RAM_CLASS("tombs", "Et", r.tombs, sizeof (engine_tomb_t));
	ENGINE_RAM_CLASS("pending", "Ep", r.pending, sizeof (engine_pending_t));
	ENGINE_RAM_CLASS("gone", "Eg", r.gone, 2 * sizeof (uint64_t));
	ENGINE_RAM_CLASS("engine", "Ei", 1, sizeof (engine_t));
	ENGINE_RAM_CLASS("pages", "Ei", 1, sizeof (engine_pages_t));
	ENGINE_RAM_CLASS("job", "Ki", 1, sizeof (engine_kernel_t));
	ENGINE_RAM_CLASS("nodes", "Pn", r.cached, sizeof (engine_mnode_t));
	ENGINE_RAM_CLASS("buffers", "Pn", r.cached, ENGINE_STORE_PAGE);
	ENGINE_RAM_CLASS("pointers", "Pa", r.capacity,
	    sizeof (engine_mnode_t *));
	ENGINE_RAM_CLASS("bits", "Pa", r.capacity, sizeof (uint8_t));
	ENGINE_RAM_CLASS("arenas", "Pa", r.arenas, sizeof (engine_marena_t));
	ENGINE_RAM_CLASS("slots", "Pa", r.arena_pages, 2 * sizeof (uint8_t));
	ENGINE_RAM_CLASS("bitmap", "Sb", r.bitmap, ENGINE_STORE_PAGES);
	ENGINE_RAM_CLASS("mirror", "Pm", r.mirror, sizeof (engine_mrow_t));
	ENGINE_RAM_CLASS("jlive", "Jd", r.jlive, sizeof (engine_j_segment_t));
	ENGINE_RAM_CLASS("jretired", "Jd", r.jretired,
	    sizeof (engine_j_segment_t));
	ENGINE_RAM_CLASS("jpending", "Jd", r.jpending,
	    sizeof (engine_j_segment_t));
#undef ENGINE_RAM_CLASS
	n = snprintf(buf + used, size - used,
	    " ram_request_B=%llu ram_end=1\n", (u_longlong_t)total);
	if (n < 0 || (size_t)n >= size - used)
		return (-ENOSPC);
	return (used + n);
}

/* Store headroom fields; a separate frame keeps the snapshot's small. */
noinline static int
engine_kernel_headroom(engine_t *engine, char *buf, size_t size)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	int n = snprintf(buf, size, "standby=%u headroom_low=%u "
	    "standby_fills=%llu standby_uses=%llu grow_enospc=%llu "
	    "admit_headroom_rejections=%llu phys_free=%llu arena_runs=%llu "
	    "standby_releases=%llu ckpt_refusals=%llu free_deferrals=%llu "
	    "free_paced_rows=%llu free_aged=%llu rej_client=%llu "
	    "rej_netfree=%llu rej_converter=%llu space_rejections=%llu "
	    "admit_restarts=%llu admit_fence_waits=%llu m_client=%llu "
	    "m_netfree=%llu ",
	    engine->store_standby_valid, engine->store_headroom_low,
	    (u_longlong_t)engine->store_standby_fills,
	    (u_longlong_t)engine->store_standby_uses,
	    (u_longlong_t)engine->store_grow_enospc,
	    (u_longlong_t)engine->admit_headroom_rejections,
	    (u_longlong_t)(engine->free != NULL ?
	    zfs_range_tree_space(engine->free) : 0),
	    (u_longlong_t)(engine->free_runs_known ?
	    engine->free_runs_cached : 0),
	    (u_longlong_t)engine->store_standby_releases,
	    (u_longlong_t)engine->checkpoint_headroom_refusals,
	    (u_longlong_t)engine->free_deferrals,
	    (u_longlong_t)engine->free_paced_rows,
	    (u_longlong_t)engine->free_aged,
	    (u_longlong_t)engine->admit[PR19262_ADMIT_CLIENT].rejected,
	    (u_longlong_t)engine->admit[PR19262_ADMIT_NETFREE].rejected,
	    (u_longlong_t)engine->admit[PR19262_ADMIT_CONVERTER].rejected,
	    (u_longlong_t)engine->admit_space_rejections,
	    (u_longlong_t)engine->admit_restarts,
	    (u_longlong_t)engine->admit_fence_waits,
	    (u_longlong_t)engine->admit[PR19262_ADMIT_CLIENT].m_pages,
	    (u_longlong_t)engine->admit[PR19262_ADMIT_NETFREE].m_pages);
	/* An overflow is caught by the caller's PAGE_SIZE check. */
	return (n < 0 ? (int)size : n);
}

/*
 * Checkpoint-exempt floor state, ending the legacy line.  Its own frame:
 * the debug build's frame limit leaves the main snprintf no room.
 */
noinline static int
engine_kernel_ckpt_status(engine_t *engine, char *buf, size_t size)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t pinned, pinned_v;
	engine_retired_pinned_now_locked(engine, &pinned, &pinned_v);
	int n = snprintf(buf, size, "ckpt_txg=%llu ckpt_until=%llu "
	    "ckpt_pins=%llu ckpt_builds=%llu ckpt_build_errors=%llu "
	    "ckpt_unpins=%llu retired_pinned=%llu retired_pinned_v=%llu "
	    "pinned_counts=%llu pinned_passes=%llu ckres_pages=%llu "
	    "ckres_disc_left=%llu ckres_min_available=%llu ckres_arms=%llu "
	    "ckres_releases=%llu ckres_refusals=%llu ckres_rejections=%llu "
	    "ckres_churn_x16=%lld ckres_rel_pages=%llu ckres_disc_draws=%llu "
	    "ckres_alone_waits=%llu\n",
	    (u_longlong_t)(engine->checkpoint.ub_txg != 0 ?
	    engine->checkpoint.ub_txg : engine->ckpt_ub.ub_txg),
	    (u_longlong_t)engine->ckpt_until,
	    (u_longlong_t)engine->ckpt_pinned_pages,
	    (u_longlong_t)engine->ckpt_builds,
	    (u_longlong_t)engine->ckpt_build_errors,
	    (u_longlong_t)engine->ckpt_unpins,
	    (u_longlong_t)pinned, (u_longlong_t)pinned_v,
	    (u_longlong_t)engine->retired_pinned_counts,
	    (u_longlong_t)engine->retired_pinned_passes,
	    (u_longlong_t)engine->ckres_pages,
	    (u_longlong_t)engine->ckres_disc_left,
	    (u_longlong_t)engine->ckres_min_available,
	    (u_longlong_t)engine->ckres_arms,
	    (u_longlong_t)engine->ckres_releases,
	    (u_longlong_t)engine->ckres_refusals,
	    (u_longlong_t)engine->ckres_rejections,
	    (longlong_t)engine->ckres_churn_x16,
	    (u_longlong_t)engine->ckres_rel_pages,
	    (u_longlong_t)engine->ckres_disc_draws,
	    (u_longlong_t)engine->ckres_alone_waits);
	/* An overflow is caught by the caller's PAGE_SIZE check. */
	return (n < 0 ? (int)size : n);
}

/*
 * Copy, journal, admission and fence counters of the status line (L22: a
 * separate frame keeps the snapshot's below the debug build's 1,024-byte
 * frame limit, as engine_kernel_headroom does).
 */
noinline static int
engine_kernel_costs(engine_t *engine, char *buf, size_t size)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	int n = snprintf(buf, size,
	    "copy_groups=%llu copy_blocks=%llu copy_bytes=%llu "
	    "copy_setup_ms=%llu copy_read_ms=%llu copy_write_ms=%llu "
	    "copy_verify_ms=%llu copy_check_ms=%llu copy_flush_ms=%llu "
	    "copy_settle_ms=%llu copy_journal_ms=%llu copy_publish_ms=%llu "
	    "copy_total_ms=%llu "
	    "j_bind_calls=%llu j_bind_pages=%llu j_bind_write_ms=%llu "
	    "j_bind_flush_ms=%llu j_head_calls=%llu j_head_write_ms=%llu "
	    "j_head_flush_ms=%llu j_forward_calls=%llu "
	    "j_forward_write_ms=%llu j_forward_flush_ms=%llu "
	    "j_rolls=%llu j_roll_ms=%llu j_lock_ms=%llu j_seal_ms=%llu "
	    "admit_rebuilds=%llu admit_rebuilt=%llu admit_walked=%llu "
	    "admit_updates=%llu admit_checked=%llu pages_checked=%llu "
	    "owner_size=%llu version_size=%llu pages_available=%llu "
	    "fence_requests=%llu fence_auto=%llu fence_growth=%llu "
	    "fence_growth_failures=%llu admit_arena_rejections=%llu "
	    "pages_capacity=%llu retired_now=%llu fence_completions=%llu "
	    "fence_suspended=%llu ",
	    (u_longlong_t)atomic_load_64(&engine->copy_cost.groups),
	    (u_longlong_t)atomic_load_64(&engine->copy_cost.blocks),
	    (u_longlong_t)atomic_load_64(&engine->copy_cost.bytes),
#define	ENGINE_COST_MS(field) \
	    (u_longlong_t)(atomic_load_64(&(field)) / MSEC2NSEC(1))
	    ENGINE_COST_MS(engine->copy_cost.setup),
	    ENGINE_COST_MS(engine->copy_cost.io[0]),
	    ENGINE_COST_MS(engine->copy_cost.io[1]),
	    ENGINE_COST_MS(engine->copy_cost.io[2]),
	    ENGINE_COST_MS(engine->copy_cost.check),
	    ENGINE_COST_MS(engine->copy_cost.flush),
	    ENGINE_COST_MS(engine->copy_cost.settle),
	    ENGINE_COST_MS(engine->copy_cost.journal),
	    ENGINE_COST_MS(engine->copy_cost.publish),
	    ENGINE_COST_MS(engine->copy_cost.total),
	    (u_longlong_t)atomic_load_64(&engine->journal_cost.calls[0]),
	    (u_longlong_t)atomic_load_64(&engine->journal_cost.pages[0]),
	    ENGINE_COST_MS(engine->journal_cost.write[0]),
	    ENGINE_COST_MS(engine->journal_cost.flush[0]),
	    (u_longlong_t)atomic_load_64(&engine->journal_cost.calls[1]),
	    ENGINE_COST_MS(engine->journal_cost.write[1]),
	    ENGINE_COST_MS(engine->journal_cost.flush[1]),
	    (u_longlong_t)atomic_load_64(&engine->journal_cost.calls[2]),
	    ENGINE_COST_MS(engine->journal_cost.write[2]),
	    ENGINE_COST_MS(engine->journal_cost.flush[2]),
	    (u_longlong_t)atomic_load_64(&engine->journal_cost.rolls),
	    ENGINE_COST_MS(engine->journal_cost.roll),
	    ENGINE_COST_MS(engine->journal_cost.lock),
	    ENGINE_COST_MS(engine->journal_cost.seal),
	    (u_longlong_t)atomic_load_64(&engine->admit_cost.rebuilds),
	    (u_longlong_t)atomic_load_64(&engine->admit_cost.rebuilt),
	    (u_longlong_t)atomic_load_64(&engine->admit_cost.walked),
	    (u_longlong_t)atomic_load_64(&engine->admit_cost.updates),
	    (u_longlong_t)atomic_load_64(&engine->admit_cost.checked),
	    (u_longlong_t)atomic_load_64(&engine->admit_cost.pages_checked),
	    (u_longlong_t)sizeof (engine_owner_t),
	    (u_longlong_t)sizeof (engine_version_t),
	    (u_longlong_t)engine->pages->available,
	    (u_longlong_t)engine->fence_requests,
	    (u_longlong_t)engine->fence_auto,
	    (u_longlong_t)engine->fence_growth_triggers,
	    (u_longlong_t)engine->fence_growth_dispatch_failures,
	    (u_longlong_t)engine->admit_arena_rejections,
	    (u_longlong_t)engine->pages->capacity,
	    (u_longlong_t)engine->retired,
	    (u_longlong_t)engine->fence_completions,
	    (u_longlong_t)engine->fence_suspended);
#undef ENGINE_COST_MS
	/* An overflow is caught by the caller's PAGE_SIZE check. */
	return (n < 0 ? (int)size : n);
}

static int
engine_kernel_snapshot(spa_t *spa, char *buf)
{
	engine_t *engine = spa->spa_reloc;
	engine_kernel_t *job = engine->kernel;
	char pool[ZFS_MAX_DATASET_NAME_LEN];
	(void) strlcpy(pool, spa_name(spa), sizeof (pool));
	char state[ENGINE_KERNEL_STATE];
	mutex_enter(&job->lock);
	(void) strlcpy(state, job->state, sizeof (state));
	uint64_t parity = job->parity, copied = job->stats.copied;
	uint64_t passes = job->stats.passes, floor = job->floor;
	int error = job->error;
	mutex_exit(&job->lock);
	pr19262_engine_census_t c = { 0 };
	/* floor= is the live control floor; job->floor only once inactive. */
	mutex_enter(&engine->store_lock);
	boolean_t active = pr19262_engine_active(spa);
	if (active)
		pr19262_engine_census(spa, parity != 0 ? parity : 2, &c);
	mutex_enter(&engine->lock);
	int n = snprintf(buf, PAGE_SIZE, "state=%s pool=%s active=%u "
	    "parity=%llu owners=%llu legacy=%llu weak=%llu retired=%llu "
	    "certified=%llu store_tolerance=%llu floor=%llu "
	    "journal_segments=%llu pages=%llu "
	    "copied=%llu passes=%llu error=%d versions=%llu "
	    "store_checked=%llu store_check_failures=%llu "
	    "pages_evicted=%llu stores=%llu store_pages=%llu "
	    "store_runs=%llu store_tree_ms=%llu store_reserve_ms=%llu "
	    "store_write_ms=%llu store_flush_ms=%llu slot_reads=%llu "
	    "slot_writes=%llu slot_skipped=%llu slot_cached=%llu ",
	    state, pool,
	    active, (u_longlong_t)parity, (u_longlong_t)c.owners,
	    (u_longlong_t)c.legacy_bytes, (u_longlong_t)c.weak_current,
	    (u_longlong_t)c.retired_bytes, (u_longlong_t)c.certified,
	    (u_longlong_t)engine->store_tolerance,
	    (u_longlong_t)(active ? c.floor : floor),
	    (u_longlong_t)c.journal_segments, (u_longlong_t)c.pages_used,
	    (u_longlong_t)copied, (u_longlong_t)passes, error,
	    (u_longlong_t)(c.current_versions + c.retired_versions),
	    (u_longlong_t)engine->store_checked,
	    (u_longlong_t)engine->store_check_failures,
	    (u_longlong_t)engine->pages_evicted,
	    (u_longlong_t)engine->store_cost.stores,
	    (u_longlong_t)engine->store_cost.pages,
	    (u_longlong_t)engine->store_cost.runs,
	    (u_longlong_t)(engine->store_cost.tree / MSEC2NSEC(1)),
	    (u_longlong_t)(engine->store_cost.reserve / MSEC2NSEC(1)),
	    (u_longlong_t)(engine->store_cost.write / MSEC2NSEC(1)),
	    (u_longlong_t)(engine->store_cost.flush / MSEC2NSEC(1)),
	    (u_longlong_t)engine->slot_cost.reads,
	    (u_longlong_t)engine->slot_cost.writes,
	    (u_longlong_t)engine->slot_cost.skipped,
	    (u_longlong_t)engine->slot_cost.cached);
	if (n >= 0 && n < PAGE_SIZE)
		n += engine_kernel_costs(engine, buf + n, PAGE_SIZE - n);
	if (n >= 0 && n < PAGE_SIZE)
		n += engine_kernel_headroom(engine, buf + n, PAGE_SIZE - n);
	if (n >= 0 && n < PAGE_SIZE)
		n += engine_kernel_ckpt_status(engine, buf + n, PAGE_SIZE - n);
	/* Append a complete RAM line or a refusal after the legacy line. */
	if (n < 0 || n >= PAGE_SIZE) {
		buf[0] = '\0';
		n = -EOVERFLOW;
	} else {
		int ram = engine_kernel_ram(spa, buf + n, PAGE_SIZE - n);
		if (ram < 0) {
			const char *why = ram == -ENOSPC ? "truncated" :
			    "arithmetic_overflow";
			ram = snprintf(buf + n, PAGE_SIZE - n,
			    "ram_v=1 ram_complete=0 ram_error=%s\n", why);
			if (ram < 0 || ram >= PAGE_SIZE - n) {
				buf[0] = '\0';
				n = -EOVERFLOW;
			} else {
				n += ram;
			}
		} else {
			n += ram;
		}
	}
	mutex_exit(&engine->lock);
	mutex_exit(&engine->store_lock);
	return (n);
}

ZFS_MODULE_VIRTUAL_PARAM_CALL(zfs, zfs_pr19262_, engine,
	engine_kernel_engine_set, engine_kernel_engine_get, ZMOD_RW,
	"Private pr19262 engine: pool name to start the engine on");
ZFS_MODULE_VIRTUAL_PARAM_CALL(zfs, zfs_pr19262_, convert,
	engine_kernel_convert_set, engine_kernel_convert_get, ZMOD_RW,
	"Private pr19262 engine: \"pool parity [batch_mib]\" converts");
ZFS_MODULE_VIRTUAL_PARAM_CALL(zfs, zfs_pr19262_, fence,
	engine_kernel_fence_set, engine_kernel_fence_get, ZMOD_RW,
	"Private pr19262 engine: pool name to fence");
ZFS_MODULE_VIRTUAL_PARAM_CALL(zfs, zfs_pr19262_, status,
	engine_kernel_status_set, engine_kernel_status_get, ZMOD_RW,
	"Private pr19262 engine: select pool, read state and census");
ZFS_MODULE_VIRTUAL_PARAM_CALL(zfs, zfs_pr19262_, r01,
	engine_kernel_r01_set, engine_kernel_r01_get, ZMOD_RW,
	"Private pr19262 R-01 observer: "
	"\"pool arm [boundary]|prepare parity|copy|release|advance|cancel|"
	"fault-arm epoch-stale|convert-eio|fault-fire\"");
ZFS_MODULE_VIRTUAL_PARAM_CALL(zfs, zfs_pr19262_, r02,
	engine_kernel_r02_set, engine_kernel_r02_get, ZMOD_RW,
	"Private pr19262 R-02 observer: \"pool arm\"");
ZFS_MODULE_VIRTUAL_PARAM_CALL(zfs, zfs_pr19262_, r02_manifest,
	engine_kernel_r02_manifest_set, engine_kernel_r02_manifest_get, ZMOD_RW,
	"Test-only read-only R-02 manifest witness: pool name");
ZFS_MODULE_VIRTUAL_PARAM_CALL(zfs, zfs_pr19262_, denominator,
	engine_kernel_denominator_set, engine_kernel_denominator_get, ZMOD_RW,
	"Private RAM denominator BP-reference slice: \"pool snapshot\"");
ZFS_MODULE_PARAM(zfs, zfs_pr19262_, convert_group, UINT, ZMOD_RW,
	"Private pr19262 engine: copies per data flush and journal round");
ZFS_MODULE_PARAM(zfs, zfs_pr19262_, convert_group_bytes, U64, ZMOD_RW,
	"Private pr19262 engine: data bytes per data flush and journal round");
ZFS_MODULE_PARAM(zfs, zfs_pr19262_, store_verify, INT, ZMOD_RW,
	"Private pr19262 engine: check incremental catalog stores (tests)");
ZFS_MODULE_PARAM(zfs, zfs_pr19262_, page_cache, U64, ZMOD_RW,
	"Private pr19262 engine: clean catalog leaves kept in memory (0: all)");
ZFS_MODULE_PARAM(zfs, zfs_pr19262_, r02_reseed, INT, ZMOD_RW,
	"Private R-02: 1 (default) admits exact-new-leaf reseed; 0 keeps D64");
ZFS_MODULE_PARAM(zfs, zfs_pr19262_, journal_group, INT, ZMOD_RW,
	"Private pr19262 engine: group commit of ZIL journal binds (L20)");
#endif	/* _KERNEL */
