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
 * Private vector admission, included by the ownership engine after its
 * converter.  Every admitted transaction reserves, per class and before it
 * dirties anything, the physical bytes it may allocate, the catalog pages
 * its rows may need and the journal frames its acknowledgements may need.
 * The arena growth those page and frame reservations can force is charged
 * to physical space, so that a catalog sync never fails for lack of space
 * while admission holds.  A standing conversion reserve keeps the parity
 * growth of every weaker backing, a worst case for still unclassified
 * legacy space and one converter batch out of ordinary admission, so that
 * a full pool still converts completely.
 */
#define	ENGINE_ADMIT_SYNC_BYTES (8ULL << 20)
#define	ENGINE_ADMIT_LOG_BYTES (4ULL << 20)
#define	ENGINE_ADMIT_SYNC_PAGES 128
#define	ENGINE_ADMIT_SYNC_FRAMES 16
#define	ENGINE_ADMIT_LEGACY_GROWTH_PCT 50
#define	ENGINE_ADMIT_ARENA_PAGES (ENGINE_STORE_PAGES - 3)
#define	ENGINE_ADMIT_HEADROOM_PAGES 512
#define	ENGINE_ADMIT_SEGMENT_FRAMES \
	(ENGINE_JOURNAL_BYTES / ENGINE_STORE_PAGE - 3)

static boolean_t
engine_admit_checking(void)
{
	if (ENGINE_MIRROR) {
		const char *env = pr19262_env("PR19262_ADMIT_VERIFY");
		return (env == NULL || strcmp(env, "0") != 0);
	}
	return (zfs_pr19262_store_verify != 0);
}

static uint64_t
engine_admit_free_pages(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine_admit_checking()) {
		uint64_t free = 0;
		for (engine_marena_t *a = engine->pages->arenas; a != NULL;
		    a = a->next) {
			if (a->pending)
				continue;
			for (uint64_t i = 0; i < a->pages; i++)
				free += engine->pages->bits[a->first + i] == 0;
		}
		ENGINE_STORE_CHECK(free == engine->pages->available);
		atomic_inc_64(&engine->admit_cost.pages_checked);
	}
	return (engine->pages->available);
}

static uint64_t
engine_admit_free_frames(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->journal_tail == NULL)
		return (0);
	uint64_t capacity = engine->journal_bytes / ENGINE_STORE_PAGE;
	uint64_t used = engine->journal_tail->seq + 3;
	uint64_t free = capacity > used ? capacity - used : 0;
	if (engine->journal_pending != NULL)
		free += ENGINE_ADMIT_SEGMENT_FRAMES;
	return (free);
}

/* Physical bytes the raw arenas may need to absorb these reservations. */
static uint64_t
engine_admit_growth(engine_t *engine, uint64_t pages, uint64_t frames)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (!engine->persistent || engine->pages->capacity == 0)
		return (0);
	uint64_t children = engine->vd->vdev_children;
	uint64_t free_pages = engine_admit_free_pages(engine);
	uint64_t free_frames = engine_admit_free_frames(engine);
	uint64_t arenas = pages > free_pages ?
	    howmany(pages - free_pages, ENGINE_ADMIT_ARENA_PAGES) : 0;
	uint64_t segments = frames > free_frames ?
	    howmany(frames - free_frames, ENGINE_ADMIT_SEGMENT_FRAMES) : 0;
	return (arenas * ENGINE_STORE_BYTES * children +
	    segments * ENGINE_JOURNAL_BYTES * children);
}

/* New catalog arenas the syncing store needs for this many pages. */
static uint64_t
engine_admit_arenas_needed(engine_t *engine, uint64_t pages)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (!engine->persistent || engine->pages->capacity == 0)
		return (0);
	uint64_t free_pages = engine_admit_free_pages(engine);
	return (pages > free_pages ?
	    howmany(pages - free_pages, ENGINE_ADMIT_ARENA_PAGES) : 0);
}

typedef struct engine_admit_slots {
	uint64_t stripe;
	uint64_t bytes;
	uint64_t slots;
} engine_admit_slots_t;

static void
engine_admit_slots_count(void *arg, uint64_t p, uint64_t size)
{
	engine_admit_slots_t *s = arg;
	uint64_t start = roundup(p, s->stripe);
	if (start < p || start - p >= size)
		return;
	s->slots += (size - (start - p)) / s->bytes;
}

/*
 * Disjoint runs engine_pages_grow() could take for a new arena: each is
 * ENGINE_STORE_BYTES per child, contiguous and stripe-aligned in the
 * physical free tree.  Total free bytes alone do not guarantee one.
 */
static uint64_t
engine_admit_arena_slots(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t children = engine->vd->vdev_children;
	engine_admit_slots_t s = { .stripe = ENGINE_STORE_PAGE * children,
	    .bytes = ENGINE_STORE_BYTES * children };
	zfs_range_tree_walk(engine->free, engine_admit_slots_count, &s);
	return (s.slots);
}

/*
 * As engine_admit_arena_slots(), with the space the reuse deferral holds
 * back counted as free: the runs there once the deferred ranges return.
 */
static uint64_t
engine_admit_arena_slots_deferred(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t children = engine->vd->vdev_children;
	engine_admit_slots_t s = { .stripe = ENGINE_STORE_PAGE * children,
	    .bytes = ENGINE_STORE_BYTES * children };
	zfs_range_tree_t *rt = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	zfs_range_tree_walk(engine->free, zfs_range_tree_add, rt);
	zfs_range_tree_walk(engine->reuse_open, zfs_range_tree_add, rt);
	for (uint_t i = 0; i < ENGINE_REUSE_BATCHES; i++)
		zfs_range_tree_walk(engine->reuse_batch[i], zfs_range_tree_add,
		    rt);
	zfs_range_tree_walk(rt, engine_admit_slots_count, &s);
	zfs_range_tree_vacate(rt, NULL, NULL);
	zfs_range_tree_destroy(rt);
	return (s.slots);
}

/*
 * A restart behind the reuse deferral ends.  The first one marks the synced
 * txg: what was deferred then is back TXG_DEFER_SIZE + 1 txgs later (the
 * store of the next txg stages it and the uberblock TXG_DEFER_SIZE txgs
 * after that releases it, both written on an idle pool too), so a
 * request that still misses is short of space, not of time: the converter
 * and a syncing growth take free, fence reclaims refill the deferral.  The
 * synced txg also ends the restarts of a frozen pool, which syncs txgs
 * without uberblocks.  As admit_arena_wait_gen, the mark is the engine's,
 * not the transaction's, since a NOWAIT caller restarts with a new dmu_tx.
 * It is cleared only by an admission that finds nothing deferred: one that
 * fits would let small requests reset the wait of a large one.
 */
static boolean_t
engine_admit_defer_wait(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t synced = spa_last_synced_txg(engine->spa);
	if (engine->admit_defer_txg == 0)
		engine->admit_defer_txg = MAX(synced, 1);
	return (synced < engine->admit_defer_txg + TXG_DEFER_SIZE + 1);
}

/*
 * A net free's wait behind the runs held for other txgs' stores ends.  A
 * heuristic bound: the first wait marks the synced txg, and the client
 * charges out then belong to txgs that have stored TXG_CONCURRENT_STATES
 * + 1 synced txgs later (their callbacks, which return the charge and
 * trim, run shortly after), so a request still short then takes back what
 * it needs rather than fail.  A waiting net free renews the mark about
 * once a synced txg (dmu_tx_wait); a mark no wait renewed for 2 *
 * TXG_CONCURRENT_STATES synced txgs is over and the next wait starts
 * afresh (a wait within that window shares the running bound).  A
 * take-back, or nothing held, clears it.
 */
static boolean_t
engine_admit_held_wait(engine_t *engine, uint64_t *mark, uint64_t *last)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t synced = spa_last_synced_txg(engine->spa);
	if (*mark == 0 || synced > *last + 2 * TXG_CONCURRENT_STATES)
		*mark = MAX(synced, 1);
	*last = synced;
	return (synced < *mark + TXG_CONCURRENT_STATES + 1);
}

static uint64_t
engine_admit_pages_total(engine_t *engine)
{
	uint64_t total = ENGINE_ADMIT_SYNC_PAGES;
	for (uint_t c = 0; c < PR19262_ADMIT_CLASSES; c++)
		total += engine->admit[c].m_pages;
	return (total);
}

static uint64_t
engine_admit_frames_total(engine_t *engine)
{
	uint64_t total = ENGINE_ADMIT_SYNC_FRAMES;
	for (uint_t c = 0; c < PR19262_ADMIT_CLASSES; c++)
		total += engine->admit[c].j_frames;
	return (total);
}

