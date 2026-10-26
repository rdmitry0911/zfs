/* SPDX-License-Identifier: CDDL-1.0 */
/* Private kernel driver: each SPA owns its conversion job and progress. */
#ifdef _KERNEL
#include <sys/zfs_context.h>

#define	ENGINE_KERNEL_STATE 16

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
} engine_kernel_t;

/* A legacy parameter selects a pool to observe, never a worker to own. */
static struct {
	kmutex_t lock;
	char pool[ZFS_MAX_DATASET_NAME_LEN];
	char cached_status[PAGE_SIZE];
} engine_selector;

static void
engine_kernel_init(void)
{
	mutex_init(&engine_selector.lock, NULL, MUTEX_DEFAULT, NULL);
	(void) strlcpy(engine_selector.cached_status,
	    "state=idle pool= active=0\n",
	    sizeof (engine_selector.cached_status));
}

static void
engine_kernel_fini(void)
{
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
	uint64_t birth = 0, floor = 0, reclaimed = 0, released = 0;
	pr19262_engine_convert_stats_t st = { 0 };
	/* convert_set transfers its open SPA reference to this worker. */
	uint64_t parity = job->parity, batch = job->batch;
	int error = pr19262_engine_epoch(spa, parity, &birth);
	if (error == 0)
		error = pr19262_engine_convert(spa, birth, batch, 4096, &st);
	if (error == 0)
		error = pr19262_engine_fence(spa, &floor, &reclaimed,
		    &released, NULL);
	if (error == 0)
		error = pr19262_engine_certify(spa, parity);
	mutex_enter(&job->lock);
	job->stats = st;
	job->birth = birth;
	job->floor = floor;
	job->reclaimed = reclaimed;
	job->released = released;
	job->error = error;
	job->running = B_FALSE;
	(void) strlcpy(job->state, error == 0 ? "converted" : "error",
	    ENGINE_KERNEL_STATE);
	mutex_exit(&job->lock);
	zfs_dbgmsg("pr19262 kernel convert pool=%s parity=%llu birth=%llu "
	    "copied=%llu error=%d", spa_name(spa), (u_longlong_t)parity,
	    (u_longlong_t)birth, (u_longlong_t)st.copied, error);
	engine_kernel_cache(spa);
	spa_close(spa, FTAG);
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
	if (job->running) {
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
	if (job->running) {
		mutex_exit(&job->lock);
		spa_close(spa, FTAG);
		return (-EBUSY);
	}
	job->running = B_TRUE;
	job->parity = parity;
	job->batch = batch << 20;
	job->error = 0;
	(void) strlcpy(job->state, "running", ENGINE_KERNEL_STATE);
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
	mutex_enter(&engine->store_lock);
	boolean_t active = pr19262_engine_active(spa);
	if (active)
		pr19262_engine_census(spa, parity != 0 ? parity : 2, &c);
	mutex_enter(&engine->lock);
	int n = snprintf(buf, PAGE_SIZE, "state=%s pool=%s active=%u "
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
	    "j_rolls=%llu j_roll_ms=%llu j_lock_ms=%llu j_seal_ms=%llu "
	    "admit_rebuilds=%llu admit_rebuilt=%llu admit_walked=%llu "
	    "admit_updates=%llu admit_checked=%llu pages_checked=%llu "
	    "owner_size=%llu version_size=%llu pages_available=%llu\n",
	    state, pool,
	    active, (u_longlong_t)parity, (u_longlong_t)c.owners,
	    (u_longlong_t)c.legacy_bytes, (u_longlong_t)c.weak_current,
	    (u_longlong_t)c.retired_bytes, (u_longlong_t)c.certified,
	    (u_longlong_t)(floor != 0 ? floor : c.floor),
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
	    (u_longlong_t)engine->slot_cost.cached,
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
	    (u_longlong_t)engine->pages->available);
#undef ENGINE_COST_MS
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
ZFS_MODULE_PARAM(zfs, zfs_pr19262_, convert_group, UINT, ZMOD_RW,
	"Private pr19262 engine: copies per data flush and journal round");
ZFS_MODULE_PARAM(zfs, zfs_pr19262_, convert_group_bytes, U64, ZMOD_RW,
	"Private pr19262 engine: data bytes per data flush and journal round");
ZFS_MODULE_PARAM(zfs, zfs_pr19262_, store_verify, INT, ZMOD_RW,
	"Private pr19262 engine: check incremental catalog stores (tests)");
ZFS_MODULE_PARAM(zfs, zfs_pr19262_, page_cache, U64, ZMOD_RW,
	"Private pr19262 engine: clean catalog leaves kept in memory (0: all)");
#endif	/* _KERNEL */
