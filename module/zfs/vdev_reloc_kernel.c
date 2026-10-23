/* SPDX-License-Identifier: CDDL-1.0 */
/*
 * Kernel driving of the private online-lifetime engine, included by the
 * converter when built into zfs.ko.  Module parameters start the engine on
 * an imported pool, run a conversion in a kernel thread, fence, and report
 * the census; nothing here is a supported interface.
 */
#ifdef _KERNEL
#include <sys/zfs_context.h>

#define	ENGINE_KERNEL_STATE 16

static struct {
	kmutex_t lock;
	char pool[ZFS_MAX_DATASET_NAME_LEN];
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
} engine_kernel;

static void
engine_kernel_init(void)
{
	mutex_init(&engine_kernel.lock, NULL, MUTEX_DEFAULT, NULL);
	(void) strlcpy(engine_kernel.state, "idle", ENGINE_KERNEL_STATE);
}

static void
engine_kernel_fini(void)
{
	mutex_destroy(&engine_kernel.lock);
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
engine_kernel_worker(void *arg)
{
	(void) arg;
	spa_t *spa;
	uint64_t birth = 0, floor = 0, reclaimed = 0, released = 0;
	pr19262_engine_convert_stats_t st;
	memset(&st, 0, sizeof (st));
	int error = spa_open(engine_kernel.pool, &spa, FTAG);
	if (error == 0) {
		error = pr19262_engine_epoch(spa, engine_kernel.parity, &birth);
		if (error == 0)
			error = pr19262_engine_convert(spa, birth,
			    engine_kernel.batch, 4096, &st);
		if (error == 0)
			error = pr19262_engine_fence(spa, &floor, &reclaimed,
			    &released, NULL);
		if (error == 0)
			error = pr19262_engine_certify(spa,
			    engine_kernel.parity);
		spa_close(spa, FTAG);
	}
	mutex_enter(&engine_kernel.lock);
	engine_kernel.stats = st;
	engine_kernel.birth = birth;
	engine_kernel.floor = floor;
	engine_kernel.reclaimed = reclaimed;
	engine_kernel.released = released;
	engine_kernel.error = error;
	engine_kernel.running = B_FALSE;
	(void) strlcpy(engine_kernel.state, error == 0 ? "converted" :
	    "error", ENGINE_KERNEL_STATE);
	mutex_exit(&engine_kernel.lock);
	zfs_dbgmsg("pr19262 kernel convert pool=%s parity=%llu birth=%llu "
	    "copied=%llu error=%d", engine_kernel.pool,
	    (u_longlong_t)engine_kernel.parity, (u_longlong_t)birth,
	    (u_longlong_t)st.copied, error);
	thread_exit();
}

/* zfs_pr19262_engine: start the engine (durable) on an imported pool. */
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
	if (pr19262_engine_active(spa)) {
		spa_close(spa, FTAG);
		return (-EEXIST);
	}
	mutex_enter(&engine.lock);
	boolean_t busy = engine.spa != NULL;
	mutex_exit(&engine.lock);
	if (busy || !spa_writeable(spa) || !spa->spa_trust_config ||
	    spa->spa_root_vdev->vdev_child[0]->vdev_ops != &vdev_raidz_ops) {
		spa_close(spa, FTAG);
		return (busy ? -EBUSY : -ENOTSUP);
	}
	pr19262_engine_start(spa);
	pr19262_engine_store_enable(spa);
	txg_wait_synced(spa_get_dsl(spa), 0);
	mutex_enter(&engine_kernel.lock);
	(void) strlcpy(engine_kernel.pool, name, sizeof (engine_kernel.pool));
	(void) strlcpy(engine_kernel.state, "idle", ENGINE_KERNEL_STATE);
	engine_kernel.error = 0;
	mutex_exit(&engine_kernel.lock);
	spa_close(spa, FTAG);
	zfs_dbgmsg("pr19262 kernel engine started on %s", name);
	return (0);
}

static int
engine_kernel_engine_get(char *buf, zfs_kernel_param_t *kp)
{
	(void) kp;
	mutex_enter(&engine_kernel.lock);
	int n = scnprintf(buf, PAGE_SIZE, "%s\n", engine_kernel.pool);
	mutex_exit(&engine_kernel.lock);
	return (n);
}

/* zfs_pr19262_convert: "pool parity [batch_mib]" starts a conversion. */
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
	boolean_t ok = pr19262_engine_active(spa) &&
	    pr19262_engine_persistent(spa) && spa_writeable(spa);
	spa_close(spa, FTAG);
	if (!ok)
		return (-ENOTSUP);
	mutex_enter(&engine_kernel.lock);
	if (engine_kernel.running || strcmp(engine_kernel.pool, name) != 0) {
		mutex_exit(&engine_kernel.lock);
		return (-EBUSY);
	}
	engine_kernel.running = B_TRUE;
	engine_kernel.parity = parity;
	engine_kernel.batch = batch << 20;
	engine_kernel.error = 0;
	(void) strlcpy(engine_kernel.state, "running", ENGINE_KERNEL_STATE);
	mutex_exit(&engine_kernel.lock);
	(void) thread_create(NULL, 0, engine_kernel_worker, NULL, 0, &p0,
	    TS_RUN, minclsyspri);
	return (0);
}

