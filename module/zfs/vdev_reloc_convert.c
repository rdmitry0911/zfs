/* SPDX-License-Identifier: CDDL-1.0 */
/*
 * Private whole-vdev converter, census, durable recovery floor and
 * certified parity, included by the ownership engine after its raw store
 * and journal.  Every copy keeps the persistent BP unchanged and moves
 * only the physical backing to the layout of the requested birth.
 */
#include <sys/dmu_objset.h>
#include <sys/dmu_traverse.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_synctask.h>
#include <sys/zil.h>
#include <sys/zil_impl.h>

static void
engine_version_unlink(engine_owner_t *e, engine_version_t *b)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	engine_version_t **link = &e->versions;
	while (*link != b) {
		VERIFY3P(*link, !=, NULL);
		link = &(*link)->next;
	}
	*link = b->next;
}

void
pr19262_engine_census(spa_t *spa, uint64_t parity,
    pr19262_engine_census_t *c)
{
	memset(c, 0, sizeof (*c));
	c->min_parity = UINT64_MAX;
	mutex_enter(&engine.lock);
	VERIFY3P(spa, ==, engine.spa);
	for (engine_owner_t *e = avl_first(&engine.owners); e != NULL;
	    e = AVL_NEXT(&engine.owners, e)) {
		c->owners++;
		c->retired_owners += e->retired;
		for (engine_version_t *b = e->versions; b != NULL;
		    b = b->next) {
			if (b == e->cur && !e->retired && !e->aborted) {
				c->current_versions++;
				c->current_bytes += b->size;
				c->min_parity = MIN(c->min_parity, b->parity);
				if (b->parity < parity) {
					c->weak_current++;
					c->weak_current_bytes += b->size;
				}
			} else {
				c->retired_versions++;
				c->retired_bytes += b->size;
				c->weak_retired += b->parity < parity;
			}
		}
	}
	if (c->min_parity == UINT64_MAX)
		c->min_parity = 0;
	c->legacy_bytes = zfs_range_tree_space(engine.legacy);
	c->legacy_segments = zfs_range_tree_numsegs(engine.legacy);
	c->unusable_bytes = zfs_range_tree_space(engine.unusable);
	c->free_bytes = zfs_range_tree_space(engine.free);
	if (engine.store_extents != NULL)
		c->store_bytes = zfs_range_tree_space(engine.store_extents);
	if (engine.journal_extents != NULL)
		c->journal_bytes = zfs_range_tree_space(engine.journal_extents);
	for (engine_j_segment_t *s = engine.journal_segments; s != NULL;
	    s = s->next)
		c->journal_segments++;
	for (engine_j_segment_t *s = engine.journal_retired; s != NULL;
	    s = s->next)
		c->journal_retired++;
	c->journal_freed_bytes = engine.journal_freed_bytes;
	c->tombstones = avl_numnodes(&engine.tombstones);
	c->floor = engine_pages.floor;
	c->certified = engine.certified;
	c->durable_txg = engine.durable_ub_txg;
	c->journal_seq = engine.journal_seq;
	c->pages_capacity = engine_pages.capacity;
	for (uint64_t i = 0; i < engine_pages.capacity; i++)
		c->pages_used += engine_pages.bits[i] != 0;
	mutex_exit(&engine.lock);
}

uint64_t
pr19262_engine_owner_incarnation(spa_t *spa, uint64_t v, uint64_t size)
{
	mutex_enter(&engine.lock);
	engine_owner_t *e = engine.spa == spa ? engine_overlap(v, size) : NULL;
	uint64_t incarnation = e == NULL ? 0 : e->incarnation;
	mutex_exit(&engine.lock);
	return (incarnation);
}

uint64_t
pr19262_engine_floor(spa_t *spa)
{
	mutex_enter(&engine.lock);
	uint64_t floor = engine.spa == spa && engine.persistent ?
	    engine_pages.floor : 0;
	mutex_exit(&engine.lock);
	return (floor);
}

/*
 * A finally freed owner may release both its native V and its physical
 * backing once the catalog that lists it retired is at or below the floor
 * and nothing references it any more.
 */
static boolean_t
engine_owner_releasable(engine_owner_t *e, uint64_t catalog)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	if (!e->retired || e->aborted || e->checkpoint_only || e->copying ||
	    e->releasing || e->pending != NULL || e->holds != 0 ||
	    e->queued_writers != 0 || e->journal_pending ||
	    e->retired_txg == 0 || e->retired_txg > catalog)
		return (B_FALSE);
	for (engine_version_t *b = e->versions; b != NULL; b = b->next)
		if (b->leases != 0 || b->writers != 0)
			return (B_FALSE);
	return (B_TRUE);
}

typedef struct engine_release {
	uint64_t catalog;
	uint64_t owners;
	uint64_t bytes;
} engine_release_t;

static void
engine_release_sync(void *arg, dmu_tx_t *tx)
{
	engine_release_t *r = arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	for (;;) {
		mutex_enter(&engine.lock);
		engine_owner_t *e = NULL;
		if (engine.spa == spa && engine.persistent) {
			for (e = avl_first(&engine.owners); e != NULL;
			    e = AVL_NEXT(&engine.owners, e))
				if (engine_owner_releasable(e, r->catalog))
					break;
		}
		if (e == NULL) {
			mutex_exit(&engine.lock);
			return;
		}
		e->releasing = B_TRUE;
		uint64_t v = e->v, size = e->size;
		engine.release_thread = curthread;
		mutex_exit(&engine.lock);
		/* The native V returns to this syncing txg's space map. */
		metaslab_free_concrete(engine.vd, v, size, B_FALSE);
		mutex_enter(&engine.lock);
		engine.release_thread = NULL;
		VERIFY3P(engine_find(v), ==, e);
		while (e->versions != NULL) {
			engine_version_t *b = e->versions;
			e->versions = b->next;
			VERIFY0(b->leases);
			zfs_range_tree_verify_not_present(engine.free, b->p,
			    b->size);
			zfs_range_tree_add(engine.free, b->p, b->size);
			if (b->retired || b == e->cur) {
				VERIFY3U(engine.retired, >=, b->size);
				engine.retired -= b->size;
			}
			r->bytes += b->size;
			kmem_free(b, sizeof (*b));
		}
		avl_remove(&engine.owners, e);
		/* Journal records of this identity may still be replayed. */
		if (e->journal_touched)
			engine_tombstone_add(v, e->incarnation,
			    engine.journal_tail != NULL ?
			    engine.journal_tail->index : 0);
		kmem_free(e, sizeof (*e));
		r->owners++;
		engine.revision++;
		mutex_exit(&engine.lock);
	}
}

