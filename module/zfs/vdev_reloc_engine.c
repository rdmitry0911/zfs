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
	engine_version_t *versions;
	engine_version_t *current;
	engine_pending_t *pending;
	boolean_t copying;
	uint64_t incarnation;
	uint64_t holds;
	uint64_t created;
	uint64_t retired_at;
	boolean_t retired;
	boolean_t aborted;
} engine_owner_t;

typedef struct engine_hold {
	uint64_t refs;
	uint_t count;
	engine_owner_t *owner[SPA_DVAS_PER_BP];
	int error;
	uint64_t started;
	boolean_t gang;
} engine_hold_t;

typedef struct engine_copy {
	engine_owner_t *owner;
	engine_version_t *source;
	engine_version_t *stage;
	blkptr_t original;
	uint64_t psize;
} engine_copy_t;

static struct {
	kmutex_t lock;
	spa_t *spa;
	vdev_t *vd;
	avl_tree_t owners;
	zfs_range_tree_t *free;
	zfs_range_tree_t *legacy;
	uint64_t total;
	uint64_t next_incarnation;
	uint64_t pairs;
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
	uint64_t revision;
	uint64_t stored_revision;
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

void
pr19262_engine_init(void)
{
	mutex_init(&engine.lock, NULL, MUTEX_DEFAULT, NULL);
	avl_create(&engine.owners, engine_compare, sizeof (engine_owner_t),
	    offsetof(engine_owner_t, avl));
}

void
pr19262_engine_fini(void)
{
	/* A translated test image must never be handed to an old importer. */
	VERIFY3P(engine.spa, ==, NULL);
	VERIFY(avl_is_empty(&engine.owners));
	avl_destroy(&engine.owners);
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

void
pr19262_engine_start(spa_t *spa)
{
	VERIFY(spa_writeable(spa));
	VERIFY(spa->spa_trust_config);
	txg_wait_synced(spa->spa_dsl_pool, 0);
	VERIFY3P(engine.spa, ==, NULL);
	/* Isolated fresh single-RAIDZ fixture; no background client writers. */
	spa_config_enter(spa, SCL_ALLOC | SCL_FREE, FTAG, RW_WRITER);
	vdev_t *root = spa->spa_root_vdev;
	VERIFY3U(root->vdev_children, ==, 1);
	vdev_t *vd = root->vdev_child[0];
	VERIFY3P(vd->vdev_ops, ==, &vdev_raidz_ops);
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
	mutex_enter(&engine.lock);
	engine.free = free;
	engine.legacy = legacy;
	engine.vd = vd;
	engine.total = vd->vdev_asize;
	engine.retired_limit = 64ULL << 20;
	engine.spa = spa;
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
	if (engine_find(v) != NULL) {
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
	e->current = engine_version_new(e, pick.found, size, txg, B_FALSE);
	e->incarnation = ++engine.next_incarnation;
	e->created = ++engine.sequence;
	zfs_range_tree_remove(engine.free, e->current->p, size);
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

void
pr19262_engine_abort(spa_t *spa, vdev_t *vd, uint64_t v, uint64_t size)
{
	mutex_enter(&engine.lock);
	if (engine.spa == spa && spa != NULL) {
		VERIFY3P(vd, ==, engine.vd);
		engine_owner_t *e = engine_find(v);
		/* Pair creation can fail before any P ownership was created. */
		if (e != NULL && !e->aborted) {
			VERIFY3U(e->size, ==, size);
			VERIFY(!e->retired);
			e->aborted = B_TRUE;
			engine.revision++;
			engine.aborts++;
			engine_abort_done(e);
		}
	}
	mutex_exit(&engine.lock);
}

boolean_t
pr19262_engine_retire(vdev_t *vd, uint64_t v, uint64_t size,
    boolean_t checkpoint)
{
	boolean_t handled = B_FALSE;
	mutex_enter(&engine.lock);
	if (engine.spa != vd->vdev_spa || engine.spa == NULL || checkpoint)
		goto out;
	VERIFY3P(vd, ==, engine.vd);
	engine_owner_t *e = engine_find(v);
	if (e == NULL) {
		VERIFY(zfs_range_tree_contains(engine.legacy, v, size));
		zfs_range_tree_remove(engine.legacy, v, size);
		e = kmem_zalloc(sizeof (*e), KM_SLEEP);
		e->v = v;
		e->size = size;
		e->current = engine_version_new(e, v, size, 0, B_TRUE);
		e->incarnation = ++engine.next_incarnation;
		avl_add(&engine.owners, e);
	}
	VERIFY3U(e->size, ==, size);
	VERIFY(!e->retired && !e->aborted);
	e->retired = B_TRUE;
	engine.revision++;
	e->retired_at = ++engine.sequence;
	engine.retired += e->current->size;
	/* No fake floor: neither authoritative V nor P becomes free. */
	handled = B_TRUE;
out:
	mutex_exit(&engine.lock);
	return (handled);
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
	if (prior != NULL)
		h->error = prior->error;
	h->started = prior != NULL ? prior->started : ++engine.sequence;
	h->gang = (prior != NULL && prior->gang) ||
	    (bp != NULL && BP_IS_GANG(bp));
	if (bp == NULL) {
		*h = *prior;
		for (uint_t i = 0; i < h->count; i++)
			h->owner[i]->holds++;
		mutex_exit(&engine.lock);
		return (h);
	}
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		if (DVA_GET_VDEV(dva) != engine.vd->vdev_id) {
			h->error = ENOTSUP;
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
				continue;
			}
			e = kmem_zalloc(sizeof (*e), KM_SLEEP);
			e->v = DVA_GET_OFFSET(dva);
			e->size = size;
			e->current = engine_version_new(e, e->v, size,
			    BP_GET_PHYSICAL_BIRTH(bp), B_TRUE);
			e->incarnation = ++engine.next_incarnation;
			zfs_range_tree_remove(engine.legacy, e->v, size);
			avl_add(&engine.owners, e);
		}
		boolean_t descendant = prior != NULL && prior->error == 0 &&
		    prior->gang && gang_child && read;
		if ((e->retired && !inherited &&
		    !(descendant && h->started < e->retired_at)) ||
		    (e->aborted && !inherited) ||
		    (descendant && e->created > h->started))
			h->error = ESTALE;
		e->holds++;
		h->owner[h->count++] = e;
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
		if (zio->io_error == 0)
			b->ready = B_TRUE;
	}
	engine_owner_t *e = b->owner;
	if (e->aborted)
		engine_abort_done(e);
	zio->io_pr19262_owner_version = NULL;
	mutex_exit(&engine.lock);
}

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
		uint64_t low = VDEV_LABEL_START_SIZE +
		    engine.arena / engine.vd->vdev_children;
		boolean_t metadata = engine.persistent &&
		    zio->io_pr19262_metadata_authority == &engine &&
		    zio->io_vd->vdev_parent == engine.vd &&
		    zio->io_offset >= low &&
		    zio->io_offset - low <= engine.arena_bytes &&
		    zio->io_size <= engine.arena_bytes -
		    (zio->io_offset - low);
		/* Labels/probes are outside the data area. */
		boolean_t unsafe = zio->io_type == ZIO_TYPE_WRITE &&
		    zio->io_vd->vdev_ops->vdev_op_leaf &&
		    zio->io_offset >= VDEV_LABEL_START_SIZE &&
		    zio->io_offset < zio->io_vd->vdev_psize -
		    VDEV_LABEL_END_SIZE &&
		    zio->io_pr19262_owner_hold == NULL && !metadata;
		mutex_exit(&engine.lock);
		return (unsafe ? ENOTSUP : 0);
	}
	if (zio->io_vd != zio->io_vd->vdev_top) {
		mutex_exit(&engine.lock);
		return (0);
	}
	VERIFY3P(zio->io_vd, ==, engine.vd);
	engine_hold_t *h = zio->io_pr19262_owner_hold;
	if ((!zio->io_pr19262_owner_copy &&
	    (h == NULL || h->error != 0)) || zio->io_bp == NULL) {
		int error = h != NULL && h->error != 0 ? h->error : ENOENT;
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
			b = e->current;
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

int
pr19262_engine_copy_begin(spa_t *spa, const blkptr_t *bp, int d,
    uint64_t birth, void **handle)
{
	*handle = NULL;
	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) || d < 0 ||
	    d >= BP_GET_NDVAS(bp))
		return (EINVAL);
	const dva_t *dva = &bp->blk_dva[d];
	uint64_t psize = BP_GET_PSIZE(bp);
	mutex_enter(&engine.lock);
	if (engine.spa != spa || spa == NULL ||
	    DVA_GET_VDEV(dva) != engine.vd->vdev_id) {
		mutex_exit(&engine.lock);
		return (ENOTSUP);
	}
	if (DVA_GET_GANG(dva))
		psize = vdev_gang_header_psize(engine.vd);
	engine_owner_t *e = engine_find(DVA_GET_OFFSET(dva));
	if (e == NULL || e->retired || e->aborted) {
		mutex_exit(&engine.lock);
		return (ESTALE);
	}
	if (e->copying || !e->current->ready || e->current->writers != 0 ||
	    e->allocation_txg > spa_last_synced_txg(spa)) {
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
		mutex_exit(&engine.lock);
		return (ENOSPC);
	}
	engine_copy_t *c = kmem_zalloc(sizeof (*c), KM_SLEEP);
	c->owner = e;
	c->source = e->current;
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

void
pr19262_engine_inspect(spa_t *spa, const blkptr_t *bp, int d,
    pr19262_engine_info_t *info)
{
	mutex_enter(&engine.lock);
	VERIFY3P(spa, ==, engine.spa);
	engine_owner_t *e = engine_find(DVA_GET_OFFSET(&bp->blk_dva[d]));
	VERIFY3P(e, !=, NULL);
	*info = (pr19262_engine_info_t) {
	    .p = e->current->p, .vsize = e->size,
	    .psize = e->current->size, .birth = e->current->birth,
	    .leases = e->current->leases };
	for (engine_version_t *b = e->versions; b != NULL; b = b->next) {
		info->versions++;
		if (b != e->current)
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
	    spa_last_synced_txg(spa) + 1, b);
	int error = zio_wait(zio);
	spa_config_exit(spa, SCL_ZIO, FTAG);
	return (error);
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
	mutex_enter(&engine.lock);
	engine_owner_t *e = c->owner;
	VERIFY(e->copying);
	VERIFY3P(e->current, ==, c->source);
	VERIFY0(c->stage->leases);
	if (error == 0 && e->retired)
		error = ESTALE;
	if (error == 0) {
		c->stage->ready = B_TRUE;
		e->current = c->stage;
		c->source->retired = B_TRUE;
		engine.retired += c->source->size;
		engine.copied++;
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
	engine_pending_t *pending = e->pending;
	e->pending = NULL;
	mutex_exit(&engine.lock);
	engine_resume(pending);
	kmem_free(c, sizeof (*c));
	return (error);
}

void
pr19262_engine_check(spa_t *spa, const char *phase)
{
	mutex_enter(&engine.lock);
	VERIFY3P(spa, ==, engine.spa);
	uint64_t owned = 0, holds = 0, different = 0, leases = 0;
	uint64_t versions = 0;
	uint64_t count = avl_numnodes(&engine.owners);
	zfs_range_seg64_t *virtual = kmem_alloc(count * sizeof (*virtual),
	    KM_SLEEP);
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
		virtual[n++] = (zfs_range_seg64_t) { e->v, e->v + e->size };
		holds += e->holds;
		different += e->v != e->current->p;
	}
	VERIFY3U(owned + zfs_range_tree_space(engine.free) +
	    zfs_range_tree_space(engine.legacy) + engine.arena_bytes *
	    engine.vd->vdev_children, ==, engine.total);
	if (engine.arena_bytes != 0)
		zfs_range_tree_add(seen, engine.arena,
		    engine.arena_bytes * engine.vd->vdev_children);
	zfs_range_tree_walk(engine.free, zfs_range_tree_add, seen);
	zfs_range_tree_walk(engine.legacy, zfs_range_tree_add, seen);
	VERIFY3U(zfs_range_tree_space(seen), ==, engine.total);
	zfs_range_tree_vacate(seen, NULL, NULL);
	zfs_range_tree_destroy(seen);
	(void) printf("PR19262_ENGINE %s owners=%llu different=%llu "
	    "owned=%llu free=%llu retired=%llu holds=%llu pairs=%llu "
	    "aborts=%llu io=%llu metadata=%llu legacy=%llu versions=%llu "
	    "leases=%llu copied=%llu copy_aborts=%llu deferred=%llu\n", phase,
	    (u_longlong_t)avl_numnodes(&engine.owners),
	    (u_longlong_t)different, (u_longlong_t)owned,
	    (u_longlong_t)zfs_range_tree_space(engine.free),
	    (u_longlong_t)engine.retired, (u_longlong_t)holds,
	    (u_longlong_t)engine.pairs, (u_longlong_t)engine.aborts,
	    (u_longlong_t)engine.translated, (u_longlong_t)engine.metadata,
	    (u_longlong_t)engine.legacy_reads, (u_longlong_t)versions,
	    (u_longlong_t)leases, (u_longlong_t)engine.copied,
	    (u_longlong_t)engine.copy_aborts, (u_longlong_t)engine.deferred);
	mutex_exit(&engine.lock);
	/* Independent native V allocation checks, outside the catalog lock. */
	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	for (uint64_t i = 0; i < n; i++) {
		int claim = metaslab_claim_impl(engine.vd, virtual[i].rs_start,
		    virtual[i].rs_end - virtual[i].rs_start, 0);
		if (claim != ENOENT) {
			mutex_enter(&engine.lock);
			engine_owner_t *e = engine_find(virtual[i].rs_start);
			(void) fprintf(stderr, "PR19262_V_CLAIM v=%llu "
			    "size=%llu allocation=%llu retired=%u result=%d "
			    "ub=%llu\n", (u_longlong_t)e->v,
			    (u_longlong_t)e->size,
			    (u_longlong_t)e->allocation_txg, e->retired, claim,
			    (u_longlong_t)spa_last_synced_txg(spa));
			mutex_exit(&engine.lock);
		}
		VERIFY3S(claim, ==, ENOENT);
	}
	spa_config_exit(spa, SCL_ALL, FTAG);
	kmem_free(virtual, count * sizeof (*virtual));
}

#include "vdev_reloc_store.c"
