/* SPDX-License-Identifier: CDDL-1.0 */
/*
 * Private normal writable-SPA V/P ownership engine, included by metaslab.c.
 * All new allocations, including MOS/ditto, are bound automatically. Native
 * V metadata remains allocated on final free; no supplied floor permits reuse.
 * The default backend is RAM-only. The optional private store pairs catalog
 * checkpoints with native uberblocks; it is not a supported disk feature.
 */
#include <sys/vdev_reloc_engine.h>
#include <sys/vdev_raidz.h>
#include <sys/zio_checksum.h>

typedef struct engine_owner engine_owner_t;

typedef struct engine_version {
	struct engine_version *next;
	engine_owner_t *owner;
	uint64_t p;
	uint64_t size;
	uint64_t birth;
	uint64_t width;
	uint64_t parity;
	uint64_t leases;
	uint64_t writers;
	uint64_t generation;
	uint64_t journal_seq;
	/* First durable catalog txg that listed this version as retired. */
	uint64_t retired_txg;
	boolean_t ready;
	boolean_t retired;
} engine_version_t;

typedef struct engine_pending {
	struct engine_pending *next;
	zio_t *zio;
} engine_pending_t;

typedef struct engine_owner {
	avl_node_t avl;
	uint64_t v;
	/* Immutable V size; each backing owns its independent P size. */
	uint64_t size;
	uint64_t allocation_txg;
	uint64_t catalog_txg;
	uint64_t claimed_txg;
	uint64_t queued_writers;
	boolean_t journal_pending;
	boolean_t journal_touched;
	boolean_t checkpoint_only;
	engine_version_t *versions;
	engine_version_t *cur;
	engine_pending_t *pending;
	boolean_t copying;
	uint64_t incarnation;
	uint64_t holds;
	uint64_t created;
	uint64_t retired_at;
	/* First durable catalog txg that listed the owner as retired. */
	uint64_t retired_txg;
	boolean_t retired;
	boolean_t aborted;
	boolean_t releasing;
} engine_owner_t;

typedef struct engine_hold {
	uint64_t refs;
	uint_t count;
	engine_owner_t *owner[SPA_DVAS_PER_BP];
	int error;
	/* Diagnostic: 1 inherited, 2 no owner (ENOENT), 3 stale verdict. */
	uint8_t error_site;
	uint64_t started;
	boolean_t gang;
	boolean_t write;
	/* DVAs on other top-level vdevs: native, never translated. */
	uint_t foreign;
} engine_hold_t;

/*
 * A released owner identity: its journal bindings are folded.  It is
 * needed only while a journal segment up to the recorded index exists.
 */
typedef struct engine_tomb {
	avl_node_t avl;
	uint64_t v;
	uint64_t incarnation;
	uint64_t segment;
	boolean_t expired;
} engine_tomb_t;

typedef struct engine_copy {
	engine_owner_t *owner;
	engine_version_t *source;
	engine_version_t *stage;
	blkptr_t original;
	uint64_t psize;
} engine_copy_t;

typedef struct engine_j_segment {
	struct engine_j_segment *next;
	uint64_t arena;
	uint64_t epoch[2];
	uint64_t seq;
	zio_cksum_t hash;
	uint64_t serial;
	uint64_t phase;
	uint64_t target;
	uint64_t target_epoch[2];
	/* Chain position, catalog txg folding every record, free gate. */
	uint64_t index;
	uint64_t fold_txg;
	uint64_t free_after;
} engine_j_segment_t;

static int engine_journal_bind_copy(engine_copy_t *);
static void engine_journal_bind_copies(engine_copy_t **, uint_t,
    const boolean_t *, int *);
static void engine_journal_test_event(spa_t *, const blkptr_t *, int);
static void engine_journal_fold(spa_t *, uint64_t);
static uint_t engine_store_tolerated(void);
/* Default converter batch the standing reserve keeps free. */
#define	ENGINE_ADMIT_BATCH (8ULL << 20)

/*
 * Replicated raw metadata: a write or a required read succeeds when at
 * most the certified parity of members are absent or failing, since any
 * later import must then still find one copy on a present member.  With
 * no certificate every member is required, as before.
 */
typedef struct engine_quorum {
	uint_t failures;
	uint_t absent;
	int error;
} engine_quorum_t;

/* One page of a raw step that every member of the vdev writes. */
typedef struct engine_member_page {
	uint64_t arena;
	uint64_t page;
	abd_t *abd;
} engine_member_page_t;

static void engine_members_flush(spa_t *, engine_quorum_t *);

static void
engine_quorum_add(engine_quorum_t *q, int error)
{
	if (error == 0)
		return;
	q->failures++;
	q->absent += error == ENXIO;
	if (q->error == 0 || error != ENXIO)
		q->error = error;
}

static int
engine_quorum_result(const engine_quorum_t *q)
{
	return (q->failures > engine_store_tolerated() ? q->error : 0);
}

static struct {
	kmutex_t lock;
	kmutex_t journal_lock;
	kmutex_t store_lock;
	kcondvar_t changed;
	spa_t *spa;
	vdev_t *vd;
	uint64_t vd_guid;
	/* Another normal-class top-level vdev: client data can fall back. */
	boolean_t foreign_normal;
	uint64_t alloc_guarded;
	avl_tree_t owners;
	avl_tree_t tombstones;
	zfs_range_tree_t *free;
	zfs_range_tree_t *legacy;
	zfs_range_tree_t *unusable;
	uint64_t total;
	uint64_t next_incarnation;
	uint64_t pairs;
	uint64_t alloc_fail_after;
	uint64_t retired;
	uint64_t aborts;
	uint64_t translated;
	uint64_t metadata;
	uint64_t legacy_reads;
	boolean_t block_new;
	uint64_t sequence;
	boolean_t fail_writes;
	uint64_t retired_limit;
	uint64_t copied;
	uint64_t copy_aborts;
	uint64_t deferred;
	engine_pending_t *parked;
	boolean_t park_next;
	boolean_t persistent;
	uint64_t arena;
	uint64_t arena_bytes;
	uint8_t *bitmap;
	uint64_t store_codec;
	zfs_range_tree_t *store_extents;
	uint64_t revision;
	uint64_t stored_revision;
	uint64_t journal_arena;
	uint64_t journal_bytes;
	uint64_t journal_epoch[2];
	uint64_t journal_root;
	uint64_t journal_root_epoch[2];
	zfs_range_tree_t *journal_extents;
	engine_j_segment_t *journal_segments;
	engine_j_segment_t *journal_tail;
	engine_j_segment_t *journal_current;
	engine_j_segment_t *journal_pending;
	/* Segments cut from the chain head, freed once no root reaches them. */
	engine_j_segment_t *journal_retired;
	uint64_t journal_segment_count;
	uint64_t journal_freed_segments;
	uint64_t journal_freed_bytes;
	uint64_t journal_seq;
	uint64_t journal_serial;
	zio_cksum_t journal_hash;
	boolean_t journal_formatted;
	boolean_t journal_ready;
	boolean_t journal_repair;
	int journal_error;
	uberblock_t checkpoint;
	/* Last uberblock written and flushed, and its catalog txg. */
	uint64_t durable_ub_txg;
	uint64_t durable_catalog_txg;
	/* Descriptor of the last stored and of the last durable catalog. */
	uint64_t stored_desc[16];
	uint64_t durable_desc[16];
	uint64_t certified;
	/* The owner-release sync task frees native V through this thread. */
	void *release_thread;
	/* Vector admission state and the standing conversion reserve. */
	engine_admit_class_t admit[PR19262_ADMIT_CLASSES];
	uint64_t admit_parity;
	uint64_t admit_batch;
	uint64_t admit_reserve;
	uint64_t admit_reserve_growth;
	uint64_t admit_reserve_legacy;
	uint64_t admit_reserve_weak;
	uint64_t admit_refreshed;
	uint64_t admit_growth_peak;
	uint64_t admit_space_rejections;
	uint64_t admit_restarts;
	/* The fence the admission schedules past the retired limit. */
	kcondvar_t fence_cv;
	boolean_t fence_pending;
	boolean_t fence_quiesced;
	/* The last scheduled fence reclaimed and released nothing. */
	boolean_t fence_stalled;
	int fence_error;
	uint64_t fence_requests;
	uint64_t fence_auto;
	uint64_t admit_fence_waits;
	uint64_t stale_verdicts;
	uint64_t retire_prints;
	uint64_t abort_prints;
	uint64_t inherit_prints;
	uint64_t hold_report_prints;
	uint64_t gate_prints;
} engine;

static engine_version_t *
engine_version_new(engine_owner_t *e, uint64_t p, uint64_t size,
    uint64_t birth, boolean_t ready)
{
	engine_version_t *b = kmem_zalloc(sizeof (*b), KM_SLEEP);
	b->owner = e;
	b->p = p;
	b->size = size;
	b->birth = birth;
	b->generation = ++engine.sequence;
	vdev_raidz_layout_for_birth(engine.vd, birth, &b->width, &b->parity);
	b->ready = ready;
	b->next = e->versions;
	e->versions = b;
	engine.revision++;
	return (b);
}

static void
engine_resume(engine_pending_t *p)
{
	while (p != NULL) {
		engine_pending_t *next = p->next;
		VERIFY3U(taskq_dispatch(system_taskq, zio_execute, p->zio,
		    TQ_SLEEP), !=, TASKQID_INVALID);
		kmem_free(p, sizeof (*p));
		p = next;
	}
}

