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
/*
 * Private whole-vdev converter, census, durable recovery floor and
 * certified parity, included by the ownership engine after its raw store
 * and journal.  Every copy keeps the persistent BP unchanged and moves
 * only the physical backing to the layout of the requested birth.
 */
#include <sys/dmu_objset.h>
#include <sys/dmu_traverse.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_synctask.h>
#include <sys/zil.h>
#include <sys/zil_impl.h>

static void
engine_version_unlink(engine_t *engine, engine_owner_t *e, engine_version_t *b)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_owner_dirty(engine, e);
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
	engine_t *engine = spa->spa_reloc;
	memset(c, 0, sizeof (*c));
	c->min_parity = UINT64_MAX;
	mutex_enter(&engine->lock);
	VERIFY3P(spa, ==, engine->spa);
	for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
	    e = AVL_NEXT(&engine->owners, e)) {
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
	c->legacy_bytes = zfs_range_tree_space(engine->legacy);
	c->legacy_segments = zfs_range_tree_numsegs(engine->legacy);
	c->unusable_bytes = zfs_range_tree_space(engine->unusable);
	c->free_bytes = zfs_range_tree_space(engine->free);
	if (engine->store_extents != NULL)
		c->store_bytes = zfs_range_tree_space(engine->store_extents);
	if (engine->journal_extents != NULL)
		c->journal_bytes =
		    zfs_range_tree_space(engine->journal_extents);
	for (engine_j_segment_t *s = engine->journal_segments; s != NULL;
	    s = s->next)
		c->journal_segments++;
	for (engine_j_segment_t *s = engine->journal_retired; s != NULL;
	    s = s->next)
		c->journal_retired++;
	c->journal_freed_bytes = engine->journal_freed_bytes;
	c->tombstones = avl_numnodes(&engine->tombstones);
	c->floor = engine->pages->floor;
	c->certified = engine->certified;
	c->durable_txg = engine->durable_ub_txg;
	c->journal_seq = engine->journal_seq;
	c->pages_capacity = engine->pages->capacity;
	for (uint64_t i = 0; i < engine->pages->capacity; i++)
		c->pages_used += engine->pages->bits[i] != 0;
	mutex_exit(&engine->lock);
}

uint64_t
pr19262_engine_owner_incarnation(spa_t *spa, uint64_t v, uint64_t size)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_owner_t *e = engine->spa == spa ? engine_overlap(engine, v,
	    size) : NULL;
	uint64_t incarnation = e == NULL ? 0 : e->incarnation;
	mutex_exit(&engine->lock);
	return (incarnation);
}

uint64_t
pr19262_engine_floor(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	uint64_t floor = engine->spa == spa && engine->persistent ?
	    engine->pages->floor : 0;
	mutex_exit(&engine->lock);
	return (floor);
}

/*
 * A finally freed owner may release both its native V and its physical
 * backing once the catalog that lists it retired is at or below the floor
 * and nothing references it any more.
 */
static boolean_t
engine_owner_releasable(engine_t *engine, engine_owner_t *e, uint64_t catalog)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (!e->retired || e->aborted || e->checkpoint_only || e->copying ||
	    e->releasing || e->pending != NULL || e->holds != 0 ||
	    e->queued_writers != 0 || e->journal_pending ||
	    e->retired_txg == 0 || e->retired_txg > catalog)
		return (B_FALSE);
#ifdef _KERNEL
	/* R-01 dereferences its selected owner until the root drains. */
	if (engine->r01.owner == e)
		return (B_FALSE);
#endif
	for (engine_version_t *b = e->versions; b != NULL; b = b->next)
		if (b->leases != 0 || b->writers != 0)
			return (B_FALSE);
	return (B_TRUE);
}

/*
 * L21: what a guarded checkpoint root C keeps from every fence (after =
 * MAX(C, this session's import)).  The version reclaim keeps a retired
 * version C's catalog or journal chain may name; the owner release keeps
 * an owner allocated at or before after, or named by a journal record C's
 * chain may replay.  The fence and the release test these, and the
 * retired-limit census counts by them, so the two cannot drift apart.
 */
static boolean_t
engine_ckpt_keeps_version(const engine_version_t *b, uint64_t after)
{
	return (b->birth <= after || b->journal_seq != 0);
}

static boolean_t
engine_ckpt_keeps_owner(const engine_owner_t *e, uint64_t after)
{
	return (e->allocation_txg <= after || e->journal_touched);
}

/*
 * A fence's release sync task.  The fence allocates it; a fence that
 * stopped waiting for its txg (a suspended pool) leaves it to the sync
 * task, which frees it when it runs.  Both flags are under engine->lock.
 */
typedef struct engine_release {
	uint64_t catalog;
	uint64_t owners;
	uint64_t bytes;
	/*
	 * L13: the fence's round, whether the bulk pot stopped it, and the
	 * run of adjacent catalog rows its last release deleted.
	 */
	uint64_t round;
	boolean_t force;
	boolean_t cut;
	boolean_t run;
	uint64_t run_v;
	uint64_t run_len;
	boolean_t done;
	boolean_t abandoned;
} engine_release_t;

static uint64_t engine_bulk_pot_locked(spa_t *, engine_t *);
static boolean_t engine_ckres_active_locked(engine_t *);
static uint64_t engine_ckres_held_locked(spa_t *, engine_t *);
static boolean_t engine_ckres_release_take_locked(spa_t *, engine_t *,
    uint64_t, boolean_t);
static uint64_t engine_bulk_path_locked(engine_t *);
static boolean_t engine_bulk_take_locked(spa_t *, engine_t *, uint64_t,
    boolean_t, boolean_t);

/*
 * L13: does releasing e extend the run of rows this release task deleted
 * last (ending at run_v)?  Rows sort by V, then incarnation.  Deleting a
 * run of adjacent rows frees the leaves and branches it empties within the
 * batch and copies only the leaf (and its parent branch) at each end, so
 * a run is charged at its first two owners only.  Adjacent: e above
 * run_v, no owner and no tombstone row left between; legacy version
 * tombstones are not in memory, so with any of them nothing is.
 */
static boolean_t
engine_release_extends_run_locked(engine_t *engine, uint64_t run_v,
    engine_owner_t *e)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->pages->legacy_dead != 0 || e->v <= run_v)
		return (B_FALSE);
	engine_owner_t *prev = AVL_PREV(&engine->owners, e);
	if (prev != NULL && prev->v >= run_v)
		return (B_FALSE);
	engine_tomb_t key = { .v = run_v, .incarnation = UINT64_MAX };
	avl_index_t where;
	engine_tomb_t *t = avl_find(&engine->tombstones, &key, &where);
	if (t == NULL)
		t = avl_nearest(&engine->tombstones, where, AVL_AFTER);
	return (t == NULL || t->v > e->v);
}

/* Re-base the growth trigger on current catalog use and retired bytes. */
static void
engine_fence_growth_mark_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine->fence_growth_pages_mark = engine->pages->capacity -
	    engine->pages->available;
	engine->fence_growth_retired_mark = engine->retired;
	engine->fence_growth_seeded = B_TRUE;
	engine->free_deferrals_mark = engine->free_deferrals;
	engine->bulk_waits_mark = engine->bulk_waits;
}

#ifdef _KERNEL
/*
 * While final-free is armed, the version R-01 still references (its
 * selected P, whose pointer R-01 keeps until drain) may be unlinked only by
 * the one R-01 final fence -- the converter's second fence, or the copy
 * worker's fence, which claims final_fence_thread first.  Any
 * other fence -- a converter pass, or one the admission or the growth
 * trigger scheduled -- retains it even after its leases dropped, so a
 * growth fence during a held boundary reclaims every other retired
 * version and never this one.
 */
static boolean_t
engine_r01_fence_retains_locked(engine_t *engine,
    const engine_version_t *version)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_r01_t *r = &engine->r01;
	return (r->boundary == PR19262_R01_FINAL_FREE &&
	    r->version == version && r->final_fence_thread != curthread);
}

/* Caller holds engine->lock; record only the selected P, not an aggregate. */
static void
engine_r01_final_p_reclaimed_locked(engine_t *engine, engine_owner_t *owner,
    const engine_version_t *version)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_r01_t *r = &engine->r01;
	/* Only the final fence's own reclaim is the R-01 final-free result. */
	if (r->final_fence_thread != curthread)
		return;
	if (r->boundary != PR19262_R01_FINAL_FREE || r->p == 0 ||
	    r->p != version->birth || r->p_offset != version->p ||
	    r->p_size != version->size || r->v != owner->v ||
	    r->incarnation != owner->incarnation)
		return;
	VERIFY0(r->final_p_reclaims);
	r->final_p_reclaims = 1;
	r->final_p_bytes = version->size;
	r->final_p_birth = version->birth;
	r->final_p_offset = version->p;
	r->final_p_size = version->size;
	r->final_p_v = owner->v;
	r->final_p_incarnation = owner->incarnation;
}
#endif

/*
 * L14 (L11 r2(a)): drain_releasable is RAM only, so owners a discard step
 * or an ordinary free retired before this load start without it, and while
 * the discard drains they would wait for its end, holding the retired bytes
 * clients are measured against.  Once per load, in the first release of a
 * draining discard, read the remaining checkpoint space map of the engine's
 * vdev and flag every retired owner whose V it does not name: what the
 * flag means (no entry can still split that V).  Syncing context: the
 * discard steps change the space map only in syncing context, so it is
 * stable here.  A read error leaves the flags as they are (the owners wait
 * for the drain, as before) and is retried at the next release.
 */
/* Every range a checkpoint space map entry still names (all SM_FREE). */
static int
engine_release_drain_named(space_map_entry_t *sme, void *arg)
{
	zfs_range_tree_t *named = arg;
	zfs_range_tree_clear(named, sme->sme_offset, sme->sme_run);
	zfs_range_tree_add(named, sme->sme_offset, sme->sme_run);
	return (0);
}

static void
engine_release_drain_flags(spa_t *spa, engine_t *engine)
{
	mutex_enter(&engine->lock);
	boolean_t needed = engine->spa == spa && engine->persistent &&
	    !engine->ckpt_drain_flags_loaded &&
	    engine->checkpoint.ub_txg == 0 &&
	    spa_feature_is_active(spa, SPA_FEATURE_POOL_CHECKPOINT);
	vdev_t *vd = engine->vd;
	mutex_exit(&engine->lock);
	if (!needed)
		return;
	zfs_range_tree_t *named = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	space_map_t *sm = vd->vdev_checkpoint_sm;
	int error = sm == NULL ? 0 : space_map_iterate(sm,
	    space_map_length(sm), engine_release_drain_named, named);
	uint64_t flagged = 0, bytes = 0;
	mutex_enter(&engine->lock);
	if (error == 0) {
		for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
		    e = AVL_NEXT(&engine->owners, e)) {
			uint64_t found, length;
			if (!e->retired || e->drain_releasable ||
			    zfs_range_tree_find_in(named, e->v, e->size,
			    &found, &length))
				continue;
			e->drain_releasable = B_TRUE;
			flagged++;
			bytes += e->cur->size;
		}
		engine->ckpt_drain_flags_loaded = B_TRUE;
		engine->ckpt_drain_flagged = flagged;
		/*
		 * v8 fixup (m1): the owners just flagged leave L21's pinned
		 * count (they are releasable now); count again.
		 */
		if (flagged != 0)
			engine->retired_pinned_valid = B_FALSE;
	}
	mutex_exit(&engine->lock);
	uint64_t entries = zfs_range_tree_numsegs(named);
	zfs_range_tree_vacate(named, NULL, NULL);
	zfs_range_tree_destroy(named);
	pr19262_printf("PR19262_DRAIN_FLAGS error=%d sm_segments=%llu "
	    "flagged=%llu bytes=%llu\n", error, (u_longlong_t)entries,
	    (u_longlong_t)flagged, (u_longlong_t)bytes);
}

static void
engine_release_sync(void *arg, dmu_tx_t *tx)
{
	engine_t *engine = dmu_tx_pool(tx)->dp_spa->spa_reloc;
	engine_release_t *r = arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	engine_release_drain_flags(spa, engine);
	for (;;) {
		mutex_enter(&engine->lock);
		engine_owner_t *e = NULL;
		/*
		 * A native checkpoint SM that is still draining may hold a
		 * split run of a V: release no V until it is done.  The
		 * feature stays active until the drain completes, also across
		 * an import (sci_timestamp does not).  While a checkpoint root
		 * C is guarded, release only owners allocated after C and after
		 * this session's import whose journal records C's chain does
		 * not replay.
		 */
		const uberblock_t *cp = engine->spa == spa ?
		    engine_ckpt_guard_locked(engine, engine->pages->floor) :
		    NULL;
		uint64_t after = cp == NULL ? 0 :
		    MAX(cp->ub_txg, engine->ckpt_session_txg);
		boolean_t draining = engine->checkpoint.ub_txg == 0 &&
		    spa_feature_is_active(spa, SPA_FEATURE_POOL_CHECKPOINT);
		/* While draining, only V no entry still names (L11). */
		if (engine->spa == spa && engine->persistent) {
			for (e = avl_first(&engine->owners); e != NULL;
			    e = AVL_NEXT(&engine->owners, e))
				if ((!draining || e->drain_releasable) &&
				    engine_owner_releasable(engine, e,
				    r->catalog) && (cp == NULL ||
				    !engine_ckpt_keeps_owner(e, after)))
					break;
		}
		/*
		 * L13: the catalog pages this release may need must fit
		 * this txg's bulk pot: a run of deleted rows copies the leaf
		 * and its parent branch at each end (two pages at its first
		 * owner, two at its second, none after), plus the txg's one
		 * root-to-leaf path.  The first owner of a fence always goes,
		 * so a full catalog still drains; the rest waits for the next
		 * round or fence.
		 */
		boolean_t extends = B_FALSE;
		if (e != NULL) {
			/* Refresh first: the path is charged once per txg. */
			(void) engine_bulk_pot_locked(spa, engine);
			extends = r->run &&
			    engine_release_extends_run_locked(engine, r->run_v,
			    e);
			uint64_t cost = extends && r->run_len > 1 ? 0 : 2;
			if (!engine_ckres_release_take_locked(spa, engine,
			    cost + engine_bulk_path_locked(engine),
			    r->force && r->owners == 0) &&
			    !engine_bulk_take_locked(spa, engine,
			    cost + engine_bulk_path_locked(engine),
			    r->force && r->owners == 0, B_FALSE)) {
				r->cut = B_TRUE;
				engine->bulk_release_cuts++;
				e = NULL;
			} else {
				engine->bulk_pathed = B_TRUE;
			}
		}
		if (e == NULL) {
			boolean_t abandoned = r->abandoned;
			r->done = B_TRUE;
			mutex_exit(&engine->lock);
			if (abandoned)
				kmem_free(r, sizeof (*r));
			return;
		}
		e->releasing = B_TRUE;
		uint64_t v = e->v, size = e->size;
		/* A journaled owner's row stays as a tombstone: no run. */
		r->run = !e->journal_touched;
		r->run_v = v;
		r->run_len = extends ? r->run_len + 1 : 1;
		engine->release_thread = curthread;
		mutex_exit(&engine->lock);
		/* The native V returns to this syncing txg's space map. */
		metaslab_free_concrete(engine->vd, v, size, B_FALSE);
		mutex_enter(&engine->lock);
		engine->release_thread = NULL;
		VERIFY3P(engine_find(engine, v), ==, e);
		while (e->versions != NULL) {
			engine_version_t *b = e->versions;
			e->versions = b->next;
			VERIFY0(b->leases);
			/* Catalogs up to this txg's store list the owner. */
			engine_reuse_add(engine, b->p, b->size);
			if (b->retired || b == e->cur) {
				VERIFY3U(engine->retired, >=, b->size);
				engine->retired -= b->size;
			}
			r->bytes += b->size;
			kmem_free(b, sizeof (*b));
		}
		engine_owner_gone(engine, e);
		avl_remove(&engine->owners, e);
		/* Journal records of this identity may still be replayed. */
		if (e->journal_touched)
			engine_tombstone_add(engine, v, e->incarnation,
			    engine->journal_tail != NULL ?
			    engine->journal_tail->index : 0);
		kmem_free(e, sizeof (*e));
		r->owners++;
		engine->revision++;
		mutex_exit(&engine->lock);
	}
}