/*
 * Reserved runs.  An admission that passes the arena test below sets aside
 * the runs it counted: while any charge is out, as many as the charge
 * total of every class needs new arenas for (engine_admit_arenas_needed(),
 * the standing sync reserve included, as the test counts it); with no
 * charge out, none, so an idle engine holds nothing and the standby
 * refills as before.  One pool serves every txg not yet stored, since any
 * store may grow into any of them.  A free-tree consumer between the
 * admission and the store (data allocations, a copy stage, a journal
 * roll, the standby fill) no longer bites them.  The runs past the need
 * go back to free at the next admission or page release (a txg's callback
 * once it is stored, a transaction's rollback, the converter's batch
 * release), so also after free pages grew (a fence's GC).  The stores
 * still to come consume at most the outstanding charges' pages, the free
 * pages first, so the need over the current free pages is still an upper
 * bound and the trim keeps every run they can need by the charges' count.
 * Two exceptions predate L15b: syncing-context growth admission never
 * sees also takes a held run first (the standby backs that work, as
 * before), and a converter batch released before its store (convert.c)
 * already gave up its hold on the arena test.  Unload drops the set with
 * store_extents; a load starts with none.
 */
static uint64_t
engine_admit_runs_target(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t pages = engine_admit_pages_total(engine);
	ASSERT3U(pages, >=, ENGINE_ADMIT_SYNC_PAGES);
	return (pages <= ENGINE_ADMIT_SYNC_PAGES ? 0 :
	    engine_admit_arenas_needed(engine, pages));
}

static void
engine_admit_runs_trim(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine_pages_reserved_runs_locked(engine) == 0)
		return;
	engine_pages_reserved_return_locked(engine,
	    engine_admit_runs_target(engine));
}

/*
 * Held runs others than the converter count on: the target the charges of
 * every other class would set (none once only the sync reserve is left),
 * at most what is held.  A converter copy waits only for these; the rest
 * its own rows need (a heuristic: a partly used arena shared with others
 * is counted as theirs).
 */
static uint64_t
engine_admit_runs_for_others(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t held = engine_pages_reserved_runs_locked(engine);
	if (held == 0)
		return (0);
	uint64_t pages = engine_admit_pages_total(engine);
	uint64_t conv = engine->admit[PR19262_ADMIT_CONVERTER].m_pages;
	uint64_t others = pages - MIN(pages, conv);
	uint64_t need = others <= ENGINE_ADMIT_SYNC_PAGES ? 0 :
	    engine_admit_arenas_needed(engine, others);
	return (MIN(held, need));
}

/*
 * The held runs admitted load still counts on: at most the arena count of
 * the charge total (a run held past it, until the next trim, counts as
 * used).  Whatever reads free runs and subtracts the admitted pages (the
 * L13 bulk and L14 discard budgets, free bytes below) adds these back, so
 * the admitted load is subtracted once, as before L15b.
 */
static uint64_t
engine_admit_runs_counted(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t held = engine_pages_reserved_runs_locked(engine);
	return (held == 0 ? 0 : MIN(held, engine_admit_arenas_needed(engine,
	    engine_admit_pages_total(engine))));
}

/*
 * Free physical bytes for client and converter admission, the report and
 * AVAIL: the free tree plus the reserved runs the growth term still
 * counts.  The growth term (engine_admit_growth) protects every arena the
 * charges need, the reserved ones included, so these bytes are never given
 * to data and the results are those of the same state with the runs left
 * in free.  A run held past the need (until the next trim) counts as used.
 * A net free has no growth term: it is admitted against the free tree.
 */
static uint64_t
engine_admit_free_bytes(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	return (zfs_range_tree_space(engine->free) +
	    engine_admit_runs_counted(engine) * ENGINE_STORE_BYTES *
	    engine->vd->vdev_children);
}

/*
 * Standing reserve: exact parity growth of every current backing weaker
 * than the planned parity, a worst case for legacy space whose block sizes
 * are unknown until classified, and one converter batch.
 */
static uint64_t
engine_admit_owner_growth_to(engine_t *engine, const engine_owner_t *e,
    uint64_t parity, boolean_t *weak)
{
	vdev_t *vd = engine->vd;
	engine_version_t *b = e->cur;
	*weak = parity != 0 && b != NULL && !e->retired &&
	    !e->aborted && b->parity < parity;
	if (!*weak)
		return (0);
	uint64_t psize = vdev_asize_to_psize_txg(vd, b->size, b->birth);
	uint64_t stronger = pr19262_vdev_asize_for_parity(vd, psize,
	    b->width, parity);
	return (stronger > b->size ? stronger - b->size : 0);
}

static uint64_t
engine_admit_owner_growth(engine_t *engine, const engine_owner_t *e,
    boolean_t *weak)
{
	return (engine_admit_owner_growth_to(engine, e, engine->admit_parity,
	    weak));
}

static void
engine_admit_owner_forget(engine_t *engine, engine_owner_t *e)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->admit_cached_parity == engine->admit_parity) {
		VERIFY3U(engine->admit_reserve_growth, >=, e->admit_growth);
		VERIFY3U(engine->admit_reserve_weak, >=, e->admit_weak);
		engine->admit_reserve_growth -= e->admit_growth;
		engine->admit_reserve_weak -= e->admit_weak;
	}
	e->admit_growth = 0;
	e->admit_weak = B_FALSE;
	engine->admit_refreshed = UINT64_MAX;
}

/* Refresh before a catalog-settled owner leaves the shared dirty queue. */
static void
engine_admit_owner_update(engine_t *engine, engine_owner_t *e)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (!e->admit_dirty ||
	    engine->admit_cached_parity != engine->admit_parity)
		return;
	engine_admit_owner_forget(engine, e);
	boolean_t weak;
	e->admit_growth = engine_admit_owner_growth(engine, e, &weak);
	e->admit_weak = weak;
	e->admit_dirty = B_FALSE;
	engine->admit_reserve_growth += e->admit_growth;
	engine->admit_reserve_weak += e->admit_weak;
	engine->admit_refreshed = UINT64_MAX;
	atomic_inc_64(&engine->admit_cost.updates);
}

static void
engine_admit_refresh(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->admit_refreshed == engine->revision &&
	    engine->admit_first == NULL && engine->admit_reserve != 0 &&
	    engine->admit_cached_parity == engine->admit_parity)
		return;
	if (engine->admit_cached_parity != engine->admit_parity) {
		engine->admit_reserve_growth = engine->admit_reserve_weak = 0;
		engine->admit_cached_parity = engine->admit_parity;
		atomic_inc_64(&engine->admit_cost.rebuilds);
		for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
		    e = AVL_NEXT(&engine->owners, e)) {
			e->admit_growth = e->admit_weak = 0;
			e->admit_dirty = B_TRUE;
			engine_admit_owner_update(engine, e);
			atomic_inc_64(&engine->admit_cost.rebuilt);
		}
	} else {
		for (engine_owner_t *e = engine->admit_first; e != NULL;
		    e = list_next(&engine->dirty, e)) {
			engine_admit_owner_update(engine, e);
			atomic_inc_64(&engine->admit_cost.walked);
		}
	}
	engine->admit_first = NULL;
	if (engine_admit_checking()) {
		uint64_t growth = 0, weak = 0;
		for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
		    e = AVL_NEXT(&engine->owners, e)) {
			boolean_t w;
			growth += engine_admit_owner_growth(engine, e, &w);
			weak += w;
			ENGINE_STORE_CHECK(!e->admit_dirty);
		}
		ENGINE_STORE_CHECK(growth == engine->admit_reserve_growth);
		ENGINE_STORE_CHECK(weak == engine->admit_reserve_weak);
		atomic_inc_64(&engine->admit_cost.checked);
	}
	uint64_t legacy = engine->admit_parity == 0 ? 0 :
	    zfs_range_tree_space(engine->legacy) *
	    ENGINE_ADMIT_LEGACY_GROWTH_PCT / 100;
	engine->admit_reserve_legacy = legacy;
	engine->admit_reserve = engine->admit_reserve_growth + legacy +
	    engine->admit_batch;
	engine->admit_refreshed = engine->revision;
}

/* Free space of the other normal-class top-level vdevs (native). */
static uint64_t
engine_admit_foreign_free(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->lock));
	if (!engine->foreign_normal)
		return (0);
	vdev_t *root = spa->spa_root_vdev;
	uint64_t total = 0;
	for (uint64_t c = 0; c < root->vdev_children; c++) {
		vdev_t *vd = root->vdev_child[c];
		if (vd == engine->vd || vd->vdev_islog || vd->vdev_mg == NULL ||
		    vd->vdev_mg->mg_class != spa_normal_class(spa))
			continue;
		mutex_enter(&vd->vdev_stat_lock);
		uint64_t space = vd->vdev_stat.vs_space;
		uint64_t alloc = vd->vdev_stat.vs_alloc;
		mutex_exit(&vd->vdev_stat_lock);
		total += space > alloc ? space - alloc : 0;
	}
	return (total);
}