/*
 * Advance the durable recovery floor to the latest written uberblock.
 * Every root below it is refused by the importer, so the backing of each
 * version that the floor root's catalog already lists as retired behind a
 * newer binding returns to the physical allocator, and finally freed
 * owners release their V and P from a sync task.  A pool checkpoint pins
 * everything until it is discarded.
 */
int
pr19262_engine_fence(spa_t *spa, uint64_t *floor, uint64_t *reclaimed,
    uint64_t *released, uint64_t *sample)
{
	*floor = *reclaimed = *released = 0;
	if (!pr19262_engine_persistent(spa) || !spa_writeable(spa))
		return (ENOTSUP);
	/* The catalog listing the retirements must be on disk first. */
	txg_wait_synced(spa->spa_dsl_pool, 0);
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine.store_lock);
	mutex_enter(&engine.lock);
	uint64_t ub = engine.durable_ub_txg;
	uint64_t catalog = engine.durable_catalog_txg;
	boolean_t checkpoint = engine.checkpoint.ub_txg != 0;
	mutex_exit(&engine.lock);
	int error = 0;
	uint64_t pages_freed = 0, journal_segments = 0, journal_bytes = 0;
	if (checkpoint)
		error = EBUSY;
	else if (ub == 0 || ub < engine_pages.floor || catalog > ub)
		error = ENGINE_EINVAL(101);
	else if (ub != engine_pages.floor || engine_pages.gc_relaxed) {
		/* Dead catalog pages go with the floor into both slots. */
		error = engine_pages_gc(spa, &pages_freed);
		if (error == 0)
			error = engine_pages_publish_control(spa, ub,
			    engine_pages.certified);
	}
	if (error == 0) {
		mutex_enter(&engine.lock);
		for (engine_owner_t *e = avl_first(&engine.owners); e != NULL;
		    e = AVL_NEXT(&engine.owners, e)) {
			engine_version_t *b = e->versions;
			while (b != NULL) {
				engine_version_t *next = b->next;
				if (e->cur != NULL && b != e->cur &&
				    b->retired &&
				    b->generation < e->cur->generation &&
				    b->retired_txg != 0 &&
				    b->retired_txg <= catalog &&
				    b->leases == 0 && b->writers == 0) {
					engine_version_unlink(e, b);
					zfs_range_tree_verify_not_present(
					    engine.free, b->p, b->size);
					zfs_range_tree_add(engine.free, b->p,
					    b->size);
					VERIFY3U(engine.retired, >=, b->size);
					engine.retired -= b->size;
					if (sample != NULL && sample[1] == 0) {
						sample[0] = b->p;
						sample[1] = b->size;
					}
					*reclaimed += b->size;
					engine.revision++;
					kmem_free(b, sizeof (*b));
				}
				b = next;
			}
		}
		*floor = engine_pages.floor;
		mutex_exit(&engine.lock);
		/* Journal segments folded into this floor root's catalog. */
		engine_journal_gc(spa, catalog, &journal_segments,
		    &journal_bytes);
	}
	if (error == 0 && pr19262_env("PR19262_ENGINE_FENCE_CUT") != NULL) {
		/*
		 * Crash with the floor durable and no later root: the store
		 * lock still blocks every uberblock write at this point.
		 */
		pr19262_printf("PR19262_FENCE_CUT floor=%llu reclaimed=%llu\n",
		    (u_longlong_t)*floor, (u_longlong_t)*reclaimed);
		pr19262_flush();
		pr19262_exit(0);
	}
	mutex_exit(&engine.store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	if (error != 0)
		return (error);
	engine_release_t r = { .catalog = catalog };
	error = dsl_sync_task(spa_name(spa), NULL, engine_release_sync, &r,
	    0, ZFS_SPACE_CHECK_NONE);
	*released = r.owners;
	pr19262_printf("PR19262_FENCE floor=%llu catalog=%llu reclaimed=%llu "
	    "released_owners=%llu released_bytes=%llu pages_freed=%llu "
	    "journal_freed=%llu journal_freed_bytes=%llu error=%d\n",
	    (u_longlong_t)*floor, (u_longlong_t)catalog,
	    (u_longlong_t)*reclaimed, (u_longlong_t)r.owners,
	    (u_longlong_t)r.bytes, (u_longlong_t)pages_freed,
	    (u_longlong_t)journal_segments, (u_longlong_t)journal_bytes,
	    error);
	return (error);
}

/* A fence the admission scheduled for clients past the retired limit. */
static void
engine_fence_task(void *arg)
{
	spa_t *spa = arg;
	uint64_t floor, reclaimed, released;
	int error = pr19262_engine_fence(spa, &floor, &reclaimed, &released,
	    NULL);
	mutex_enter(&engine.lock);
	engine.fence_error = error;
	engine.fence_auto += error == 0;
	engine.fence_stalled = error == 0 && reclaimed == 0 && released == 0;
	engine.fence_pending = B_FALSE;
	cv_broadcast(&engine.fence_cv);
	mutex_exit(&engine.lock);
	pr19262_printf("PR19262_FENCE_AUTO error=%d floor=%llu reclaimed=%llu "
	    "released=%llu\n", error, (u_longlong_t)floor,
	    (u_longlong_t)reclaimed, (u_longlong_t)released);
}

void
pr19262_engine_fence_schedule(spa_t *spa)
{
	VERIFY3U(taskq_dispatch(system_taskq, engine_fence_task, spa,
	    TQ_SLEEP), !=, TASKQID_INVALID);
}

/* Before the sync thread stops: no new fence, and none in flight. */
void
pr19262_engine_quiesce(spa_t *spa)
{
	mutex_enter(&engine.lock);
	if (engine.spa == spa) {
		engine.fence_quiesced = B_TRUE;
		while (engine.fence_pending)
			cv_wait(&engine.fence_cv, &engine.lock);
	}
	mutex_exit(&engine.lock);
}

/*
 * Certify that every backing any importable root can select has at least
 * the given parity.  This requires a completed sweep, a fence that
 * reclaimed every weaker retired version, and no pool checkpoint.  The
 * control record carries the certificate first; the vdev config follows
 * so that vdev open tolerates that many missing children.
 */