/*
 * One owner release sync task in a txg of its own, and the wait for it:
 * a fence's only one before L13, its rounds since.  The export thread
 * waits for us: do not reopen the SPA by name.  A wait that stopped on a
 * suspension leaves the task to run after resume, which frees r; the
 * release may then stop early, and release_carry has the next fence
 * finish it.
 */
static int
engine_release_round(spa_t *spa, uint64_t catalog, uint64_t round,
    boolean_t force, boolean_t suspend, uint64_t floor, uint64_t reclaimed,
    engine_release_t *out)
{
	engine_t *engine = spa->spa_reloc;
	txg_wait_flag_t wflags = suspend ? TXG_WAIT_SUSPEND : TXG_WAIT_NONE;
	dsl_pool_t *dp = spa_get_dsl(spa);
	dmu_tx_t *tx;
	int error;
	for (;;) {
		tx = dmu_tx_create_dd(dp->dp_mos_dir);
		/*
		 * NOTHROTTLE: no dirty-data delay, and this tx holds nothing,
		 * so a NOWAIT assign fails only on a suspension.  A resume
		 * racing the check below waits one sync and retries.
		 */
		error = dmu_tx_assign(tx, suspend ?
		    DMU_TX_NOWAIT | DMU_TX_NOTHROTTLE :
		    DMU_TX_WAIT | DMU_TX_SUSPEND);
		if (error == 0)
			break;
		/*
		 * failmode=continue answers a suspension with EIO, and this
		 * tx has no other EIO source.
		 */
		if (suspend && (error == EIO ||
		    (error == ERESTART && spa_suspended(spa)))) {
			dmu_tx_abort(tx);
			return (SET_ERROR(ESHUTDOWN));
		}
		if (suspend && error == ERESTART) {
			dmu_tx_wait(tx);
			dmu_tx_abort(tx);
			continue;
		}
		dmu_tx_abort(tx);
		return (error);
	}
	engine_release_t *r = kmem_zalloc(sizeof (*r), KM_SLEEP);
	r->catalog = catalog;
	r->round = round;
	r->force = force;
	uint64_t txg = dmu_tx_get_txg(tx);
	dsl_sync_task_nowait(dp, engine_release_sync, r, tx);
	dmu_tx_commit(tx);
	int werr = txg_wait_synced_flags(dp, txg, wflags);
	mutex_enter(&engine->lock);
	if (werr != 0 && !r->done) {
		/* The sync task runs after resume and frees r. */
		r->abandoned = B_TRUE;
		engine->release_carry = B_TRUE;
		mutex_exit(&engine->lock);
		pr19262_printf("PR19262_FENCE_SUSPENDED floor=%llu "
		    "catalog=%llu reclaimed=%llu txg=%llu error=%d\n",
		    (u_longlong_t)floor, (u_longlong_t)catalog,
		    (u_longlong_t)reclaimed, (u_longlong_t)txg, werr);
		return (werr);
	}
	VERIFY(r->done);
	*out = *r;
	engine->bulk_release_rounds += round != 0;
	if (r->cut && engine->bulk_prints++ < 16)
		pr19262_printf("PR19262_BULK_PACE kind=release txg=%llu "
		    "round=%llu released=%llu available=%llu\n",
		    (u_longlong_t)txg, (u_longlong_t)round,
		    (u_longlong_t)r->owners,
		    (u_longlong_t)engine->pages->available);
	mutex_exit(&engine->lock);
	kmem_free(r, sizeof (*r));
	return (0);
}

/*
 * Advance the durable recovery floor to the latest written uberblock.
 * Every root below it is refused by the importer, so the backing of each
 * version that the floor root's catalog already lists as retired behind a
 * newer binding returns to the physical allocator, and finally freed
 * owners release their V and P from a sync task.
 *
 * A pool checkpoint root C stays importable although the floor passes it
 * (checkpoint-exempt floor): the catalog pages C reaches stay pinned; a
 * version is reclaimed only if born after both C and this session's
 * import, an owner released only if allocated after both, neither if a
 * journal record C's chain replays names it (in-session flags); no cut
 * journal segment is freed.  A discard keeps these guards until a floor
 * at the first root that no longer names C.  A failed walk of C's pages
 * refuses (EBUSY).
 *
 * suspend: return ESHUTDOWN instead of waiting while the pool is
 * suspended.  Only a scheduled fence (the fence task) sets it; a direct
 * fence -- the converter's, R-01's final fence, the sysfs fence -- waits
 * for resume as before.  Before the floor moves nothing has changed.
 * Once it moved, the floor root, the in-memory reclaim and the journal
 * collection are complete; a fence that then stops waiting leaves the
 * owner release to its queued sync task (or, if none was queued, to the
 * next fence) and reports no release.  Raw catalog I/O between the two
 * waits is not suspension-aware: a suspension there holds the fence in
 * zio_wait until resume, as before.
 *
 * catalogp (L15, may be NULL): the durable catalog txg the fence reclaimed
 * and released against, 0 if it failed before taking it.
 */
static int
engine_fence_impl(spa_t *spa, uint64_t *floor, uint64_t *reclaimed,
    uint64_t *released, uint64_t *sample, boolean_t suspend,
    uint64_t *catalogp)
{
	engine_t *engine = spa->spa_reloc;
	txg_wait_flag_t wflags = suspend ? TXG_WAIT_SUSPEND : TXG_WAIT_NONE;
	*floor = *reclaimed = *released = 0;
	if (catalogp != NULL)
		*catalogp = 0;
	if (!pr19262_engine_persistent(spa) || !spa_writeable(spa))
		return (ENOTSUP);
	/* The catalog listing the retirements must be on disk first. */
	int werr = txg_wait_synced_flags(spa->spa_dsl_pool, 0, wflags);
	if (werr != 0)
		return (werr);
	/* A copy of the guarded checkpoint root, for its page walk. */
	uberblock_t *pin = kmem_alloc(sizeof (*pin), KM_SLEEP);
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine->store_lock);
	mutex_enter(&engine->lock);
	uint64_t ub = engine->durable_ub_txg;
	uint64_t catalog = engine->durable_catalog_txg;
	/* L15: the catalog this fence reclaims and releases against. */
	if (catalogp != NULL)
		*catalogp = catalog;
	const uberblock_t *cp = engine_ckpt_guard_locked(engine, ub);
	boolean_t guard = cp != NULL;
	boolean_t paged = guard && pr19262_engine_store_ub(cp);
	uint64_t cp_txg = guard ? cp->ub_txg : 0;
	/* Born at or before this session's import: see ckpt_session_txg. */
	uint64_t cp_after = guard ? MAX(cp_txg, engine->ckpt_session_txg) :
	    0;
	if (paged)
		*pin = *cp;
	/* A pool checkpoint the engine has not recorded yet: refuse. */
	boolean_t unknown = !guard && spa->spa_checkpoint_txg != 0;
	mutex_exit(&engine->lock);
	int error = 0;
	uint64_t pages_freed = 0, journal_segments = 0, journal_bytes = 0;
	uint64_t ckpt_kept = 0, pinned = 0;
	if (unknown)
		error = SET_ERROR(EBUSY);
	else if (ub == 0 || ub < engine->pages->floor || catalog > ub)
		error = ENGINE_EINVAL(101);
	else if (ub != engine->pages->floor || engine->pages->gc_relaxed) {
		/* C's catalog pages stay with the floor past C. */
		if (paged)
			error = engine_ckpt_pins_build(spa, pin);
		if (paged && error == 0)
			pinned = engine->ckpt_pinned_pages;
		/* Dead catalog pages go with the floor into both slots. */
		if (error == 0)
			error = engine_pages_gc(spa, &pages_freed,
			    paged ? engine->ckpt_pins : NULL);
		if (error == 0)
			error = engine_pages_publish_control(spa, ub,
			    engine->pages->certified);
	}
	if (error == 0 && !guard) {
		/* No root at or above this floor names a checkpoint. */
		mutex_enter(&engine->lock);
		if (engine_ckpt_guard_locked(engine, ub) == NULL &&
		    (engine->ckpt_pins != NULL || engine->ckpt_until != 0))
			engine_ckpt_unpin_locked(engine);
		mutex_exit(&engine->lock);
	}
	if (error == 0) {
		mutex_enter(&engine->lock);
		for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
		    e = AVL_NEXT(&engine->owners, e)) {
			engine_version_t *b = e->versions;
			while (b != NULL) {
				engine_version_t *next = b->next;
				boolean_t eligible = e->cur != NULL &&
				    b != e->cur && b->retired &&
				    b->generation < e->cur->generation &&
				    b->retired_txg != 0 &&
				    b->retired_txg <= catalog &&
				    b->leases == 0 && b->writers == 0;
#ifdef _KERNEL
				if (eligible &&
				    engine_r01_fence_retains_locked(engine, b))
					eligible = B_FALSE;
#endif
				/* C's catalog or journal chain may name it. */
				if (eligible && guard &&
				    engine_ckpt_keeps_version(b, cp_after)) {
					eligible = B_FALSE;
					ckpt_kept++;
				}
				if (eligible) {
					engine_version_unlink(engine, e, b);
					/* The durable catalog lists it. */
					engine_reuse_add(engine, b->p,
					    b->size);
					VERIFY3U(engine->retired, >=, b->size);
					engine->retired -= b->size;
					if (sample != NULL && sample[1] == 0) {
						sample[0] = b->p;
						sample[1] = b->size;
					}
#ifdef _KERNEL
					engine_r01_final_p_reclaimed_locked(
					    engine, e, b);
#endif
					*reclaimed += b->size;
					engine->revision++;
					kmem_free(b, sizeof (*b));
				}
				b = next;
			}
		}
		*floor = engine->pages->floor;
		/* Every fence re-bases the growth trigger on what it left. */
		engine_fence_growth_mark_locked(engine);
		mutex_exit(&engine->lock);
		/*
		 * Journal segments folded into this floor root's catalog.  A
		 * paged C keeps every cut segment: one cut after C's catalog
		 * is reached from C's root, and C's catalog quarantines the
		 * ones cut before it, so C's import reserves them again.
		 */
		engine_journal_gc(spa, catalog, paged ? 0 : catalog,
		    &journal_segments, &journal_bytes);
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
	mutex_exit(&engine->store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	kmem_free(pin, sizeof (*pin));
	if (error != 0)
		return (error);
	/*
	 * L13: a release the bulk pot cut continues in the next txg while
	 * its round released more than its forced first owner; every round
	 * releases only owners retired at or below this fence's catalog.
	 */
	engine_release_t rr = { 0 };
	boolean_t carry = B_FALSE;
	for (uint64_t round = 0; ; round++) {
		engine_release_t one;
		error = engine_release_round(spa, catalog, round, round == 0,
		    suspend, *floor, *reclaimed, &one);
		if (error != 0) {
			/* A cut earlier round leaves the rest to a fence. */
			if (carry) {
				mutex_enter(&engine->lock);
				engine->release_carry = B_TRUE;
				mutex_exit(&engine->lock);
			}
			*released = rr.owners;
			return (error);
		}
		rr.owners += one.owners;
		rr.bytes += one.bytes;
		carry = one.cut;
		if (!one.cut || one.owners <= 1)
			break;
	}
	mutex_enter(&engine->lock);
	engine->release_carry = carry;
	engine_fence_growth_mark_locked(engine);
	mutex_exit(&engine->lock);
	*released = rr.owners;
	pr19262_printf("PR19262_FENCE floor=%llu catalog=%llu reclaimed=%llu "
	    "released_owners=%llu released_bytes=%llu pages_freed=%llu "
	    "journal_freed=%llu journal_freed_bytes=%llu error=%d "
	    "ckpt=%llu ckpt_pins=%llu ckpt_kept=%llu\n",
	    (u_longlong_t)*floor, (u_longlong_t)catalog,
	    (u_longlong_t)*reclaimed, (u_longlong_t)rr.owners,
	    (u_longlong_t)rr.bytes, (u_longlong_t)pages_freed,
	    (u_longlong_t)journal_segments, (u_longlong_t)journal_bytes,
	    error, (u_longlong_t)cp_txg,
	    (u_longlong_t)pinned,
	    (u_longlong_t)ckpt_kept);
	return (error);
}

/*
 * A direct fence (the converter, R-01's final fence, the sysfs fence,
 * ztest) answers for every owner releasable at its floor: while the bulk
 * pot cut its release (L13), fence again -- each one returns the dead
 * pages the last left.  It stops when a fence released no more than its
 * forced first owner (a nearly full catalog: the scheduled fence chain
 * finishes the rest) and after ENGINE_FENCE_DIRECT_ROUNDS fences.  Once
 * one fence succeeded, a later one's error is not the answer: the first
 * fence alone met the caller's contract before L13.
 */
#define	ENGINE_FENCE_DIRECT_ROUNDS	256

int
pr19262_engine_fence(spa_t *spa, uint64_t *floor, uint64_t *reclaimed,
    uint64_t *released, uint64_t *sample)
{
	engine_t *engine = spa->spa_reloc;
	uint64_t total = 0, total_reclaimed = 0, last_floor = 0;
	int error;
	for (uint_t i = 0; ; i++) {
		error = engine_fence_impl(spa, floor, reclaimed, released,
		    sample, B_FALSE, NULL);
		if (error != 0 && i != 0) {
			/* Report the fences that succeeded. */
			*floor = last_floor;
			error = 0;
			break;
		}
		total += *released;
		total_reclaimed += *reclaimed;
		last_floor = *floor;
		mutex_enter(&engine->lock);
		/*
		 * L15 (L13-L15 fixup): each fence of the loop that returned
		 * retired bytes is progress for the timing-wait bound below,
		 * visible as it happens, not only after the whole loop.  It
		 * leaves fence_stalled to the scheduled fences: a client still
		 * past the limit after it keeps a real stall.
		 */
		if (error == 0 && (*reclaimed != 0 || *released != 0) &&
		    engine->spa == spa)
			engine->fence_returns++;
		boolean_t carry = engine->release_carry;
		mutex_exit(&engine->lock);
		if (error != 0 || !carry || *released <= 1 ||
		    i + 1 >= ENGINE_FENCE_DIRECT_ROUNDS)
			break;
	}
	*released = total;
	*reclaimed = total_reclaimed;
	return (error);
}

/*
 * L21: the checkpoint state a fence started now judges retired bytes by:
 * C's guard at the durable root that fence will take as its floor (the
 * release then runs at that floor too), the session bound with it, and a
 * checkpoint space map still draining after a discard (the owner release
 * then takes only drain_releasable owners).  B_FALSE: none of it.
 */
typedef struct engine_ckpt_keep {
	boolean_t guard;
	uint64_t ckpt;
	uint64_t after;
	boolean_t draining;
} engine_ckpt_keep_t;

static boolean_t
engine_ckpt_keep_locked(engine_t *engine, engine_ckpt_keep_t *k)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	const uberblock_t *cp = engine_ckpt_guard_locked(engine,
	    engine->durable_ub_txg);
	k->guard = cp != NULL;
	k->ckpt = cp != NULL ? cp->ub_txg : 0;
	k->after = cp != NULL ? MAX(cp->ub_txg, engine->ckpt_session_txg) :
	    0;
	k->draining = engine->spa != NULL && engine->checkpoint.ub_txg == 0 &&
	    spa_feature_is_active(engine->spa, SPA_FEATURE_POOL_CHECKPOINT);
	return (k->guard || k->draining);
}

/*
 * L15: of the retired bytes, those the root at this catalog already lists
 * as retired, as a fence against it sees them: an owner listed retired at
 * or below it may return all of its retired bytes, a retired version listed
 * at or below it its own.  The others were retired after that root, and a
 * later fence's catalog lists them.  total: every retired byte counted.
 *
 * L21: with k, pinned returns the retired bytes the checkpoint keeps from
 * every fence (engine_ckpt_keeps_owner/_version, the draining space map),
 * and pinned_v the V of the retired owners among them: native space the
 * DMU has freed but the pool still holds.  No fence returns them before the
 * discard, so the listed sum leaves them out.
 */