static uint64_t
engine_admit_protected(engine_t *engine, uint_t class)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	switch (class) {
	case PR19262_ADMIT_CLIENT:
		engine_admit_refresh(engine);
		return (ENGINE_ADMIT_SYNC_BYTES + ENGINE_ADMIT_LOG_BYTES +
		    engine->admit_reserve);
	case PR19262_ADMIT_NETFREE:
		return (0);
	default:
		return (ENGINE_ADMIT_SYNC_BYTES + ENGINE_ADMIT_LOG_BYTES);
	}
}

/* Set the planned parity and batch the standing reserve accounts for. */
void
pr19262_engine_plan(spa_t *spa, uint64_t parity, uint64_t batch)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->spa == spa && spa != NULL) {
		engine->admit_parity = parity;
		engine->admit_batch = batch != 0 ? batch : ENGINE_ADMIT_BATCH;
		engine->admit_refreshed = UINT64_MAX;
		engine_admit_refresh(engine);
	}
	mutex_exit(&engine->lock);
}

/* Pages one new catalog row may cost: the COW path plus a split. */
uint64_t
pr19262_engine_admit_row_pages(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	uint64_t pages = engine->spa == spa && engine->persistent ?
	    engine->pages->levels + 2 : 0;
	mutex_exit(&engine->lock);
	return (pages);
}

/*
 * Catalog pages the store of one txg may need for rows admitted into it.
 * Each row's own COW path is levels + 2 pages (the old per-write charge),
 * but a store copies each tree page at most once per txg (engine_mcow).
 * The tree the store will copy is today's (used) plus the new live pages
 * the earlier txgs not yet stored may add (ahead: the sum of their growth,
 * not of their charges, whose copies replace live pages one for one), so
 * the copies are at most base = used + ahead.  This txg's own growth is a
 * new sibling per leaf a row lands in full -- at most MIN(rows, base), and
 * at most one per ENGINE_M_ROWS rows the tree can hold by then (live plus
 * the rows of the earlier txgs and this one), since only a full leaf
 * splits -- plus one path per half-full leaf of inserted rows; *growth
 * returns it for later txgs' ahead.  Small writes in one txg share leaves
 * and paths; charging each its own path over-reserved hundreds of times
 * and refused writes on empty pools.
 */
uint64_t
pr19262_engine_admit_txg_pages(spa_t *spa, uint64_t rows, uint64_t ahead,
    uint64_t ahead_rows, uint64_t *growth)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	uint64_t pages = 0;
	*growth = 0;
	if (engine->spa == spa && engine->persistent) {
		uint64_t path = engine->pages->levels + 2;
		/*
		 * Work outside the txg budgets may also add pages and rows
		 * before this txg's store: converter batches (their reserved
		 * pages, and rows at one path each) and syncing-context work
		 * (up to its standing page reserve per store).
		 */
		uint64_t conv = engine->admit[PR19262_ADMIT_CONVERTER].m_pages;
		uint64_t base = engine->pages->capacity -
		    engine->pages->available + ahead + conv +
		    ENGINE_ADMIT_SYNC_PAGES;
		uint64_t splits = (howmany(rows, MAX(ENGINE_M_ROWS / 2, 1)) +
		    1) * path;
		/*
		 * Rows physically in leaves: live rows, tombstoned head rows
		 * (kept until expiry) and legacy version tombstones (never
		 * deleted), plus the rows that may arrive first.
		 */
		uint64_t present = engine->pages->live +
		    avl_numnodes(&engine->tombstones) +
		    engine->pages->legacy_dead + ahead_rows + rows +
		    conv / MAX(path, 1) + ENGINE_ADMIT_SYNC_PAGES *
		    ENGINE_M_ROWS / MAX(path, 1);
		uint64_t full = present / MAX(ENGINE_M_ROWS, 1) + 1;
		*growth = MIN(MIN(rows, base), full) + splits;
		pages = MIN(rows * path, base + *growth);
		*growth = MIN(*growth, pages);
	}
	mutex_exit(&engine->lock);
	return (pages);
}

/*
 * L18: would this converter request fit with no client page or frame
 * charge outstanding?  Fills the alone_* fields of *r.
 */
static boolean_t
engine_admit_conv_alone(engine_t *engine, uint64_t free, uint64_t need,
    uint64_t m_pages, uint64_t j_frames, engine_conv_refusal_t *r)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t pages = ENGINE_ADMIT_SYNC_PAGES + m_pages;
	uint64_t frames = ENGINE_ADMIT_SYNC_FRAMES + j_frames;
	for (uint_t k = 0; k < PR19262_ADMIT_CLASSES; k++) {
		if (k == PR19262_ADMIT_CLIENT)
			continue;
		pages += engine->admit[k].m_pages;
		frames += engine->admit[k].j_frames;
	}
	uint64_t deferred = engine_reuse_bytes(engine);
	uint64_t a_growth = engine_admit_growth(engine, pages, frames);
	uint64_t a_arenas = engine_admit_arenas_needed(engine, pages);
	if (m_pages != 0 && engine->persistent &&
	    engine->pages->capacity != 0 && !engine->store_standby_valid &&
	    engine->store_headroom_low && pages +
	    ENGINE_ADMIT_HEADROOM_PAGES > engine_admit_free_pages(engine))
		a_arenas = MAX(a_arenas, 1);
	/*
	 * L15b: the runs held outside free count, as in the admission's
	 * arena check; with no client charge out they would be trimmed back.
	 */
	uint64_t a_held = engine_pages_reserved_runs_locked(engine);
	uint64_t a_slots = a_arenas == 0 ? 0 : a_held >= a_arenas ? a_held :
	    a_held + (deferred != 0 ?
	    engine_admit_arena_slots_deferred(engine) :
	    engine_admit_arena_slots(engine));
	uint64_t later = free + deferred;
	uint64_t a_protected = ENGINE_ADMIT_SYNC_BYTES +
	    ENGINE_ADMIT_LOG_BYTES + a_growth;
	r->alone_growth = a_growth;
	r->alone_arenas = a_arenas;
	r->alone_slots = a_slots;
	r->deferred = deferred;
	/*
	 * Outstanding credit stays in the need: as clients allocate it free
	 * shrinks by the same amount, and net-free credit is not held back.
	 */
	return (a_slots >= a_arenas && later >= a_protected &&
	    need <= later - a_protected);
}

/*
 * L18: a converter request admission refused.  It fits alone when it
 * would fit with no client page or frame charge outstanding (those of open
 * and syncing client transactions, released as their txgs sync; their byte
 * credit stays counted) and with the reuse deferral returned: then clients'
 * charges starve it (the arena-run check, see A26 in L18's DESIGN), and
 * the answer is ERESTART, so that the converter retries after a sync with
 * priority (pr19262_engine_admit_priority) instead of ending the
 * conversion with ENOSPC.  Otherwise ENOSPC, as before.  The refusal is
 * kept for the converter's report.
 */
noinline static int
engine_admit_conv_refused(engine_t *engine, uint64_t reason, uint64_t free,
    uint64_t reserved, uint64_t need, uint64_t growth, uint64_t arenas,
    uint64_t slots, uint64_t p_bytes, uint64_t m_pages, uint64_t j_frames)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_conv_refusal_t *r = &engine->admit_conv_last;
	boolean_t alone = engine_admit_conv_alone(engine, free, need, m_pages,
	    j_frames, r);
	r->reason = reason;
	r->alone = alone;
	r->free = free;
	r->reserved = reserved;
	r->need = need;
	r->growth = growth;
	r->arenas = arenas;
	r->slots = slots;
	r->client_pages = engine->admit[PR19262_ADMIT_CLIENT].m_pages;
	r->p_bytes = p_bytes;
	r->m_pages = m_pages;
	r->j_frames = j_frames;
	engine->admit_conv_refusals++;
	engine->admit_conv_alone += alone;
	return (alone ? ERESTART : ENOSPC);
}

/*
 * Admit a vector against free physical space.  reserved is the caller's
 * outstanding unconsumed physical credit (the transaction layer's view);
 * the converter's own batch reservations are added here.  Pages and
 * frames are reserved on success; physical bytes are tracked here only for
 * the converter, whose copies draw directly on the physical free tree.
 */
