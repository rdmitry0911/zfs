/* SPDX-License-Identifier: CDDL-1.0 */
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
 * Standing reserve: exact parity growth of every current backing weaker
 * than the planned parity, a worst case for legacy space whose block sizes
 * are unknown until classified, and one converter batch.
 */
static uint64_t
engine_admit_owner_growth(engine_t *engine, const engine_owner_t *e,
    boolean_t *weak)
{
	vdev_t *vd = engine->vd;
	engine_version_t *b = e->cur;
	*weak = engine->admit_parity != 0 && b != NULL && !e->retired &&
	    !e->aborted && b->parity < engine->admit_parity;
	if (!*weak)
		return (0);
	uint64_t psize = vdev_asize_to_psize_txg(vd, b->size, b->birth);
	uint64_t stronger = pr19262_vdev_asize_for_parity(vd, psize,
	    b->width, engine->admit_parity);
	return (stronger > b->size ? stronger - b->size : 0);
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
	engine_admit_class_t *c = &engine->admit[class];
	uint64_t free = zfs_range_tree_space(engine->free);
	uint64_t growth = engine_admit_growth(engine,
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
	if (class == PR19262_ADMIT_CLIENT &&
	    engine->retired >= engine->retired_limit) {
		/*
		 * Retired bytes return to the allocator at a fence.  The
		 * engine schedules one itself and the client restarts
		 * behind it (DMU_TX_WAIT waits for a sync and retries).  A
		 * fence that cannot run (checkpoint, read-only pool) or
		 * that reclaimed nothing leaves ENOSPC as the answer until
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
		c->rejected++;
		engine->admit_space_rejections++;
		mutex_exit(&engine->lock);
		return (ENOSPC);
	}
	if (class == PR19262_ADMIT_CONVERTER)
		c->p_bytes += p_bytes;
	c->m_pages += m_pages;
	c->j_frames += j_frames;
	c->admitted++;
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
	out->free = zfs_range_tree_space(engine->free);
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
	pr19262_flush();
}

/*
 * Physical bytes the native dataset accounting cannot see: raw arenas,
 * parity growth of converted backing, retired backing not yet reclaimed,
 * plus whatever the admission reserve exceeds the native slop by.  The
 * pool's dspace is reduced by this, so that AVAIL and admission agree.
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
	uint64_t free = zfs_range_tree_space(engine->free);
	uint64_t unusable = zfs_range_tree_space(engine->unusable);
	uint64_t physical = engine->total - free - unusable;
	uint64_t virtual = vd->vdev_stat.vs_alloc;
	uint64_t hidden = physical > virtual ? physical - virtual : 0;
	uint64_t protected = engine_admit_protected(engine,
	    PR19262_ADMIT_CLIENT) +
	    engine_admit_growth(engine, engine_admit_pages_total(engine),
	    engine_admit_frames_total(engine));
	uint64_t ratio = vd->vdev_deflate_ratio;
	mutex_exit(&engine->lock);
	if (ratio == 0)
		return (0);
	uint64_t slop = spa->spa_dedup_dspace == ~0ULL ? 0 :
	    spa_get_slop_space(spa);
	uint64_t hidden_d = (hidden >> SPA_MINBLOCKSHIFT) * ratio;
	uint64_t protected_d = (protected >> SPA_MINBLOCKSHIFT) * ratio;
	return (hidden_d + (protected_d > slop ? protected_d - slop : 0));
}