static uint64_t
engine_retired_listed_locked(engine_t *engine, uint64_t catalog,
    const engine_ckpt_keep_t *k, uint64_t *total, uint64_t *pinned,
    uint64_t *pinned_v)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t listed = 0;
	*total = *pinned = *pinned_v = 0;
	for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
	    e = AVL_NEXT(&engine->owners, e)) {
		boolean_t owner = e->retired && e->retired_txg != 0 &&
		    e->retired_txg <= catalog;
		/* As the owner release keeps it (engine_release_sync). */
		boolean_t okeep = k != NULL && e->retired &&
		    ((k->guard && engine_ckpt_keeps_owner(e, k->after)) ||
		    (k->draining && !e->drain_releasable));
		if (okeep)
			*pinned_v += e->size;
		for (engine_version_t *b = e->versions; b != NULL;
		    b = b->next) {
			/* As the owner release counts them. */
			if (b == e->cur ? !e->retired : !b->retired)
				continue;
			*total += b->size;
			/*
			 * The current backing returns only with its owner; an
			 * older one also by the version reclaim, which only
			 * C's guard limits.  An older one of a live owner
			 * counts only if the owner is kept too, so that it
			 * stays kept when the owner retires (a version kept
			 * by birth or journal_seq has its owner kept today:
			 * birth >= allocation_txg, and a bind sets
			 * journal_touched).
			 */
			boolean_t keep = b == e->cur ? okeep :
			    k != NULL && k->guard &&
			    engine_ckpt_keeps_version(b, k->after) &&
			    (e->retired ? okeep :
			    engine_ckpt_keeps_owner(e, k->after));
			if (keep) {
				*pinned += b->size;
				continue;
			}
			if (owner || (b != e->cur && b->retired_txg != 0 &&
			    b->retired_txg <= catalog))
				listed += b->size;
		}
	}
	return (listed);
}

static void
engine_retired_pinned_set_locked(engine_t *engine,
    const engine_ckpt_keep_t *k, uint64_t pinned, uint64_t pinned_v)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine->retired_pinned = pinned;
	engine->retired_pinned_v = pinned_v;
	engine->retired_pinned_ckpt = k->ckpt;
	engine->retired_pinned_after = k->after;
	engine->retired_pinned_draining = k->draining;
	engine->retired_pinned_txg = spa_last_synced_txg(engine->spa);
	engine->retired_pinned_valid = B_TRUE;
	engine->retired_pinned_counts++;
}

/*
 * L21: the last count if it was made under the checkpoint state a fence
 * started now would see (k), else none: a count made under another state
 * (no C yet, C still held before a discard, a drain since ended) says
 * nothing about this one.
 */
static boolean_t
engine_retired_pinned_current_locked(engine_t *engine,
    const engine_ckpt_keep_t *k)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	return (engine->retired_pinned_valid &&
	    engine->retired_pinned_ckpt == k->ckpt &&
	    engine->retired_pinned_after == k->after &&
	    engine->retired_pinned_draining == k->draining);
}

/*
 * L21: the retired bytes a fence could still return: engine->retired less
 * those the checkpoint keeps from every fence.  The count is an O(owners)
 * walk under engine->lock, so it is kept with the checkpoint state it was
 * made under.  While that state holds, a byte the count found kept stays
 * kept (birth, allocation_txg and the journal flags never revert) and
 * stays retired (only a fence's reclaim or release unretires, and they
 * keep it), so the difference may only overstate what a fence returns:
 * bytes retired or journal-bound since the count are taken as returnable.
 * With recount (open context), an estimate at the limit is counted again,
 * at most once per synced txg and not while the client would restart
 * behind a pending fence (that fence counts at its end); otherwise
 * (syncing context) the estimate stands and the fence it schedules counts
 * at its end.  A count is dropped whenever no checkpoint state is left
 * (here, at a fence's end, in the dataset space) and whenever the pool's
 * checkpoint changes (pr19262_engine_checkpoint): two drains, or a drain
 * and a later hold, never share one.
 */
static uint64_t
engine_retired_unpinned_locked(engine_t *engine, boolean_t recount)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_ckpt_keep_t k;
	if (engine->spa == NULL || !engine_ckpt_keep_locked(engine, &k)) {
		engine->retired_pinned_valid = B_FALSE;
		engine->retired_pinned = engine->retired_pinned_v = 0;
		return (engine->retired);
	}
	boolean_t fresh = engine_retired_pinned_current_locked(engine, &k);
	uint64_t pinned = fresh ? engine->retired_pinned : 0;
	uint64_t unpinned = engine->retired - MIN(pinned, engine->retired);
	boolean_t restarts = engine->fence_pending &&
	    engine->fence_error == 0 && !engine->fence_stalled;
	if (recount && !restarts && unpinned >= engine->retired_limit &&
	    (!fresh ||
	    engine->retired_pinned_txg != spa_last_synced_txg(engine->spa))) {
		uint64_t total, pinned_v;
		(void) engine_retired_listed_locked(engine, 0, &k, &total,
		    &pinned, &pinned_v);
		engine_retired_pinned_set_locked(engine, &k, pinned, pinned_v);
		unpinned = engine->retired - MIN(pinned, engine->retired);
		if (engine->retired_pinned_prints++ < 32)
			pr19262_printf("PR19262_RETIRED_PINNED retired=%llu "
			    "pinned=%llu pinned_v=%llu unpinned=%llu "
			    "limit=%llu ckpt=%llu after=%llu draining=%u "
			    "counts=%llu passes=%llu\n",
			    (u_longlong_t)engine->retired,
			    (u_longlong_t)pinned, (u_longlong_t)pinned_v,
			    (u_longlong_t)unpinned,
			    (u_longlong_t)engine->retired_limit,
			    (u_longlong_t)k.ckpt, (u_longlong_t)k.after,
			    k.draining,
			    (u_longlong_t)engine->retired_pinned_counts,
			    (u_longlong_t)engine->retired_pinned_passes);
	}
	return (unpinned);
}

/*
 * L21: the V of the checkpoint-pinned retired owners, for the dataset
 * space (pr19262_engine_hidden_dspace), from the last count if it is
 * current: 0 once the discard or the drain changed the state.
 */
static void
engine_retired_pinned_now_locked(engine_t *engine, uint64_t *pinned,
    uint64_t *pinned_v)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_ckpt_keep_t k;
	*pinned = *pinned_v = 0;
	if (engine->spa == NULL || !engine_ckpt_keep_locked(engine, &k)) {
		engine->retired_pinned_valid = B_FALSE;
		return;
	}
	if (!engine_retired_pinned_current_locked(engine, &k))
		return;
	*pinned = engine->retired_pinned;
	*pinned_v = engine->retired_pinned_v;
}

static uint64_t
engine_retired_pinned_v_locked(engine_t *engine)
{
	uint64_t pinned, pinned_v;
	engine_retired_pinned_now_locked(engine, &pinned, &pinned_v);
	return (pinned_v);
}

/* A fence the admission scheduled for clients past the retired limit. */
static void
engine_fence_task(void *arg)
{
	engine_t *engine = ((spa_t *)arg)->spa_reloc;
	spa_t *spa = arg;
	uint64_t floor, reclaimed, released, catalog = 0;
#ifndef _KERNEL
	mutex_enter(&engine->lock);
	pr19262_export_fence_test_t *test = engine->export_fence_test;
	mutex_exit(&engine->lock);
	if (test != NULL) {
		mutex_enter(&test->lock);
		test->paused = B_TRUE;
		cv_broadcast(&test->changed);
		while (!test->release)
			cv_wait(&test->changed, &test->lock);
		mutex_exit(&test->lock);
	}
	mutex_enter(&engine->lock);
	engine->fence_task_thread = curthread;
	mutex_exit(&engine->lock);
#endif
	/*
	 * A scheduled fence does not wait out a suspension in its txg waits
	 * or its tx assign: it returns ESHUTDOWN, frees this taskq thread
	 * and fence_pending, and a later store or admission schedules
	 * another.
	 */
	int error = engine_fence_impl(spa, &floor, &reclaimed, &released,
	    NULL, B_TRUE, &catalog);
	boolean_t suspended = error == ESHUTDOWN;
	mutex_enter(&engine->lock);
#ifndef _KERNEL
	engine->fence_task_thread = NULL;
	if (test != NULL) {
		mutex_enter(&test->lock);
		test->task_done = B_TRUE;
		test->fence_error = error;
		cv_broadcast(&test->changed);
		mutex_exit(&test->lock);
	}
#endif
	/*
	 * A growth fence's failure (a checkpoint, a suspended pool) does not
	 * become the clients' answer; its success, like any fence's, clears
	 * an earlier error and sets fence_stalled below.  A suspension is no
	 * client's answer either: clients restart behind the next fence.
	 */
	if ((!engine->fence_growth_running && !suspended) || error == 0)
		engine->fence_error = error;
	engine->fence_growth_running = B_FALSE;
	engine->fence_auto += error == 0;
	/* The arena wait counts fences that ran, not suspended ones. */
	engine->fence_completions += !suspended;
	engine->fence_suspended += suspended;
	/*
	 * Stalled tells clients past the retired limit that waiting for a
	 * fence will not help.  A growth fence that reclaimed nothing while
	 * retired is below that limit says nothing about them.
	 *
	 * L15: nor does an empty fence whose catalog listed less than the
	 * limit of the retired bytes: the rest were retired after its floor
	 * root (by a free or a copy while it ran, or after a fence that ran
	 * just before it took what its catalog listed), and the next fence,
	 * which first waits for the open txg to sync, finds them listed.
	 * That is a timing wait: clients restart behind the next fence.
	 * Bytes listed but not returned (a checkpoint, a lease, a hold) stay
	 * a stall.  One timing wait per fence return: an empty fence after
	 * one, with no direct fence return since (fence_returns), stalls.
	 */
	if (!suspended) {
		/*
		 * L21: bytes the checkpoint keeps from every fence are left out
		 * of both tests: this fence could not return them, nor can the
		 * next one before the discard.  Under a checkpoint a fence past
		 * the limit also refreshes the pinned count admission uses.
		 */
		engine_ckpt_keep_t k;
		boolean_t keep = engine->spa == spa &&
		    engine_ckpt_keep_locked(engine, &k);
		boolean_t past = error == 0 &&
		    engine->retired >= engine->retired_limit;
		boolean_t none = reclaimed == 0 && released == 0;
		uint64_t total = 0, listed = 0, pinned = 0, pinned_v = 0;
		if (past && (none || keep))
			listed = engine_retired_listed_locked(engine, catalog,
			    keep ? &k : NULL, &total, &pinned, &pinned_v);
		if (past && keep)
			engine_retired_pinned_set_locked(engine, &k, pinned,
			    pinned_v);
		if (!keep)
			engine->retired_pinned_valid = B_FALSE;
		boolean_t empty = past && none && engine->retired -
		    MIN(pinned, engine->retired) >= engine->retired_limit;
		boolean_t timing = empty && listed < engine->retired_limit &&
		    (!engine->fence_timing ||
		    engine->fence_returns != engine->fence_timing_mark);
		engine->fence_stalled = empty && !timing;
		engine->fence_timing = timing;
		if (timing) {
			engine->fence_timing_mark = engine->fence_returns;
			engine->fence_timing_waits++;
		}
		if (empty)
			pr19262_printf("PR19262_FENCE_STALL floor=%llu "
			    "catalog=%llu retired=%llu listed=%llu "
			    "counted=%llu limit=%llu timing_waits=%llu "
			    "verdict=%s pinned=%llu\n", (u_longlong_t)floor,
			    (u_longlong_t)catalog,
			    (u_longlong_t)engine->retired,
			    (u_longlong_t)listed, (u_longlong_t)total,
			    (u_longlong_t)engine->retired_limit,
			    (u_longlong_t)engine->fence_timing_waits,
			    timing ? "wait" : "stalled",
			    (u_longlong_t)pinned);
	}
	/*
	 * The growth trigger measures from what this fence left behind.  A
	 * fence suspended before its floor moved keeps the marks, so the
	 * first store after resume re-triggers.  One that moved the floor
	 * re-based them there; owners whose release it could not queue wait
	 * for the next fence.
	 */
	if (!suspended)
		engine_fence_growth_mark_locked(engine);
	engine->fence_pending = B_FALSE;
	/*
	 * L13: a release the bulk pot stopped continues in another fence;
	 * each one releases at least one owner, so the chain ends.
	 */
	boolean_t again = error == 0 && engine->release_carry &&
	    !engine->fence_quiesced && spa_writeable(spa) &&
	    spa->spa_load_state == SPA_LOAD_NONE && !spa_suspended(spa);
#ifndef _KERNEL
	/* The export-fence test and R-02 knobs own fence scheduling. */
	if (engine->export_fence_test != NULL ||
	    zfs_pr19262_r02_reseed_cut != 0 ||
	    atomic_load_32(&zfs_pr19262_r02_new_endpoint_faults) != 0)
		again = B_FALSE;
#endif
	if (again) {
		engine->fence_pending = B_TRUE;
		/* Its failure, like a growth fence's, is no client's answer. */
		engine->fence_growth_running = B_TRUE;
		engine->fence_requests++;
	} else if (error != 0 && !suspended) {
		/* Failed before its release: the next trigger retries. */
		engine->release_carry = B_FALSE;
	}
	cv_broadcast(&engine->fence_cv);
	uint64_t used = engine->fence_growth_pages_mark;
	uint64_t retired = engine->retired;
	mutex_exit(&engine->lock);
	pr19262_printf("PR19262_FENCE_AUTO error=%d floor=%llu reclaimed=%llu "
	    "released=%llu pages_used=%llu retired=%llu\n", error,
	    (u_longlong_t)floor, (u_longlong_t)reclaimed,
	    (u_longlong_t)released, (u_longlong_t)used,
	    (u_longlong_t)retired);
	if (again && taskq_dispatch(engine_fence_tq, engine_fence_task, spa,
	    TQ_NOSLEEP) == TASKQID_INVALID) {
		/* The next store's growth check sees release_carry. */
		mutex_enter(&engine->lock);
		engine->fence_pending = B_FALSE;
		engine->fence_growth_running = B_FALSE;
		engine->fence_requests--;
		cv_broadcast(&engine->fence_cv);
		mutex_exit(&engine->lock);
	}
}

void
pr19262_engine_fence_schedule(spa_t *spa)
{
	VERIFY3U(taskq_dispatch(engine_fence_tq, engine_fence_task, spa,
	    TQ_SLEEP), !=, TASKQID_INVALID);
}

/*
 * Metadata-only TXG churn -- a held scrub or resilver, a parked R-01 root,
 * idle syncs -- retires owners and leaves superseded catalog pages that
 * only a fence returns, and no client admission may arrive to schedule
 * one.  Called after every successful catalog store, in syncing context:
 * it never fences (a fence waits for TXGs), it dispatches the admission's
 * fence task.  One fence is pending at a time, and the growth triggers need
 * growth past the marks the last completed fence left, so a fence that
 * cannot reclaim (a held lease) is not redispatched every TXG by them.  A
 * checkpoint does not make a fence futile: the floor keeps only the pages
 * C's root reaches.  At critical headroom every store that used pages
 * asks for one, and after a deferred long free every store until the next
 * fence.
 */
#define	ENGINE_FENCE_GROWTH_PAGES	512
#define	ENGINE_FENCE_LOW_PAGES		256
#define	ENGINE_FENCE_LOW_STEP		64
#define	ENGINE_FENCE_RETIRED_STEP	(8ULL << 20)

/*
 * Long-free pacing.  A free retires catalog rows in syncing context, which
 * admission never sees: one dmu_free_long_range chunk (up to 256 L1
 * blocks) can retire a whole file in one txg and need more new pages than
 * are free.  The rows not yet synced share the bulk budget the async
 * destroy, a fence's release and the checkpoint discard draw on (L13; it
 * leaves out what admitted clients own, and counts arena runs only past
 * the kept ones on a healthy pool), priced as they are: a page per row, a
 * page per ENGINE_M_ROWS rows, one path.  Before L13 pacing was off at
 * two runs and the standby, and a row cost a sixth of a page (G1, G4).
 * A chunk past the budget waits for the next txg; the first chunk of a
 * txg, a fairly waiting one and one that waited ENGINE_FREE_AGE_TXGS may
 * go past it only while the free pages beyond what admitted clients own
 * absorb it without growth.  A free that cannot fit waits for fences.
 */
#define	ENGINE_FREE_RESERVE_PAGES	256
#define	ENGINE_FREE_AGE_TXGS		PR19262_FREE_AGE_TXGS
#define	ENGINE_FREE_FAIR_TXGS		4

static uint64_t engine_admit_pages_total(engine_t *);
static uint64_t engine_bulk_budget_now_locked(spa_t *, engine_t *, uint64_t);
static uint64_t engine_bulk_rows_locked(engine_t *, uint64_t);
static boolean_t engine_checkpoint_pins_locked(spa_t *, engine_t *);
static uint64_t engine_ckres_share_locked(spa_t *, engine_t *);