static void
engine_defer(zio_t *zio, engine_pending_t **list)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	engine_pending_t *p = kmem_alloc(sizeof (*p), KM_SLEEP);
	p->zio = zio;
	p->next = *list;
	*list = p;
	/* Resume at this same dispatch stage, without blocking its executor. */
	zio->io_stage >>= 1;
}

void
pr19262_engine_park_next(void)
{
	mutex_enter(&engine.lock);
	VERIFY(!engine.park_next && engine.parked == NULL);
	engine.park_next = B_TRUE;
	mutex_exit(&engine.lock);
}

uint64_t
pr19262_engine_waiting(boolean_t writers)
{
	uint64_t count = 0;
	mutex_enter(&engine.lock);
	if (!writers) {
		for (engine_pending_t *p = engine.parked; p != NULL;
		    p = p->next)
			count++;
	} else {
		for (engine_owner_t *e = avl_first(&engine.owners); e != NULL;
		    e = AVL_NEXT(&engine.owners, e))
			for (engine_pending_t *p = e->pending; p != NULL;
			    p = p->next)
				count++;
	}
	mutex_exit(&engine.lock);
	return (count);
}

void
pr19262_engine_unpark(void)
{
	mutex_enter(&engine.lock);
	engine_pending_t *p = engine.parked;
	engine.parked = NULL;
	mutex_exit(&engine.lock);
	engine_resume(p);
}

void
pr19262_engine_fail_writes(boolean_t fail)
{
	mutex_enter(&engine.lock);
	engine.fail_writes = fail;
	mutex_exit(&engine.lock);
}

void
pr19262_engine_retired_limit(uint64_t limit)
{
	mutex_enter(&engine.lock);
	engine.retired_limit = limit;
	mutex_exit(&engine.lock);
}

void
pr19262_engine_block_new(boolean_t block)
{
	mutex_enter(&engine.lock);
	engine.block_new = block;
	mutex_exit(&engine.lock);
}

void
pr19262_engine_alloc_fail_after(uint64_t attempts)
{
	mutex_enter(&engine.lock);
	engine.alloc_fail_after = attempts;
	mutex_exit(&engine.lock);
}

uint64_t
pr19262_engine_limit(spa_t *spa, boolean_t netfree)
{
	uint64_t limit = UINT64_MAX;
	mutex_enter(&engine.lock);
	if (engine.spa == spa && spa != NULL) {
		uint64_t protected = (netfree ? 8ULL : 16ULL) << 20;
		uint64_t free = zfs_range_tree_space(engine.free);
		limit = free > protected ? free - protected : 0;
		if (!netfree && engine.retired >= engine.retired_limit)
			limit = 0;
	}
	mutex_exit(&engine.lock);
	return (limit);
}

static int
engine_compare(const void *arg1, const void *arg2)
{
	const engine_owner_t *a = arg1, *b = arg2;
	return (a->v < b->v ? -1 : a->v > b->v ? 1 : 0);
}

static engine_owner_t *
engine_find(uint64_t v)
{
	engine_owner_t key = { .v = v };
	return (avl_find(&engine.owners, &key, NULL));
}

static int
engine_tomb_compare(const void *arg1, const void *arg2)
{
	const engine_tomb_t *a = arg1, *b = arg2;
	if (a->v != b->v)
		return (a->v < b->v ? -1 : 1);
	return (a->incarnation < b->incarnation ? -1 :
	    a->incarnation > b->incarnation ? 1 : 0);
}

static boolean_t
engine_tombstoned(uint64_t v, uint64_t incarnation)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	engine_tomb_t key = { .v = v, .incarnation = incarnation };
	return (avl_find(&engine.tombstones, &key, NULL) != NULL);
}

static void
engine_tombstones_clear(void)
{
	void *cookie = NULL;
	engine_tomb_t *t;
	while ((t = avl_destroy_nodes(&engine.tombstones, &cookie)) != NULL)
		kmem_free(t, sizeof (*t));
}

/* Record a released journaled identity; later segments cannot bind it. */
static void
engine_tombstone_add(uint64_t v, uint64_t incarnation, uint64_t segment)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	engine_tomb_t *t = kmem_zalloc(sizeof (*t), KM_SLEEP);
	t->v = v;
	t->incarnation = incarnation;
	t->segment = segment;
	avl_index_t where;
	engine_tomb_t *found = avl_find(&engine.tombstones, t, &where);
	if (found != NULL) {
		found->segment = MAX(found->segment, segment);
		found->expired = B_FALSE;
		kmem_free(t, sizeof (*t));
		return;
	}
	avl_insert(&engine.tombstones, t, where);
}

static engine_owner_t *
engine_overlap(uint64_t v, uint64_t size)
{
	engine_owner_t key = { .v = v };
	avl_index_t where;
	engine_owner_t *e = avl_find(&engine.owners, &key, &where);
	if (e != NULL)
		return (e);
	e = avl_nearest(&engine.owners, where, AVL_BEFORE);
	if (e != NULL && v - e->v < e->size)
		return (e);
	e = avl_nearest(&engine.owners, where, AVL_AFTER);
	return (e != NULL && e->v - v < size ? e : NULL);
}

#ifdef _KERNEL
static void engine_kernel_init(void);
static void engine_kernel_fini(void);
#endif

void
pr19262_engine_init(void)
{
#ifdef _KERNEL
	engine_kernel_init();
#endif
	mutex_init(&engine.lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&engine.journal_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&engine.store_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&engine.changed, NULL, CV_DEFAULT, NULL);
	cv_init(&engine.fence_cv, NULL, CV_DEFAULT, NULL);
	avl_create(&engine.owners, engine_compare, sizeof (engine_owner_t),
	    offsetof(engine_owner_t, avl));
	avl_create(&engine.tombstones, engine_tomb_compare,
	    sizeof (engine_tomb_t), offsetof(engine_tomb_t, avl));
}

void
pr19262_engine_fini(void)
{
#ifdef _KERNEL
	engine_kernel_fini();
#endif
	/* A translated test image must never be handed to an old importer. */
	VERIFY3P(engine.spa, ==, NULL);
	VERIFY(avl_is_empty(&engine.owners));
	avl_destroy(&engine.owners);
	engine_tombstones_clear();
	avl_destroy(&engine.tombstones);
	cv_destroy(&engine.changed);
	cv_destroy(&engine.fence_cv);
	mutex_destroy(&engine.journal_lock);
	mutex_destroy(&engine.store_lock);
	mutex_destroy(&engine.lock);
}

boolean_t
pr19262_engine_active(spa_t *spa)
{
	mutex_enter(&engine.lock);
	boolean_t active = engine.spa == spa && spa != NULL;
	mutex_exit(&engine.lock);
	return (active);
}

static uint64_t engine_admit_protected(uint_t);

/* The translated vdev: the first RAIDZ top-level vdev, or the one named. */
static vdev_t *
engine_target_vdev(spa_t *spa, uint64_t guid)
{
	vdev_t *root = spa->spa_root_vdev;
	for (uint64_t c = 0; c < root->vdev_children; c++) {
		vdev_t *vd = root->vdev_child[c];
		if (vd->vdev_ops != &vdev_raidz_ops || vd->vdev_islog)
			continue;
		if (guid == 0 || vd->vdev_guid == guid)
			return (vd);
	}
	return (NULL);
}

/* Another normal-class top-level vdev can take client data instead. */
static boolean_t
engine_foreign_normal(spa_t *spa, vdev_t *target)
{
	vdev_t *root = spa->spa_root_vdev;
	for (uint64_t c = 0; c < root->vdev_children; c++) {
		vdev_t *vd = root->vdev_child[c];
		if (vd != target && !vd->vdev_islog && vd->vdev_mg != NULL &&
		    vd->vdev_mg->mg_class == spa_normal_class(spa))
			return (B_TRUE);
	}
	return (B_FALSE);
}

vdev_t *
pr19262_engine_vdev(spa_t *spa)
{
	mutex_enter(&engine.lock);
	vdev_t *vd = engine.spa == spa && spa != NULL ? engine.vd : NULL;
	mutex_exit(&engine.lock);
	return (vd);
}