/* Name the owners whose current version is weaker than the target. */
static void
engine_weak_report(uint64_t parity)
{
	uint64_t n = 0;
	mutex_enter(&engine.lock);
	for (engine_owner_t *e = avl_first(&engine.owners);
	    e != NULL && n < 8; e = AVL_NEXT(&engine.owners, e)) {
		engine_version_t *b = e->cur;
		if (b == NULL || e->retired || e->aborted ||
		    b->parity >= parity)
			continue;
		n++;
		pr19262_printf("PR19262_WEAK_OWNER v=%llu size=%llu "
		    "alloc_txg=%llu catalog_txg=%llu claimed_txg=%llu inc=%llu "
		    "holds=%llu queued_writers=%llu journal_pending=%u "
		    "journal_touched=%u checkpoint_only=%u copying=%u p=%llu "
		    "birth=%llu width=%llu "
		    "parity=%llu leases=%llu writers=%llu ready=%u "
		    "journal_seq=%llu created=%llu\n", (u_longlong_t)e->v,
		    (u_longlong_t)e->size, (u_longlong_t)e->allocation_txg,
		    (u_longlong_t)e->catalog_txg, (u_longlong_t)e->claimed_txg,
		    (u_longlong_t)e->incarnation, (u_longlong_t)e->holds,
		    (u_longlong_t)e->queued_writers, e->journal_pending,
		    e->journal_touched, e->checkpoint_only, e->copying,
		    (u_longlong_t)b->p, (u_longlong_t)b->birth,
		    (u_longlong_t)b->width, (u_longlong_t)b->parity,
		    (u_longlong_t)b->leases, (u_longlong_t)b->writers, b->ready,
		    (u_longlong_t)b->journal_seq, (u_longlong_t)e->created);
	}
	mutex_exit(&engine.lock);
}

int
pr19262_engine_certify(spa_t *spa, uint64_t parity)
{
	if (!pr19262_engine_persistent(spa) || !spa_writeable(spa))
		return (ENOTSUP);
	pr19262_engine_census_t c;
	pr19262_engine_census(spa, parity, &c);
	mutex_enter(&engine.lock);
	boolean_t checkpoint = engine.checkpoint.ub_txg != 0;
	mutex_exit(&engine.lock);
	if (c.legacy_bytes != 0 || c.weak_current != 0 ||
	    c.weak_retired != 0 || c.floor == 0 || checkpoint ||
	    parity == 0 || parity > VDEV_RAIDZ_MAXPARITY ||
	    engine.vd->vdev_children < parity + 1) {
		pr19262_printf("PR19262_CERTIFY_REFUSED parity=%llu "
		    "legacy=%llu weak_current=%llu weak_retired=%llu "
		    "floor=%llu checkpoint=%u\n", (u_longlong_t)parity,
		    (u_longlong_t)c.legacy_bytes, (u_longlong_t)c.weak_current,
		    (u_longlong_t)c.weak_retired, (u_longlong_t)c.floor,
		    checkpoint);
		if (c.weak_current != 0)
			engine_weak_report(parity);
		return (EBUSY);
	}
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine.store_lock);
	int error = engine_pages_publish_control(spa, engine_pages.floor,
	    parity);
	if (error == 0) {
		mutex_enter(&engine.lock);
		engine.certified = parity;
		mutex_exit(&engine.lock);
	}
	mutex_exit(&engine.store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	if (error != 0)
		return (error);
	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_WRITER);
	pr19262_vdev_certify(engine.vd, parity);
	vdev_config_dirty(engine.vd);
	spa_config_exit(spa, SCL_CONFIG, FTAG);
	txg_wait_synced(spa->spa_dsl_pool, 0);
	pr19262_printf("PR19262_CERTIFIED parity=%llu floor=%llu "
	    "current=%llu bytes=%llu\n", (u_longlong_t)parity,
	    (u_longlong_t)c.floor, (u_longlong_t)c.current_versions,
	    (u_longlong_t)c.current_bytes);
	return (0);
}

/*
 * Copies one flush and journal round carries, and the bytes they may
 * buffer: a copy run alone paid a data flush, a frame and a head.
 */
static uint_t zfs_pr19262_convert_group = 128;
static uint64_t engine_convert_group_bytes = 8ULL << 20;

/* A copy begun by the walk, run with the group it joined. */
typedef struct engine_convert_entry {
	void *copy;
	blkptr_t bp;
	int d;
	boolean_t member;
	boolean_t zil_chain;
	pr19262_engine_info_t before;
} engine_convert_entry_t;

typedef struct engine_convert {
	spa_t *spa;
	uint64_t birth;
	uint64_t parity;
	uint64_t batch;
	uint64_t pending;
	/* Vector admitted for this pass, released after its fence. */
	uint64_t admitted_p;
	uint64_t admitted_m;
	uint64_t admitted_j;
	boolean_t starved;
	/* Consecutive full passes that copied nothing but saw busy blocks. */
	uint64_t idle_passes;
	/* Inside a log chain: an unreadable block is an unwritten tail. */
	boolean_t zil_chain;
	/*
	 * The sweep's cursor: the MOS, then each dataset object in order,
	 * and the bookmark the traversal left where the last slice stopped.
	 */
	boolean_t mos_done;
	uint64_t cursor;
	zbookmark_phys_t resume;
	/*
	 * The current slice yields at the first block past this time, less
	 * the time the group it holds would take to drain at the last
	 * group's pace (at most half a slice): it then drains by the end.
	 */
	hrtime_t slice_end;
	hrtime_t slice_ns;
	hrtime_t drain_ns;
	uint64_t drain_psize;
	boolean_t yielded;
	pr19262_engine_convert_stats_t *stats;
	blkptr_t *gang;
	uint_t gang_count;
	uint_t gang_capacity;
	/*
	 * The copies begun and not yet run, the holds of their blocks, and
	 * their sizes: new backing for the batch, buffers for the cap.
	 */
	engine_convert_entry_t *group;
	engine_hold_t **group_holds;
	uint_t group_count;
	uint_t group_hold_count;
	uint_t group_max;
	uint64_t group_asize;
	uint64_t group_psize;
} engine_convert_t;