int
pr19262_engine_admit(spa_t *spa, uint_t class, uint64_t reserved,
    uint64_t p_bytes, uint64_t m_pages, uint64_t j_frames)
{
	if (spa == NULL)
		return (0);
	engine_t *engine = spa->spa_reloc;
	VERIFY3U(class, <, PR19262_ADMIT_CLASSES);
	mutex_enter(&engine->lock);
	if (engine->spa != spa || spa == NULL) {
		mutex_exit(&engine->lock);
		return (0);
	}
	/*
	 * L18: a converter pass that clients starved runs with priority: new
	 * client charges restart behind a sync (as behind a fence) until that
	 * pass ends.  Net frees pass; they only return space.
	 */
	if (class == PR19262_ADMIT_CLIENT && engine->admit_conv_priority) {
		engine->admit_conv_priority_waits++;
		mutex_exit(&engine->lock);
		return (ERESTART);
	}
	engine_admit_class_t *c = &engine->admit[class];
	engine_admit_runs_trim(engine);
	if (engine_pages_reserved_runs_locked(engine) == 0)
		engine->netfree_wait_mark = 0;
	/* A net free can allocate only from the free tree. */
	uint64_t free = class == PR19262_ADMIT_NETFREE ?
	    zfs_range_tree_space(engine->free) :
	    engine_admit_free_bytes(engine);
	/* Nothing deferred: no restart behind the deferral is pending. */
	if (engine_reuse_bytes(engine) == 0)
		engine->admit_defer_txg = 0;
	/*
	 * A free must not fail for the catalog growth that others' reserved
	 * pages may need: its own rows are paced (dmu_free_long_range) or
	 * few (an unlink), and refusing it only keeps space pinned.
	 */
	uint64_t growth = class == PR19262_ADMIT_NETFREE ? 0 :
	    engine_admit_growth(engine,
	    engine_admit_pages_total(engine) + m_pages,
	    engine_admit_frames_total(engine) + j_frames);
	uint64_t protected = engine_admit_protected(engine, class) + growth;
	uint64_t need = reserved + p_bytes +
	    engine->admit[PR19262_ADMIT_CONVERTER].p_bytes;
	/*
	 * Client data may also land on other normal-class vdevs, which the
	 * allocation guard steers it to once the target reaches its reserve.
	 */
	uint64_t foreign = class == PR19262_ADMIT_CONVERTER ? 0 :
	    engine_admit_foreign_free(spa);
	uint64_t available = (free > protected ? free - protected : 0) +
	    foreign;
	/*
	 * L21: the limit counts only retired bytes a fence could return.  A
	 * checkpoint keeps the rest from every fence until its discard, so a
	 * fence scheduled for them returns nothing and a refusal would answer
	 * ENOSPC with free space left.  Like native checkpointed space they
	 * stay used: out of free (the space test below charges them) and in
	 * the dataset space (pr19262_engine_hidden_dspace).
	 */
	boolean_t past = class == PR19262_ADMIT_CLIENT &&
	    engine->retired >= engine->retired_limit;
	if (past && engine_retired_unpinned_locked(engine, B_TRUE) <
	    engine->retired_limit) {
		past = B_FALSE;
		engine->retired_pinned_passes++;
	}
	if (past) {
		/*
		 * Retired bytes return to the allocator at a fence.  The
		 * engine schedules one itself and the client restarts
		 * behind it (DMU_TX_WAIT waits for a sync and retries).  A
		 * fence that cannot run (read-only or quiesced pool), that
		 * failed, or that reclaimed and released nothing
		 * (fence_stalled; under a checkpoint it still returns what
		 * C's root does not reach) leaves ENOSPC as the answer until
		 * one succeeds and makes room.
		 */
		boolean_t dispatch = !engine->fence_pending &&
		    !engine->fence_quiesced && engine->persistent &&
		    spa_writeable(spa);
		if (dispatch) {
			engine->fence_pending = B_TRUE;
			engine->fence_requests++;
		}
		boolean_t wait = engine->fence_pending &&
		    engine->fence_error == 0 && !engine->fence_stalled;
		if (wait)
			engine->admit_fence_waits++;
		else
			c->rejected++;
		mutex_exit(&engine->lock);
		if (dispatch)
			pr19262_engine_fence_schedule(spa);
		return (wait ? ERESTART : ENOSPC);
	}
	/*
	 * L19: under a pool checkpoint, page work leaves C's catalog reserve
	 * to the syncing-context work that must go on until the discard: the
	 * free pages and the runs past the two kept back must cover what is
	 * admitted, this request, the sync reserve and R_C (a net free, whose
	 * unlinks the reserve is for, only R_disc and a quarter of R_churn).
	 * A client restarts behind a fence (it returns the dead pages C's
	 * root does not reach) while fences shrink the shortfall, then gets
	 * ENOSPC; a net free and the converter get ENOSPC (the converter
	 * does not retry: see engine_convert_starved).
	 */
	if (class == PR19262_ADMIT_CONVERTER)
		engine->ckres_conv_refused = B_FALSE;
	uint64_t ckres = m_pages == 0 ? 0 :
	    engine_ckres_held_locked(spa, engine);
	if (ckres != 0) {
		uint64_t cover = engine_ckres_cover_locked(engine,
		    engine_ckres_runs_cached_locked(engine));
		uint64_t keep = class == PR19262_ADMIT_NETFREE ?
		    engine->ckres_disc + (ckres - engine->ckres_disc) / 4 :
		    ckres;
		uint64_t want = engine_admit_pages_total(engine) + m_pages +
		    ENGINE_FREE_RESERVE_PAGES + keep;
		if (want > cover) {
			uint64_t deficit = want - cover;
			engine->ckres_rejections++;
			boolean_t wait = B_FALSE, dispatch = B_FALSE;
			/*
			 * L22: a client request that fits with no client
			 * charge outstanding restarts behind the next txg
			 * sync, never ENOSPC: those charges (the per-txg page
			 * estimate of open and syncing txgs) return as the
			 * txgs sync, as for L18's converter that fits alone.
			 * It asks no fence (a sync, not a fence, returns
			 * them) and leaves the fence-stall marks below to the
			 * requests that do not fit even then.
			 */
			boolean_t alone = class == PR19262_ADMIT_CLIENT &&
			    deficit <= c->m_pages;
			if (alone) {
				wait = B_TRUE;
				engine->ckres_alone_waits++;
			} else if (class == PR19262_ADMIT_CLIENT) {
				boolean_t stalled =
				    engine->ckres_wait_gen != 0 &&
				    engine->fence_completions >=
				    engine->ckres_wait_gen &&
				    deficit >= engine->ckres_wait_deficit;
				dispatch = !stalled && !engine->fence_pending &&
				    !engine->fence_quiesced &&
				    spa_writeable(spa);
				if (dispatch) {
					engine->fence_pending = B_TRUE;
					engine->fence_requests++;
				}
				wait = !stalled && engine->fence_pending &&
				    engine->fence_error == 0;
				if (wait) {
					engine->ckres_wait_gen =
					    engine->fence_completions + 1;
					engine->ckres_wait_deficit = deficit;
					engine->admit_fence_waits++;
				}
			}
			if (class == PR19262_ADMIT_CONVERTER)
				engine->ckres_conv_refused = B_TRUE;
			if (!wait)
				c->rejected++;
			mutex_exit(&engine->lock);
			if (dispatch)
				pr19262_engine_fence_schedule(spa);
			return (wait ? ERESTART : ENOSPC);
		}
		if (class == PR19262_ADMIT_CLIENT)
			engine->ckres_wait_gen = 0;
	}
	/*
	 * Work whose catalog pages need a new arena is admitted only while
	 * enough arena runs exist (in free, or already set aside), so the
	 * syncing store never meets ENOSPC growing for admitted work: it
	 * fails here, before syncing context.  The runs counted are set
	 * aside on success, so nothing takes them before the store does.
	 * A client restarts behind a fence that may return dead pages; the
	 * converter is told ENOSPC, as for space.
	 */
	uint64_t needed = class == PR19262_ADMIT_NETFREE ? 0 :
	    engine_admit_arenas_needed(engine,
	    engine_admit_pages_total(engine) + m_pages);
	uint64_t arenas = needed;
	/*
	 * The syncing store's standby run is gone and no run is left to
	 * refill it, so the store can only use free pages.  Admitted work
	 * leaves ENGINE_ADMIT_HEADROOM_PAGES of them (beyond the standing
	 * sync reserve) to the syncing-context work admission never sees
	 * (scans, sync tasks, frees) between the fences that return dead
	 * pages; past that it needs a new arena and is refused below, a
	 * client after at most one fence wait.
	 */
	boolean_t headroom = class != PR19262_ADMIT_NETFREE && m_pages != 0 &&
	    engine->persistent && engine->pages->capacity != 0 &&
	    !engine->store_standby_valid && engine->store_headroom_low &&
	    engine_admit_pages_total(engine) + m_pages +
	    ENGINE_ADMIT_HEADROOM_PAGES > engine_admit_free_pages(engine);
	if (headroom)
		arenas = MAX(arenas, 1);
	/* Runs set aside count, and need no walk while they suffice. */
	uint64_t held = engine_pages_reserved_runs_locked(engine);
	uint64_t slots = arenas == 0 ? 0 : held >= arenas ? held :
	    held + engine_admit_arena_slots(engine);
	if (class != PR19262_ADMIT_NETFREE && slots >= arenas)
		engine->admit_arena_wait_gen = 0;
	if (arenas != 0 && slots < arenas) {
		engine->admit_arena_rejections++;
		engine->admit_headroom_rejections += headroom;
		if (class == PR19262_ADMIT_CLIENT) {
			/*
			 * The runs exist but are deferred: a catalog still
			 * lists the space, and it joins free within
			 * TXG_DEFER_SIZE stores.  Restart behind them, as
			 * for a fence, rather than answer ENOSPC.
			 */
			if (engine_reuse_bytes(engine) != 0 &&
			    held + engine_admit_arena_slots_deferred(engine) >=
			    arenas && engine_admit_defer_wait(engine)) {
				engine->admit_restarts++;
				mutex_exit(&engine->lock);
				return (ERESTART);
			}
			/* Pages not covered by free pages or arena runs. */
			uint64_t need_pages = engine_admit_pages_total(engine) +
			    m_pages;
			uint64_t cover = engine_admit_free_pages(engine) +
			    slots * ENGINE_ADMIT_ARENA_PAGES;
			uint64_t deficit = need_pages > cover ?
			    need_pages - cover : 0;
			/*
			 * A fence completed since the last wait and did not
			 * shrink the deficit: waiting again would only loop,
			 * so the answer is ENOSPC.
			 */
			boolean_t stalled = engine->admit_arena_wait_gen != 0 &&
			    engine->fence_completions >=
			    engine->admit_arena_wait_gen &&
			    deficit >= engine->admit_arena_wait_deficit;
			boolean_t dispatch = !stalled &&
			    !engine->fence_pending &&
			    !engine->fence_quiesced && engine->persistent &&
			    spa_writeable(spa);
			if (dispatch) {
				engine->fence_pending = B_TRUE;
				engine->fence_requests++;
			}
			boolean_t wait = !stalled && engine->fence_pending &&
			    engine->fence_error == 0;
			if (wait) {
				/* Completions + 1: nonzero, and the next. */
				engine->admit_arena_wait_gen =
				    engine->fence_completions + 1;
				engine->admit_arena_wait_deficit = deficit;
			}
			if (wait)
				engine->admit_fence_waits++;
			else
				c->rejected++;
			mutex_exit(&engine->lock);
			if (dispatch)
				pr19262_engine_fence_schedule(spa);
			return (wait ? ERESTART : ENOSPC);
		}
		/* Only the converter gets here (a net free needs no arena). */
		c->rejected++;
		int r = engine_admit_conv_refused(engine, 1, free, reserved,
		    need, growth, arenas, slots, p_bytes, m_pages, j_frames);
		mutex_exit(&engine->lock);
		return (r);
	}
	/*
	 * A net free whose own bytes fit only with the runs held for other
	 * txgs' stores (and what the reuse deferral returns) restarts behind
	 * those stores instead of failing a delete: their callbacks return
	 * the runs.  When the deferral alone covers it, the deferral's own
	 * restart below waits instead.  Past the bound, once the whole
	 * request (others' credit included) fits with the held runs, it takes
	 * back, run by run, what it needs (the stores fall back to free runs
	 * and the standby, as before L15b); while others' credit still stands
	 * in the way it restarts behind that.  ENOSPC stays the answer only
	 * when its own bytes would not fit even with every held run and the
	 * deferred bytes.
	 */
	uint64_t held_bytes = held == 0 ? 0 :
	    held * ENGINE_STORE_BYTES * engine->vd->vdev_children;
	uint64_t reuse = held == 0 || class != PR19262_ADMIT_NETFREE ? 0 :
	    engine_reuse_bytes(engine);
	if (class == PR19262_ADMIT_NETFREE && held != 0 &&
	    need - reserved > available &&
	    need - reserved <= available + reuse + held_bytes &&
	    !(reuse != 0 && need <= available + reuse &&
	    engine_admit_defer_wait(engine))) {
		if (engine_admit_held_wait(engine, &engine->netfree_wait_mark,
		    &engine->netfree_wait_last)) {
			engine->admit_held_waits++;
			engine->admit_restarts++;
			mutex_exit(&engine->lock);
			return (ERESTART);
		}
		if (need <= available + held_bytes) {
			while (need > available &&
			    engine_pages_reserved_take_back_locked(engine, 0)) {
				free = zfs_range_tree_space(engine->free);
				available = (free > protected ?
				    free - protected : 0) + foreign;
			}
			engine->netfree_wait_mark = 0;
			if (engine->free_runs_known)
				engine->free_runs_cached =
				    engine_pages_runs_locked(engine);
		} else if (need - reserved <= available + held_bytes ||
		    engine_admit_defer_wait(engine)) {
			/*
			 * Others' credit is still out (restart behind it, as
			 * below), or the deferred bytes are, within their own
			 * wait; past that the checks below answer.
			 */
			engine->admit_restarts++;
			mutex_exit(&engine->lock);
			return (ERESTART);
		}
	}
	if ((free < protected && foreign == 0) || need > available) {
		/*
		 * The reservations of transactions still open or syncing
		 * (reserved, worst-case sizes) are released when their txg
		 * syncs.  A request that fits once they are gone restarts,
		 * as the native accounting does, so that DMU_TX_WAIT waits
		 * for a sync instead of failing.  The converter admits whole
		 * batches and is told ENOSPC, as before.
		 */
		if (class != PR19262_ADMIT_CONVERTER &&
		    reserved != 0 && free >= protected &&
		    need - reserved <= available) {
			engine->admit_restarts++;
			mutex_exit(&engine->lock);
			return (ERESTART);
		}
		/*
		 * The same when only the reuse deferral stands in the way:
		 * what it holds back returns within TXG_DEFER_SIZE stores,
		 * and the request fits then.
		 */
		uint64_t later = free + engine_reuse_bytes(engine);
		if (class != PR19262_ADMIT_CONVERTER && later > free &&
		    (later >= protected || foreign != 0) &&
		    need <= (later > protected ? later - protected : 0) +
		    foreign && engine_admit_defer_wait(engine)) {
			engine->admit_restarts++;
			mutex_exit(&engine->lock);
			return (ERESTART);
		}
		c->rejected++;
		engine->admit_space_rejections++;
		int r = class != PR19262_ADMIT_CONVERTER ? ENOSPC :
		    engine_admit_conv_refused(engine, 2, free, reserved, need,
		    growth, arenas, slots, p_bytes, m_pages, j_frames);
		mutex_exit(&engine->lock);
		return (r);
	}
	if (class == PR19262_ADMIT_CONVERTER)
		c->p_bytes += p_bytes;
	c->m_pages += m_pages;
	c->j_frames += j_frames;
	c->admitted++;
	/*
	 * Set aside the runs just counted, now with this request: the target
	 * is needed (0 with no charge out), and slots >= arenas >= needed
	 * says free holds the ones not yet set aside.
	 */
	uint64_t target = needed == 0 ? 0 : engine_admit_runs_target(engine);
	uint64_t fills = engine->store_reserve_fills;
	while (engine_pages_reserved_runs_locked(engine) < target) {
		boolean_t filled = engine_pages_reserved_fill_locked(engine);
		ASSERT(filled);
		if (!filled)
			break;
	}
	/* Long-free pacing and the discard budget read the cached count. */
	if (engine->store_reserve_fills != fills && engine->free_runs_known)
		engine->free_runs_cached = engine_pages_runs_locked(engine);
	c->peak_p = MAX(c->peak_p, need);
	c->peak_m = MAX(c->peak_m, c->m_pages);
	c->peak_j = MAX(c->peak_j, c->j_frames);
	engine->admit_growth_peak = MAX(engine->admit_growth_peak, growth);
	mutex_exit(&engine->lock);
	return (0);
}