/* True when no DVA of the block lives on the translated vdev. */
boolean_t
pr19262_engine_bp_foreign(spa_t *spa, const blkptr_t *bp)
{
	if (bp == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return (B_FALSE);
	mutex_enter(&engine.lock);
	boolean_t foreign = engine.spa == spa && spa != NULL;
	for (int d = 0; foreign && d < BP_GET_NDVAS(bp); d++)
		foreign = DVA_GET_VDEV(&bp->blk_dva[d]) != engine.vd->vdev_id;
	mutex_exit(&engine.lock);
	return (foreign);
}

void
pr19262_engine_start(spa_t *spa)
{
	VERIFY(spa_writeable(spa));
	VERIFY(spa->spa_trust_config);
	txg_wait_synced(spa->spa_dsl_pool, 0);
	VERIFY3P(engine.spa, ==, NULL);
	/*
	 * Fresh fixture without background client writers.  The first RAIDZ
	 * top-level vdev is translated; log, special and further normal
	 * vdevs stay native.
	 */
	spa_config_enter(spa, SCL_ALLOC | SCL_FREE, FTAG, RW_WRITER);
	vdev_t *vd = engine_target_vdev(spa, 0);
	VERIFY3P(vd, !=, NULL);
	zfs_range_tree_t *free = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	zfs_range_tree_t *legacy = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	zfs_range_tree_add(legacy, 0, vd->vdev_asize);
	for (uint64_t i = 0; i < vd->vdev_ms_count; i++) {
		metaslab_t *ms = vd->vdev_ms[i];
		mutex_enter(&ms->ms_lock);
		VERIFY0(metaslab_load(ms));
		zfs_range_tree_walk(ms->ms_allocatable, zfs_range_tree_add,
		    free);
		zfs_range_tree_walk(ms->ms_allocatable, zfs_range_tree_remove,
		    legacy);
		mutex_exit(&ms->ms_lock);
	}
	/* Rows past the last metaslab belong to neither domain. */
	zfs_range_tree_t *unusable = zfs_range_tree_create(NULL,
	    ZFS_RANGE_SEG64, NULL, 0, 0);
	VERIFY3U(vd->vdev_ms_shift, !=, 0);
	uint64_t covered = P2ALIGN_TYPED(vd->vdev_asize,
	    1ULL << vd->vdev_ms_shift, uint64_t);
	if (covered < vd->vdev_asize) {
		zfs_range_tree_clear(free, covered, vd->vdev_asize - covered);
		zfs_range_tree_clear(legacy, covered, vd->vdev_asize - covered);
		zfs_range_tree_add(unusable, covered, vd->vdev_asize - covered);
	}
	mutex_enter(&engine.lock);
	engine.free = free;
	engine.legacy = legacy;
	engine.unusable = unusable;
	engine.vd = vd;
	engine.vd_guid = vd->vdev_guid;
	engine.foreign_normal = engine_foreign_normal(spa, vd);
	engine.total = vd->vdev_asize;
	engine.retired_limit = 64ULL << 20;
	engine.spa = spa;
	engine.admit_batch = ENGINE_ADMIT_BATCH;
	engine.admit_refreshed = UINT64_MAX;
	/* The strongest layout present is the default conversion target. */
	vdev_raidz_layout_for_birth(vd, spa_last_synced_txg(spa) + 1, NULL,
	    &engine.admit_parity);
	mutex_exit(&engine.lock);
	spa_config_exit(spa, SCL_ALLOC | SCL_FREE, FTAG);
}

typedef struct engine_pick {
	uint64_t start;
	uint64_t end;
	uint64_t size;
	uint64_t found;
} engine_pick_t;

static void
engine_pick_p(void *arg, uint64_t start, uint64_t size)
{
	engine_pick_t *p = arg;
	uint64_t low = MAX(start, p->start);
	uint64_t high = MIN(start + size, p->end);
	if (high >= low && high - low >= p->size)
		p->found = high - p->size;
}

typedef struct engine_alloc_pick {
	uint64_t low;
	uint64_t high;
	uint64_t min;
	uint64_t max;
	uint64_t mult;
	uint64_t align;
	uint64_t found;
	uint64_t size;
	uint64_t native_max;
} engine_alloc_pick_t;

static uint64_t
engine_alloc_size(engine_alloc_pick_t *pick, uint64_t low, uint64_t high)
{
	if (high <= low)
		return (0);
	return (rounddown(MIN(high - low, pick->max), pick->mult));
}

static void
engine_alloc_p(void *arg, uint64_t start, uint64_t size)
{
	engine_alloc_pick_t *pick = arg;
	uint64_t low = MAX(start, pick->low);
	uint64_t high = MIN(start + size, pick->high);
	uint64_t span = engine_alloc_size(pick, low, high);
	if (span >= pick->min && span > pick->size) {
		pick->size = span;
		pick->found = high - span;
	}
}

static boolean_t
engine_alloc_gap(engine_alloc_pick_t *pick, uint64_t low, uint64_t high)
{
	low = P2ROUNDUP(low, pick->align);
	uint64_t span = engine_alloc_size(pick, low, high);
	if (span < pick->min)
		return (B_FALSE);
	pick->found = low;
	pick->size = span;
	return (B_TRUE);
}

static void
engine_alloc_v(void *arg, uint64_t start, uint64_t size)
{
	engine_alloc_pick_t *pick = arg;
	if (pick->found != UINT64_MAX)
		return;
	uint64_t low = P2ROUNDUP(start, pick->align);
	uint64_t high = start + size;
	pick->native_max = MAX(pick->native_max,
	    engine_alloc_size(pick, low, high));
	engine_owner_t key = { .v = low };
	avl_index_t where;
	engine_owner_t *e = avl_find(&engine.owners, &key, &where);
	if (e == NULL) {
		e = avl_nearest(&engine.owners, where, AVL_BEFORE);
		if (e == NULL || low - e->v >= e->size)
			e = avl_nearest(&engine.owners, where, AVL_AFTER);
	}
	while (e != NULL && e->v < high) {
		if (e->v > low && engine_alloc_gap(pick, low, e->v))
			return;
		low = MAX(low, e->v + e->size);
		if (low >= high)
			return;
		e = AVL_NEXT(&engine.owners, e);
	}
	(void) engine_alloc_gap(pick, low, high);
}

/*
 * The owner AVL is also the V exclusion index, including aborted owners
 * awaiting their last hold or physical lease. The candidate and its P pair
 * are committed under the same engine lock, before native V is removed.
 */
pr19262_engine_alloc_result_t
pr19262_engine_alloc(spa_t *spa, vdev_t *vd, zfs_range_tree_t *native,
    uint64_t ms_start, uint64_t size, uint64_t max_size, uint64_t txg,
    uint64_t *offset, uint64_t *actual_size)
{
	pr19262_engine_alloc_result_t result = PR19262_ALLOC_INACTIVE;
	engine_owner_t *e = NULL;
	engine_version_t *b = NULL;
	/* Budget lookup takes the transaction lock: before the engine lock. */
	boolean_t client = pr19262_tx_client_txg(spa, txg);
	mutex_enter(&engine.lock);
	if (engine.spa != spa || spa == NULL || vd != engine.vd)
		goto out;
	/*
	 * With another normal-class vdev to fall back to, client data never
	 * eats into the protected reserve of the translated vdev: the
	 * allocator moves on to the native vdev (class fallback).
	 */
	if (engine.foreign_normal && client) {
		uint64_t free = zfs_range_tree_space(engine.free);
		uint64_t protected =
		    engine_admit_protected(PR19262_ADMIT_CLIENT);
		if (free < protected || free - protected < size) {
			engine.alloc_guarded++;
			result = PR19262_ALLOC_P_BLOCKED;
			goto out;
		}
	}
	engine_alloc_pick_t p = {
		.low = ms_start,
		.high = ms_start + (1ULL << vd->vdev_ms_shift),
		.min = size,
		.max = max_size,
		.mult = vdev_alloc_factor(vd, txg),
		.align = 1ULL << vd->vdev_ashift,
		.found = UINT64_MAX
	};
	zfs_range_tree_walk(engine.free, engine_alloc_p, &p);
	if (p.found == UINT64_MAX) {
		result = PR19262_ALLOC_P_BLOCKED;
		goto out;
	}
	engine_alloc_pick_t v = p;
	v.max = p.size;
	v.size = 0;
	v.found = UINT64_MAX;
	zfs_range_tree_walk(native, engine_alloc_v, &v);
	if (v.found == UINT64_MAX) {
		result = v.native_max < size ? PR19262_ALLOC_NATIVE_FULL :
		    PR19262_ALLOC_V_BLOCKED;
		goto out;
	}
	if (engine.alloc_fail_after != 0 && --engine.alloc_fail_after == 0) {
		pr19262_printf("PR19262_ALLOC_NOMEM v=%llu size=%llu p=%llu "
		    "eligible=1\n", (u_longlong_t)v.found,
		    (u_longlong_t)v.size,
		    (u_longlong_t)(p.found + p.size - v.size));
		result = PR19262_ALLOC_NOMEM;
		goto out;
	}
	/* Reserve ticket memory without sleeping or issuing I/O. */
	e = kmem_zalloc(sizeof (*e), KM_NOSLEEP);
	b = kmem_zalloc(sizeof (*b), KM_NOSLEEP);
	if (e == NULL || b == NULL) {
		result = PR19262_ALLOC_NOMEM;
		goto out;
	}
	VERIFY3P(engine_overlap(v.found, v.size), ==, NULL);
	VERIFY(!zfs_range_tree_contains(engine.legacy, v.found, v.size));
	e->v = v.found;
	e->size = v.size;
	e->allocation_txg = e->catalog_txg = txg;
	e->incarnation = ++engine.next_incarnation;
	e->created = ++engine.sequence;
	b->owner = e;
	b->p = p.found + p.size - v.size;
	b->size = v.size;
	b->birth = txg;
	b->generation = ++engine.sequence;
	vdev_raidz_layout_for_birth(vd, txg, &b->width, &b->parity);
	e->cur = e->versions = b;
	zfs_range_tree_remove(engine.free, b->p, b->size);
	avl_add(&engine.owners, e);
	engine.pairs++;
	engine.revision++;
	*offset = v.found;
	*actual_size = v.size;
	e = NULL;
	b = NULL;
	result = PR19262_ALLOC_OK;
out:
	mutex_exit(&engine.lock);
	if (e != NULL)
		kmem_free(e, sizeof (*e));
	if (b != NULL)
		kmem_free(b, sizeof (*b));
	return (result);
}

int
pr19262_engine_pair(spa_t *spa, vdev_t *vd, uint64_t v, uint64_t size,
    uint64_t txg)
{
	int error = 0;
	mutex_enter(&engine.lock);
	if (engine.spa != spa || spa == NULL)
		goto out;
	VERIFY3P(vd, ==, engine.vd);
	VERIFY(!zfs_range_tree_contains(engine.legacy, v, size));
	engine_owner_t *collision = engine_overlap(v, size);
	if (collision != NULL) {
		pr19262_printf("PR19262_V_PAIR_COLLISION v=%llu size=%llu "
		    "txg=%llu owner=%llu owner_size=%llu pending=%u "
		    "retired=%u aborted=%u\n", (u_longlong_t)v,
		    (u_longlong_t)size, (u_longlong_t)txg,
		    (u_longlong_t)collision->v,
		    (u_longlong_t)collision->size, collision->journal_pending,
		    collision->retired, collision->aborted);
		error = ENOSPC;
		goto out;
	}
	/* Preserve native ditto separation: P stays in the chosen V MS. */
	uint64_t ms_start = P2ALIGN_TYPED(v, 1ULL << vd->vdev_ms_shift,
	    uint64_t);
	engine_pick_t pick = { ms_start, ms_start +
		(1ULL << vd->vdev_ms_shift), size, UINT64_MAX };
	zfs_range_tree_walk(engine.free, engine_pick_p, &pick);
	if (pick.found == UINT64_MAX) {
		error = ENOSPC;
		goto out;
	}
	engine_owner_t *e = kmem_zalloc(sizeof (*e), KM_SLEEP);
	e->v = v;
	e->size = size;
	e->allocation_txg = txg;
	e->catalog_txg = txg;
	e->cur = engine_version_new(e, pick.found, size, txg, B_FALSE);
	e->incarnation = ++engine.next_incarnation;
	e->created = ++engine.sequence;
	zfs_range_tree_remove(engine.free, e->cur->p, size);
	avl_add(&engine.owners, e);
	engine.pairs++;
out:
	mutex_exit(&engine.lock);
	return (error);
}

static void
engine_abort_done(engine_owner_t *e)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	VERIFY(e->aborted);
	if (e->holds != 0)
		return;
	VERIFY(!e->copying && e->pending == NULL);
	for (engine_version_t *b = e->versions; b != NULL; b = b->next)
		if (b->leases != 0)
			return;
	while (e->versions != NULL) {
		engine_version_t *b = e->versions;
		e->versions = b->next;
		zfs_range_tree_verify_not_present(engine.free, b->p, b->size);
		zfs_range_tree_add(engine.free, b->p, b->size);
		kmem_free(b, sizeof (*b));
	}
	avl_remove(&engine.owners, e);
	kmem_free(e, sizeof (*e));
}