static int
engine_kernel_convert_get(char *buf, zfs_kernel_param_t *kp)
{
	(void) kp;
	mutex_enter(&engine_kernel.lock);
	int n = scnprintf(buf, PAGE_SIZE, "%s %llu %llu\n", engine_kernel.pool,
	    (u_longlong_t)engine_kernel.parity,
	    (u_longlong_t)(engine_kernel.batch >> 20));
	mutex_exit(&engine_kernel.lock);
	return (n);
}

/* zfs_pr19262_fence: one fence on the named pool. */
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
	error = pr19262_engine_fence(spa, &floor, &reclaimed, &released,
	    NULL);
	spa_close(spa, FTAG);
	if (error == 0) {
		mutex_enter(&engine_kernel.lock);
		engine_kernel.floor = floor;
		engine_kernel.reclaimed = reclaimed;
		engine_kernel.released = released;
		mutex_exit(&engine_kernel.lock);
	}
	return (-error);
}

static int
engine_kernel_fence_get(char *buf, zfs_kernel_param_t *kp)
{
	(void) kp;
	mutex_enter(&engine_kernel.lock);
	int n = scnprintf(buf, PAGE_SIZE, "floor=%llu reclaimed=%llu "
	    "released=%llu\n", (u_longlong_t)engine_kernel.floor,
	    (u_longlong_t)engine_kernel.reclaimed,
	    (u_longlong_t)engine_kernel.released);
	mutex_exit(&engine_kernel.lock);
	return (n);
}

/* zfs_pr19262_status: state and census of the engine pool. */
static int
engine_kernel_status_set(const char *val, zfs_kernel_param_t *kp)
{
	(void) val, (void) kp;
	return (-EPERM);
}