static int
engine_convert_collect(spa_t *spa, const blkptr_t *bp, void *arg)
{
	(void) spa;
	engine_convert_t *c = arg;
	if (c->gang_count == c->gang_capacity) {
		uint_t capacity = c->gang_capacity == 0 ? 16 :
		    c->gang_capacity * 2;
		blkptr_t *gang = pr19262_alloc(capacity * sizeof (*gang),
		    KM_SLEEP);
		if (c->gang != NULL) {
			memcpy(gang, c->gang, c->gang_count * sizeof (*gang));
			kmem_free(c->gang, c->gang_capacity * sizeof (*gang));
		}
		c->gang = gang;
		c->gang_capacity = capacity;
	}
	c->gang[c->gang_count++] = *bp;
	return (0);
}

static boolean_t
engine_convert_weak(engine_convert_t *c, const blkptr_t *bp, int d)
{
	boolean_t weak = B_FALSE;
	mutex_enter(&engine.lock);
	engine_owner_t *e = engine_find(DVA_GET_OFFSET(&bp->blk_dva[d]));
	if (e != NULL && !e->retired && !e->aborted && e->cur != NULL)
		weak = e->cur->parity < c->parity;
	mutex_exit(&engine.lock);
	return (weak);
}

static boolean_t
engine_convert_grouped(engine_convert_t *c, uint64_t v)
{
	for (uint_t i = 0; i < c->group_count; i++) {
		engine_convert_entry_t *g = &c->group[i];
		if (DVA_GET_OFFSET(&g->bp.blk_dva[g->d]) == v)
			return (B_TRUE);
	}
	return (B_FALSE);
}

/*
 * Run the copies begun since the last drain as one group, then account
 * for each as a copy run on its own was.  Every hold is released; the
 * first error the walk does not tolerate is returned.
 */
static int
engine_convert_drain(engine_convert_t *c)
{
	uint_t n = c->group_count;
	if (n == 0)
		return (0);
	hrtime_t start = gethrtime();
	uint64_t psize = c->group_psize;
	spa_t *spa = c->spa;
	pr19262_engine_convert_stats_t *st = c->stats;
	void **handles = kmem_alloc(n * sizeof (void *), KM_SLEEP);
	int *errors = kmem_alloc(n * sizeof (int), KM_SLEEP);
	boolean_t *tail = kmem_alloc(n * sizeof (boolean_t), KM_SLEEP);
	/*
	 * A log chain's block may be its never-written tail: read it as the
	 * ZIL reads its chain, so that its checksum failure is not charged
	 * to the children as a device error.
	 */
	for (uint_t i = 0; i < n; i++) {
		engine_convert_entry_t *g = &c->group[i];
		handles[i] = g->copy;
		tail[i] = g->zil_chain && !g->member && !BP_IS_GANG(&g->bp);
	}
	pr19262_engine_copy_group(handles, n, tail, errors);
	int first = 0;
	for (uint_t i = 0; i < n; i++) {
		engine_convert_entry_t *g = &c->group[i];
		const blkptr_t *bp = &g->bp;
		int error = errors[i];
		if (error == ECKSUM && tail[i]) {
			error = pr19262_engine_rebind_unwritten(spa, bp, g->d,
			    c->birth);
			pr19262_printf("PR19262_ZIL_TAIL v=%llu psize=%llu "
			    "birth=%llu ready=%u alloc_txg=%llu rebind=%d\n",
			    (u_longlong_t)DVA_GET_OFFSET(&bp->blk_dva[g->d]),
			    (u_longlong_t)BP_GET_PSIZE(bp),
			    (u_longlong_t)g->before.birth, g->before.ready,
			    (u_longlong_t)g->before.catalog_txg, error);
			if (error == 0)
				st->unwritten++;
		}
		if (error == ESTALE) {
			/*
			 * Freed while being copied (a client free, and the
			 * fence the admission schedules may already have
			 * released it): the copy was abandoned by its
			 * finish, nothing is left to convert.
			 */
			st->stale++;
			continue;
		}
		if (error != 0) {
			if (first == 0)
				first = error;
			continue;
		}
		pr19262_engine_info_t after;
		pr19262_engine_inspect(spa, bp, g->d, &after);
		VERIFY3U(after.p, !=, g->before.p);
		VERIFY3U(after.birth, ==, c->birth);
		st->copied++;
		st->old_bytes += g->before.psize;
		st->new_bytes += after.psize;
		c->pending += after.psize;
		mutex_enter(&engine.lock);
		st->peak_retired_bytes = MAX(st->peak_retired_bytes,
		    engine.retired);
		mutex_exit(&engine.lock);
	}
	for (uint_t i = 0; i < c->group_hold_count; i++)
		pr19262_engine_rele(c->group_holds[i]);
	c->group_count = c->group_hold_count = 0;
	c->group_asize = c->group_psize = 0;
	kmem_free(tail, n * sizeof (boolean_t));
	kmem_free(errors, n * sizeof (int));
	kmem_free(handles, n * sizeof (void *));
	c->drain_ns = gethrtime() - start;
	c->drain_psize = psize;
	return (first);
}

/* One concrete allocation: a plain block, a gang header or a gang member. */
/* The time the group held now would take to drain at the last pace. */
static hrtime_t
engine_convert_drain_estimate(const engine_convert_t *c)
{
	return (c->drain_psize == 0 ? 0 :
	    c->drain_ns * c->group_psize / c->drain_psize);
}