static uint64_t
engine_free_row_budget_locked(spa_t *spa, engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	/*
	 * Pace once growth is at most one run away (the open txg may be two
	 * stores late).  Also under a checkpoint: the checkpoint-exempt floor
	 * fence still returns every dead page C's root does not reach.
	 */
	if (engine->spa != spa || !engine->persistent ||
	    engine->pages->capacity == 0 || !engine->free_runs_known ||
	    !spa_writeable(spa))
		return (UINT64_MAX);
	/*
	 * Once per synced txg (no budget walk per chunk); pages admitted to
	 * clients since then come off it.
	 */
	uint64_t synced = spa_last_synced_txg(spa);
	uint64_t admitted = engine_admit_pages_total(engine);
	if (engine->free_budget_synced != synced + 1) {
		engine->free_budget_synced = synced + 1;
		/* L22: under C's reserve it leaves the clients' share. */
		engine->free_budget_pages =
		    engine_bulk_budget_now_locked(spa, engine,
		    engine_ckres_share_locked(spa, engine));
		engine->free_budget_admitted = admitted;
	}
	uint64_t pages = engine->free_budget_pages;
	if (pages == UINT64_MAX)
		return (UINT64_MAX);
	uint64_t grown = admitted > engine->free_budget_admitted ?
	    admitted - engine->free_budget_admitted : 0;
	return (engine_bulk_rows_locked(engine,
	    pages > grown ? pages - grown : 0));
}

/*
 * Rows of a free that the free pages past what admitted clients own (and
 * the reserve) absorb without growth: the most any single admission may
 * reach past the budget (L13, G4).
 */
static uint64_t
engine_free_row_spare_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	/*
	 * L19: under a checkpoint its catalog reserve is held too, and
	 * (L22) the clients' share past it: a long free waits for the
	 * discard, clients cannot.
	 */
	uint64_t held = engine_admit_pages_total(engine) +
	    ENGINE_FREE_RESERVE_PAGES +
	    engine_ckres_held_locked(engine->spa, engine) +
	    engine_ckres_share_locked(engine->spa, engine);
	uint64_t available = engine->pages->available;
	return (engine_bulk_rows_locked(engine,
	    available > held ? available - held : 0));
}

/*
 * Checkpoint discard pacing (L11): catalog pages a discard may dirty rows
 * worth of in one txg, or UINT64_MAX (no paged store: nothing to grow).
 * Half the spare pages past those admitted clients hold and the long-free
 * reserve, at most half of the headroom admission leaves to syncing-context
 * work it never sees (frees, the root's own blobs); half an arena more only
 * while two runs are free and the spare is at least the cap, so growth
 * never takes the standby or the run that refills it.  A discard that finds
 * no room waits; it is never aged past this.
 *
 * The spare counts free pages and, as admission covers admitted pages with
 * free pages and arena runs (engine_admit_arena_slots), each run past the
 * two kept back (L14): admitted load that already grows into runs would
 * otherwise hold the free pages and starve the discard.  Admitted pages,
 * the discard's and the bonus together still leave one run beyond the
 * standby, and the cap stays below an arena, so a txg the open txg's
 * admission raced past is covered by the standby as before.  While the
 * store's cache counts two runs (its bound) the count is fresh: the cache
 * may be stale.  The fresh count stops once more runs could not raise the
 * budget; with fewer runs than that it walks the whole free tree, at most
 * once per txg and per wait poll.
 */
static uint64_t engine_admit_pages_total(engine_t *);
static uint64_t engine_admit_runs_counted(engine_t *);

/* Half of admission's ENGINE_ADMIT_HEADROOM_PAGES for unadmitted work. */
#define	ENGINE_CKPT_DISCARD_MAX_PAGES	256
/* The standby's refill and one more stay out of the discard's spare. */
#define	ENGINE_CKPT_DISCARD_KEPT_RUNS	2
/* As admission's ENGINE_ADMIT_ARENA_PAGES: a new arena's usable pages. */
#define	ENGINE_CKPT_DISCARD_RUN_PAGES	(ENGINE_STORE_PAGES - 3)

static uint64_t
engine_ckpt_discard_budget_locked(spa_t *spa, engine_t *engine,
    uint64_t share)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->spa != spa || !engine->persistent ||
	    engine->pages->capacity == 0)
		return (UINT64_MAX);
	uint64_t available = engine->pages->available;
	/*
	 * Pages admitted clients and the sync reserve already own, and under
	 * a checkpoint its catalog reserve (L19; the discard alone may draw
	 * its share of it, pr19262_engine_checkpoint_discard), and share
	 * more for the work that can wait for the discard (L22).
	 */
	uint64_t held = engine_admit_pages_total(engine) +
	    ENGINE_FREE_RESERVE_PAGES + engine_ckres_held_locked(spa, engine) +
	    share;
	/*
	 * Fewer than two runs give the budget of none.  The store's cache
	 * (refreshed by every store) says so without a walk; it may miss a
	 * run returned since, for at most a txg, on the safe side.  Else
	 * count afresh, up to where more runs could not raise the budget:
	 * past held and the spare of a full budget, runs change nothing.
	 */
	uint64_t runs;
	if (engine->free_runs_known && engine->free_runs_cached <
	    ENGINE_CKPT_DISCARD_KEPT_RUNS) {
		runs = engine->free_runs_cached;
	} else {
		uint64_t full = held + 2 * ENGINE_CKPT_DISCARD_MAX_PAGES;
		uint64_t limit = ENGINE_CKPT_DISCARD_KEPT_RUNS +
		    (full > available ? howmany(full - available,
		    ENGINE_CKPT_DISCARD_RUN_PAGES) : 0);
		/* The held runs added below count toward the limit too. */
		uint64_t counted = engine_admit_runs_counted(engine);
		limit = MAX(ENGINE_CKPT_DISCARD_KEPT_RUNS,
		    limit - MIN(limit, counted));
		runs = engine_pages_runs_bounded_locked(engine, limit);
	}
	/*
	 * L15b holds the runs admitted pages need outside free: count them
	 * back, as held is subtracted below (else the admitted load would be
	 * subtracted twice and the discard starve again).
	 */
	runs += engine_admit_runs_counted(engine);
	uint64_t cover = available + (runs > ENGINE_CKPT_DISCARD_KEPT_RUNS ?
	    (runs - ENGINE_CKPT_DISCARD_KEPT_RUNS) *
	    ENGINE_CKPT_DISCARD_RUN_PAGES : 0);
	uint64_t spare = cover > held ? cover - held : 0;
	/*
	 * L22: with a share (work that can wait for the discard) no bonus:
	 * past half the spare it would reach into the clients' share.
	 */
	return (MIN(spare / 2, ENGINE_CKPT_DISCARD_MAX_PAGES) +
	    (share == 0 && runs >= ENGINE_CKPT_DISCARD_KEPT_RUNS &&
	    spare >= ENGINE_CKPT_DISCARD_MAX_PAGES ? ENGINE_STORE_PAGES / 2 :
	    0));
}

/*
 * The catalog pages one discard piece may need: a row per row it dirties
 * (a leaf each, as long-free pacing charges), one more for a new row's
 * split.  Zero for a piece already retired.
 */
static uint64_t
engine_ckpt_discard_cost(const engine_owner_t *e)
{
	if (e == NULL)
		return (2);
	return (e->retired ? 0 : engine_store_owner_count(e));
}

/*
 * Retire the checkpoint-only owners and legacy extents of [v, v + size),
 * a checkpoint space map entry being discarded, in syncing context.  The
 * catalog rows this dirties must fit the store of this txg (L11): the
 * retirement stops at the last piece boundary within the txg's budget and
 * *done says how far it got (size: all of it; 0: nothing, the entry
 * waits).  Validation covers exactly that prefix, before any change.
 */
int
pr19262_engine_checkpoint_discard(vdev_t *vd, uint64_t v, uint64_t size,
    boolean_t *handled, uint64_t *done)
{
	spa_t *spa = vd->vdev_spa;
	engine_t *engine = spa->spa_reloc;
	*handled = B_FALSE;
	*done = size;
	mutex_enter(&engine->lock);
	if (engine->spa == NULL || engine->spa != spa)
		goto out;
	if (vd != engine->vd)
		goto out;
	if (v >= engine->total || size == 0 || size > engine->total - v) {
		mutex_exit(&engine->lock);
		return (ENGINE_EINVAL(1));
	}
	uint64_t txg = spa_syncing_txg(spa);
	/* One pot per txg, shared with the L13 bulk retirements. */
	(void) engine_bulk_pot_locked(spa, engine);
	/* The root-to-leaf path of the entry's first leaf, once. */
	uint64_t path = MAX(engine->pages->levels, 1) + 1;
	uint64_t budget = engine->ckpt_discard_budget, charged = 0, rows = 0;
	/*
	 * L19: while C's catalog reserve is held, a step whose first piece
	 * the pot cannot fund draws that piece from the discard's own share
	 * of it (ckres_disc_left), the pot untouched: nothing else may take
	 * it, and the pins C holds drop at the first fence past the discard.
	 */
	boolean_t disc = B_FALSE;
	uint64_t align = 1ULL << vd->vdev_ashift;
	uint64_t cut = v, cut_charged = 0, cut_rows = 0;
	/* SM runs may merge legacy extents and split or merge owners. */
	for (uint64_t cursor = v; cursor < v + size; ) {
		engine_owner_t *e = engine_checkpoint_owner(engine, cursor);
		uint64_t next;
		if (e != NULL) {
			if (!e->checkpoint_only && !e->retired) {
				mutex_exit(&engine->lock);
				return (ENGINE_EINVAL(2));
			}
			next = MIN(v + size, e->v + e->size);
		} else {
			uint64_t found, length;
			if (!zfs_range_tree_find_in(engine->legacy, cursor,
			    v + size - cursor, &found, &length) ||
			    found != cursor) {
				mutex_exit(&engine->lock);
				return (ENGINE_EINVAL(6));
			}
			next = cursor + length;
		}
		uint64_t cost = engine_ckpt_discard_cost(e);
		uint64_t first = charged == 0 && cost != 0 ? path : 0;
		if (cost != 0 && (budget == 0 ||
		    charged + first + cost > budget) && charged == 0 &&
		    !disc && engine_ckres_active_locked(engine) &&
		    path + cost <= engine->ckres_disc_left) {
			disc = B_TRUE;
			budget = path + cost;
		}
		if (cost != 0 && (budget == 0 ||
		    charged + first + cost > budget)) {
			/* What this piece needs in a txg of its own. */
			engine->ckpt_discard_need = path + cost;
			break;
		}
		charged += first + cost;
		rows += cost;
		cursor = next;
		/* The rest must be a valid space map entry again. */
		if (P2PHASE(cursor, align) == 0 || cursor == v + size) {
			cut = cursor;
			cut_charged = charged;
			cut_rows = rows;
		}
	}
	if (cut != v + size) {
		engine->ckpt_discard_cuts++;
		engine->ckpt_discard_stalls += cut == v;
		if (engine->ckpt_discard_prints++ < 16)
			pr19262_printf("PR19262_CKPT_DISCARD_PACE txg=%llu "
			    "v=%llu size=%llu done=%llu budget=%llu "
			    "charged=%llu need=%llu available=%llu\n",
			    (u_longlong_t)txg, (u_longlong_t)v,
			    (u_longlong_t)size, (u_longlong_t)(cut - v),
			    (u_longlong_t)budget, (u_longlong_t)cut_charged,
			    (u_longlong_t)engine->ckpt_discard_need,
			    (u_longlong_t)engine->pages->available);
	}
	VERIFY3U(cut_charged, <=, budget);
	if (disc) {
		VERIFY3U(cut_charged, <=, engine->ckres_disc_left);
		engine->ckres_disc_left -= cut_charged;
		engine->ckres_disc_draws += cut_charged != 0;
	} else {
		engine->ckpt_discard_budget = budget - cut_charged;
	}
	engine->ckpt_discard_rows += cut_rows;
	for (uint64_t cursor = v; cursor < cut; ) {
		engine_owner_t *e = engine_checkpoint_owner(engine, cursor);
		if (e == NULL) {
			uint64_t found, length;
			VERIFY(zfs_range_tree_find_in(engine->legacy, cursor,
			    cut - cursor, &found, &length));
			VERIFY3U(found, ==, cursor);
			/*
			 * This is a retired identity extent, not a live BP.
			 * Old catalogs may still reach its native checkpoint.
			 * Keep both V and P until their ordinary durable floor.
			 */
			e = engine_owner_alloc(KM_SLEEP);
			e->v = cursor;
			e->size = length;
			e->cur = engine_version_new(engine, e, cursor,
			    length, 0, B_TRUE);
			e->incarnation = ++engine->next_incarnation;
			e->created = ++engine->sequence;
			zfs_range_tree_remove(engine->legacy, cursor, length);
			avl_add(&engine->owners, e);
		}
		if (!e->retired) {
			e->checkpoint_only = B_FALSE;
			e->retired = B_TRUE;
			e->retired_at = ++engine->sequence;
			engine->retired += e->cur->size;
			engine->revision++;
			engine_owner_dirty(engine, e);
			/* Its whole V leaves the space map in this txg. */
			e->drain_releasable = e->v >= v &&
			    e->v + e->size <= cut;
		}
		cursor = MIN(cut, e->v + e->size);
	}
	*handled = B_TRUE;
	*done = cut - v;
out:
	mutex_exit(&engine->lock);
	return (0);
}

/*
 * The discard thread's step retired nothing: no catalog room for its next
 * piece in this txg (L11).  Wait in open context, never in a sync task,
 * until that piece fits a txg's budget or the thread is cancelled.  Once
 * per wait, ask for a fence: it returns the dead pages the last stores
 * left and, once its floor passes the discard, the pages C's root pinned.
 * Later fences come from the store's own growth check, as space is freed.
 * Polls once a second under engine.lock: no I/O, no txg.
 */
void
pr19262_engine_checkpoint_discard_wait(spa_t *spa, zthr_t *zthr)
{
	engine_t *engine = spa->spa_reloc;
	boolean_t asked = B_FALSE;
	mutex_enter(&engine->lock);
	engine->ckpt_discard_waits++;
	mutex_exit(&engine->lock);
	/* zthr_iscancelled takes the zthr's lock: never under engine.lock. */
	while (!zthr_iscancelled(zthr)) {
		mutex_enter(&engine->lock);
		if (engine->spa != spa || !engine->persistent ||
		    engine_ckpt_discard_budget_locked(spa, engine, 0) >=
		    engine->ckpt_discard_need ||
		    (engine_ckres_active_locked(engine) &&
		    engine->ckres_disc_left >= engine->ckpt_discard_need)) {
			mutex_exit(&engine->lock);
			break;
		}
		if (!asked && !engine->fence_pending &&
		    !engine->fence_quiesced && spa_writeable(spa) &&
		    spa->spa_load_state == SPA_LOAD_NONE &&
		    !spa_suspended(spa)) {
			asked = B_TRUE;
			engine->fence_pending = B_TRUE;
			engine->fence_requests++;
			engine->ckpt_discard_fences++;
			mutex_exit(&engine->lock);
			pr19262_engine_fence_schedule(spa);
			continue;
		}
		if (!engine->ckpt_discard_warned && asked &&
		    !engine->fence_pending) {
			engine->ckpt_discard_warned = B_TRUE;
			/*
			 * Name the cause (L14): without the pages admitted
			 * writes hold until their txgs sync, the free pages
			 * would fit the piece; or the pages are gone.  needed
			 * is what the piece needs free in a txg of its own
			 * (the reserves, the standing admission pages, twice
			 * the piece); a piece past the cap never fits.
			 */
			uint64_t writes =
			    engine->admit[PR19262_ADMIT_CLIENT].m_pages +
			    engine->admit[PR19262_ADMIT_NETFREE].m_pages;
			uint64_t held = engine_admit_pages_total(engine) +
			    ENGINE_FREE_RESERVE_PAGES +
			    engine_ckres_held_locked(spa, engine);
			uint64_t need = engine->ckpt_discard_need;
			uint64_t available = engine->pages->available;
			if (writes != 0 &&
			    need <= ENGINE_CKPT_DISCARD_MAX_PAGES &&
			    available + writes >= held + 2 * need)
				cmn_err(CE_WARN, "pool '%s': the checkpoint "
				    "discard waits for relocation catalog "
				    "pages that admitted writes hold (%llu of "
				    "%llu catalog pages free, %llu held by "
				    "writes); it continues as the writes sync",
				    spa_name(spa), (u_longlong_t)available,
				    (u_longlong_t)engine->pages->capacity,
				    (u_longlong_t)writes);
			else
				cmn_err(CE_WARN, "pool '%s': the checkpoint "
				    "discard waits for relocation catalog "
				    "space (%llu of %llu catalog pages free, "
				    "%llu needed); it continues as space is "
				    "freed", spa_name(spa),
				    (u_longlong_t)available,
				    (u_longlong_t)engine->pages->capacity,
				    (u_longlong_t)(held + 2 * need));
		}
		(void) cv_timedwait_idle(&engine->fence_cv, &engine->lock,
		    ddi_get_lbolt() + SEC_TO_TICK(1));
		mutex_exit(&engine->lock);
	}
}