/* Diagnostic: a journal-touched allocation retained as retired at abort. */
static void
engine_abort_report(spa_t *spa, engine_owner_t *e, uint64_t v,
    uint64_t size, uint64_t txg)
{
	pr19262_printf("PR19262_ABORT_RETAINED v=%llu size=%llu txg=%llu "
	    "holds=%llu copying=%u syncing_txg=%llu last_synced=%llu\n",
	    (u_longlong_t)v, (u_longlong_t)size, (u_longlong_t)txg,
	    (u_longlong_t)e->holds, e->copying,
	    (u_longlong_t)spa_syncing_txg(spa),
	    (u_longlong_t)spa_last_synced_txg(spa));
	pr19262_flush();
	pr19262_backtrace();
}

boolean_t
pr19262_engine_abort(spa_t *spa, vdev_t *vd, uint64_t v, uint64_t size,
    uint64_t txg)
{
	boolean_t retained = B_FALSE;
	boolean_t classified = B_FALSE;
	mutex_enter(&engine.lock);
	if (engine.spa == spa && spa != NULL && vd == engine.vd) {
		engine_owner_t *e = engine_find(v);
		if (e != NULL) {
			VERIFY3U(e->size, ==, size);
			VERIFY3U(e->allocation_txg, ==, txg);
			VERIFY(!e->aborted);
			classified = B_TRUE;
			/* Admission and abort choose exactly one outcome. */
			if (engine.persistent && e->journal_touched) {
				if (!e->retired) {
					e->retired = B_TRUE;
					e->retired_at = ++engine.sequence;
					engine.retired += e->cur->size;
					engine.revision++;
				}
				retained = B_TRUE;
				if (engine.abort_prints++ < 4)
					engine_abort_report(spa, e, v, size,
					    txg);
				goto out;
			}
			VERIFY(!e->retired);
			e->aborted = B_TRUE;
			engine.revision++;
			engine.aborts++;
			engine_abort_done(e);
		}
	}
out:
	mutex_exit(&engine.lock);
	if (classified) {
		/* The observer records only; caller still holds ms_lock. */
		blkptr_t bp = { 0 };
		DVA_SET_VDEV(&bp.blk_dva[0], vd->vdev_id);
		DVA_SET_OFFSET(&bp.blk_dva[0], v);
		DVA_SET_ASIZE(&bp.blk_dva[0], size);
		BP_SET_BIRTH(&bp, txg, txg);
		engine_journal_test_event(spa, &bp, retained ? 4 : 3);
	}
	return (retained);
}

boolean_t
pr19262_engine_retire(vdev_t *vd, uint64_t v, uint64_t size,
    boolean_t checkpoint)
{
	boolean_t handled = B_FALSE;
	mutex_enter(&engine.lock);
	if (engine.spa != vd->vdev_spa || engine.spa == NULL ||
	    vd != engine.vd)
		goto out;
	/* A fenced owner release performs the native free itself. */
	if (engine.release_thread == curthread)
		goto out;
	engine_owner_t *e = engine_find(v);
	if (e == NULL) {
		VERIFY(zfs_range_tree_contains(engine.legacy, v, size));
		zfs_range_tree_remove(engine.legacy, v, size);
		e = kmem_zalloc(sizeof (*e), KM_SLEEP);
		e->v = v;
		e->size = size;
		e->cur = engine_version_new(e, v, size, 0, B_TRUE);
		e->incarnation = ++engine.next_incarnation;
		avl_add(&engine.owners, e);
	}
	VERIFY3U(e->size, ==, size);
	VERIFY(!e->retired && !e->aborted);
	if (checkpoint) {
		e->checkpoint_only = B_TRUE;
		engine.revision++;
		/* Keep native checkpoint space-map accounting. */
		goto out;
	}
	e->retired = B_TRUE;
	engine.revision++;
	e->retired_at = ++engine.sequence;
	engine.retired += e->cur->size;
	if (engine.retire_prints++ < 24) {
		spa_t *spa = vd->vdev_spa;
		pr19262_printf("PR19262_RETIRE v=%llu size=%llu birth=%llu "
		    "allocation_txg=%llu sync_context=%u syncing_txg=%llu "
		    "last_synced=%llu retired_txg=%llu\n", (u_longlong_t)v,
		    (u_longlong_t)size, (u_longlong_t)e->cur->birth,
		    (u_longlong_t)e->allocation_txg,
		    dsl_pool_sync_context(spa->spa_dsl_pool),
		    (u_longlong_t)spa_syncing_txg(spa),
		    (u_longlong_t)spa_last_synced_txg(spa),
		    (u_longlong_t)e->retired_txg);
	}
	/* No fake floor: neither authoritative V nor P becomes free. */
	handled = B_TRUE;
out:
	mutex_exit(&engine.lock);
	return (handled);
}

void
pr19262_engine_checkpoint(spa_t *spa, const uberblock_t *checkpoint)
{
	mutex_enter(&engine.lock);
	if (engine.spa == spa && engine.persistent) {
		if (checkpoint != NULL)
			engine.checkpoint = *checkpoint;
		else
			memset(&engine.checkpoint, 0,
			    sizeof (engine.checkpoint));
		engine.revision++;
	}
	mutex_exit(&engine.lock);
}

static engine_owner_t *
engine_checkpoint_owner(uint64_t v)
{
	engine_owner_t key = { .v = v };
	avl_index_t where;
	engine_owner_t *e = avl_find(&engine.owners, &key, &where);
	if (e == NULL)
		e = avl_nearest(&engine.owners, where, AVL_BEFORE);
	if (e == NULL || v < e->v || v - e->v >= e->size)
		return (NULL);
	return (e);
}

int
pr19262_engine_checkpoint_validate(spa_t *spa, const uberblock_t *checkpoint)
{
	uberblock_t absent = { 0 };
	const uberblock_t *expected = checkpoint != NULL ? checkpoint : &absent;
	mutex_enter(&engine.lock);
	int error = 0;
	if (engine.spa == spa && engine.persistent &&
	    memcmp(&engine.checkpoint, expected,
	    sizeof (engine.checkpoint)) != 0)
		error = ENGINE_EINVAL(4);
	mutex_exit(&engine.lock);
	return (error);
}

int
pr19262_engine_checkpoint_discard(vdev_t *vd, uint64_t v, uint64_t size,
    boolean_t *handled)
{
	*handled = B_FALSE;
	mutex_enter(&engine.lock);
	if (engine.spa == NULL || engine.spa != vd->vdev_spa)
		goto out;
	if (vd != engine.vd)
		goto out;
	if (v >= engine.total || size == 0 || size > engine.total - v) {
		mutex_exit(&engine.lock);
		return (ENGINE_EINVAL(1));
	}
	/* SM runs can merge allocations or split one allocation into runs. */
	for (uint64_t cursor = v; cursor < v + size; ) {
		engine_owner_t *e = engine_checkpoint_owner(cursor);
		if (e == NULL || (!e->checkpoint_only && !e->retired)) {
			mutex_exit(&engine.lock);
			return (ENGINE_EINVAL(2));
		}
		cursor = MIN(v + size, e->v + e->size);
	}
	for (uint64_t cursor = v; cursor < v + size; ) {
		engine_owner_t *e = engine_checkpoint_owner(cursor);
		if (!e->retired) {
			e->checkpoint_only = B_FALSE;
			e->retired = B_TRUE;
			e->retired_at = ++engine.sequence;
			engine.retired += e->cur->size;
			engine.revision++;
		}
		cursor = MIN(v + size, e->v + e->size);
	}
	*handled = B_TRUE;
out:
	mutex_exit(&engine.lock);
	return (0);
}