static int
engine_convert_extent(engine_convert_t *c, const blkptr_t *bp,
    boolean_t member)
{
	spa_t *spa = c->spa;
	pr19262_engine_convert_stats_t *st = c->stats;
	boolean_t mine = B_FALSE;
	for (int d = 0; d < BP_GET_NDVAS(bp); d++)
		mine |= DVA_GET_VDEV(&bp->blk_dva[d]) == engine.vd->vdev_id;
	if (!mine)
		return (0);
	st->visited++;
	st->gang_members += member;
	/* The hold classifies a still-legacy allocation into an owner. */
	engine_hold_t *h = pr19262_engine_hold(spa, bp, NULL, B_TRUE, B_FALSE);
	if (h == NULL)
		return (ENOENT);
	boolean_t grouped = B_FALSE;
	int error = h->error;
	if (error == ESTALE) {
		/* Freed between the root snapshot and this visit. */
		st->stale++;
		error = 0;
		goto out;
	}
	if (error != 0)
		goto out;
	for (int d = 0; d < BP_GET_NDVAS(bp) && error == 0; d++) {
		if (DVA_GET_VDEV(&bp->blk_dva[d]) != engine.vd->vdev_id ||
		    !engine_convert_weak(c, bp, d))
			continue;
		/* A block the walk meets twice (a clone) is copied once. */
		if (engine_convert_grouped(c, DVA_GET_OFFSET(&bp->blk_dva[d])))
			continue;
		st->candidates++;
		/* Admit this copy's bytes, catalog row and journal frame. */
		uint64_t psize = DVA_GET_GANG(&bp->blk_dva[d]) ?
		    vdev_gang_header_psize(engine.vd) : BP_GET_PSIZE(bp);
		uint64_t asize = vdev_psize_to_asize_txg(engine.vd, psize,
		    c->birth);
		uint64_t pages = pr19262_engine_admit_row_pages(spa);
		if (pr19262_engine_admit(spa, PR19262_ADMIT_CONVERTER,
		    pr19262_tx_reserved(), asize, pages, 1) != 0) {
			st->starved++;
			c->starved = B_TRUE;
			break;
		}
		c->admitted_p += asize;
		c->admitted_m += pages;
		c->admitted_j++;
		void *copy;
		error = pr19262_engine_copy_begin(spa, bp, d, c->birth, &copy);
		if (error == EBUSY) {
			pr19262_engine_info_t info;
			pr19262_engine_inspect(spa, bp, d, &info);
			/* An unwritten log tail is rebound, not copied. */
			if (c->zil_chain && !member && !BP_IS_GANG(bp) &&
			    !info.ready && info.leases == 0 && !info.copying) {
				error = pr19262_engine_rebind_unwritten(spa, bp,
				    d, c->birth);
				if (error == 0) {
					st->unwritten++;
					continue;
				}
				if (error == ESTALE) {
					/* The chain moved on: stale. */
					st->stale++;
					error = 0;
					continue;
				}
				if (error != EBUSY)
					break;
			}
			st->busy++;
			error = 0;
			/* Name a block that stays busy across whole passes. */
			if (c->idle_passes >= 2) {
				pr19262_printf("PR19262_CONVERT_BUSY v=%llu "
				    "psize=%llu birth=%llu leases=%llu "
				    "old_leases=%llu versions=%llu holds=%llu "
				    "queued=%llu copying=%u ready=%u "
				    "catalog_txg=%llu type=%u level=%u\n",
				    (u_longlong_t)DVA_GET_OFFSET(
				    &bp->blk_dva[d]),
				    (u_longlong_t)BP_GET_PSIZE(bp),
				    (u_longlong_t)info.birth,
				    (u_longlong_t)info.leases,
				    (u_longlong_t)info.old_leases,
				    (u_longlong_t)info.versions,
				    (u_longlong_t)info.holds,
				    (u_longlong_t)info.queued_writers,
				    info.copying, info.ready,
				    (u_longlong_t)info.catalog_txg,
				    (uint_t)BP_GET_TYPE(bp),
				    (uint_t)BP_GET_LEVEL(bp));
			}
			continue;
		}
		if (error == ESTALE) {
			st->stale++;
			error = 0;
			continue;
		}
		if (error != 0)
			break;
		/* The copy runs with its group: one flush and journal round. */
		engine_convert_entry_t *g = &c->group[c->group_count++];
		g->copy = copy;
		g->bp = *bp;
		g->d = d;
		g->member = member;
		g->zil_chain = c->zil_chain;
		pr19262_engine_inspect(spa, bp, d, &g->before);
		c->group_asize += asize;
		c->group_psize += psize;
		grouped = B_TRUE;
	}
out:
	/* The hold keeps the block's owners until its copies are counted. */
	if (grouped)
		c->group_holds[c->group_hold_count++] = h;
	else
		pr19262_engine_rele(h);
	/*
	 * A log chain is copied block by block, as the chain is followed.  A
	 * group drains at half a slice's work too: under load a full group
	 * outlasted the slice's early yield (at most half a slice), and the
	 * slice held the open txg past its time.
	 */
	if (grouped && error == 0 && (c->zil_chain ||
	    c->group_count >= c->group_max ||
	    c->group_psize >= engine_convert_group_bytes ||
	    (c->slice_ns != 0 &&
	    engine_convert_drain_estimate(c) >= c->slice_ns / 2)))
		error = engine_convert_drain(c);
	return (error);
}

static int
engine_convert_bp(engine_convert_t *c, const blkptr_t *bp)
{
	if (!BP_IS_GANG(bp))
		return (engine_convert_extent(c, bp, B_FALSE));
	/* Collect the header and every member; copy outside the walk. */
	c->gang_count = 0;
	int error = pr19262_zio_walk_gang(c->spa, bp, NULL,
	    engine_convert_collect, c);
	if (error == ESTALE) {
		c->stats->stale++;
		return (0);
	}
	for (uint_t i = 0; error == 0 && i < c->gang_count; i++)
		error = engine_convert_extent(c, &c->gang[i], i != 0);
	return (error);
}

static int
engine_convert_visit(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	(void) spa, (void) zilog, (void) zb, (void) dnp;
	engine_convert_t *c = arg;
	if (bp == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) ||
	    BP_IS_REDACTED(bp))
		return (0);
	int error = engine_convert_bp(c, bp);
	/* A full or starved batch ends this pass; the caller fences first. */
	if (error == 0 && ((c->batch != 0 &&
	    c->pending + c->group_asize >= c->batch) || c->starved))
		error = ERESTART;
	/* A slice holds the open txg: near its time it yields here. */
	hrtime_t drain = engine_convert_drain_estimate(c);
	if (error == 0 && gethrtime() + MIN(drain, c->slice_ns / 2) >=
	    c->slice_end) {
		c->yielded = B_TRUE;
		error = ERESTART;
	}
	return (error);
}

/*
 * Longest one slice of the sweep holds the open txg.  The hold keeps any
 * free from landing under the walk; a sweep that held it throughout, as
 * a full walk costs about a scrub, would stop the pool's txgs (and every
 * sync task, and spa_namespace_lock behind spa_async) for that long.
 */
static uint64_t engine_convert_slice_ms = 1000;

/*
 * One slice of a sweep: walk from the cursor until the visitor stops
 * (ERESTART, the bookmark is in c->resume) or the sweep reaches its end
 * (0).  Each slice walks a fresh root under its own hold, so a stop and
 * a resume never read a block freed after the root it started from.
 */