void
pr19262_engine_admit_release(spa_t *spa, uint_t class, uint64_t p_bytes,
    uint64_t m_pages, uint64_t j_frames)
{
	engine_t *engine = spa->spa_reloc;
	VERIFY3U(class, <, PR19262_ADMIT_CLASSES);
	mutex_enter(&engine->lock);
	if (engine->spa == spa && spa != NULL) {
		engine_admit_class_t *c = &engine->admit[class];
		if (class == PR19262_ADMIT_CONVERTER) {
			VERIFY3U(c->p_bytes, >=, p_bytes);
			c->p_bytes -= p_bytes;
		}
		VERIFY3U(c->m_pages, >=, m_pages);
		VERIFY3U(c->j_frames, >=, j_frames);
		c->m_pages -= m_pages;
		c->j_frames -= j_frames;
		/* Pages returned: the pooled runs past the need go back. */
		if (m_pages != 0)
			engine_admit_runs_trim(engine);
	}
	mutex_exit(&engine->lock);
}

/*
 * Report the admission state and verify its invariant: free physical space
 * never drops below what sync-context metadata, log blocks, the standing
 * conversion reserve and the growth of outstanding reservations need.
 */
void
pr19262_engine_admit_report(spa_t *spa, const char *label,
    pr19262_engine_admit_report_t *out)
{
	engine_t *engine = spa->spa_reloc;
	memset(out, 0, sizeof (*out));
	mutex_enter(&engine->lock);
	VERIFY3P(spa, ==, engine->spa);
	engine_admit_refresh(engine);
	out->free = engine_admit_free_bytes(engine);
	out->sync_bytes = ENGINE_ADMIT_SYNC_BYTES;
	out->log_bytes = ENGINE_ADMIT_LOG_BYTES;
	out->reserve = engine->admit_reserve;
	out->reserve_growth = engine->admit_reserve_growth;
	out->reserve_legacy = engine->admit_reserve_legacy;
	out->reserve_weak = engine->admit_reserve_weak;
	out->batch = engine->admit_batch;
	out->parity = engine->admit_parity;
	out->pages_total = engine_admit_pages_total(engine);
	out->frames_total = engine_admit_frames_total(engine);
	out->pages_free =
	    engine->persistent ? engine_admit_free_pages(engine) : 0;
	out->frames_free =
	    engine->persistent ? engine_admit_free_frames(engine) : 0;
	out->growth = engine_admit_growth(engine, out->pages_total,
	    out->frames_total);
	out->growth_peak = engine->admit_growth_peak;
	out->levels = engine->pages->levels;
	out->converter_bytes = engine->admit[PR19262_ADMIT_CONVERTER].p_bytes;
	out->space_rejections = engine->admit_space_rejections;
	out->restarts = engine->admit_restarts;
	out->fence_requests = engine->fence_requests;
	out->fence_auto = engine->fence_auto;
	out->fence_waits = engine->admit_fence_waits;
	for (uint_t c = 0; c < PR19262_ADMIT_CLASSES; c++)
		out->classes[c] = engine->admit[c];
	out->protected = ENGINE_ADMIT_SYNC_BYTES + ENGINE_ADMIT_LOG_BYTES +
	    engine->admit_reserve + out->growth;
	out->foreign = engine_admit_foreign_free(spa);
	out->guarded = engine->alloc_guarded;
	uint64_t runs[10] = { engine_pages_reserved_runs_locked(engine),
	    engine->persistent ? engine_admit_arena_slots(engine) : 0,
	    engine->store_reserve_fills, engine->store_reserve_uses,
	    engine->store_reserve_returns, engine->store_reserve_peak,
	    engine->store_standby_valid, engine->admit_held_waits,
	    engine->copy_held_waits, engine->admit_held_takes };
	mutex_exit(&engine->lock);
	pr19262_printf("PR19262_ADMIT label=%s free=%llu protected=%llu "
	    "foreign=%llu guarded=%llu "
	    "sync=%llu log=%llu reserve=%llu reserve_growth=%llu "
	    "reserve_legacy=%llu reserve_weak=%llu batch=%llu parity=%llu "
	    "growth=%llu growth_peak=%llu pages_total=%llu pages_free=%llu "
	    "frames_total=%llu frames_free=%llu levels=%llu "
	    "converter_bytes=%llu space_rejections=%llu "
	    "client=%llu/%llu/%llu/%llu netfree=%llu/%llu "
	    "converter=%llu/%llu/%llu/%llu restarts=%llu "
	    "fence=%llu/%llu/%llu\n", label,
	    (u_longlong_t)out->free, (u_longlong_t)out->protected,
	    (u_longlong_t)out->foreign, (u_longlong_t)out->guarded,
	    (u_longlong_t)out->sync_bytes, (u_longlong_t)out->log_bytes,
	    (u_longlong_t)out->reserve, (u_longlong_t)out->reserve_growth,
	    (u_longlong_t)out->reserve_legacy,
	    (u_longlong_t)out->reserve_weak, (u_longlong_t)out->batch,
	    (u_longlong_t)out->parity, (u_longlong_t)out->growth,
	    (u_longlong_t)out->growth_peak, (u_longlong_t)out->pages_total,
	    (u_longlong_t)out->pages_free, (u_longlong_t)out->frames_total,
	    (u_longlong_t)out->frames_free, (u_longlong_t)out->levels,
	    (u_longlong_t)out->converter_bytes,
	    (u_longlong_t)out->space_rejections,
	    (u_longlong_t)out->classes[0].admitted,
	    (u_longlong_t)out->classes[0].rejected,
	    (u_longlong_t)out->classes[0].peak_m,
	    (u_longlong_t)out->classes[0].peak_j,
	    (u_longlong_t)out->classes[1].admitted,
	    (u_longlong_t)out->classes[1].rejected,
	    (u_longlong_t)out->classes[2].admitted,
	    (u_longlong_t)out->classes[2].rejected,
	    (u_longlong_t)out->classes[2].peak_m,
	    (u_longlong_t)out->classes[2].peak_j,
	    (u_longlong_t)out->restarts, (u_longlong_t)out->fence_requests,
	    (u_longlong_t)out->fence_auto, (u_longlong_t)out->fence_waits);
	pr19262_printf("PR19262_ADMIT_CACHE label=%s rebuilds=%llu "
	    "rebuilt=%llu walked=%llu updates=%llu checked=%llu "
	    "pages_checked=%llu verify=%u owner_size=%llu version_size=%llu\n",
	    label, (u_longlong_t)atomic_load_64(&engine->admit_cost.rebuilds),
	    (u_longlong_t)atomic_load_64(&engine->admit_cost.rebuilt),
	    (u_longlong_t)atomic_load_64(&engine->admit_cost.walked),
	    (u_longlong_t)atomic_load_64(&engine->admit_cost.updates),
	    (u_longlong_t)atomic_load_64(&engine->admit_cost.checked),
	    (u_longlong_t)atomic_load_64(&engine->admit_cost.pages_checked),
	    engine_admit_checking(), (u_longlong_t)sizeof (engine_owner_t),
	    (u_longlong_t)sizeof (engine_version_t));
	pr19262_printf("PR19262_ADMIT_RUNS label=%s reserved=%llu "
	    "free_runs=%llu fills=%llu uses=%llu returns=%llu peak=%llu "
	    "standby=%llu held_waits=%llu copy_waits=%llu takes=%llu\n",
	    label, (u_longlong_t)runs[0],
	    (u_longlong_t)runs[1], (u_longlong_t)runs[2],
	    (u_longlong_t)runs[3], (u_longlong_t)runs[4],
	    (u_longlong_t)runs[5], (u_longlong_t)runs[6],
	    (u_longlong_t)runs[7], (u_longlong_t)runs[8],
	    (u_longlong_t)runs[9]);
	pr19262_flush();
}