/*
 * L13: this txg's pot of catalog pages for unadmitted bulk retirement in
 * syncing context -- the checkpoint discard, the async destroy and a
 * fence's owner release draw on one budget, so together they stay inside
 * what L11 allows one of them.  Refreshed at the txg's first draw.
 */
/*
 * Arena runs a txg's pot may count (L13), and the pages of one: a bounded
 * walk of the free tree once per txg.
 */
#define	ENGINE_BULK_RUNS_MAX	34
#define	ENGINE_BULK_RUNS_KEPT	2
#define	ENGINE_BULK_RUN_PAGES	(ENGINE_STORE_PAGES - 3)

/*
 * L13: the catalog pages unadmitted syncing-context work may take per txg
 * now: L11's discard budget, or on a healthy pool -- the standby held and
 * runs past the two kept back -- half of what is spare across the free
 * pages and those runs past what admitted work owns (L11's cap is for
 * low headroom).  The larger of the two; UINT64_MAX without a paged store.
 */
static uint64_t
engine_bulk_budget_now_locked(spa_t *spa, engine_t *engine, uint64_t share)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t budget = engine_ckpt_discard_budget_locked(spa, engine, share);
	/*
	 * The last store's count (kept current across L15b's fills and
	 * returns): no free-tree walk per call; plus the runs L15b holds for
	 * the admitted pages subtracted below.
	 */
	uint64_t runs = engine->store_standby_valid && budget != UINT64_MAX &&
	    engine->free_runs_known ? engine->bulk_runs_cached +
	    engine_admit_runs_counted(engine) : 0;
	if (runs > ENGINE_BULK_RUNS_KEPT) {
		uint64_t cover = engine->pages->available +
		    (runs - ENGINE_BULK_RUNS_KEPT) * ENGINE_BULK_RUN_PAGES;
		uint64_t held = engine_admit_pages_total(engine) +
		    ENGINE_FREE_RESERVE_PAGES +
		    engine_ckres_held_locked(spa, engine) + share;
		budget = MAX(budget, cover > held ? (cover - held) / 2 : 0);
	}
	return (budget);
}

/* L13: rows whose page price (a page each, branches, a path) fits pages. */
static uint64_t
engine_bulk_rows_locked(engine_t *engine, uint64_t pages)
{
	uint64_t path = MAX(engine->pages->levels, 1) + 1;
	return (pages > path ? (pages - path) * ENGINE_M_ROWS /
	    (ENGINE_M_ROWS + 1) : 0);
}

static uint64_t
engine_bulk_pot_locked(spa_t *spa, engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t txg = spa_syncing_txg(spa);
	if (engine->ckpt_discard_txg != txg) {
		engine->ckpt_discard_txg = txg;
		uint64_t budget = engine_bulk_budget_now_locked(spa, engine, 0);
		/*
		 * L22: the pot the async destroy may draw under C's reserve,
		 * the clients' share held too; the rest it leaves.
		 */
		uint64_t share = engine_ckres_share_locked(spa, engine);
		uint64_t pausable = share == 0 ? budget :
		    engine_bulk_budget_now_locked(spa, engine, share);
		/* Long-free rows admitted to this txg already drew on it. */
		uint_t slot = txg & TXG_MASK;
		uint64_t lf = engine->free_pace_txg[slot] == txg ?
		    engine->free_pace_rows[slot] : 0;
		uint64_t lfp = lf == 0 ? 0 : lf + lf / ENGINE_M_ROWS +
		    MAX(engine->pages->levels, 1) + 1;
		if (budget != UINT64_MAX) {
			budget = budget > lfp ? budget - lfp : 0;
			pausable = pausable > lfp ? pausable - lfp : 0;
		}
		engine->ckpt_discard_budget = budget;
		engine->bulk_pot_keep = budget != UINT64_MAX &&
		    budget > pausable ? budget - pausable : 0;
		engine->bulk_pot_fresh = engine->ckpt_discard_budget;
		engine->bulk_rows = 0;
		engine->bulk_pathed = B_FALSE;
		engine->bulk_destroy_charged = B_FALSE;
	}
	return (engine->ckpt_discard_budget);
}

/* L13: the root-to-leaf path, charged once per txg (after the pot). */
static uint64_t
engine_bulk_path_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	return (engine->bulk_pathed ? 0 : MAX(engine->pages->levels, 1) + 1);
}

/*
 * Rows a gang DVA's header and members may add (L13): a row and a split
 * for each pointer the engine vdev's gang header holds (dynamic gang
 * headers); nested gangs are not counted.
 */
static uint64_t
engine_bulk_gang_rows(engine_t *engine)
{
	return (2 * (1 + gbh_nblkptrs(vdev_gang_header_psize(engine->vd))));
}

/*
 * L13: take cost catalog pages from this txg's pot.  force takes them
 * past the pot (the caller cannot stop there); first lets one piece past
 * a non-empty pot nothing has drawn on yet in this txg, so a piece that
 * costs more than the cap still goes.  Returns B_FALSE, taking nothing,
 * when they do not fit.
 */
static boolean_t
engine_bulk_take_locked(spa_t *spa, engine_t *engine, uint64_t cost,
    boolean_t force, boolean_t first)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t pot = engine_bulk_pot_locked(spa, engine);
	if (pot == UINT64_MAX)
		return (B_TRUE);
	boolean_t untouched = pot != 0 && pot == engine->bulk_pot_fresh;
	if (cost > pot && !force && !(first && untouched))
		return (B_FALSE);
	engine->ckpt_discard_budget = cost > pot ? 0 : pot - cost;
	return (B_TRUE);
}

/*
 * L13: may the async destroy free bp in this txg?  Its free retires the
 * owner of each DVA on the engine vdev (rows: engine_store_owner_count),
 * or makes a row for a legacy extent (a row and a split), or under a
 * checkpoint marks the owner checkpoint-only (the same rows).  Charged
 * to the bulk pot; must: charged even past it.  A refusal makes the
 * destroy pause to the next txg; one that left the destroy without
 * progress in this txg asks the store's growth check for a fence.
 */
boolean_t
pr19262_engine_free_bulk(spa_t *spa, const blkptr_t *bp, boolean_t must)
{
	engine_t *engine = spa->spa_reloc;
	if (engine == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return (B_TRUE);
	mutex_enter(&engine->lock);
	if (engine->spa != spa || !engine->persistent ||
	    engine->pages->capacity == 0) {
		mutex_exit(&engine->lock);
		return (B_TRUE);
	}
	uint64_t rows = 0;
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		if (DVA_GET_VDEV(dva) != engine->vd->vdev_id)
			continue;
		engine_owner_t *e = engine_find(engine, DVA_GET_OFFSET(dva));
		rows += e == NULL ? 2 : e->retired ? 0 :
		    engine_store_owner_count(e);
		if (BP_IS_GANG(bp))
			rows += engine_bulk_gang_rows(engine);
	}
	/*
	 * A page per row (a leaf each, as L11 and long-free pacing charge:
	 * rows changed in place free no leaf), a page per ENGINE_M_ROWS rows
	 * for the branches above them, and the root-to-leaf path at the
	 * txg's first destroy charge.
	 */
	(void) engine_bulk_pot_locked(spa, engine);
	uint64_t cost = rows + (engine->bulk_rows + rows) / ENGINE_M_ROWS -
	    engine->bulk_rows / ENGINE_M_ROWS + engine_bulk_path_locked(engine);
	/*
	 * L22: under C's reserve a pausable block leaves the pot's pages
	 * that hold the clients' share (bulk_pot_keep): the destroy waits
	 * for the discard, a client cannot.  Without the reserve nothing
	 * changes (keep 0: L13's first block past the pot still goes).
	 */
	boolean_t active = engine_ckres_active_locked(engine);
	uint64_t keep = active ? engine->bulk_pot_keep : 0;
	boolean_t ok = rows == 0 || ((must || keep == 0 ||
	    cost + keep <= engine->ckpt_discard_budget) &&
	    engine_bulk_take_locked(spa, engine, cost, must, !active));
	if (ok && rows != 0) {
		engine->bulk_rows += rows;
		engine->bulk_pathed = B_TRUE;
		engine->bulk_destroy_rows += rows;
		engine->bulk_destroy_charged = B_TRUE;
		/* Progress: a later stall asks for a fence again. */
		engine->bulk_stall_txg = 0;
		engine->bulk_destroy_asked = B_FALSE;
	} else if (!ok) {
		engine->bulk_destroy_pauses++;
		/*
		 * No progress in this txg: it forces no txg of its own
		 * (pr19262_engine_free_stalled), and it asks for one fence
		 * per stall, not one per txg: a fence that cannot help (C's
		 * pinned pages) would otherwise keep the txgs coming.
		 */
		if (!engine->bulk_destroy_charged) {
			engine->bulk_stall_txg = spa_syncing_txg(spa);
			if (!engine->bulk_destroy_asked) {
				engine->bulk_destroy_asked = B_TRUE;
				engine->bulk_waits++;
			}
		}
		if (engine->bulk_prints++ < 16)
			pr19262_printf("PR19262_BULK_PACE kind=destroy "
			    "txg=%llu rows=%llu pot=%llu fresh=%llu "
			    "charged_rows=%llu progress=%d available=%llu\n",
			    (u_longlong_t)spa_syncing_txg(spa),
			    (u_longlong_t)rows,
			    (u_longlong_t)engine->ckpt_discard_budget,
			    (u_longlong_t)engine->bulk_pot_fresh,
			    (u_longlong_t)engine->bulk_rows,
			    engine->bulk_destroy_charged,
			    (u_longlong_t)engine->pages->available);
	}
	mutex_exit(&engine->lock);
	return (ok);
}

/*
 * L13: the last synced txg's async destroy waited without progress, so
 * the txg sync thread need not hurry for the free bpobj (open context).
 */
boolean_t
pr19262_engine_free_stalled(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	if (engine == NULL)
		return (B_FALSE);
	mutex_enter(&engine->lock);
	boolean_t stalled = engine->spa == spa && engine->bulk_stall_txg != 0 &&
	    engine->bulk_stall_txg >= spa_last_synced_txg(spa);
	mutex_exit(&engine->lock);
	return (stalled);
}

/* Rows one long-free chunk may retire, or UINT64_MAX: no pacing. */
uint64_t
pr19262_engine_free_chunk_rows(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	if (engine == NULL)
		return (UINT64_MAX);
	mutex_enter(&engine->lock);
	uint64_t budget = engine_free_row_budget_locked(spa, engine);
	mutex_exit(&engine->lock);
	return (budget);
}

/*
 * Charge a chunk of rows to txg, or say it must wait for the next txg.
 * The budget covers every txg not yet synced, not each one.  waited counts
 * the txgs this free has waited since its last paced admission.  Since
 * L13 the caller cuts a chunk below one L1 when the budget is smaller
 * (unit is unused).  Past the budget a chunk goes only as the txg's first
 * (within cap: what the free pages absorb without growth), as a minimal
 * piece (PR19262_FREE_PIECE_ROWS, not under a checkpoint), after
 * ENGINE_FREE_FAIR_TXGS waits while in-flight rows are within the budget,
 * or aged after ENGINE_FREE_AGE_TXGS waits within cap.  Returns
 * PR19262_FREE_DEFER, PR19262_FREE_ADMIT or PR19262_FREE_AGED (the caller
 * then allows the next aged admission only after PR19262_FREE_REAGE_TXGS
 * more).
 */
int
pr19262_engine_free_pace(spa_t *spa, uint64_t txg, uint64_t rows,
    uint64_t unit, uint64_t waited)
{
	engine_t *engine = spa->spa_reloc;
	if (engine == NULL)
		return (PR19262_FREE_ADMIT);
	mutex_enter(&engine->lock);
	uint64_t budget = engine_free_row_budget_locked(spa, engine);
	if (budget == UINT64_MAX) {
		mutex_exit(&engine->lock);
		return (PR19262_FREE_ADMIT);
	}
	uint_t slot = txg & TXG_MASK;
	if (engine->free_pace_txg[slot] != txg) {
		engine->free_pace_txg[slot] = txg;
		engine->free_pace_rows[slot] = 0;
	}
	/* Rows charged to txgs that have not synced yet. */
	uint64_t synced = spa_last_synced_txg(spa), used = 0;
	for (uint_t t = 0; t < TXG_SIZE; t++)
		if (engine->free_pace_txg[t] > synced)
			used += engine->free_pace_rows[t];
	/* Past the budget, never past what the free pages absorb (G4). */
	uint64_t cap = MAX(budget, engine_free_row_spare_locked(engine));
	(void) unit;
	/*
	 * A minimal piece always goes, funded by the sync reserve, so a free
	 * never waits on fences alone -- except under a pool checkpoint,
	 * whose pinned pages no fence returns (A18 R4): it waits for the
	 * discard there instead.
	 */
	boolean_t piece = used == 0 && rows <= PR19262_FREE_PIECE_ROWS &&
	    !engine_checkpoint_pins_locked(spa, engine) &&
	    !engine_ckres_active_locked(engine);
	boolean_t fits = rows == 0 || piece ||
	    (rows <= budget && used <= budget - rows) ||
	    (used == 0 && rows <= cap) ||
	    (waited >= ENGINE_FREE_FAIR_TXGS && rows <= budget &&
	    used <= budget);
	boolean_t aged = !fits && waited >= ENGINE_FREE_AGE_TXGS &&
	    rows <= cap && used <= cap - rows;
	/*
	 * Under a pool checkpoint a waiting free forces no txg and asks for
	 * one fence per wait: every forced txg there leaks pinned pages.
	 */
	boolean_t pinned = engine_checkpoint_pins_locked(spa, engine);
	if (fits || aged) {
		engine->free_pace_rows[slot] += rows;
		engine->free_paced_rows += rows;
		engine->free_aged += aged;
		engine->free_ckpt_asked = B_FALSE;
	} else if (!pinned || !engine->free_ckpt_asked) {
		engine->free_deferrals++;
		engine->free_ckpt_asked = pinned;
	}
	mutex_exit(&engine->lock);
	return (aged ? PR19262_FREE_AGED : fits ? PR19262_FREE_ADMIT :
	    pinned ? PR19262_FREE_WAIT : PR19262_FREE_DEFER);
}

/*
 * A checkpoint pins the catalog pages its root reaches until a discard
 * (checkpoint-exempt floor): a fence returns only other dead pages, so
 * rewriting what C reaches needs new pages.  Refuse a new one, before any
 * sync, unless the syncing store has its standby run (or a run to refill
 * it) and one more run to grow into.
 */
/*
 * L19 (A18 R4): the catalog reserve under a pool checkpoint.  While C pins
 * the catalog pages its root reaches, the first change to each needs a new
 * page and no fence returns the old one; a free under C only marks its
 * owner checkpoint-only.  Every txg's MOS and space-map churn, the discard
 * and admitted work therefore take free pages one way until the pins drop.
 * R_C = MIN(R_disc + R_churn, capacity / ENGINE_CKRES_CAP_DIV) pages are
 * held for the work that must go on: R_disc, one discard piece (a path and
 * ENGINE_CKRES_DISC_ROWS rows), which only the discard may draw; R_churn,
 * ENGINE_CKRES_TXGS txgs of churn at the larger of
 * ENGINE_CKRES_CHURN_SEED pages per txg and this session's EWMA (1,024
 * pages at the seed: about 233 txgs at the ~4.4 pages per txg measured
 * net under C at low headroom while releases starved, L13 R4; less churn
 * with the release draw below).  Accounting only, as
 * held in the L11 budget, on top of the pin set: the bulk pot and the
 * long-free cap hold it; destroy and long free take no first piece past
 * the pot under it; the owner release may draw R_churn past the pot (it
 * deletes the churn's own rows); client and converter admission leave
 * it, a net free R_disc and a quarter of R_churn.  A checkpoint that
 * cannot have it (free pages and the runs past the two kept back, less
 * what admitted work and the sync reserve hold) is refused.  It is
 * armed when C is recorded, also on an import with C (at once, or at the
 * first use), and released when C's pins drop (the first fence past a
 * discard, which the growth check asks for) or the engine unloads; a
 * rewind import has no C.
 */