static int
engine_convert_slice(engine_convert_t *c)
{
	dsl_pool_t *dp = spa_get_dsl(c->spa);
	objset_t *mos = dp->dp_meta_objset;
	int flags = TRAVERSE_PRE | TRAVERSE_PREFETCH_METADATA |
	    TRAVERSE_NO_DECRYPT;
	int error = 0;
	if (!c->mos_done) {
		error = pr19262_traverse_mos_resume(c->spa, &c->resume, flags,
		    engine_convert_visit, c);
		if (error != 0)
			return (error);
		c->mos_done = B_TRUE;
		c->cursor = 1;
		memset(&c->resume, 0, sizeof (c->resume));
		c->resume.zb_objset = c->cursor;
	}
	for (; error == 0;
	    error = dmu_object_next(mos, &c->cursor, B_FALSE, 0)) {
		dmu_object_info_t doi;
		dsl_dataset_t *ds;
		/* Objects freed since the last slice are skipped. */
		if (dmu_object_info(mos, c->cursor, &doi) != 0 ||
		    doi.doi_bonus_type != DMU_OT_DSL_DATASET)
			continue;
		dsl_pool_config_enter(dp, FTAG);
		error = dsl_dataset_hold_obj(dp, c->cursor, FTAG, &ds);
		dsl_pool_config_exit(dp, FTAG);
		if (error == ENOENT) {
			error = 0;
			continue;
		}
		if (error != 0)
			return (error);
		if (c->resume.zb_objset != c->cursor) {
			memset(&c->resume, 0, sizeof (c->resume));
			c->resume.zb_objset = c->cursor;
		}
		/* Like traverse_pool: each block in the dataset born in it. */
		error = traverse_dataset_resume(ds,
		    dsl_dataset_phys(ds)->ds_prev_snap_txg, &c->resume, flags,
		    engine_convert_visit, c);
		dsl_dataset_rele(ds, FTAG);
		if (error != 0)
			return (error);
		memset(&c->resume, 0, sizeof (c->resume));
	}
	return (error == ESRCH ? 0 : error);
}

static int
engine_convert_zil_block(zilog_t *zilog, const blkptr_t *bp, void *arg,
    uint64_t txg)
{
	(void) zilog, (void) txg;
	engine_convert_t *c = arg;
	if (BP_IS_HOLE(bp))
		return (0);
	c->stats->zil_blocks++;
	c->zil_chain = B_TRUE;
	int error = engine_convert_bp(c, bp);
	c->zil_chain = B_FALSE;
	return (error);
}

static int
engine_convert_zil_record(zilog_t *zilog, const lr_t *lrc, void *arg,
    uint64_t txg)
{
	(void) zilog, (void) txg;
	engine_convert_t *c = arg;
	/* Only an indirect write (record without data) carries a BP. */
	if (lrc->lrc_txtype != TX_WRITE ||
	    lrc->lrc_reclen != sizeof (lr_write_t))
		return (0);
	const lr_write_t *lr = (const lr_write_t *)lrc;
	if (BP_IS_HOLE(&lr->lr_blkptr))
		return (0);
	c->stats->zil_blocks++;
	return (engine_convert_bp(c, &lr->lr_blkptr));
}

/* Every log chain the traversal does not visit in a writable pool. */
static int
engine_convert_zil(engine_convert_t *c)
{
	spa_t *spa = c->spa;
	dsl_pool_t *dp = spa->spa_dsl_pool;
	objset_t *mos = dp->dp_meta_objset;
	int error = 0;
	for (uint64_t obj = 1; error == 0;
	    error = dmu_object_next(mos, &obj, B_FALSE, 0)) {
		dmu_object_info_t doi;
		error = dmu_object_info(mos, obj, &doi);
		if (error != 0)
			break;
		if (doi.doi_bonus_type != DMU_OT_DSL_DATASET)
			continue;
		dsl_dataset_t *ds = NULL;
		objset_t *os = NULL;
		dsl_pool_config_enter(dp, FTAG);
		error = dsl_dataset_hold_obj(dp, obj, FTAG, &ds);
		/* Only a head has a live chain; $ORIGIN has no objset. */
		if (error == 0 && !ds->ds_is_snapshot &&
		    (dp->dp_origin_snap == NULL ||
		    ds->ds_dir != dp->dp_origin_snap->ds_dir))
			error = dmu_objset_from_ds(ds, &os);
		dsl_pool_config_exit(dp, FTAG);
		if (error != 0) {
			if (ds != NULL)
				dsl_dataset_rele(ds, FTAG);
			break;
		}
		if (os == NULL) {
			dsl_dataset_rele(ds, FTAG);
			continue;
		}
		zilog_t *live = dmu_objset_zil(os);
		zil_header_t *zh = (zil_header_t *)live->zl_header;
		zilog_t *zilog = zil_alloc(mos, zh);
		error = zil_parse(zilog, engine_convert_zil_block,
		    engine_convert_zil_record, c, zh->zh_claim_txg, B_FALSE);
		zil_free(zilog);
		dsl_dataset_rele(ds, FTAG);
	}
	return (error == ESRCH ? 0 : error);
}

/*
 * Sweep every allocation of the engine's vdev that a persistent root or
 * log chain references and move each backing weaker than the layout of
 * birth.  The sweep walks in slices; each holds the open txg so that no
 * block is freed under its walk, for at most engine_convert_slice_ms, and
 * the next resumes on a fresh root where it stopped.  A pass converts at
 * most batch bytes, then fences so that the old backing returns before
 * the next pass, which resumes the sweep.  Returns when a full sweep finds
 * nothing.
 */