/*
 * Physical bytes the native dataset accounting cannot see: raw arenas,
 * parity growth of converted backing, retired backing not yet reclaimed,
 * the parity growth (an upper bound) of every current backing weaker than
 * the planned parity, plus whatever the rest of the admission reserve
 * exceeds the native slop by.  The pool's dspace is reduced by this, so
 * that ordinary (NORMAL) native space checks never promise what admission
 * protects.  The weak backings' growth is consumed for good as the
 * converter runs, so it is hidden on top of the native slop, not inside
 * it: had it shared the slop, a pool filled to AVAIL 0 would convert into
 * its slop and leave AVAIL 0 until clients freed what the conversion took
 * from it, instead of returning freed space at once.  When the conversion
 * is planned before clients fill the pool, the converter so keeps the slop
 * whole, as native device removal does; nothing checks it at the start.
 * The rest still shares the slop: the sync, log and batch bytes and arena
 * growth are transient, and the legacy worst case is an estimate for space
 * not yet classified (once classified, a weak backing's growth is counted
 * as above) -- except while a conversion runs (L18, admit_converting),
 * which was admitted with the legacy worst case on top of the slop.
 * Native AVAIL is thus stricter than client admission by whatever of the
 * slop the deflated rest does not cover.
 */
uint64_t
pr19262_engine_hidden_dspace(spa_t *spa)
{
	if (spa == NULL)
		return (0);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->spa != spa || spa == NULL || engine->vd == NULL) {
		mutex_exit(&engine->lock);
		return (0);
	}
	vdev_t *vd = engine->vd;
	uint64_t free = engine_admit_free_bytes(engine);
	uint64_t unusable = zfs_range_tree_space(engine->unusable);
	uint64_t physical = engine->total - free - unusable;
	uint64_t virtual = vd->vdev_stat.vs_alloc;
	uint64_t hidden = physical > virtual ? physical - virtual : 0;
	/*
	 * L21: the V of checkpoint-pinned retired owners is still allocated
	 * but the DMU freed it, as native checkpointed space, which
	 * dsl_pool_adjustedsize() subtracts (spa_get_checkpoint_space()).
	 */
	hidden += engine_retired_pinned_v_locked(engine);
	/*
	 * Net-free reservations leave the growth term, as at admission
	 * (pr19262_engine_admit gives a net free no growth reserve): a burst
	 * of unlinks reserving pages per transaction must not hide arenas'
	 * worth of space from native ZFS, which would then refuse the very
	 * frees that return space.  Client and converter admission still
	 * protect physical space for every class's pages (their growth term
	 * counts engine_admit_pages_total(), net-free pages included).
	 */
	engine_admit_class_t *nf = &engine->admit[PR19262_ADMIT_NETFREE];
	uint64_t pages = engine_admit_pages_total(engine);
	uint64_t frames = engine_admit_frames_total(engine);
	uint64_t protected = engine_admit_protected(engine,
	    PR19262_ADMIT_CLIENT) +
	    engine_admit_growth(engine, pages - MIN(pages, nf->m_pages),
	    frames - MIN(frames, nf->j_frames));
	/*
	 * Refreshed by engine_admit_protected(); part of protected.  L18:
	 * while a conversion runs its legacy worst case is kept on top of the
	 * slop as well: converting a legacy block costs its real growth, and
	 * the conversion was admitted with that room (engine_convert_room).
	 */
	uint64_t conv = MIN(engine->admit_reserve_growth +
	    (engine->admit_converting ? engine->admit_reserve_legacy : 0),
	    protected);
	uint64_t ratio = vd->vdev_deflate_ratio;
	mutex_exit(&engine->lock);
	if (ratio == 0)
		return (0);
	uint64_t slop = spa->spa_dedup_dspace == ~0ULL ? 0 :
	    spa_get_slop_space(spa);
	uint64_t hidden_d = (hidden >> SPA_MINBLOCKSHIFT) * ratio;
	uint64_t conv_d = (conv >> SPA_MINBLOCKSHIFT) * ratio;
	uint64_t transient_d = ((protected - conv) >> SPA_MINBLOCKSHIFT) *
	    ratio;
	return (hidden_d + conv_d +
	    (transient_d > slop ? transient_d - slop : 0));
}