static int
engine_kernel_status_get(char *buf, zfs_kernel_param_t *kp)
{
	(void) kp;
	mutex_enter(&engine_kernel.lock);
	char pool[ZFS_MAX_DATASET_NAME_LEN];
	(void) strlcpy(pool, engine_kernel.pool, sizeof (pool));
	char state[ENGINE_KERNEL_STATE];
	(void) strlcpy(state, engine_kernel.state, sizeof (state));
	uint64_t parity = engine_kernel.parity;
	uint64_t copied = engine_kernel.stats.copied;
	uint64_t passes = engine_kernel.stats.passes;
	uint64_t floor = engine_kernel.floor;
	int error = engine_kernel.error;
	mutex_exit(&engine_kernel.lock);
	pr19262_engine_census_t c;
	memset(&c, 0, sizeof (c));
	boolean_t active = B_FALSE;
	spa_t *spa;
	if (pool[0] != '\0' && spa_open(pool, &spa, FTAG) == 0) {
		active = pr19262_engine_active(spa);
		if (active)
			pr19262_engine_census(spa, parity != 0 ? parity : 2,
			    &c);
		spa_close(spa, FTAG);
	}
	int n = scnprintf(buf, PAGE_SIZE, "state=%s pool=%s active=%u "
	    "parity=%llu owners=%llu legacy=%llu weak=%llu retired=%llu "
	    "certified=%llu floor=%llu journal_segments=%llu pages=%llu "
	    "copied=%llu passes=%llu error=%d versions=%llu "
	    "store_checked=%llu store_check_failures=%llu "
	    "pages_evicted=%llu stores=%llu store_pages=%llu "
	    "store_runs=%llu store_tree_ms=%llu store_reserve_ms=%llu "
	    "store_write_ms=%llu store_flush_ms=%llu slot_reads=%llu "
	    "slot_writes=%llu slot_skipped=%llu slot_cached=%llu "
	    "copy_groups=%llu copy_blocks=%llu copy_bytes=%llu "
	    "copy_setup_ms=%llu copy_read_ms=%llu copy_write_ms=%llu "
	    "copy_verify_ms=%llu copy_check_ms=%llu copy_flush_ms=%llu "
	    "copy_settle_ms=%llu copy_journal_ms=%llu copy_publish_ms=%llu "
	    "copy_total_ms=%llu "
	    "j_bind_calls=%llu j_bind_pages=%llu j_bind_write_ms=%llu "
	    "j_bind_flush_ms=%llu j_head_calls=%llu j_head_write_ms=%llu "
	    "j_head_flush_ms=%llu j_forward_calls=%llu "
	    "j_forward_write_ms=%llu j_forward_flush_ms=%llu "
	    "j_rolls=%llu j_roll_ms=%llu j_lock_ms=%llu j_seal_ms=%llu\n",
	    state, pool,
	    active, (u_longlong_t)parity, (u_longlong_t)c.owners,
	    (u_longlong_t)c.legacy_bytes, (u_longlong_t)c.weak_current,
	    (u_longlong_t)c.retired_bytes, (u_longlong_t)c.certified,
	    (u_longlong_t)(floor != 0 ? floor : c.floor),
	    (u_longlong_t)c.journal_segments, (u_longlong_t)c.pages_used,
	    (u_longlong_t)copied, (u_longlong_t)passes, error,
	    (u_longlong_t)(c.current_versions + c.retired_versions),
	    (u_longlong_t)engine_store_checked,
	    (u_longlong_t)engine_store_check_failures,
	    (u_longlong_t)engine_pages_evicted,
	    (u_longlong_t)engine_store_cost.stores,
	    (u_longlong_t)engine_store_cost.pages,
	    (u_longlong_t)engine_store_cost.runs,
	    (u_longlong_t)(engine_store_cost.tree / MSEC2NSEC(1)),
	    (u_longlong_t)(engine_store_cost.reserve / MSEC2NSEC(1)),
	    (u_longlong_t)(engine_store_cost.write / MSEC2NSEC(1)),
	    (u_longlong_t)(engine_store_cost.flush / MSEC2NSEC(1)),
	    (u_longlong_t)engine_slot_cost.reads,
	    (u_longlong_t)engine_slot_cost.writes,
	    (u_longlong_t)engine_slot_cost.skipped,
	    (u_longlong_t)engine_slot_cost.cached,
	    (u_longlong_t)atomic_load_64(&engine_copy_cost.groups),
	    (u_longlong_t)atomic_load_64(&engine_copy_cost.blocks),
	    (u_longlong_t)atomic_load_64(&engine_copy_cost.bytes),
#define	ENGINE_COST_MS(field) \
	    (u_longlong_t)(atomic_load_64(&(field)) / MSEC2NSEC(1))
	    ENGINE_COST_MS(engine_copy_cost.setup),
	    ENGINE_COST_MS(engine_copy_cost.io[0]),
	    ENGINE_COST_MS(engine_copy_cost.io[1]),
	    ENGINE_COST_MS(engine_copy_cost.io[2]),
	    ENGINE_COST_MS(engine_copy_cost.check),
	    ENGINE_COST_MS(engine_copy_cost.flush),
	    ENGINE_COST_MS(engine_copy_cost.settle),
	    ENGINE_COST_MS(engine_copy_cost.journal),
	    ENGINE_COST_MS(engine_copy_cost.publish),
	    ENGINE_COST_MS(engine_copy_cost.total),
	    (u_longlong_t)atomic_load_64(&engine_journal_cost.calls[0]),
	    (u_longlong_t)atomic_load_64(&engine_journal_cost.pages[0]),
	    ENGINE_COST_MS(engine_journal_cost.write[0]),
	    ENGINE_COST_MS(engine_journal_cost.flush[0]),
	    (u_longlong_t)atomic_load_64(&engine_journal_cost.calls[1]),
	    ENGINE_COST_MS(engine_journal_cost.write[1]),
	    ENGINE_COST_MS(engine_journal_cost.flush[1]),
	    (u_longlong_t)atomic_load_64(&engine_journal_cost.calls[2]),
	    ENGINE_COST_MS(engine_journal_cost.write[2]),
	    ENGINE_COST_MS(engine_journal_cost.flush[2]),
	    (u_longlong_t)atomic_load_64(&engine_journal_cost.rolls),
	    ENGINE_COST_MS(engine_journal_cost.roll),
	    ENGINE_COST_MS(engine_journal_cost.lock),
	    ENGINE_COST_MS(engine_journal_cost.seal));
#undef ENGINE_COST_MS
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
	engine_kernel_status_set, engine_kernel_status_get, ZMOD_RD,
	"Private pr19262 engine: state and census");
ZFS_MODULE_PARAM(zfs, zfs_pr19262_, convert_group, UINT, ZMOD_RW,
	"Private pr19262 engine: copies per data flush and journal round");
ZFS_MODULE_PARAM(zfs, zfs_pr19262_, convert_group_bytes, U64, ZMOD_RW,
	"Private pr19262 engine: data bytes per data flush and journal round");
ZFS_MODULE_PARAM(zfs, zfs_pr19262_, store_verify, INT, ZMOD_RW,
	"Private pr19262 engine: check incremental catalog stores (tests)");
ZFS_MODULE_PARAM(zfs, zfs_pr19262_, page_cache, U64, ZMOD_RW,
	"Private pr19262 engine: clean catalog leaves kept in memory (0: all)");
#endif	/* _KERNEL */