int
pr19262_engine_convert(spa_t *spa, uint64_t birth, uint64_t batch,
    uint64_t max_passes, pr19262_engine_convert_stats_t *stats)
{
	memset(stats, 0, sizeof (*stats));
	if (!pr19262_engine_active(spa) || !spa_writeable(spa))
		return (ENOTSUP);
	if (batch != 0 && !pr19262_engine_persistent(spa))
		return (EINVAL);
	engine_convert_t c = { .spa = spa, .birth = birth, .batch = batch,
	    .stats = stats };
	/* A block's DVAs join one group: room for all past the cap. */
	c.group_max = MAX(1, MIN(zfs_pr19262_convert_group, 1024));
	uint_t group_capacity = c.group_max + SPA_DVAS_PER_BP;
	vdev_raidz_layout_for_birth(engine.vd, birth, NULL, &c.parity);
	dsl_pool_t *dp = spa->spa_dsl_pool;
	/*
	 * The standing reserve already holds the growth of every weaker
	 * backing and one batch; refuse to start when the pool is so full
	 * that even the reserve is missing, rather than fail half way.
	 */
	pr19262_engine_plan(spa, c.parity, batch);
	pr19262_engine_admit_report_t admit;
	pr19262_engine_admit_report(spa, "convert-start", &admit);
	/* The reserve is the converter's to spend; one batch must fit now. */
	uint64_t needed = admit.sync_bytes + admit.log_bytes + admit.batch +
	    admit.growth;
	if (admit.free < needed) {
		pr19262_printf("PR19262_CONVERT_SPACE free=%llu needed=%llu "
		    "reserve=%llu growth=%llu\n", (u_longlong_t)admit.free,
		    (u_longlong_t)needed, (u_longlong_t)admit.reserve,
		    (u_longlong_t)admit.growth);
		return (ENOSPC);
	}
	c.group = kmem_alloc(group_capacity * sizeof (*c.group), KM_SLEEP);
	c.group_holds = kmem_alloc(group_capacity * sizeof (*c.group_holds),
	    KM_SLEEP);
	int error = 0;
	/* A sweep spans passes: it resumes where a full batch stopped it. */
	uint64_t sweep_copied = 0, sweep_busy = 0;
	for (uint64_t pass = 0; pass < max_passes; pass++) {
		stats->passes++;
		uint64_t copied = stats->copied;
		c.starved = B_FALSE;
		c.pending = 0;
		boolean_t partial = B_FALSE, swept = B_FALSE;
		for (boolean_t first = B_TRUE; error == 0 && !partial &&
		    !swept; first = B_FALSE) {
			/*
			 * A pass starts on a fresh sync; the slices after its
			 * first only wait for the txg below their own hold,
			 * which, while nobody kicked a sync, is no wait.
			 */
			if (first)
				txg_wait_synced(dp, 0);
			/* Hold the open txg: nothing is freed under it. */
			dmu_tx_t *fence = dmu_tx_create_dd(dp->dp_mos_dir);
			VERIFY0(dmu_tx_assign(fence, DMU_TX_WAIT));
			hrtime_t held = gethrtime();
			/*
			 * The txg before the held one may still be quiescing
			 * or syncing, and its sync frees blocks that the last
			 * synced root (the walk's root) references.  Wait for
			 * every txg below the held one, so that the root is
			 * the held txg's predecessor and no free can land
			 * under the walk.
			 */
			txg_wait_synced(dp, dmu_tx_get_txg(fence) - 1);
			c.yielded = B_FALSE;
			c.slice_ns = MSEC2NSEC(engine_convert_slice_ms);
			c.slice_end = gethrtime() + c.slice_ns;
			error = engine_convert_slice(&c);
			/* The slice's copies finish under its hold. */
			int ran = engine_convert_drain(&c);
			if (error == ERESTART) {
				error = 0;
				partial = !c.yielded;
			} else if (error == 0) {
				swept = B_TRUE;
			} else {
				pr19262_printf("PR19262_CONVERT_PASS_ERROR "
				    "step=traverse error=%d pass=%llu\n",
				    error, (u_longlong_t)pass);
			}
			if (error == 0 && ran != 0) {
				pr19262_printf("PR19262_CONVERT_PASS_ERROR "
				    "step=copy error=%d pass=%llu\n",
				    ran, (u_longlong_t)pass);
				error = ran;
			}
			if (swept && error == 0) {
				error = engine_convert_zil(&c);
				ran = engine_convert_drain(&c);
				if (error == 0)
					error = ran;
				if (error != 0)
					pr19262_printf(
					    "PR19262_CONVERT_PASS_ERROR "
					    "step=zil error=%d pass=%llu\n",
					    error, (u_longlong_t)pass);
			}
			dmu_tx_commit(fence);
			hrtime_t now = gethrtime();
			stats->slices++;
			stats->hold_ms_max = MAX(stats->hold_ms_max,
			    NSEC2MSEC(now - held));
			if (now > c.slice_end)
				stats->slice_over_ms_max = MAX(
				    stats->slice_over_ms_max,
				    NSEC2MSEC(now - c.slice_end));
		}
		if (error != 0)
			break;
		if (batch != 0) {
			uint64_t floor, reclaimed, released;
			int e = pr19262_engine_fence(spa, &floor, &reclaimed,
			    &released, stats->sample);
			if (e == 0) {
				stats->fences++;
				stats->reclaimed_bytes += reclaimed;
				stats->released_owners += released;
				stats->floor = floor;
			} else if (e != EBUSY) {
				error = e;
				break;
			}
		}
		/* The pass's copies are catalogued or reclaimed: release. */
		pr19262_engine_admit_release(spa, PR19262_ADMIT_CONVERTER,
		    c.admitted_p, c.admitted_m, c.admitted_j);
		c.admitted_p = c.admitted_m = c.admitted_j = 0;
		if (c.starved && stats->copied == copied) {
			/* No space for one copy, nothing left to reclaim. */
			pr19262_engine_admit_report(spa, "convert-starved",
			    &admit);
			error = ENOSPC;
			break;
		}
		pr19262_printf("PR19262_CONVERT_PASS pass=%llu partial=%u "
		    "visited=%llu copied=%llu busy=%llu stale=%llu "
		    "starved=%llu candidates=%llu zil_blocks=%llu "
		    "unwritten=%llu slices=%llu hold_ms_max=%llu "
		    "slice_over_ms_max=%llu\n",
		    (u_longlong_t)stats->passes, !swept,
		    (u_longlong_t)stats->visited, (u_longlong_t)stats->copied,
		    (u_longlong_t)stats->busy, (u_longlong_t)stats->stale,
		    (u_longlong_t)stats->starved,
		    (u_longlong_t)stats->candidates,
		    (u_longlong_t)stats->zil_blocks,
		    (u_longlong_t)stats->unwritten,
		    (u_longlong_t)stats->slices,
		    (u_longlong_t)stats->hold_ms_max,
		    (u_longlong_t)stats->slice_over_ms_max);
		if (!swept)
			continue;
		/* A full sweep that found nothing ends the conversion. */
		if (stats->copied == sweep_copied && stats->busy == sweep_busy)
			break;
		/* Busy across many idle sweeps is a defect, not a wait. */
		if (stats->copied == sweep_copied)
			c.idle_passes++;
		else
			c.idle_passes = 0;
		if (c.idle_passes >= 8) {
			pr19262_printf("PR19262_CONVERT_STUCK passes=%llu "
			    "busy=%llu\n", (u_longlong_t)stats->passes,
			    (u_longlong_t)(stats->busy - sweep_busy));
			error = EBUSY;
			break;
		}
		sweep_copied = stats->copied;
		sweep_busy = stats->busy;
		c.mos_done = B_FALSE;
		memset(&c.resume, 0, sizeof (c.resume));
	}
	if (error != 0)
		pr19262_engine_admit_release(spa, PR19262_ADMIT_CONVERTER,
		    c.admitted_p, c.admitted_m, c.admitted_j);
	if (c.gang != NULL)
		kmem_free(c.gang, c.gang_capacity * sizeof (*c.gang));
	VERIFY0(c.group_count);
	kmem_free(c.group_holds, group_capacity * sizeof (*c.group_holds));
	kmem_free(c.group, group_capacity * sizeof (*c.group));
	pr19262_engine_admit_report(spa, "convert-end", &admit);
	return (error);
}