/*
 * L18: would the last refused converter request fit alone now?  The
 * converter asks after its starved pass's fence and release: the fence
 * may have returned dead pages and runs, so a refusal that did not fit
 * alone may now (then the next pass runs with priority instead of ending
 * the conversion).  reserved is the clients' outstanding credit now.
 */
boolean_t
pr19262_engine_admit_conv_alone(spa_t *spa, uint64_t reserved)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	boolean_t alone = B_FALSE;
	if (engine->spa == spa && spa != NULL) {
		engine_conv_refusal_t *r = &engine->admit_conv_last;
		engine_conv_refusal_t now = *r;
		uint64_t need = reserved + r->p_bytes +
		    engine->admit[PR19262_ADMIT_CONVERTER].p_bytes;
		/* v8 fixup (m2): held runs count, as inside admission. */
		alone = engine_admit_conv_alone(engine,
		    engine_admit_free_bytes(engine), need, r->m_pages,
		    r->j_frames, &now);
		r->alone_after = alone;
	}
	mutex_exit(&engine->lock);
	return (alone);
}

/* L18: a conversion runs (its legacy worst case is hidden on top). */
void
pr19262_engine_admit_converting(spa_t *spa, boolean_t on)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->spa == spa && spa != NULL)
		engine->admit_converting = on;
	mutex_exit(&engine->lock);
}

/* L18: hold new client charges back for the converter's next pass. */
void
pr19262_engine_admit_priority(spa_t *spa, boolean_t on)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->spa == spa && spa != NULL)
		engine->admit_conv_priority = on;
	mutex_exit(&engine->lock);
}

/* L18: the bytes pr19262_engine_hidden_dspace() hides for this plan. */
static int64_t
engine_admit_plan_hidden(uint64_t conv, uint64_t transient, uint64_t ratio,
    uint64_t slop)
{
	uint64_t conv_d = (conv >> SPA_MINBLOCKSHIFT) * ratio;
	uint64_t transient_d = (transient >> SPA_MINBLOCKSHIFT) * ratio;
	return ((int64_t)(conv_d +
	    (transient_d > slop ? transient_d - slop : 0)));
}

/*
 * L18: the room a conversion to parity (with this batch) has on top of the
 * native slop, in native AVAIL units (deflated bytes, signed).  It is the
 * pool's native AVAIL, unclamped: dspace less checkpointed space, deferred
 * frees, used and the slop, where dspace already lost what
 * pr19262_engine_hidden_dspace() hides for the plan in place: the planned
 * weak growth on top of the slop (L17) and whatever the transient rest of
 * the reserve exceeds the slop by.  Less delta: how much more would be
 * hidden for the plan asked for (the weak growth to that parity is walked
 * if it is not the plan in place) with the legacy worst case on top of the
 * slop as well.  So the whole remaining need is counted once: the weak
 * growth and the legacy worst case on top of the slop, the catalog and
 * journal growth the outstanding charges forecast and the batch inside it
 * (the transient part shares the slop, as native sync-context metadata
 * does).  The caller has synced, so that dspace reflects the plan in place.
 */
void
pr19262_engine_convert_room(spa_t *spa, uint64_t parity, uint64_t batch,
    pr19262_engine_room_t *out)
{
	engine_t *engine = spa->spa_reloc;
	dsl_pool_t *dp = spa_get_dsl(spa);
	memset(out, 0, sizeof (*out));
	out->parity = parity;
	out->space = dsl_pool_adjustedsize(dp, ZFS_SPACE_CHECK_NONE);
	out->slop = spa_get_slop_space(spa);
	dsl_dir_t *root = dp->dp_root_dir;
	mutex_enter(&root->dd_lock);
	out->used = dsl_dir_phys(root)->dd_used_bytes;
	mutex_exit(&root->dd_lock);
	out->avail = (int64_t)out->space - (int64_t)out->used -
	    (int64_t)out->slop;
	if (batch == 0)
		batch = ENGINE_ADMIT_BATCH;
	mutex_enter(&engine->lock);
	if (engine->spa != spa || engine->vd == NULL) {
		mutex_exit(&engine->lock);
		out->room = out->avail;
		out->planned = B_TRUE;
		return;
	}
	vdev_t *vd = engine->vd;
	engine_admit_class_t *nf = &engine->admit[PR19262_ADMIT_NETFREE];
	uint64_t pages = engine_admit_pages_total(engine);
	uint64_t frames = engine_admit_frames_total(engine);
	/* As pr19262_engine_hidden_dspace(): refreshes the reserve. */
	uint64_t growth_term = engine_admit_growth(engine,
	    pages - MIN(pages, nf->m_pages),
	    frames - MIN(frames, nf->j_frames));
	uint64_t protected = engine_admit_protected(engine,
	    PR19262_ADMIT_CLIENT) + growth_term;
	/* What hidden_dspace keeps on top of the slop now, and the rest. */
	uint64_t conv = MIN(engine->admit_reserve_growth +
	    (engine->admit_converting ? engine->admit_reserve_legacy : 0),
	    protected);
	uint64_t transient = protected - conv;
	/* sync + log + the growth forecast: the part no plan changes. */
	uint64_t base = protected - MIN(protected,
	    engine->admit_reserve_growth + engine->admit_reserve_legacy +
	    engine->admit_batch);
	out->protected = protected;
	out->growth_term = growth_term;
	out->free = zfs_range_tree_space(engine->free);
	out->store = engine->store_extents != NULL ?
	    zfs_range_tree_space(engine->store_extents) : 0;
	out->journal = engine->journal_extents != NULL ?
	    zfs_range_tree_space(engine->journal_extents) : 0;
	out->retired = engine->retired;
	out->planned = engine->admit_parity == parity &&
	    engine->admit_batch == batch;
	uint64_t growth = engine->admit_reserve_growth;
	uint64_t weak = engine->admit_reserve_weak;
	uint64_t legacy = engine->admit_reserve_legacy;
	if (engine->admit_parity != parity) {
		growth = weak = 0;
		for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
		    e = AVL_NEXT(&engine->owners, e)) {
			boolean_t w;
			growth += engine_admit_owner_growth_to(engine, e,
			    parity, &w);
			weak += w;
		}
		legacy = parity == 0 ? 0 : zfs_range_tree_space(
		    engine->legacy) * ENGINE_ADMIT_LEGACY_GROWTH_PCT / 100;
	}
	uint64_t ratio = vd->vdev_deflate_ratio;
	out->runs = (ENGINE_STORE_BYTES + engine->journal_bytes) *
	    vd->vdev_children;
	mutex_exit(&engine->lock);
	out->ratio = ratio;
	out->growth = growth;
	out->legacy = legacy;
	out->transient = base + batch + legacy;
	out->weak = weak;
	/*
	 * The plan asked for, with the legacy worst case on top of the slop
	 * as well (a converted legacy block costs AVAIL its real growth),
	 * as pr19262_engine_hidden_dspace() hides it while a conversion runs
	 * (admit_converting); less what it hides now.
	 */
	if (ratio != 0) {
		out->delta = engine_admit_plan_hidden(growth + legacy,
		    base + batch, ratio, out->slop) -
		    engine_admit_plan_hidden(conv, transient, ratio, out->slop);
	}
	out->room = out->avail - out->delta;
}