#define	ENGINE_CKRES_TXGS	256
#define	ENGINE_CKRES_CHURN_SEED	4
#define	ENGINE_CKRES_DISC_ROWS	64
#define	ENGINE_CKRES_CAP_DIV	4

static void
engine_ckres_size_locked(engine_t *engine, uint64_t *disc, uint64_t *total)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	int64_t ewma = engine->ckres_churn_x16 / 16;
	uint64_t churn = MAX(ENGINE_CKRES_CHURN_SEED,
	    ewma > 0 ? (uint64_t)ewma : 0);
	uint64_t path = MAX(engine->pages->levels, 1) + 1;
	uint64_t cap = engine->pages->capacity / ENGINE_CKRES_CAP_DIV;
	*total = MIN(path + ENGINE_CKRES_DISC_ROWS +
	    MIN(churn, cap) * ENGINE_CKRES_TXGS, cap);
	*disc = MIN(path + ENGINE_CKRES_DISC_ROWS, *total);
}

/* Free pages plus the runs past the two kept back (as the L11 budget). */
static uint64_t
engine_ckres_cover_locked(engine_t *engine, uint64_t runs)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	return (engine->pages->available +
	    (runs > ENGINE_CKPT_DISCARD_KEPT_RUNS ?
	    (runs - ENGINE_CKPT_DISCARD_KEPT_RUNS) *
	    ENGINE_CKPT_DISCARD_RUN_PAGES : 0));
}

/* The store's cached run count plus the runs L15b holds (no walk). */
static uint64_t
engine_ckres_runs_cached_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	return ((engine->free_runs_known ? engine->bulk_runs_cached : 0) +
	    engine_admit_runs_counted(engine));
}

static boolean_t
engine_ckres_active_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	return (engine->ckres_pages != 0 &&
	    (engine->checkpoint.ub_txg != 0 || engine->ckpt_until != 0));
}

static void
engine_ckres_arm_locked(spa_t *spa, engine_t *engine, const char *why)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->spa != spa || spa == NULL || !engine->persistent ||
	    engine->pages->capacity == 0)
		return;
	engine_ckres_size_locked(engine, &engine->ckres_disc,
	    &engine->ckres_pages);
	engine->ckres_disc_left = engine->ckres_disc;
	engine->ckres_rel_txg = engine->ckres_rel_left = 0;
	engine->ckres_rel_pages = engine->ckres_disc_draws = 0;
	engine->ckres_used_valid = B_FALSE;
	engine->ckres_warned = B_FALSE;
	engine->ckres_wait_gen = engine->ckres_wait_deficit = 0;
	engine->ckres_min_available = engine->pages->available;
	engine->ckres_arms++;
	pr19262_printf("PR19262_CKRES op=arm why=%s ckpt=%llu pages=%llu "
	    "disc=%llu churn_x16=%lld available=%llu capacity=%llu "
	    "admitted=%llu keep=%llu runs=%llu standby=%u\n", why,
	    (u_longlong_t)engine->checkpoint.ub_txg,
	    (u_longlong_t)engine->ckres_pages,
	    (u_longlong_t)engine->ckres_disc,
	    (longlong_t)engine->ckres_churn_x16,
	    (u_longlong_t)engine->pages->available,
	    (u_longlong_t)engine->pages->capacity,
	    (u_longlong_t)engine_admit_pages_total(engine),
	    (u_longlong_t)(engine_admit_pages_total(engine) +
	    ENGINE_FREE_RESERVE_PAGES + engine->ckres_pages),
	    (u_longlong_t)engine_ckres_runs_cached_locked(engine),
	    engine->store_standby_valid);
}

/*
 * The reserve's pages to hold, 0 without one.  An engine that has C but no
 * reserve (an import that recorded C before the engine was attached) arms
 * it here, at its first use.
 */
static uint64_t
engine_ckres_held_locked(spa_t *spa, engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->spa != spa || spa == NULL || !engine->persistent ||
	    engine->pages->capacity == 0)
		return (0);
	if (engine->ckres_pages == 0 && engine->checkpoint.ub_txg != 0)
		engine_ckres_arm_locked(spa, engine, "load");
	return (engine_ckres_active_locked(engine) ? engine->ckres_pages : 0);
}

/*
 * L22: the clients' share under the reserve.  The work that can wait for
 * the discard -- the async destroy and a long free -- stops this many
 * pages above the line where client admission refuses (admitted pages,
 * the sync reserve and R_C), and pauses there instead of taking them:
 * a client that finds the line reached gets ENOSPC (a zvol: EIO), while
 * the destroy resumes in the next txg and completes after the discard.
 * A quarter of R_churn (256 pages at the seed, one txg's full pot): the
 * share of R_churn a net free leaves too, so it scales with the churn
 * this session measured and the capacity cap.  0 without a reserve.
 */
static uint64_t
engine_ckres_share_locked(spa_t *spa, engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t held = engine_ckres_held_locked(spa, engine);
	return (held > engine->ckres_disc ?
	    (held - engine->ckres_disc) / 4 : 0);
}

/* R_churn still left: cover past admitted work, the sync reserve, R_disc. */
static uint64_t
engine_ckres_churn_left_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t churn = engine->ckres_pages - engine->ckres_disc;
	uint64_t base = engine_admit_pages_total(engine) +
	    ENGINE_FREE_RESERVE_PAGES + engine->ckres_disc;
	uint64_t cover = engine_ckres_cover_locked(engine,
	    engine_ckres_runs_cached_locked(engine));
	return (cover > base ? MIN(cover - base, churn) : 0);
}

/*
 * The owner release past the pot under the reserve (L19).  It deletes the
 * rows of owners retired after C (the txgs' own MOS and space-map churn);
 * the leaves it rewrites are post-C copies, which the next fence returns,
 * while rows it leaves behind would grow the catalog every txg.  So it may
 * draw on R_churn (never R_disc): per txg at most half of what is left,
 * at most ENGINE_CKPT_DISCARD_MAX_PAGES, as the L11 budget halves its
 * spare.  Tried before the pot (whose pages the destroy and long frees
 * wait for); a fence's forced first owner that fits neither still goes
 * past both, as before L19 (engine_bulk_take_locked force; at most a few
 * pages and a path from the sync reserve, back at the next fence), so
 * every fence releases one owner and the carry chain drains.
 */
static boolean_t
engine_ckres_release_take_locked(spa_t *spa, engine_t *engine, uint64_t cost,
    boolean_t forced)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (forced || engine_ckres_held_locked(spa, engine) == 0)
		return (B_FALSE);
	uint64_t txg = spa_syncing_txg(spa);
	if (engine->ckres_rel_txg != txg) {
		engine->ckres_rel_txg = txg;
		engine->ckres_rel_left = MIN(
		    engine_ckres_churn_left_locked(engine) / 2,
		    ENGINE_CKPT_DISCARD_MAX_PAGES);
	}
	if (cost > engine->ckres_rel_left)
		return (B_FALSE);
	engine->ckres_rel_left -= cost;
	engine->ckres_rel_pages += cost;
	return (B_TRUE);
}

/*
 * After every store under the reserve (growth check, syncing context): the
 * churn EWMA, the lowest free pages seen, and once, when less than a
 * quarter of R_churn is left, the warning that names the checkpoint.
 */
static void
engine_ckres_store_locked(spa_t *spa, engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine_ckres_held_locked(spa, engine) == 0) {
		engine->ckres_used_valid = B_FALSE;
		return;
	}
	uint64_t used = engine->pages->capacity - engine->pages->available;
	if (engine->ckres_used_valid)
		engine->ckres_churn_x16 += (int64_t)used -
		    (int64_t)engine->ckres_used_last -
		    engine->ckres_churn_x16 / 16;
	engine->ckres_used_last = used;
	engine->ckres_used_valid = B_TRUE;
	engine->ckres_min_available = MIN(engine->ckres_min_available,
	    engine->pages->available);
	uint64_t churn = engine->ckres_pages - engine->ckres_disc;
	uint64_t left = engine_ckres_churn_left_locked(engine);
	if (engine->ckres_warned || left >= churn / 4)
		return;
	engine->ckres_warned = B_TRUE;
	pr19262_printf("PR19262_CKRES op=low pages=%llu churn_left=%llu "
	    "available=%llu capacity=%llu churn_x16=%lld\n",
	    (u_longlong_t)engine->ckres_pages, (u_longlong_t)left,
	    (u_longlong_t)engine->pages->available,
	    (u_longlong_t)engine->pages->capacity,
	    (longlong_t)engine->ckres_churn_x16);
	cmn_err(CE_WARN, "pool '%s': a pool checkpoint holds the relocation "
	    "catalog (%llu of %llu catalog pages free, %llu of the %llu-page "
	    "checkpoint reserve left for sync work); writes that need catalog "
	    "pages fail with ENOSPC; discard it ('zpool checkpoint -d %s')",
	    spa_name(spa), (u_longlong_t)engine->pages->available,
	    (u_longlong_t)engine->pages->capacity, (u_longlong_t)left,
	    (u_longlong_t)engine->ckres_pages, spa_name(spa));
}

/*
 * L19: a new checkpoint also needs its catalog reserve: the free pages and
 * the runs past the two kept back must cover what admitted work and the
 * sync reserve hold plus R_C, else it is refused (ENOSPC) with a warning
 * that says what is missing.
 */
int
pr19262_engine_checkpoint_admit(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	if (engine == NULL)
		return (0);
	mutex_enter(&engine->lock);
	int error = 0;
	uint64_t runs = engine->spa == spa && engine->persistent &&
	    engine->pages->capacity != 0 ? engine_pages_runs_locked(engine) :
	    UINT64_MAX;
	/* Without the standby (not yet refilled), one run refills it. */
	if (runs != UINT64_MAX && (engine->store_standby_valid ? runs == 0 :
	    runs < 2)) {
		engine->checkpoint_headroom_refusals++;
		error = SET_ERROR(ENOSPC);
	}
	boolean_t run_rule = error != 0;
	uint64_t disc = 0, total = 0, cover = 0, want = 0;
	if (runs != UINT64_MAX) {
		engine_ckres_size_locked(engine, &disc, &total);
		cover = engine_ckres_cover_locked(engine,
		    runs + engine_admit_runs_counted(engine));
		want = engine_admit_pages_total(engine) +
		    ENGINE_FREE_RESERVE_PAGES + total;
		if (error == 0 && cover < want) {
			engine->ckres_refusals++;
			error = SET_ERROR(ENOSPC);
		}
	}
	if (error != 0) {
		pr19262_printf("PR19262_CKRES op=refuse pages=%llu disc=%llu "
		    "cover=%llu want=%llu available=%llu capacity=%llu "
		    "runs=%llu standby=%u\n", (u_longlong_t)total,
		    (u_longlong_t)disc, (u_longlong_t)cover,
		    (u_longlong_t)want, (u_longlong_t)engine->pages->available,
		    (u_longlong_t)engine->pages->capacity, (u_longlong_t)runs,
		    engine->store_standby_valid);
		if (run_rule)
			cmn_err(CE_WARN, "pool '%s': checkpoint refused: the "
			    "relocation catalog has no free arena run left "
			    "to grow into beside its standby (%llu of %llu "
			    "catalog pages free); free space and retry",
			    spa_name(spa),
			    (u_longlong_t)engine->pages->available,
			    (u_longlong_t)engine->pages->capacity);
		else
			cmn_err(CE_WARN, "pool '%s': checkpoint refused: the "
			    "relocation catalog cannot hold the %llu-page "
			    "reserve a checkpoint needs (%llu catalog pages "
			    "needed with admitted work, %llu free of %llu and "
			    "%llu free arena runs, the standby and one run "
			    "kept back); free space and retry", spa_name(spa),
			    (u_longlong_t)total, (u_longlong_t)want,
			    (u_longlong_t)engine->pages->available,
			    (u_longlong_t)engine->pages->capacity,
			    (u_longlong_t)runs);
	}
	mutex_exit(&engine->lock);
	return (error);
}

/* A pool checkpoint keeps the catalog pages its root reaches pinned. */
static boolean_t
engine_checkpoint_pins_locked(spa_t *spa, engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	return (engine->checkpoint.ub_txg != 0 ||
	    spa->spa_checkpoint_info.sci_timestamp != 0);
}

/*
 * Refill the store's standby run; once neither the standby nor a run to
 * refill it is left, say why once.  Syncing context, engine->lock held.
 */
static void
engine_store_headroom_locked(spa_t *spa, engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_pages_standby_fill_locked(engine);
	/*
	 * Once per store; long-free pacing, status and the L13 bulk budget
	 * read the cache (one bounded walk: L13 counts up to
	 * ENGINE_BULK_RUNS_MAX, everything else up to 2).
	 */
	engine->bulk_runs_cached = engine_pages_runs_bounded_locked(engine,
	    ENGINE_BULK_RUNS_MAX);
	engine->free_runs_cached = MIN(engine->bulk_runs_cached, 2);
	engine->free_runs_known = B_TRUE;
	if (engine->store_standby_valid || !engine->store_headroom_low ||
	    engine->store_headroom_warned)
		return;
	engine->store_headroom_warned = B_TRUE;
	/*
	 * A checkpoint keeps only the pages its root reaches; fences return
	 * every other dead page.  While a discard drains, it is already
	 * running: 'zpool checkpoint -d' would only fail (L14).
	 */
	boolean_t draining = engine->checkpoint.ub_txg == 0 &&
	    !spa_has_checkpoint(spa) &&
	    spa_feature_is_active(spa, SPA_FEATURE_POOL_CHECKPOINT);
	boolean_t pinned = !draining &&
	    engine_checkpoint_pins_locked(spa, engine);
	cmn_err(CE_WARN, "pool '%s': the relocation catalog has no free "
	    "%llu-byte arena run left (%llu of %llu catalog pages free); "
	    "writes needing more catalog space fail with ENOSPC%s%s%s",
	    spa_name(spa), (u_longlong_t)(ENGINE_STORE_BYTES *
	    engine->vd->vdev_children),
	    (u_longlong_t)engine->pages->available,
	    (u_longlong_t)engine->pages->capacity,
	    pinned ? "; a pool checkpoint keeps the catalog pages it "
	    "reaches until it is discarded ('zpool checkpoint -d " :
	    draining ? "; the checkpoint discard in progress returns "
	    "catalog space as it proceeds" : "; freeing space may restore "
	    "a run",
	    pinned ? spa_name(spa) : "", pinned ? "')" : "");
}

static void
engine_fence_growth_check(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->spa != spa || !engine->persistent ||
	    !spa_writeable(spa) || spa->spa_load_state != SPA_LOAD_NONE ||
	    spa_suspended(spa) || engine->pages->capacity == 0) {
		mutex_exit(&engine->lock);
		return;
	}
	engine_store_headroom_locked(spa, engine);
	engine_ckres_store_locked(spa, engine);
	if (engine->fence_pending || engine->fence_quiesced) {
		mutex_exit(&engine->lock);
		return;
	}
#ifndef _KERNEL
	/*
	 * The export-fence test owns fence scheduling while armed.  An armed
	 * R-02 crash cut or endpoint fault knob does too: a growth fence
	 * could otherwise move the floor between the cell's stages.
	 */
	if (engine->export_fence_test != NULL ||
	    zfs_pr19262_r02_reseed_cut != 0 ||
	    atomic_load_32(&zfs_pr19262_r02_new_endpoint_faults) != 0) {
		mutex_exit(&engine->lock);
		return;
	}