void *
pr19262_engine_hold(spa_t *spa, const blkptr_t *bp, void *inherit,
    boolean_t read, boolean_t gang_child)
{
	if (bp != NULL && (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp)))
		return (NULL);
	mutex_enter(&engine.lock);
	if (engine.spa != spa || spa == NULL) {
		mutex_exit(&engine.lock);
		return (NULL);
	}
	engine_hold_t *prior = inherit;
	if (engine.block_new && prior == NULL) {
		mutex_exit(&engine.lock);
		return (NULL);
	}
	if (bp == NULL && prior == NULL) {
		mutex_exit(&engine.lock);
		return (NULL);
	}
	engine_hold_t *h = kmem_zalloc(sizeof (*h), KM_SLEEP);
	h->refs = 1;
	h->write = !read;
	if (prior != NULL) {
		h->error = prior->error;
		h->error_site = prior->error != 0 ? 1 : 0;
	}
	if (prior != NULL && prior->error == ESTALE &&
	    engine.inherit_prints++ < 4) {
		pr19262_printf("PR19262_HOLD_INHERIT_STALE bp=%d write=%u "
		    "prior_count=%u prior_write=%u prior_gang=%u "
		    "prior_started=%llu\n", bp != NULL, !read, prior->count,
		    prior->write, prior->gang, (u_longlong_t)prior->started);
		pr19262_flush();
		pr19262_backtrace();
	}
	h->started = prior != NULL ? prior->started : ++engine.sequence;
	h->gang = (prior != NULL && prior->gang) ||
	    (bp != NULL && BP_IS_GANG(bp));
	if (bp == NULL) {
		*h = *prior;
		for (uint_t i = 0; i < h->count; i++) {
			h->owner[i]->holds++;
			h->owner[i]->queued_writers += h->write;
		}
		mutex_exit(&engine.lock);
		return (h);
	}
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		if (DVA_GET_VDEV(dva) != engine.vd->vdev_id) {
			/* Another top-level vdev: native, never translated. */
			h->foreign++;
			continue;
		}
		engine_owner_t *e = engine_find(DVA_GET_OFFSET(dva));
		boolean_t inherited = B_FALSE;
		if (prior != NULL && prior->error == 0)
			for (uint_t i = 0; i < prior->count; i++)
				inherited |= prior->owner[i] == e && e != NULL;
		if (e == NULL) {
			uint64_t size = DVA_GET_GANG(dva) ?
			    vdev_gang_header_asize(engine.vd,
			    BP_GET_PHYSICAL_BIRTH(bp)) : DVA_GET_ASIZE(dva);
			if (!zfs_range_tree_contains(engine.legacy,
			    DVA_GET_OFFSET(dva), size)) {
				h->error = ENOENT;
				h->error_site = 2;
				continue;
			}
			e = kmem_zalloc(sizeof (*e), KM_SLEEP);
			e->v = DVA_GET_OFFSET(dva);
			e->size = size;
			e->cur = engine_version_new(e, e->v, size,
			    BP_GET_PHYSICAL_BIRTH(bp), B_TRUE);
			e->incarnation = ++engine.next_incarnation;
			zfs_range_tree_remove(engine.legacy, e->v, size);
			avl_add(&engine.owners, e);
		}
		boolean_t descendant = prior != NULL && prior->error == 0 &&
		    prior->gang && gang_child && read;
		/* A released and reused V must not serve an older BP. */
		boolean_t reused = e->allocation_txg != 0 && !inherited &&
		    !BP_GET_DEDUP(bp) &&
		    BP_GET_PHYSICAL_BIRTH(bp) != e->allocation_txg;
		boolean_t gone = (e->retired && !inherited &&
		    !(descendant && h->started < e->retired_at)) ||
		    (e->aborted && !inherited) ||
		    (descendant && e->created > h->started);
		if (reused || gone) {
			h->error = ESTALE;
			h->error_site = 3;
			if (engine.stale_verdicts++ < 16)
				pr19262_printf("PR19262_HOLD_STALE v=%llu "
				    "reused=%u gone=%u retired=%u aborted=%u "
				    "birth=%llu allocation_txg=%llu write=%u "
				    "level=%u type=%u retired_txg=%llu "
				    "last_synced=%llu\n",
				    (u_longlong_t)e->v, reused, gone,
				    e->retired, e->aborted,
				    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(bp),
				    (u_longlong_t)e->allocation_txg, h->write,
				    (uint_t)BP_GET_LEVEL(bp),
				    (uint_t)BP_GET_TYPE(bp),
				    (u_longlong_t)e->retired_txg,
				    (u_longlong_t)spa_last_synced_txg(spa));
			if (engine.stale_verdicts <= 3) {
				pr19262_flush();
				pr19262_backtrace();
			}
		}
		e->holds++;
		e->queued_writers += h->write;
		h->owner[h->count++] = e;
	}
	if (h->count == 0 && h->foreign != 0 && h->error == 0) {
		/* Every DVA lives on a native vdev: no engine identity. */
		mutex_exit(&engine.lock);
		kmem_free(h, sizeof (*h));
		return (NULL);
	}
	mutex_exit(&engine.lock);
	return (h);
}

void
pr19262_engine_rele(void *arg)
{
	engine_hold_t *h = arg;
	if (h == NULL)
		return;
	mutex_enter(&engine.lock);
	for (uint_t i = 0; i < h->count; i++) {
		engine_owner_t *e = h->owner[i];
		VERIFY3U(e->holds, >, 0);
		e->holds--;
		VERIFY3U(e->queued_writers, >=, h->write);
		e->queued_writers -= h->write;
		if (e->aborted)
			engine_abort_done(e);
	}
	mutex_exit(&engine.lock);
	kmem_free(h, sizeof (*h));
}

void
pr19262_engine_io_rele(zio_t *zio)
{
	engine_version_t *b = zio->io_pr19262_owner_version;
	if (b == NULL)
		return;
	mutex_enter(&engine.lock);
	VERIFY3U(b->leases, >, 0);
	b->leases--;
	if (zio->io_type == ZIO_TYPE_WRITE) {
		VERIFY3U(b->writers, >, 0);
		b->writers--;
	}
	if (zio->io_error == 0)
		b->ready = B_TRUE;
	engine_owner_t *e = b->owner;
	if (e->aborted)
		engine_abort_done(e);
	zio->io_pr19262_owner_version = NULL;
	mutex_exit(&engine.lock);
}

/* Diagnostic: the hold behind a stale dispatch verdict. */
static void
engine_hold_report(const engine_hold_t *h)
{
	pr19262_printf("PR19262_HOLD_REPORT error=%d site=%u count=%u "
	    "write=%u gang=%u started=%llu foreign=%u\n", h->error,
	    h->error_site, h->count, h->write, h->gang,
	    (u_longlong_t)h->started, h->foreign);
	for (uint_t i = 0; i < h->count; i++) {
		const engine_owner_t *e = h->owner[i];
		pr19262_printf("PR19262_HOLD_OWNER i=%u v=%llu size=%llu "
		    "incarnation=%llu allocation_txg=%llu retired=%u "
		    "aborted=%u copying=%u holds=%llu retired_txg=%llu\n", i,
		    (u_longlong_t)e->v, (u_longlong_t)e->size,
		    (u_longlong_t)e->incarnation,
		    (u_longlong_t)e->allocation_txg, e->retired, e->aborted,
		    e->copying, (u_longlong_t)e->holds,
		    (u_longlong_t)e->retired_txg);
	}
	pr19262_flush();
}

/* Diagnostic: report a zio's hold from the pipeline, ungated. */
void
pr19262_engine_hold_report(void *hold)
{
	mutex_enter(&engine.lock);
	if (hold == NULL)
		pr19262_printf("PR19262_HOLD_REPORT null hold\n");
	else
		engine_hold_report(hold);
	pr19262_printf("PR19262_HOLD_REPORT_COUNTER value=%llu\n",
	    (u_longlong_t)engine.hold_report_prints);
	mutex_exit(&engine.lock);
}

/* A refused physical write names itself: nothing else reports it. */
static uint_t engine_dispatch_unsafe_prints;