/*
 * Append a full-width epoch of the given parity to the target vdev through
 * a sync task (open-context check, syncing-context check and store), then
 * wait until the epoch's first txg is synced, so that every later write
 * is laid out by it.  Idempotent for the last epoch of that parity.
 */
#define	ENGINE_EPOCH_ENTRIES 48

typedef struct engine_epoch_task {
	uint64_t vdev;
	uint64_t parity;
	uint64_t birth;
	uint64_t nent;
	uint64_t table[3 * (ENGINE_EPOCH_ENTRIES + 1)];
} engine_epoch_task_t;

static uint64_t
engine_epoch_table(vdev_t *vd, uint64_t parity, uint64_t start,
    uint64_t *table)
{
	vdev_raidz_t *vdrz = vd->vdev_tsd;
	mutex_enter(&vdrz->vd_expand_lock);
	uint64_t n = vdrz->vd_parity_epoch_count;
	if (n > ENGINE_EPOCH_ENTRIES)
		n = 0;
	if (n != 0)
		memcpy(table, vdrz->vd_parity_epochs,
		    n * 3 * sizeof (uint64_t));
	mutex_exit(&vdrz->vd_expand_lock);
	if (n == 0) {
		table[0] = 0;
		table[1] = vdrz->vd_physical_width;
		table[2] = vdrz->vd_nparity;
		n = 1;
	}
	table[3 * n] = MAX(start, table[3 * (n - 1)] + 1);
	table[3 * n + 1] = vdrz->vd_physical_width;
	table[3 * n + 2] = parity;
	return (n + 1);
}

static int
engine_epoch_check(void *arg, dmu_tx_t *tx)
{
	engine_epoch_task_t *t = arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	boolean_t lock = !dmu_tx_is_syncing(tx);
	if (lock && !spa_config_tryenter(spa, SCL_CONFIG, FTAG, RW_READER))
		return (SET_ERROR(EBUSY));
	vdev_t *vd = vdev_lookup_top(spa, t->vdev);
	int error;
	if (vd == NULL || vd->vdev_ops != &vdev_raidz_ops) {
		error = SET_ERROR(ENOENT);
	} else if (lock) {
		uint64_t n = engine_epoch_table(vd, t->parity, 0, t->table);
		error = vdev_raidz_parity_epochs_set_check(vd, t->table, n,
		    NULL);
	} else {
		uint64_t n = engine_epoch_table(vd, t->parity,
		    dmu_tx_get_txg(tx) + TXG_CONCURRENT_STATES, t->table);
		error = vdev_raidz_parity_epochs_set_check_tx(vd, t->table, n,
		    tx, NULL);
	}
	if (lock)
		spa_config_exit(spa, SCL_CONFIG, FTAG);
	return (error);
}

static void
engine_epoch_sync(void *arg, dmu_tx_t *tx)
{
	engine_epoch_task_t *t = arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	vdev_t *vd = vdev_lookup_top(spa, t->vdev);
	t->nent = engine_epoch_table(vd, t->parity,
	    dmu_tx_get_txg(tx) + TXG_CONCURRENT_STATES, t->table);
	VERIFY0(vdev_raidz_parity_epochs_set(vd, t->table, t->nent, tx));
	t->birth = t->table[3 * (t->nent - 1)];
	spa_history_log_internal(spa, "pr19262 epoch", tx,
	    "vdev=%llu parity=%llu birth=%llu", (u_longlong_t)t->vdev,
	    (u_longlong_t)t->parity, (u_longlong_t)t->birth);
}

int
pr19262_engine_epoch(spa_t *spa, uint64_t parity, uint64_t *birth)
{
	vdev_t *vd = pr19262_engine_vdev(spa);
	if (vd == NULL || parity < 1 || parity > VDEV_RAIDZ_MAXPARITY ||
	    vd->vdev_children < parity + 1)
		return (SET_ERROR(EINVAL));
	vdev_raidz_t *vdrz = vd->vdev_tsd;
	mutex_enter(&vdrz->vd_expand_lock);
	uint64_t n = vdrz->vd_parity_epoch_count;
	const uint64_t *last = n != 0 ?
	    vdrz->vd_parity_epochs + 3 * (n - 1) : NULL;
	uint64_t last_start = last != NULL ? last[0] : 0;
	uint64_t last_parity = last != NULL ? last[2] : vdrz->vd_nparity;
	mutex_exit(&vdrz->vd_expand_lock);
	if (last_parity == parity &&
	    last_start <= spa_last_synced_txg(spa) + 1) {
		*birth = last_start;
		pr19262_engine_plan(spa, parity, 0);
		return (0);
	}
	engine_epoch_task_t *t = kmem_zalloc(sizeof (*t), KM_SLEEP);
	t->vdev = vd->vdev_id;
	t->parity = parity;
	int error = dsl_sync_task(spa_name(spa), engine_epoch_check,
	    engine_epoch_sync, t, 5, ZFS_SPACE_CHECK_NORMAL);
	if (error == 0) {
		*birth = t->birth;
		while (spa_last_synced_txg(spa) + 1 < t->birth)
			txg_wait_synced(spa_get_dsl(spa),
			    spa_last_synced_txg(spa) + 1);
		txg_wait_synced(spa_get_dsl(spa), 0);
		pr19262_engine_plan(spa, parity, 0);
	}
	kmem_free(t, sizeof (*t));
	return (error);
}

#include "vdev_reloc_kernel.c"