#endif
	/* The first store after load or reset only seeds the marks. */
	if (!engine->fence_growth_seeded) {
		engine_fence_growth_mark_locked(engine);
		mutex_exit(&engine->lock);
		return;
	}
	uint64_t used = engine->pages->capacity - engine->pages->available;
	/* Usage that fell below the mark (a direct fence) re-bases it. */
	if (used < engine->fence_growth_pages_mark)
		engine->fence_growth_pages_mark = used;
	if (engine->retired < engine->fence_growth_retired_mark)
		engine->fence_growth_retired_mark = engine->retired;
	uint64_t pages_mark = engine->fence_growth_pages_mark;
	/*
	 * L21: past the limit by bytes a fence could return (the last count
	 * of checkpoint-pinned bytes, not redone in syncing context).
	 */
	boolean_t retired = engine->retired >= engine->retired_limit &&
	    engine_retired_unpinned_locked(engine, B_FALSE) >=
	    engine->retired_limit &&
	    engine->retired >= engine->fence_growth_retired_mark +
	    MAX(engine->retired_limit / 4, ENGINE_FENCE_RETIRED_STEP);
	boolean_t grown = used >= pages_mark + ENGINE_FENCE_GROWTH_PAGES;
	boolean_t low = engine->pages->available < ENGINE_FENCE_LOW_PAGES &&
	    used >= pages_mark + ENGINE_FENCE_LOW_STEP;
	/*
	 * Without the standby and a run to refill it, only dead pages a fence
	 * returns keep the store inside its capacity: fence after every
	 * store that used pages.  A checkpoint does not make that futile:
	 * the floor keeps only the pages C's root reaches.
	 */
	boolean_t critical = !engine->store_standby_valid &&
	    engine->store_headroom_low && used > pages_mark;
	/* Long frees were deferred: a fence returns the pages they need. */
	boolean_t paced = engine->free_deferrals !=
	    engine->free_deferrals_mark;
	/*
	 * L13: an async destroy waited without progress, or a release was
	 * cut: a fence returns pages, and releases the rest.
	 */
	boolean_t bulk = engine->bulk_waits != engine->bulk_waits_mark ||
	    engine->release_carry;
	/*
	 * L19: a discard removed C: the first fence whose floor reaches
	 * ckpt_until drops C's pins, and the catalog reserve with them.
	 */
	boolean_t unpin = engine_ckres_active_locked(engine) &&
	    engine->checkpoint.ub_txg == 0 && engine->ckpt_until != 0 &&
	    spa_syncing_txg(spa) >= engine->ckpt_until;
	if (!retired && !grown && !low && !critical && !paced && !bulk &&
	    !unpin) {
		mutex_exit(&engine->lock);
		return;
	}
	engine->fence_pending = B_TRUE;
	engine->fence_growth_running = B_TRUE;
	engine->fence_requests++;
	engine->fence_growth_triggers++;
	mutex_exit(&engine->lock);
	if (taskq_dispatch(engine_fence_tq, engine_fence_task, spa,
	    TQ_NOSLEEP) == TASKQID_INVALID) {
		/* Retried after the next catalog store; not a request. */
		mutex_enter(&engine->lock);
		engine->fence_pending = B_FALSE;
		engine->fence_growth_running = B_FALSE;
		engine->fence_requests--;
		engine->fence_growth_triggers--;
		engine->fence_growth_dispatch_failures++;
		cv_broadcast(&engine->fence_cv);
		mutex_exit(&engine->lock);
	}
}

#ifndef _KERNEL
void
pr19262_engine_export_fence_arm(spa_t *spa,
    pr19262_export_fence_test_t *test)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	VERIFY3P(engine->spa, ==, spa);
	VERIFY(!engine->fence_pending);
	engine->export_fence_test = test;
	mutex_exit(&engine->lock);
}
#endif