int
pr19262_engine_dispatch(zio_t *zio)
{
	mutex_enter(&engine.lock);
	if (engine.spa != zio->io_spa || engine.spa == NULL) {
		mutex_exit(&engine.lock);
		return (0);
	}
	if (zio->io_type == ZIO_TYPE_TRIM) {
		mutex_exit(&engine.lock);
		return (ENOTSUP);
	}
	if (zio->io_type != ZIO_TYPE_READ &&
	    zio->io_type != ZIO_TYPE_WRITE) {
		mutex_exit(&engine.lock);
		return (0);
	}
	if (zio->io_logical != NULL &&
	    zio->io_logical->io_pr19262_physical) {
		mutex_exit(&engine.lock);
		/* Isolated-fixture P constructors lack engine authority. */
		return (ENOTSUP);
	}
	if (zio->io_flags & ZIO_FLAG_PHYSICAL) {
		uint64_t low = VDEV_LABEL_START_SIZE;
		boolean_t metadata = engine.persistent &&
		    zio->io_pr19262_metadata_authority == &engine &&
		    zio->io_vd->vdev_parent == engine.vd &&
		    zio->io_offset >= low &&
		    zio->io_offset - low <=
		    engine.total / engine.vd->vdev_children &&
		    zio->io_size <= engine.total / engine.vd->vdev_children -
		    (zio->io_offset - low) &&
		    ((engine.store_extents != NULL &&
		    zfs_range_tree_contains(engine.store_extents,
		    (zio->io_offset - low) * engine.vd->vdev_children,
		    zio->io_size * engine.vd->vdev_children)) ||
		    (engine.journal_extents != NULL &&
		    zfs_range_tree_contains(engine.journal_extents,
		    (zio->io_offset - low) * engine.vd->vdev_children,
		    zio->io_size * engine.vd->vdev_children)));
		/* Labels/probes are outside the data area. */
		boolean_t unsafe = zio->io_type == ZIO_TYPE_WRITE &&
		    zio->io_vd->vdev_ops->vdev_op_leaf &&
		    zio->io_vd->vdev_top == engine.vd &&
		    zio->io_offset >= VDEV_LABEL_START_SIZE &&
		    zio->io_offset < zio->io_vd->vdev_psize -
		    VDEV_LABEL_END_SIZE &&
		    zio->io_pr19262_owner_hold == NULL && !metadata;
		if (unsafe && engine_dispatch_unsafe_prints++ < 8)
			pr19262_printf("PR19262_DISPATCH_UNSAFE child=%llu "
			    "offset=%llu size=%llu flags=%llx\n",
			    (u_longlong_t)zio->io_vd->vdev_id,
			    (u_longlong_t)zio->io_offset,
			    (u_longlong_t)zio->io_size,
			    (u_longlong_t)zio->io_flags);
		mutex_exit(&engine.lock);
		return (unsafe ? ENOTSUP : 0);
	}
	if (zio->io_vd != zio->io_vd->vdev_top || zio->io_vd != engine.vd) {
		/* Leaf I/O below the target, or a native top-level vdev. */
		mutex_exit(&engine.lock);
		return (0);
	}
	engine_hold_t *h = zio->io_pr19262_owner_hold;
	if ((!zio->io_pr19262_owner_copy &&
	    (h == NULL || h->error != 0)) || zio->io_bp == NULL) {
		int error = h != NULL && h->error != 0 ? h->error : ENOENT;
		if (error == ESTALE && h != NULL &&
		    engine.hold_report_prints++ < 4)
			engine_hold_report(h);
		mutex_exit(&engine.lock);
		return (error);
	}
	uint64_t v = zio->io_pr19262_owner_mapped ?
	    zio->io_pr19262_owner_v : zio->io_offset;
	engine_owner_t *e = NULL;
	engine_version_t *b = zio->io_pr19262_owner_version;
	if (zio->io_pr19262_owner_copy) {
		VERIFY3P(b, !=, NULL);
		e = b->owner;
		VERIFY(e->copying);
		VERIFY3U(e->v, ==, v);
	} else {
		for (uint_t i = 0; i < h->count; i++)
			if (h->owner[i]->v == v)
				e = h->owner[i];
	}
	if (e == NULL) {
		if (engine_find(v) != NULL ||
		    !zfs_range_tree_contains(engine.legacy, v,
		    vdev_psize_to_asize_txg(engine.vd, zio->io_size,
		    BP_GET_PHYSICAL_BIRTH(zio->io_bp)))) {
			mutex_exit(&engine.lock);
			return (ENOENT);
		}
		engine.legacy_reads++;
		mutex_exit(&engine.lock);
		return (0);
	}
	if (b == NULL && e->copying && zio->io_type == ZIO_TYPE_WRITE) {
		engine_defer(zio, &e->pending);
		engine.deferred++;
		mutex_exit(&engine.lock);
		return (EINPROGRESS);
	}
	boolean_t selected = !zio->io_pr19262_owner_mapped;
	if (selected) {
		if (b == NULL)
			b = e->cur;
		b->leases++;
		if (zio->io_type == ZIO_TYPE_WRITE)
			b->writers++;
		zio->io_pr19262_owner_version = b;
	}
	VERIFY3P(b->owner, ==, e);
	uint64_t sectors = howmany(zio->io_size,
	    1ULL << engine.vd->vdev_ashift);
	uint64_t required = roundup(sectors + b->parity *
	    howmany(sectors, b->width - b->parity), b->parity + 1) <<
	    engine.vd->vdev_ashift;
	VERIFY3U(required, <=, b->size);
	/* Keep the original V-BP for checksum/crypto/gang verification. */
	blkptr_t *physical = &zio->io_pr19262_owner_bp;
	*physical = *zio->io_bp;
	boolean_t matched = B_FALSE;
	for (int d = 0; d < BP_GET_NDVAS(physical); d++) {
		dva_t *dva = &physical->blk_dva[d];
		if (DVA_GET_VDEV(dva) == engine.vd->vdev_id &&
		    DVA_GET_OFFSET(dva) == v) {
			DVA_SET_OFFSET(dva, b->p);
			DVA_SET_ASIZE(dva, b->size);
			matched = B_TRUE;
		}
	}
	VERIFY(matched);
	BP_SET_PHYSICAL_BIRTH(physical, b->birth);
	if (engine.fail_writes && zio->io_type == ZIO_TYPE_WRITE &&
	    BP_GET_LEVEL(zio->io_bp) == 0 &&
	    !DMU_OT_IS_METADATA(BP_GET_TYPE(zio->io_bp))) {
		mutex_exit(&engine.lock);
		return (EIO);
	}
	zio->io_pr19262_owner_mapped = B_TRUE;
	zio->io_pr19262_owner_v = v;
	zio->io_offset = b->p;
	zio->io_pr19262_backing_birth = b->birth;
	zio->io_pr19262_backing_width = b->width;
	zio->io_pr19262_backing_parity = b->parity;
	/* Operation TXG is never replaced by an encoding/backing birth. */
	engine.translated++;
	engine.legacy_reads += e->v == b->p;
	if (BP_GET_LEVEL(zio->io_bp) != 0 ||
	    DMU_OT_IS_METADATA(BP_GET_TYPE(zio->io_bp)))
		engine.metadata++;
	if (selected && engine.park_next &&
	    zio->io_type == ZIO_TYPE_READ && !zio->io_pr19262_owner_copy) {
		engine.park_next = B_FALSE;
		engine_defer(zio, &engine.parked);
		mutex_exit(&engine.lock);
		return (EINPROGRESS);
	}
	mutex_exit(&engine.lock);
	return (0);
}

static int
engine_copy_begin(spa_t *spa, const blkptr_t *bp, int d, uint64_t birth,
    void **handle, boolean_t unwritten)
{
	*handle = NULL;
	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) || d < 0 ||
	    d >= BP_GET_NDVAS(bp))
		return (ENGINE_EINVAL(3));
	const dva_t *dva = &bp->blk_dva[d];
	uint64_t psize = BP_GET_PSIZE(bp);
	mutex_enter(&engine.lock);
	if (engine.spa != spa || spa == NULL ||
	    DVA_GET_VDEV(dva) != engine.vd->vdev_id) {
		mutex_exit(&engine.lock);
		return (ENOTSUP);
	}
	if (engine.persistent && engine.journal_error != 0) {
		int error = engine.journal_error;
		mutex_exit(&engine.lock);
		return (error);
	}
	if (DVA_GET_GANG(dva))
		psize = vdev_gang_header_psize(engine.vd);
	engine_owner_t *e = engine_find(DVA_GET_OFFSET(dva));
	if (e == NULL || e->retired || e->aborted) {
		mutex_exit(&engine.lock);
		return (ESTALE);
	}
	/*
	 * A block allocated ahead of its write (a log chain's tail) has
	 * nothing to copy; it may be rebound while nothing writes it, since
	 * writes dispatched from now on defer behind the copying flag.  A
	 * legacy extent classified by a read is marked ready because its
	 * content is unknown; when the caller has read it and found no
	 * valid content (the unwritten tail a log header points at, born
	 * before the engine), the rebind is allowed as well.
	 */
	boolean_t legacy = e->allocation_txg == 0 && e->cur->p == e->v &&
	    e->cur->journal_seq == 0;
	if (e->copying || (e->cur->ready == unwritten &&
	    !(unwritten && legacy)) ||
	    e->cur->writers != 0 || e->queued_writers != 0 ||
	    e->catalog_txg > spa_last_synced_txg(spa)) {
		mutex_exit(&engine.lock);
		return (EBUSY);
	}
	uint64_t size = vdev_psize_to_asize_txg(engine.vd, psize, birth);
	uint64_t ms = P2ALIGN_TYPED(e->v, 1ULL << engine.vd->vdev_ms_shift,
	    uint64_t);
	engine_pick_t pick = { ms,
	    ms + (1ULL << engine.vd->vdev_ms_shift), size, UINT64_MAX };
	zfs_range_tree_walk(engine.free, engine_pick_p, &pick);
	if (pick.found == UINT64_MAX) {
		/*
		 * The V's own metaslab is full: a relocation may take backing
		 * anywhere on the vdev rather than fail, so a metaslab packed
		 * with weak data does not need its own headroom.
		 */
		pick.start = 0;
		pick.end = engine.total;
		zfs_range_tree_walk(engine.free, engine_pick_p, &pick);
	}
	if (pick.found == UINT64_MAX) {
		mutex_exit(&engine.lock);
		return (ENOSPC);
	}
	engine_copy_t *c = kmem_zalloc(sizeof (*c), KM_SLEEP);
	c->owner = e;
	c->source = e->cur;
	c->original = *bp;
	c->psize = psize;
	c->stage = engine_version_new(e, pick.found, size, birth, B_FALSE);
	zfs_range_tree_remove(engine.free, pick.found, size);
	e->copying = B_TRUE;
	e->holds++;
	c->source->leases++;
	*handle = c;
	mutex_exit(&engine.lock);
	return (0);
}

