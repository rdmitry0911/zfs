/* SPDX-License-Identifier: CDDL-1.0 */
/*
 * Private normal writable-SPA V/P ownership engine, included by metaslab.c.
 * All new allocations, including MOS/ditto, are bound automatically. Native
 * V metadata remains allocated on final free; no supplied floor permits reuse.
 * State is RAM only. This is NOT an importable on-disk feature/converter.
 */
#include <sys/vdev_reloc_engine.h>

typedef struct engine_owner {
	avl_node_t avl;
	uint64_t v;
	uint64_t p;
	uint64_t size;
	uint64_t birth;
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
} engine;

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
	e->p = pick.found;
	e->size = size;
	e->birth = txg;
	e->incarnation = ++engine.next_incarnation;
	e->created = ++engine.sequence;
	zfs_range_tree_remove(engine.free, e->p, size);
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
	zfs_range_tree_verify_not_present(engine.free, e->p, e->size);
	zfs_range_tree_add(engine.free, e->p, e->size);
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
		e->v = e->p = v;
		e->size = size;
		e->incarnation = ++engine.next_incarnation;
		avl_add(&engine.owners, e);
	}
	VERIFY3U(e->size, ==, size);
	VERIFY(!e->retired && !e->aborted);
	e->retired = B_TRUE;
	e->retired_at = ++engine.sequence;
	engine.retired += size;
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
			e->v = e->p = DVA_GET_OFFSET(dva);
			e->size = size;
			e->birth = BP_GET_PHYSICAL_BIRTH(bp);
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
		return (0);
	}
	if (zio->io_flags & ZIO_FLAG_PHYSICAL) {
		/* Labels/probes are outside the data area. */
		boolean_t unsafe = zio->io_type == ZIO_TYPE_WRITE &&
		    zio->io_vd->vdev_ops->vdev_op_leaf &&
		    zio->io_offset >= VDEV_LABEL_START_SIZE &&
		    zio->io_offset < zio->io_vd->vdev_psize -
		    VDEV_LABEL_END_SIZE &&
		    zio->io_pr19262_owner_hold == NULL;
		mutex_exit(&engine.lock);
		return (unsafe ? ENOTSUP : 0);
	}
	if (zio->io_vd != zio->io_vd->vdev_top) {
		mutex_exit(&engine.lock);
		return (0);
	}
	VERIFY3P(zio->io_vd, ==, engine.vd);
	engine_hold_t *h = zio->io_pr19262_owner_hold;
	if (h == NULL || h->error != 0 || zio->io_bp == NULL) {
		int error = h != NULL && h->error != 0 ? h->error : ENOENT;
		mutex_exit(&engine.lock);
		return (error);
	}
	uint64_t v = zio->io_pr19262_owner_mapped ?
	    zio->io_pr19262_owner_v : zio->io_offset;
	engine_owner_t *e = NULL;
	for (uint_t i = 0; i < h->count; i++)
		if (h->owner[i]->v == v)
			e = h->owner[i];
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
	uint64_t required = vdev_psize_to_asize_txg(engine.vd,
	    zio->io_size, e->birth);
	VERIFY3U(required, <=, e->size);
	/* Keep the original V-BP for checksum/crypto/gang verification. */
	blkptr_t *physical = &zio->io_pr19262_owner_bp;
	*physical = *zio->io_bp;
	boolean_t matched = B_FALSE;
	for (int d = 0; d < BP_GET_NDVAS(physical); d++) {
		dva_t *dva = &physical->blk_dva[d];
		if (DVA_GET_VDEV(dva) == engine.vd->vdev_id &&
		    DVA_GET_OFFSET(dva) == v) {
			DVA_SET_OFFSET(dva, e->p);
			DVA_SET_ASIZE(dva, e->size);
			matched = B_TRUE;
		}
	}
	VERIFY(matched);
	BP_SET_PHYSICAL_BIRTH(physical, e->birth);
	if (engine.fail_writes && zio->io_type == ZIO_TYPE_WRITE &&
	    BP_GET_LEVEL(zio->io_bp) == 0 &&
	    !DMU_OT_IS_METADATA(BP_GET_TYPE(zio->io_bp))) {
		mutex_exit(&engine.lock);
		return (EIO);
	}
	zio->io_pr19262_owner_mapped = B_TRUE;
	zio->io_pr19262_owner_v = v;
	zio->io_offset = e->p;
	/* Operation TXG is never replaced by an encoding/backing birth. */
	engine.translated++;
	engine.legacy_reads += e->v == e->p;
	if (BP_GET_LEVEL(zio->io_bp) != 0 ||
	    DMU_OT_IS_METADATA(BP_GET_TYPE(zio->io_bp)))
		engine.metadata++;
	mutex_exit(&engine.lock);
	return (0);
}

void
pr19262_engine_check(spa_t *spa, const char *phase)
{
	mutex_enter(&engine.lock);
	VERIFY3P(spa, ==, engine.spa);
	uint64_t owned = 0, holds = 0, different = 0;
	uint64_t count = avl_numnodes(&engine.owners);
	zfs_range_seg64_t *virtual = kmem_alloc(count * sizeof (*virtual),
	    KM_SLEEP);
	uint64_t n = 0;
	zfs_range_tree_t *seen = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	for (engine_owner_t *e = avl_first(&engine.owners); e != NULL;
	    e = AVL_NEXT(&engine.owners, e)) {
		zfs_range_tree_verify_not_present(engine.free, e->p, e->size);
		owned += e->size;
		zfs_range_tree_add(seen, e->p, e->size);
		VERIFY(!e->aborted);
		virtual[n++] = (zfs_range_seg64_t) { e->v, e->v + e->size };
		holds += e->holds;
		different += e->v != e->p;
	}
	VERIFY3U(owned + zfs_range_tree_space(engine.free) +
	    zfs_range_tree_space(engine.legacy), ==, engine.total);
	zfs_range_tree_walk(engine.free, zfs_range_tree_add, seen);
	zfs_range_tree_walk(engine.legacy, zfs_range_tree_add, seen);
	VERIFY3U(zfs_range_tree_space(seen), ==, engine.total);
	zfs_range_tree_vacate(seen, NULL, NULL);
	zfs_range_tree_destroy(seen);
	(void) printf("PR19262_ENGINE %s owners=%llu different=%llu "
	    "owned=%llu free=%llu retired=%llu holds=%llu pairs=%llu "
	    "aborts=%llu io=%llu metadata=%llu legacy=%llu\n", phase,
	    (u_longlong_t)avl_numnodes(&engine.owners),
	    (u_longlong_t)different, (u_longlong_t)owned,
	    (u_longlong_t)zfs_range_tree_space(engine.free),
	    (u_longlong_t)engine.retired, (u_longlong_t)holds,
	    (u_longlong_t)engine.pairs, (u_longlong_t)engine.aborts,
	    (u_longlong_t)engine.translated, (u_longlong_t)engine.metadata,
	    (u_longlong_t)engine.legacy_reads);
	mutex_exit(&engine.lock);
	/* Independent native V allocation checks, outside the catalog lock. */
	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	for (uint64_t i = 0; i < n; i++)
		VERIFY3S(metaslab_claim_impl(engine.vd, virtual[i].rs_start,
		    virtual[i].rs_end - virtual[i].rs_start, 0), ==, ENOENT);
	spa_config_exit(spa, SCL_ALL, FTAG);
	kmem_free(virtual, count * sizeof (*virtual));
}