/* Before the sync thread stops: no new fence, and none in flight. */
void
pr19262_engine_quiesce(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->spa == spa) {
		engine->fence_quiesced = B_TRUE;
#ifndef _KERNEL
		pr19262_export_fence_test_t *test = engine->export_fence_test;
		if (engine->fence_pending && test != NULL) {
			mutex_enter(&test->lock);
			test->quiescing = B_TRUE;
			cv_broadcast(&test->changed);
			mutex_exit(&test->lock);
		}
#endif
		while (engine->fence_pending)
			cv_wait(&engine->fence_cv, &engine->lock);
	}
	mutex_exit(&engine->lock);
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
engine_weak_report(engine_t *engine, uint64_t parity)
{
	uint64_t n = 0;
	mutex_enter(&engine->lock);
	for (engine_owner_t *e = avl_first(&engine->owners);
	    e != NULL && n < 8; e = AVL_NEXT(&engine->owners, e)) {
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
	mutex_exit(&engine->lock);
}

int
pr19262_engine_certify(spa_t *spa, uint64_t parity)
{
	engine_t *engine = spa->spa_reloc;
	if (!pr19262_engine_persistent(spa) || !spa_writeable(spa))
		return (ENOTSUP);
	/* A certificate is a promise about every durable raw-store member. */
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	boolean_t ready = engine_store_members_ready(engine->vd);
	spa_config_exit(spa, SCL_STATE, FTAG);
	if (!ready) {
		pr19262_printf("PR19262_CERTIFY_DEFERRED parity=%llu members\n",
		    (u_longlong_t)parity);
		return (ENXIO);
	}
	pr19262_engine_census_t c;
	pr19262_engine_census(spa, parity, &c);
	mutex_enter(&engine->lock);
	/* A split native checkpoint SM must finish draining before reuse. */
	boolean_t checkpoint = engine->checkpoint.ub_txg != 0 ||
	    spa->spa_checkpoint_info.sci_timestamp != 0;
	mutex_exit(&engine->lock);
	if (c.legacy_bytes != 0 || c.weak_current != 0 ||
	    c.weak_retired != 0 || c.floor == 0 || checkpoint ||
	    parity == 0 || parity > VDEV_RAIDZ_MAXPARITY ||
	    engine->vd->vdev_children < parity + 1) {
		pr19262_printf("PR19262_CERTIFY_REFUSED parity=%llu "
		    "legacy=%llu weak_current=%llu weak_retired=%llu "
		    "floor=%llu checkpoint=%u\n", (u_longlong_t)parity,
		    (u_longlong_t)c.legacy_bytes, (u_longlong_t)c.weak_current,
		    (u_longlong_t)c.weak_retired, (u_longlong_t)c.floor,
		    checkpoint);
		if (c.weak_current != 0)
			engine_weak_report(engine, parity);
		return (EBUSY);
	}
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine->store_lock);
	int error = engine_pages_publish_control(spa, engine->pages->floor,
	    parity);
	if (error == 0) {
		mutex_enter(&engine->lock);
		engine->certified = parity;
		mutex_exit(&engine->lock);
	}
	mutex_exit(&engine->store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	if (error != 0)
		return (error);
	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_WRITER);
	pr19262_vdev_certify(engine->vd, parity);
	vdev_config_dirty(engine->vd);
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
#define	ENGINE_CONVERT_FIRST_GROUP	8
/* Data bytes per copy group: one data flush and journal round each. */
static uint64_t zfs_pr19262_convert_group_bytes = 8ULL << 20;

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
	/* L18: the refusal was the clients' charges (admission ERESTART). */
	boolean_t starved_clients;
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
	/* Where a slice's time went, printed for a slice that ran past it. */
	hrtime_t visit_end;
	hrtime_t walk_gap_max;
	hrtime_t slice_drains;
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
	engine_t *engine = c->spa->spa_reloc;
	boolean_t weak = B_FALSE;
	mutex_enter(&engine->lock);
	engine_owner_t *e = engine_find(engine,
	    DVA_GET_OFFSET(&bp->blk_dva[d]));
	if (e != NULL && !e->retired && !e->aborted && e->cur != NULL)
		weak = e->cur->parity < c->parity;
	mutex_exit(&engine->lock);
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
	engine_t *engine = c->spa->spa_reloc;
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
		if ((error == ENOSPC || error == EBUSY) &&
		    engine_journal_roll_pending(engine)) {
			/*
			 * A journal roll found no run: it is pending, not
			 * failed, and the slice's hold keeps it so until the
			 * next pass retries it.  The block waits as busy.
			 */
			st->busy++;
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
		/*
		 * The conversion epoch fixes geometry; the copy records the
		 * concrete raw-write TXG as its backing birth for DTL/recovery.
		 */
		VERIFY3U(after.birth, !=, 0);
		st->copied++;
		st->old_bytes += g->before.psize;
		st->new_bytes += after.psize;
		c->pending += after.psize;
		mutex_enter(&engine->lock);
		st->peak_retired_bytes = MAX(st->peak_retired_bytes,
		    engine->retired);
		mutex_exit(&engine->lock);
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
	c->slice_drains += c->drain_ns;
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
	engine_t *engine = c->spa->spa_reloc;
	spa_t *spa = c->spa;
	pr19262_engine_convert_stats_t *st = c->stats;
	boolean_t mine = B_FALSE;
	for (int d = 0; d < BP_GET_NDVAS(bp); d++)
		mine |= DVA_GET_VDEV(&bp->blk_dva[d]) == engine->vd->vdev_id;
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
		if (DVA_GET_VDEV(&bp->blk_dva[d]) != engine->vd->vdev_id ||
		    !engine_convert_weak(c, bp, d))
			continue;
		/* A block the walk meets twice (a clone) is copied once. */
		if (engine_convert_grouped(c, DVA_GET_OFFSET(&bp->blk_dva[d])))
			continue;
		st->candidates++;
		/* A pending journal roll, retried once due: wait as busy. */
		int roll = engine_journal_roll_wait(spa);
		if (roll == EBUSY) {
			st->busy++;
			continue;
		}
		/* A sticky roll error is the pass's answer (L14). */
		if (roll != 0) {
			error = roll;
			break;
		}
		/* Admit this copy's bytes, catalog row and journal frame. */
		uint64_t psize = DVA_GET_GANG(&bp->blk_dva[d]) ?
		    vdev_gang_header_psize(engine->vd) : BP_GET_PSIZE(bp);
		uint64_t asize = vdev_psize_to_asize_txg(engine->vd, psize,
		    c->birth);
		uint64_t pages = pr19262_engine_admit_row_pages(spa);
		int admitted = pr19262_engine_admit(spa,
		    PR19262_ADMIT_CONVERTER, pr19262_tx_reserved(spa), asize,
		    pages, 1);
		if (admitted != 0) {
			st->starved++;
			c->starved = B_TRUE;
			c->starved_clients = admitted == ERESTART;
			break;
		}
		c->admitted_p += asize;
		c->admitted_m += pages;
		c->admitted_j++;
		void *copy;
		error = engine_copy_begin(spa, bp, d, c->birth, &copy, B_FALSE,
		    B_TRUE);
		if (error == EAGAIN) {
			/*
			 * Only runs held for others' unstored txgs fit it:
			 * drop this copy's charge now (a starved copy must
			 * not hold one through the pass) and end the pass as
			 * starved by clients (v8 fixup m3): their charges hold
			 * the runs, so L18 runs the next pass with priority,
			 * and the held runs return as their txgs store.
			 */
			pr19262_engine_admit_release(spa,
			    PR19262_ADMIT_CONVERTER, asize, pages, 1);
			c->admitted_p -= asize;
			c->admitted_m -= pages;
			c->admitted_j--;
			st->starved++;
			c->starved = B_TRUE;
			c->starved_clients = B_TRUE;
			error = 0;
			break;
		}
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
				/* A journal roll found no run: pending. */
				if (error != EBUSY && (error != ENOSPC ||
				    !engine_journal_roll_pending(engine)))
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
	 * slice held the open txg past its time.  Before any drain has set
	 * a pace, a small first group measures it: a full first group after
	 * a cold import drained for up to 2 s with nothing to bound it.
	 */
	if (grouped && error == 0 && (c->zil_chain ||
	    c->group_count >= c->group_max ||
	    (c->drain_psize == 0 &&
	    c->group_count >= ENGINE_CONVERT_FIRST_GROUP) ||
	    c->group_psize >= zfs_pr19262_convert_group_bytes ||
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
	/* The traversal's own time between two visits (reads, prefetch). */
	hrtime_t now = gethrtime();
	if (c->visit_end != 0)
		c->walk_gap_max = MAX(c->walk_gap_max, now - c->visit_end);
	c->visit_end = now;
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
	c->visit_end = gethrtime();
	if (error == 0 && c->visit_end + MIN(drain, c->slice_ns / 2) >=
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

#ifdef _KERNEL
/* The public converter's first fence has already published Q. */
static int
engine_convert_r01_final_free(spa_t *spa, uint64_t *floor,
    uint64_t *reclaimed, uint64_t *released, uint64_t *sample,
    boolean_t *did_fence)
{
	int error = pr19262_engine_r01_final_free_wait(spa);
	if (error == 0)
		error = pr19262_engine_r01_final_free_second_fence(spa, floor,
		    reclaimed, released, sample, did_fence);
	return (error);
}
#endif

/*
 * L18: a conversion keeps the native slop.  Its whole remaining need --
 * the parity growth of the remaining weak backing (hidden on top of the
 * slop since L17) and the legacy worst case, both on top of the slop, the
 * catalog and journal growth the outstanding charges forecast and one
 * batch inside it -- must fit in native AVAIL (pr19262_engine_convert_room).
 * While it runs the legacy worst case is hidden from AVAIL on top of the
 * slop too (admit_converting), so clients cannot take that room back.  The
 * engine vdev must also hold the client-protected amount itself.  At the
 * start a conversion that does not fit is refused with ENOSPC and changes
 * nothing (in the kernel before its epoch); before each later pass one
 * that no longer fits pauses (below).  Retired backing
 * counts as room (the next fence returns it), except under a pool
 * checkpoint, which keeps it.  The slack, in native AVAIL units, is the
 * engine's own metadata that shares the slop as native sync-context
 * metadata does -- one catalog arena and one journal segment (L17's growth
 * forecast, which an arena allocation moves into hidden space) -- plus
 * ENGINE_CONVERT_ROOM_DRIFT for syncing-context writes native ZFS makes
 * past AVAIL 0 (the epoch task's own MOS writes).  A later pass also
 * tolerates one batch (at most ENGINE_ADMIT_BATCH): clients admitted
 * against the dspace of the last sync may overshoot by what the
 * converter's pass hid since.
 */
#define	ENGINE_CONVERT_ROOM_DRIFT (4LL << 20)
#define	ENGINE_CONVERT_PAUSE_ROUNDS 30
#define	ENGINE_CONVERT_STARVE_ROUNDS 16

static void
engine_convert_room_print(const char *what, const char *stage,
    uint64_t pass, uint64_t round, const pr19262_engine_room_t *r,
    int64_t forgiven, int64_t slack)
{
	pr19262_printf("PR19262_CONVERT_%s stage=%s pass=%llu round=%llu "
	    "room=%lld forgiven=%lld slack=%lld short=%lld avail=%lld "
	    "delta=%lld space=%llu used=%llu slop=%llu growth=%llu "
	    "legacy=%llu transient=%llu free=%llu protected=%llu store=%llu "
	    "journal=%llu retired=%llu weak=%llu parity=%llu planned=%llu\n",
	    what, stage,
	    (u_longlong_t)pass, (u_longlong_t)round, (longlong_t)r->room,
	    (longlong_t)forgiven, (longlong_t)slack,
	    (longlong_t)MAX(0, -(r->room + forgiven + slack)),
	    (longlong_t)r->avail, (longlong_t)r->delta,
	    (u_longlong_t)r->space, (u_longlong_t)r->used,
	    (u_longlong_t)r->slop, (u_longlong_t)r->growth,
	    (u_longlong_t)r->legacy, (u_longlong_t)r->transient,
	    (u_longlong_t)r->free, (u_longlong_t)r->protected,
	    (u_longlong_t)r->store, (u_longlong_t)r->journal,
	    (u_longlong_t)r->retired, (u_longlong_t)r->weak,
	    (u_longlong_t)r->parity, (u_longlong_t)r->planned);
}

/* L18: bytes in native AVAIL units (the engine vdev's deflate ratio). */
static int64_t
engine_convert_deflate(const pr19262_engine_room_t *r, uint64_t bytes)
{
	return ((int64_t)((bytes >> SPA_MINBLOCKSHIFT) * r->ratio));
}

/*
 * L18: the room that returns: retired backing (the next fence returns it)
 * and the journal segments the conversion added since it started
 * (journal0: they wait to be folded into a stored catalog and are freed by
 * a later fence).  Neither under a pool checkpoint, which keeps both.
 * Catalog arenas are permanent: their growth is not forgiven (the slack
 * holds one arena).
 */
static int64_t
engine_convert_forgiven(spa_t *spa, const pr19262_engine_room_t *r,
    uint64_t journal0)
{
	if (spa_has_checkpoint(spa))
		return (0);
	uint64_t bytes = r->retired +
	    (r->journal > journal0 ? r->journal - journal0 : 0);
	return (engine_convert_deflate(r, bytes));
}

static int64_t
engine_convert_slack(const pr19262_engine_room_t *r, uint64_t batch)
{
	return (ENGINE_CONVERT_ROOM_DRIFT + engine_convert_deflate(r, r->runs +
	    MIN(batch, ENGINE_ADMIT_BATCH)));
}

#ifdef _KERNEL
static boolean_t engine_kernel_leaving(spa_t *);
#else
static boolean_t
engine_kernel_leaving(spa_t *spa)
{
	(void) spa;
	return (B_FALSE);
}
#endif

/*
 * L18: refuse a conversion whose whole need does not fit (see above).
 * *journal (if not NULL) returns the journal extents measured.
 */
noinline static int
engine_convert_room_check(spa_t *spa, uint64_t parity, uint64_t batch,
    const char *stage, uint64_t *journal)
{
	pr19262_engine_room_t r;
	/* dspace is recomputed at the end of each sync: the plan's AVAIL. */
	txg_wait_synced(spa_get_dsl(spa), 0);
	pr19262_engine_convert_room(spa, parity, batch, &r);
	if (journal != NULL)
		*journal = r.journal;
	int64_t forgiven = engine_convert_forgiven(spa, &r, r.journal);
	int64_t slack = engine_convert_slack(&r, 0);
	/*
	 * The engine vdev itself must hold the reserve as well (native AVAIL
	 * also counts other normal-class vdevs, where no copy can go); the
	 * growth forecast may exceed free for a moment after a fill.
	 */
	boolean_t vdev_fits = r.free + (spa_has_checkpoint(spa) ? 0 :
	    r.retired) + r.growth_term >= r.protected;
	/* Nothing left to grow takes nothing from the slop. */
	boolean_t fits = (r.room + forgiven >= -slack && vdev_fits) ||
	    (r.growth == 0 && r.legacy == 0);
	engine_convert_room_print(fits ? "ROOM" : "SPACE", stage, 0, 0, &r,
	    forgiven, slack);
	if (fits)
		return (0);
	if (r.room + forgiven >= -slack) {
		uint64_t have = r.free + (spa_has_checkpoint(spa) ? 0 :
		    r.retired) + r.growth_term;
		cmn_err(CE_WARN, "pool '%s': conversion to parity %llu "
		    "refused: its vdev needs %llu more free bytes for the "
		    "conversion reserve; free space on it, then convert again",
		    spa_name(spa), (u_longlong_t)parity,
		    (u_longlong_t)(r.protected - have));
	} else {
		cmn_err(CE_WARN, "pool '%s': conversion to parity %llu "
		    "refused: it needs %lld more bytes of available space to "
		    "start without eating the pool's slop; free space, then "
		    "convert again", spa_name(spa), (u_longlong_t)parity,
		    (longlong_t)(-(r.room + forgiven + slack)));
	}
	return (SET_ERROR(ENOSPC));
}

/*
 * L18: before a later pass, the rest of the conversion must still fit.
 * The planned growth and the legacy worst case are counted on top of the
 * slop, so it stops fitting only when something else took the room:
 * legacy blocks whose growth the worst case under-counts (3-wide raidz1),
 * syncing-context work, clients writing in the txg before a plan change or
 * in native classes past AVAIL 0, catalog arenas past the one the slack
 * holds.  Journal segments added since the start (journal0) are not
 * counted.  Pause instead of eating the slop: sync and check again, up to
 * ENGINE_CONVERT_PAUSE_ROUNDS times and as many seconds, or until the pool
 * is exported or unloaded, then end with ENOSPC.  No client is held while
 * it waits (priority is set after this check).  The plan and every
 * converted block stay, so a later conversion resumes where it stopped.
 */
noinline static int
engine_convert_pause(spa_t *spa, uint64_t parity, uint64_t batch,
    uint64_t pass, uint64_t journal0)
{
	pr19262_engine_room_t r;
	hrtime_t until = gethrtime() + SEC2NSEC(ENGINE_CONVERT_PAUSE_ROUNDS);
	for (uint64_t round = 0; ; round++) {
		pr19262_engine_convert_room(spa, parity, batch, &r);
		int64_t forgiven = engine_convert_forgiven(spa, &r, journal0);
		int64_t slack = engine_convert_slack(&r,
		    batch != 0 ? batch : ENGINE_ADMIT_BATCH);
		if (r.room + forgiven >= -slack ||
		    (r.growth == 0 && r.legacy == 0)) {
			if (round != 0)
				engine_convert_room_print("RESUME", "pass",
				    pass, round, &r, forgiven, slack);
			return (0);
		}
		if (round == 0)
			engine_convert_room_print("PAUSE", "pass", pass, round,
			    &r, forgiven, slack);
		if (round == ENGINE_CONVERT_PAUSE_ROUNDS ||
		    gethrtime() > until || !spa_writeable(spa) ||
		    engine_kernel_leaving(spa)) {
			engine_convert_room_print("SPACE", "pass", pass,
			    round, &r, forgiven, slack);
			cmn_err(CE_WARN, "pool '%s': conversion to parity %llu "
			    "stopped at pass %llu: it needs %lld more bytes of "
			    "available space to go on without eating the "
			    "pool's slop; free space, then convert again (it "
			    "resumes)", spa_name(spa), (u_longlong_t)parity,
			    (u_longlong_t)pass,
			    (longlong_t)(-(r.room + forgiven + slack)));
			return (SET_ERROR(ENOSPC));
		}
		delay(SEC_TO_TICK(1));
		txg_wait_synced(spa_get_dsl(spa), 0);
	}
}

/*
 * L18: a pass that copied nothing because admission refused its first copy.
 * Report the refusal; retry (with priority) when clients' charges caused
 * it, or when the request fits alone now that the pass's fence returned
 * pages and runs, up to ENGINE_CONVERT_STARVE_ROUNDS passes in a row.  Its
 * own frame: pr19262_engine_convert's is near the kernel's limit.
 */
noinline static boolean_t
engine_convert_starved(spa_t *spa, uint64_t pass, uint64_t rounds,
    boolean_t clients)
{
	engine_t *e = spa->spa_reloc;
	boolean_t after = !clients &&
	    pr19262_engine_admit_conv_alone(spa, pr19262_tx_reserved(spa));
	mutex_enter(&e->lock);
	engine_conv_refusal_t r = e->admit_conv_last;
	uint64_t waits = e->admit_conv_priority_waits;
	/* L19: C's catalog reserve refused it: no retry until the discard. */
	boolean_t ckres = e->ckres_conv_refused;
	mutex_exit(&e->lock);
	boolean_t retry = (clients || after) && !ckres &&
	    rounds < ENGINE_CONVERT_STARVE_ROUNDS;
	pr19262_printf("PR19262_CONVERT_STARVED pass=%llu round=%llu "
	    "retry=%u after_fence=%u reason=%s alone=%llu free=%llu "
	    "reserved=%llu need=%llu "
	    "growth=%llu arenas=%llu slots=%llu client_pages=%llu "
	    "alone_growth=%llu alone_arenas=%llu alone_slots=%llu "
	    "deferred=%llu priority_waits=%llu ckres=%u\n", (u_longlong_t)pass,
	    (u_longlong_t)rounds, retry, after, r.reason == 1 ? "arena" :
	    r.reason == 2 ? "space" : "none", (u_longlong_t)r.alone,
	    (u_longlong_t)r.free, (u_longlong_t)r.reserved,
	    (u_longlong_t)r.need, (u_longlong_t)r.growth,
	    (u_longlong_t)r.arenas, (u_longlong_t)r.slots,
	    (u_longlong_t)r.client_pages, (u_longlong_t)r.alone_growth,
	    (u_longlong_t)r.alone_arenas, (u_longlong_t)r.alone_slots,
	    (u_longlong_t)r.deferred, (u_longlong_t)waits, ckres);
	return (retry);
}

/*
 * The converter's admission report (printed by pr19262_engine_admit_report)
 * and its start check: one batch and the standing reserve must fit now.
 * Separate frames keep pr19262_engine_convert's small (L22: the report
 * is its largest local, and the kernel debug build's frame limit is 1,024
 * bytes).
 */
noinline static void
engine_convert_report(spa_t *spa, const char *why)
{
	pr19262_engine_admit_report_t admit;
	pr19262_engine_admit_report(spa, why, &admit);
}

noinline static int
engine_convert_space_check(spa_t *spa, uint64_t parity, uint64_t batch)
{
	/*
	 * The standing reserve already holds the growth of every weaker
	 * backing and one batch; refuse to start when the pool is so full
	 * that even the reserve is missing, rather than fail half way.
	 */
	pr19262_engine_plan(spa, parity, batch);
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
	return (0);
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
	engine_t *engine = spa->spa_reloc;
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
	vdev_raidz_layout_for_birth(engine->vd, birth, NULL, &c.parity);
	dsl_pool_t *dp = spa->spa_dsl_pool;
	int space_error = engine_convert_space_check(spa, c.parity, batch);
	if (space_error != 0)
		return (space_error);
	/*
	 * L18: the whole remaining need must fit on top of the slop (the room
	 * counts the legacy worst case there, the flag below not yet set).
	 */
	uint64_t journal0 = 0;
	int room_error = engine_convert_room_check(spa, c.parity, batch,
	    "start", &journal0);
	if (room_error != 0)
		return (room_error);
#ifdef _KERNEL
	int test_error = pr19262_engine_r01_convert_test_wait(spa, birth);
	if (test_error != 0)
		return (test_error);
#endif
	/*
	 * L18: from here the legacy worst case is hidden on top of the slop
	 * too, so clients cannot take that room back; the sync publishes it.
	 */
	pr19262_engine_admit_converting(spa, B_TRUE);
	txg_wait_synced(dp, 0);
	c.group = kmem_alloc(group_capacity * sizeof (*c.group), KM_SLEEP);
	c.group_holds = kmem_alloc(group_capacity * sizeof (*c.group_holds),
	    KM_SLEEP);
	int error = 0;
	/* A sweep spans passes: it resumes where a full batch stopped it. */
	uint64_t sweep_copied = 0, sweep_busy = 0;
	/* journal_roll_enospc when the current run of idle sweeps began. */
	mutex_enter(&engine->lock);
	uint64_t idle_roll_enospc = engine->journal_roll_enospc;
	mutex_exit(&engine->lock);
	/*
	 * L18: the next pass's first slice runs with priority over new client
	 * charges; a starved pass retried so does not use up max_passes.
	 */
	boolean_t priority = B_FALSE;
	uint64_t starved_rounds = 0, retried = 0;
	for (uint64_t pass = 0; pass < max_passes + retried; pass++) {
		stats->passes++;
		uint64_t copied = stats->copied;
		c.starved = B_FALSE;
		c.starved_clients = B_FALSE;
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
			/* L18: the rest must still fit; else pause. */
			if (first && pass != 0) {
				error = engine_convert_pause(spa, c.parity,
				    batch, pass, journal0);
				if (error != 0)
					break;
			}
			/*
			 * L18: hold new client charges back, and let the ones
			 * synced txgs held go (their callbacks release them).
			 */
			if (first && priority) {
				pr19262_engine_admit_priority(spa, B_TRUE);
				txg_wait_synced(dp, 0);
				txg_wait_callbacks(dp);
			}
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
			c.visit_end = c.walk_gap_max = c.slice_drains = 0;
			error = engine_convert_slice(&c);
			/* L18: priority ends with the first slice's walk. */
			if (priority) {
				pr19262_engine_admit_priority(spa, B_FALSE);
				priority = B_FALSE;
			}
			hrtime_t walked = gethrtime();
			/* The slice's copies finish under its hold. */
			int ran = engine_convert_drain(&c);
			hrtime_t drained = gethrtime();
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
			if (now > c.slice_end + MSEC2NSEC(200)) {
				hrtime_t begun = c.slice_end - c.slice_ns;
				pr19262_printf("PR19262_SLICE_OVER pass=%llu "
				    "over_ms=%llu walk_ms=%llu gap_max_ms=%llu "
				    "drains_ms=%llu last_drain_ms=%llu "
				    "after_ms=%llu yielded=%u swept=%u\n",
				    (u_longlong_t)pass,
				    (u_longlong_t)NSEC2MSEC(now - c.slice_end),
				    (u_longlong_t)NSEC2MSEC(walked - begun),
				    (u_longlong_t)NSEC2MSEC(c.walk_gap_max),
				    (u_longlong_t)NSEC2MSEC(c.slice_drains),
				    (u_longlong_t)NSEC2MSEC(drained - walked),
				    (u_longlong_t)NSEC2MSEC(now - drained),
				    c.yielded, swept);
			}
		}
		if (error != 0)
			break;
		if (batch != 0) {
			uint64_t floor, reclaimed, released;
			int e = 0;
			e = pr19262_engine_fence(spa, &floor, &reclaimed,
			    &released, stats->sample);
			if (e == 0) {
				stats->fences++;
				stats->reclaimed_bytes += reclaimed;
				stats->released_owners += released;
				stats->floor = floor;
#ifdef _KERNEL
				/* First fence retains P. */
				uint64_t final_floor = 0, final_reclaimed = 0;
				uint64_t final_released = 0;
				boolean_t final_fence = B_FALSE;
				e = engine_convert_r01_final_free(spa,
				    &final_floor, &final_reclaimed,
				    &final_released, stats->sample,
				    &final_fence);
				if (e != 0) {
					error = e;
					break;
				}
				if (final_fence) {
					stats->fences++;
					stats->reclaimed_bytes +=
					    final_reclaimed;
					stats->released_owners +=
					    final_released;
					stats->floor = final_floor;
				}
#endif
			} else {
				if (e != EBUSY) {
					error = e;
					break;
				}
			}
		}
		/* The pass's copies are catalogued or reclaimed: release. */
		pr19262_engine_admit_release(spa, PR19262_ADMIT_CONVERTER,
		    c.admitted_p, c.admitted_m, c.admitted_j);
		c.admitted_p = c.admitted_m = c.admitted_j = 0;
		if (stats->copied != copied)
			starved_rounds = 0;
		if (c.starved && stats->copied == copied) {
			boolean_t retry = engine_convert_starved(spa, pass,
			    starved_rounds, c.starved_clients);
			if (retry) {
				/*
				 * L18: clients' charges starved it, and it
				 * fits alone (at its refusal or after this
				 * pass's fence): the next pass's first slice
				 * runs with priority (clients restart behind
				 * a sync) instead of ending the conversion.
				 */
				starved_rounds++;
				retried++;
				priority = B_TRUE;
			} else {
				/* No space for one copy, nothing to reclaim. */
				engine_convert_report(spa, "convert-starved");
				error = ENOSPC;
				break;
			}
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
		/*
		 * A journal roll the segment needs holds every candidate busy.
		 * Whether waiting can still bring its run is not decidable
		 * here (a later fence may return a whole catalog arena), so
		 * the 8 idle sweeps stay; but when the roll is what held
		 * them and its due retries failed, the answer is ENOSPC, not
		 * EBUSY, and STUCK says so (L14).
		 */
		mutex_enter(&engine->lock);
		boolean_t roll = engine_journal_roll_needed_locked(engine);
		uint64_t roll_enospc = engine->journal_roll_enospc;
		mutex_exit(&engine->lock);
		if (stats->copied != sweep_copied)
			idle_roll_enospc = roll_enospc;
		boolean_t full = roll && roll_enospc != idle_roll_enospc;
		if (c.idle_passes >= 8) {
			pr19262_printf("PR19262_CONVERT_STUCK passes=%llu "
			    "busy=%llu roll_needed=%u roll_enospc=%llu "
			    "error=%d\n", (u_longlong_t)stats->passes,
			    (u_longlong_t)(stats->busy - sweep_busy), roll,
			    (u_longlong_t)roll_enospc, full ? ENOSPC : EBUSY);
			error = full ? SET_ERROR(ENOSPC) : SET_ERROR(EBUSY);
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
	/* L18: clients wait for no pass once the conversion ends. */
	pr19262_engine_admit_priority(spa, B_FALSE);
	pr19262_engine_admit_converting(spa, B_FALSE);
	if (c.gang != NULL)
		kmem_free(c.gang, c.gang_capacity * sizeof (*c.gang));
	VERIFY0(c.group_count);
	kmem_free(c.group_holds, group_capacity * sizeof (*c.group_holds));
	kmem_free(c.group, group_capacity * sizeof (*c.group));
	engine_convert_report(spa, "convert-end");
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