boolean_t
pr19262_engine_free_range(spa_t *spa, uint64_t p, uint64_t size)
{
	mutex_enter(&engine.lock);
	VERIFY3P(spa, ==, engine.spa);
	boolean_t free = zfs_range_tree_contains(engine.free, p, size);
	mutex_exit(&engine.lock);
	return (free);
}

/* Diagnostic: the owner state behind a block pointer, at a named site. */
void
pr19262_engine_describe(spa_t *spa, const blkptr_t *bp, const char *site,
    uint64_t a, uint64_t b)
{
	const dva_t *dva = &bp->blk_dva[0];
	mutex_enter(&engine.lock);
	engine_owner_t *e = engine.spa == spa && engine.vd != NULL &&
	    DVA_GET_VDEV(dva) == engine.vd->vdev_id ?
	    engine_find(DVA_GET_OFFSET(dva)) : NULL;
	uint64_t versions = 0;
	for (engine_version_t *v = e != NULL ? e->versions : NULL; v != NULL;
	    v = v->next)
		versions++;
	pr19262_printf("PR19262_DESCRIBE site=%s a=%llu b=%llu vdev=%llu "
	    "v=%llu asize=%llu pbirth=%llu lbirth=%llu type=%u level=%u "
	    "dedup=%u owner=%d size=%llu incarnation=%llu allocation_txg=%llu "
	    "retired=%u aborted=%u copying=%u holds=%llu versions=%llu "
	    "cur_birth=%llu cur_p=%llu\n", site, (u_longlong_t)a,
	    (u_longlong_t)b, (u_longlong_t)DVA_GET_VDEV(dva),
	    (u_longlong_t)DVA_GET_OFFSET(dva), (u_longlong_t)DVA_GET_ASIZE(dva),
	    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(bp),
	    (u_longlong_t)BP_GET_LOGICAL_BIRTH(bp), (uint_t)BP_GET_TYPE(bp),
	    (uint_t)BP_GET_LEVEL(bp), (uint_t)BP_GET_DEDUP(bp), e != NULL,
	    (u_longlong_t)(e != NULL ? e->size : 0),
	    (u_longlong_t)(e != NULL ? e->incarnation : 0),
	    (u_longlong_t)(e != NULL ? e->allocation_txg : 0),
	    e != NULL ? e->retired : 0, e != NULL ? e->aborted : 0,
	    e != NULL ? e->copying : 0,
	    (u_longlong_t)(e != NULL ? e->holds : 0), (u_longlong_t)versions,
	    (u_longlong_t)(e != NULL && e->cur != NULL ? e->cur->birth : 0),
	    (u_longlong_t)(e != NULL && e->cur != NULL ? e->cur->p : 0));
	mutex_exit(&engine.lock);
	/* The caller may assert next: do not lose this line in a buffer. */
	pr19262_flush();
}

void
pr19262_engine_inspect(spa_t *spa, const blkptr_t *bp, int d,
    pr19262_engine_info_t *info)
{
	mutex_enter(&engine.lock);
	VERIFY3P(spa, ==, engine.spa);
	engine_owner_t *e = engine_find(DVA_GET_OFFSET(&bp->blk_dva[d]));
	VERIFY3P(e, !=, NULL);
	*info = (pr19262_engine_info_t) {
	    .p = e->cur->p, .vsize = e->size,
	    .psize = e->cur->size, .birth = e->cur->birth,
	    .leases = e->cur->leases, .incarnation = e->incarnation,
	    .holds = e->holds, .journal_seq = e->cur->journal_seq,
	    .aborted = e->aborted, .retired = e->retired,
	    .journal_touched = e->journal_touched,
	    .queued_writers = e->queued_writers, .copying = e->copying,
	    .ready = e->cur->ready, .catalog_txg = e->catalog_txg };
	for (engine_version_t *b = e->versions; b != NULL; b = b->next) {
		info->versions++;
		if (b != e->cur)
			info->old_leases += b->leases;
	}
	mutex_exit(&engine.lock);
}

static int
engine_copy_io(engine_copy_t *c, engine_version_t *b, abd_t *abd,
    zio_type_t type)
{
	spa_t *spa = engine.spa;
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	zio_t *zio = pr19262_engine_physical_io(spa, &c->original,
	    engine.vd, c->owner->v, abd, c->psize, type,
	    spa_last_synced_txg(spa) + 1, b, 0);
	int error = zio_wait(zio);
	spa_config_exit(spa, SCL_ZIO, FTAG);
	return (error);
}

/* A copy freed meanwhile is stale; otherwise its stage takes ready. */
static boolean_t
engine_copy_settle(engine_copy_t *c, int *error, boolean_t ready)
{
	mutex_enter(&engine.lock);
	engine_owner_t *e = c->owner;
	VERIFY(e->copying);
	VERIFY3P(e->cur, ==, c->source);
	VERIFY0(c->stage->leases);
	if (*error == 0 && e->retired)
		*error = ESTALE;
	boolean_t journal_attempted = *error == 0 && engine.persistent;
	if (*error == 0)
		c->stage->ready = ready;
	mutex_exit(&engine.lock);
	return (journal_attempted);
}

/* Publish the bound stage, or keep or return it, and end the copy. */
static int
engine_copy_publish(engine_copy_t *c, int error, boolean_t journal_attempted)
{
	mutex_enter(&engine.lock);
	engine_owner_t *e = c->owner;
	if (error == 0 && e->retired)
		error = ESTALE;
	if (error == 0) {
		e->cur = c->stage;
		c->source->retired = B_TRUE;
		engine.retired += c->source->size;
		engine.copied++;
		engine.revision++;
		/* The next catalogs list the published version. */
		if (journal_attempted)
			engine_journal_fold(engine.spa, e->catalog_txg);
	} else if (journal_attempted) {
		/* A prepared record can survive a failed publication. */
		c->stage->retired = B_TRUE;
		engine.retired += c->stage->size;
		engine.copy_aborts++;
		engine.revision++;
	} else {
		VERIFY3P(e->versions, ==, c->stage);
		e->versions = c->stage->next;
		zfs_range_tree_add(engine.free, c->stage->p, c->stage->size);
		kmem_free(c->stage, sizeof (*c->stage));
		engine.copy_aborts++;
		engine.revision++;
	}
	VERIFY3U(c->source->leases, >, 0);
	c->source->leases--;
	VERIFY3U(e->holds, >, 0);
	e->holds--;
	e->copying = B_FALSE;
	cv_broadcast(&engine.changed);
	engine_pending_t *pending = e->pending;
	e->pending = NULL;
	mutex_exit(&engine.lock);
	engine_resume(pending);
	kmem_free(c, sizeof (*c));
	return (error);
}

static int
engine_copy_finish(engine_copy_t *c, int error, boolean_t ready)
{
	boolean_t journal_attempted = engine_copy_settle(c, &error, ready);
	if (journal_attempted)
		error = engine_journal_bind_copy(c);
	return (engine_copy_publish(c, error, journal_attempted));
}

int
pr19262_engine_copy_run(void *arg, boolean_t fail_verify)
{
	engine_copy_t *c = arg;
	abd_t *source = abd_alloc_for_io(c->psize, B_FALSE);
	abd_t *verify = abd_alloc_for_io(c->psize, B_FALSE);
	int error = engine_copy_io(c, c->source, source, ZIO_TYPE_READ);
	if (error == 0)
		error = engine_copy_io(c, c->stage, source, ZIO_TYPE_WRITE);
	if (error == 0 && fail_verify) {
		/* Corrupt real staging bytes for the negative control. */
		abd_copy(verify, source, c->psize);
		uint64_t corrupt;
		abd_copy_to_buf(&corrupt, verify, sizeof (corrupt));
		corrupt ^= 1;
		abd_copy_from_buf_off(verify, &corrupt, 0, sizeof (corrupt));
		error = engine_copy_io(c, c->stage, verify, ZIO_TYPE_WRITE);
	}
	if (error == 0)
		error = engine_copy_io(c, c->stage, verify, ZIO_TYPE_READ);
	if (error == 0 && abd_cmp(source, verify) != 0)
		error = ECKSUM;
	abd_free(source);
	abd_free(verify);
	/* Flush data before publishing its J binding: every member at once. */
	if (error == 0 && engine.persistent) {
		spa_t *spa = engine.spa;
		engine_quorum_t q = { 0 };
		spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
		engine_members_flush(spa, &q);
		spa_config_exit(spa, SCL_ZIO, FTAG);
		error = engine_quorum_result(&q);
	}
	return (engine_copy_finish(c, error, B_TRUE));
}