#ifndef _KERNEL
/* Journal roll test (L10): see vdev_reloc_engine.h. */
int
pr19262_engine_test_jroll_probe(spa_t *spa, pr19262_jroll_probe_t *out)
{
	engine_t *engine = spa->spa_reloc;
	memset(out, 0, sizeof (*out));
	mutex_enter(&engine->journal_lock);
	mutex_enter(&engine->lock);
	if (engine->spa != spa || !engine->persistent) {
		mutex_exit(&engine->lock);
		mutex_exit(&engine->journal_lock);
		return (EINVAL);
	}
	uint64_t children = engine->vd->vdev_children;
	out->seq = engine->journal_seq;
	out->frames = engine->journal_bytes / ENGINE_STORE_PAGE;
	out->segments = engine->journal_segment_count;
	out->rolls = atomic_load_64(&engine->journal_cost.rolls);
	out->error = engine->journal_error;
#ifdef ENGINE_JOURNAL_ROLL_RETRY
	out->retry = 1;
	out->pending = engine->journal_roll_pending;
	out->pending_txg = engine->journal_roll_txg;
	out->enospc = engine->journal_roll_enospc;
	out->waits = engine->journal_roll_waits;
	out->recovered = engine->journal_roll_recovered;
#endif
	out->deferred = engine_reuse_bytes(engine);
	out->standby = engine->store_standby_valid;
	engine_admit_slots_t s = { .stripe = ENGINE_STORE_PAGE * children,
	    .bytes = engine->journal_bytes * children };
	zfs_range_tree_walk(engine->free, engine_admit_slots_count, &s);
	out->runs = s.slots;
	out->free_bytes = zfs_range_tree_space(engine->free);
	mutex_exit(&engine->lock);
	mutex_exit(&engine->journal_lock);
	out->synced = spa_last_synced_txg(spa);
	return (0);
}

/* L14 convert-busy8 cell: see vdev_reloc_engine.h. */
int
pr19262_engine_test_jroll_busy(spa_t *spa, const blkptr_t *bp, int on)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_owner_t *e = engine->spa == spa ?
	    engine_find(engine, DVA_GET_OFFSET(&bp->blk_dva[0])) : NULL;
	if (e == NULL || (!on && e->queued_writers == 0)) {
		mutex_exit(&engine->lock);
		return (ENOENT);
	}
	if (on)
		e->queued_writers++;
	else
		e->queued_writers--;
	mutex_exit(&engine->lock);
	return (0);
}

/* L14 r2(a) cell: see vdev_reloc_engine.h. */
int
pr19262_engine_test_retired_probe(spa_t *spa, pr19262_retired_probe_t *out)
{
	engine_t *engine = spa->spa_reloc;
	memset(out, 0, sizeof (*out));
	mutex_enter(&engine->lock);
	if (engine->spa != spa) {
		mutex_exit(&engine->lock);
		return (EINVAL);
	}
	out->retired = engine->retired;
	out->limit = engine->retired_limit;
	out->draining = engine->checkpoint.ub_txg == 0 &&
	    spa_feature_is_active(spa, SPA_FEATURE_POOL_CHECKPOINT);
	out->stalled = engine->fence_stalled;
	out->fence_error = engine->fence_error;
	out->client_rejected = engine->admit[PR19262_ADMIT_CLIENT].rejected;
	out->arena_rejections = engine->admit_arena_rejections;
	out->headroom_rejections = engine->admit_headroom_rejections;
	out->fence_waits = engine->admit_fence_waits;
	out->fence_requests = engine->fence_requests;
	/* Retired owners a draining fence skips (convert.c release sync). */
	for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
	    e = AVL_NEXT(&engine->owners, e)) {
		if (e->retired && !e->drain_releasable) {
			out->unflagged += e->cur->size;
			out->unflagged_owners++;
		}
	}
	mutex_exit(&engine->lock);
	return (0);
}

static void
engine_test_jroll_holes(void *arg, uint64_t start, uint64_t size)
{
	zfs_range_tree_t *holes = arg;
	uint64_t step = 2ULL << 20;
	uint64_t at = P2ROUNDUP(start, step);
	if (at == start)
		at += step;
	for (; at < start + size; at += step) {
		uint64_t first = MAX(start, at - 4096);
		zfs_range_tree_add(holes, first, at - first);
	}
}

static void
engine_test_jroll_clear(void *arg, uint64_t start, uint64_t size)
{
	zfs_range_tree_clear(arg, start, size);
}

/*
 * op 1: carve, the run deferred; 2: carve, the run held back (no run at
 * all); 3: the held run into the deferral; 0: the holes back.
 */
int
pr19262_engine_test_jroll_squeeze(spa_t *spa, int op, uint64_t *run)
{
	engine_t *engine = spa->spa_reloc;
	uint64_t children = engine->vd->vdev_children;
	uint64_t bytes = engine->journal_bytes * children;
	*run = UINT64_MAX;
	mutex_enter(&engine->lock);
	if (engine->spa != spa || !engine->persistent ||
	    ((op == 1 || op == 2) && engine->test_jholes != NULL) ||
	    ((op == 0 || op == 3) && engine->test_jholes == NULL) ||
	    (op == 3 && engine->test_jheld == 0)) {
		mutex_exit(&engine->lock);
		return (EINVAL);
	}
	if (op == 0 || op == 3) {
		if (op == 3) {
			/* Freed after the durable catalog: deferred. */
			*run = engine->test_jheld - 1;
			engine_reuse_add(engine, *run, bytes);
			engine->test_jheld = 0;
		} else {
			if (engine->test_jheld != 0)
				zfs_range_tree_add(engine->free,
				    engine->test_jheld - 1, bytes);
			engine->test_jheld = 0;
			zfs_range_tree_vacate(engine->test_jholes,
			    zfs_range_tree_add, engine->free);
			zfs_range_tree_destroy(engine->test_jholes);
			engine->test_jholes = NULL;
		}
		mutex_exit(&engine->lock);
		return (0);
	}
	(void) engine_pages_standby_release_locked(engine);
	engine_store_pick_arg_t pick = { children * ENGINE_STORE_PAGE,
	    bytes, UINT64_MAX };
	zfs_range_tree_walk(engine->free, engine_store_pick, &pick);
	if (pick.found == UINT64_MAX) {
		mutex_exit(&engine->lock);
		return (ENOSPC);
	}
	zfs_range_tree_remove(engine->free, pick.found, bytes);
	engine->test_jholes = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	zfs_range_tree_walk(engine->free, engine_test_jroll_holes,
	    engine->test_jholes);
	zfs_range_tree_walk(engine->test_jholes, engine_test_jroll_clear,
	    engine->free);
	if (op == 1) {
		/* Freed after the durable catalog: deferred, as a cut is. */
		engine_reuse_add(engine, pick.found, bytes);
	} else {
		engine->test_jheld = pick.found + 1;
	}
	*run = pick.found;
	mutex_exit(&engine->lock);
	return (0);
}
#endif