/*
 * Copy a group of extents with each step issued for all of them at once:
 * the reads, the writes, the verifying reads, then one flush of every
 * member before one write of all their journal frames and one head.  A
 * copy run alone pays the three flush rounds by itself.  errors[i]
 * receives each copy's result; every handle is consumed.  A source that
 * may never have been written (speculative[i], a log chain's tail) is
 * read without charging its checksum failure to the children.
 */
void
pr19262_engine_copy_group(void **handles, uint_t n,
    const boolean_t *speculative, int *errors)
{
	engine_copy_t **cv = (engine_copy_t **)handles;
	spa_t *spa = engine.spa;
	abd_t **source = kmem_alloc(n * sizeof (abd_t *), KM_SLEEP);
	abd_t **verify = kmem_alloc(n * sizeof (abd_t *), KM_SLEEP);
	zio_t **zio = kmem_alloc(n * sizeof (zio_t *), KM_SLEEP);
	boolean_t *attempted = kmem_alloc(n * sizeof (boolean_t), KM_SLEEP);
	for (uint_t i = 0; i < n; i++) {
		errors[i] = 0;
		source[i] = abd_alloc_for_io(cv[i]->psize, B_FALSE);
		verify[i] = abd_alloc_for_io(cv[i]->psize, B_FALSE);
	}
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	/* Read the sources, write the stages, read the stages back. */
	for (uint_t step = 0; step < 3; step++) {
		for (uint_t i = 0; i < n; i++) {
			engine_copy_t *c = cv[i];
			zio[i] = errors[i] != 0 ? NULL :
			    pr19262_engine_physical_io(spa, &c->original,
			    engine.vd, c->owner->v,
			    step == 2 ? verify[i] : source[i], c->psize,
			    step == 1 ? ZIO_TYPE_WRITE : ZIO_TYPE_READ,
			    spa_last_synced_txg(spa) + 1,
			    step == 0 ? c->source : c->stage,
			    step == 0 && speculative != NULL &&
			    speculative[i] ? ZIO_FLAG_SPECULATIVE : 0);
		}
		for (uint_t i = 0; i < n; i++)
			if (zio[i] != NULL)
				errors[i] = zio_wait(zio[i]);
	}
	boolean_t any = B_FALSE;
	for (uint_t i = 0; i < n; i++) {
		if (errors[i] == 0 && abd_cmp(source[i], verify[i]) != 0)
			errors[i] = ECKSUM;
		any |= errors[i] == 0;
		abd_free(source[i]);
		abd_free(verify[i]);
	}
	/* Flush data before publishing its J bindings: every member at once. */
	if (any && engine.persistent) {
		engine_quorum_t q = { 0 };
		engine_members_flush(spa, &q);
		int error = engine_quorum_result(&q);
		for (uint_t i = 0; i < n; i++)
			if (errors[i] == 0)
				errors[i] = error;
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	for (uint_t i = 0; i < n; i++)
		attempted[i] = engine_copy_settle(cv[i], &errors[i], B_TRUE);
	engine_journal_bind_copies(cv, n, attempted, errors);
	for (uint_t i = 0; i < n; i++)
		errors[i] = engine_copy_publish(cv[i], errors[i],
		    attempted[i]);
	kmem_free(attempted, n * sizeof (boolean_t));
	kmem_free(zio, n * sizeof (zio_t *));
	kmem_free(verify, n * sizeof (abd_t *));
	kmem_free(source, n * sizeof (abd_t *));
}

int
pr19262_engine_copy_begin(spa_t *spa, const blkptr_t *bp, int d,
    uint64_t birth, void **handle)
{
	return (engine_copy_begin(spa, bp, d, birth, handle, B_FALSE));
}

/*
 * A log block allocated ahead of its write carries nothing to copy: the
 * chain ends at it until the ZIL writes it.  Bind it to a fresh backing
 * of the requested layout so that write lands in the stronger layout.
 */
int
pr19262_engine_rebind_unwritten(spa_t *spa, const blkptr_t *bp, int d,
    uint64_t birth)
{
	void *handle;
	int error = engine_copy_begin(spa, bp, d, birth, &handle, B_TRUE);
	if (error != 0)
		return (error);
	return (engine_copy_finish(handle, 0, B_FALSE));
}

void
pr19262_engine_check(spa_t *spa, const char *phase)
{
	mutex_enter(&engine.lock);
	VERIFY3P(spa, ==, engine.spa);
	uint64_t owned = 0, holds = 0, different = 0, leases = 0;
	uint64_t versions = 0, checkpoint_only = 0;
	uint64_t count = avl_numnodes(&engine.owners);
	zfs_range_seg64_t *virtual = pr19262_alloc(count * sizeof (*virtual),
	    KM_SLEEP);
	boolean_t *pending = kmem_alloc(count * sizeof (*pending), KM_SLEEP);
	uint64_t n = 0;
	zfs_range_tree_t *seen = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	for (engine_owner_t *e = avl_first(&engine.owners); e != NULL;
	    e = AVL_NEXT(&engine.owners, e)) {
		for (engine_version_t *b = e->versions; b != NULL;
		    b = b->next) {
			zfs_range_tree_verify_not_present(engine.free,
			    b->p, b->size);
			owned += b->size;
			zfs_range_tree_add(seen, b->p, b->size);
			leases += b->leases;
			versions++;
		}
		VERIFY(!e->aborted);
		pending[n] = e->journal_pending;
		virtual[n++] = (zfs_range_seg64_t) { e->v, e->v + e->size };
		holds += e->holds;
		checkpoint_only += e->checkpoint_only;
		different += e->v != e->cur->p;
	}
	VERIFY3U(owned + zfs_range_tree_space(engine.free) +
	    zfs_range_tree_space(engine.legacy) +
	    zfs_range_tree_space(engine.unusable) +
	    (engine.store_extents == NULL ? 0 :
	    zfs_range_tree_space(engine.store_extents)) +
	    (engine.journal_extents == NULL ? 0 :
	    zfs_range_tree_space(engine.journal_extents)), ==, engine.total);
	if (engine.store_extents != NULL)
		zfs_range_tree_walk(engine.store_extents,
		    zfs_range_tree_add, seen);
	if (engine.journal_extents != NULL)
		zfs_range_tree_walk(engine.journal_extents,
		    zfs_range_tree_add, seen);
	zfs_range_tree_walk(engine.free, zfs_range_tree_add, seen);
	zfs_range_tree_walk(engine.legacy, zfs_range_tree_add, seen);
	zfs_range_tree_walk(engine.unusable, zfs_range_tree_add, seen);
	VERIFY3U(zfs_range_tree_space(seen), ==, engine.total);
	zfs_range_tree_vacate(seen, NULL, NULL);
	zfs_range_tree_destroy(seen);
	pr19262_printf("PR19262_ENGINE %s owners=%llu different=%llu "
	    "owned=%llu free=%llu retired=%llu holds=%llu pairs=%llu "
	    "aborts=%llu io=%llu metadata=%llu legacy=%llu versions=%llu "
	    "leases=%llu copied=%llu copy_aborts=%llu deferred=%llu "
	    "checkpoint=%llu cp_catalog=%llu checkpoint_only=%llu\n", phase,
	    (u_longlong_t)avl_numnodes(&engine.owners),
	    (u_longlong_t)different, (u_longlong_t)owned,
	    (u_longlong_t)zfs_range_tree_space(engine.free),
	    (u_longlong_t)engine.retired, (u_longlong_t)holds,
	    (u_longlong_t)engine.pairs, (u_longlong_t)engine.aborts,
	    (u_longlong_t)engine.translated, (u_longlong_t)engine.metadata,
	    (u_longlong_t)engine.legacy_reads, (u_longlong_t)versions,
	    (u_longlong_t)leases, (u_longlong_t)engine.copied,
	    (u_longlong_t)engine.copy_aborts, (u_longlong_t)engine.deferred,
	    (u_longlong_t)engine.checkpoint.ub_txg,
	    (u_longlong_t)engine.checkpoint.ub_pr19262[6],
	    (u_longlong_t)checkpoint_only);
	mutex_exit(&engine.lock);
	/* Independent native V allocation checks, outside the catalog lock. */
	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	for (uint64_t i = 0; i < n; i++) {
		int claim = metaslab_claim_impl(engine.vd, virtual[i].rs_start,
		    virtual[i].rs_end - virtual[i].rs_start, 0);
		int expected = pending[i] ? 0 : ENOENT;
		if (claim != expected) {
			mutex_enter(&engine.lock);
			engine_owner_t *e = engine_find(virtual[i].rs_start);
			pr19262_printf("PR19262_V_CLAIM v=%llu "
			    "size=%llu allocation=%llu retired=%u result=%d "
			    "ub=%llu\n", (u_longlong_t)e->v,
			    (u_longlong_t)e->size,
			    (u_longlong_t)e->allocation_txg, e->retired, claim,
			    (u_longlong_t)spa_last_synced_txg(spa));
			mutex_exit(&engine.lock);
		}
		VERIFY3S(claim, ==, expected);
	}
	spa_config_exit(spa, SCL_ALL, FTAG);
	kmem_free(virtual, count * sizeof (*virtual));
	kmem_free(pending, count * sizeof (*pending));
}

static int engine_journal_format(spa_t *);
static int engine_journal_recover(spa_t *);
#include "vdev_reloc_store.c"
#include "vdev_reloc_journal.c"
#include "vdev_reloc_convert.c"
#include "vdev_reloc_admit.c"
