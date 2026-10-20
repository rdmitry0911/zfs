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

static uint64_t
engine_admit_free_pages(void)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	uint64_t free = 0;
	for (engine_marena_t *a = engine_pages.arenas; a != NULL;
	    a = a->next) {
		if (a->pending)
			continue;
		for (uint64_t i = 0; i < a->pages; i++)
			free += engine_pages.bits[a->first + i] == 0;
	}
	return (free);
}

static uint64_t
engine_admit_free_frames(void)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	if (engine.journal_tail == NULL)
		return (0);
	uint64_t capacity = engine.journal_bytes / ENGINE_STORE_PAGE;
	uint64_t used = engine.journal_tail->seq + 3;
	uint64_t free = capacity > used ? capacity - used : 0;
	if (engine.journal_pending != NULL)
		free += ENGINE_ADMIT_SEGMENT_FRAMES;
	return (free);
}

/* Physical bytes the raw arenas may need to absorb these reservations. */
static uint64_t
engine_admit_growth(uint64_t pages, uint64_t frames)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	if (!engine.persistent || engine_pages.capacity == 0)
		return (0);
	uint64_t children = engine.vd->vdev_children;
	uint64_t free_pages = engine_admit_free_pages();
	uint64_t free_frames = engine_admit_free_frames();
	uint64_t arenas = pages > free_pages ?
	    howmany(pages - free_pages, ENGINE_ADMIT_ARENA_PAGES) : 0;
	uint64_t segments = frames > free_frames ?
	    howmany(frames - free_frames, ENGINE_ADMIT_SEGMENT_FRAMES) : 0;
	return (arenas * ENGINE_STORE_BYTES * children +
	    segments * ENGINE_JOURNAL_BYTES * children);
}

static uint64_t
engine_admit_pages_total(void)
{
	uint64_t total = ENGINE_ADMIT_SYNC_PAGES;
	for (uint_t c = 0; c < PR19262_ADMIT_CLASSES; c++)
		total += engine.admit[c].m_pages;
	return (total);
}

static uint64_t
engine_admit_frames_total(void)
{
	uint64_t total = ENGINE_ADMIT_SYNC_FRAMES;
	for (uint_t c = 0; c < PR19262_ADMIT_CLASSES; c++)
		total += engine.admit[c].j_frames;
	return (total);
}

/*
 * Standing reserve: exact parity growth of every current backing weaker
 * than the planned parity, a worst case for legacy space whose block sizes
 * are unknown until classified, and one converter batch.
 */
static void
engine_admit_refresh(void)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	if (engine.admit_refreshed == engine.revision &&
	    engine.admit_reserve != 0)
		return;
	vdev_t *vd = engine.vd;
	uint64_t growth = 0, weak = 0;
	if (engine.admit_parity != 0) {
		for (engine_owner_t *e = avl_first(&engine.owners); e != NULL;
		    e = AVL_NEXT(&engine.owners, e)) {
			engine_version_t *b = e->current;
			if (b == NULL || e->retired || e->aborted ||
			    b->parity >= engine.admit_parity)
				continue;
			uint64_t psize = vdev_asize_to_psize_txg(vd, b->size,
			    b->birth);
			uint64_t stronger = pr19262_vdev_asize_for_parity(vd,
			    psize, b->width, engine.admit_parity);
			if (stronger > b->size)
				growth += stronger - b->size;
			weak++;
		}
	}
	uint64_t legacy = engine.admit_parity == 0 ? 0 :
	    zfs_range_tree_space(engine.legacy) *
	    ENGINE_ADMIT_LEGACY_GROWTH_PCT / 100;
	engine.admit_reserve_growth = growth;
	engine.admit_reserve_legacy = legacy;
	engine.admit_reserve_weak = weak;
	engine.admit_reserve = growth + legacy + engine.admit_batch;
	engine.admit_refreshed = engine.revision;
}

static uint64_t
engine_admit_protected(uint_t class)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	switch (class) {
	case PR19262_ADMIT_CLIENT:
		engine_admit_refresh();
		return (ENGINE_ADMIT_SYNC_BYTES + ENGINE_ADMIT_LOG_BYTES +
		    engine.admit_reserve);
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
	mutex_enter(&engine.lock);
	if (engine.spa == spa && spa != NULL) {
		engine.admit_parity = parity;
		engine.admit_batch = batch != 0 ? batch : ENGINE_ADMIT_BATCH;
		engine.admit_refreshed = UINT64_MAX;
		engine_admit_refresh();
	}
	mutex_exit(&engine.lock);
}

/* Pages one new catalog row may cost: the COW path plus a split. */
uint64_t
pr19262_engine_admit_row_pages(spa_t *spa)
{
	mutex_enter(&engine.lock);
	uint64_t pages = engine.spa == spa && engine.persistent ?
	    engine_pages.levels + 2 : 0;
	mutex_exit(&engine.lock);
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
	VERIFY3U(class, <, PR19262_ADMIT_CLASSES);
	mutex_enter(&engine.lock);
	if (engine.spa != spa || spa == NULL) {
		mutex_exit(&engine.lock);
		return (0);
	}
	engine_admit_class_t *c = &engine.admit[class];
	uint64_t free = zfs_range_tree_space(engine.free);
	uint64_t growth = engine_admit_growth(
	    engine_admit_pages_total() + m_pages,
	    engine_admit_frames_total() + j_frames);
	uint64_t protected = engine_admit_protected(class) + growth;
	uint64_t need = reserved + p_bytes +
	    engine.admit[PR19262_ADMIT_CONVERTER].p_bytes;
	boolean_t blocked = class == PR19262_ADMIT_CLIENT &&
	    engine.retired >= engine.retired_limit;
	if (blocked || free < protected || need > free - protected) {
		c->rejected++;
		if (!blocked)
			engine.admit_space_rejections++;
		mutex_exit(&engine.lock);
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
	engine.admit_growth_peak = MAX(engine.admit_growth_peak, growth);
	mutex_exit(&engine.lock);
	return (0);
}

void
pr19262_engine_admit_release(spa_t *spa, uint_t class, uint64_t p_bytes,
    uint64_t m_pages, uint64_t j_frames)
{
	VERIFY3U(class, <, PR19262_ADMIT_CLASSES);
	mutex_enter(&engine.lock);
	if (engine.spa == spa && spa != NULL) {
		engine_admit_class_t *c = &engine.admit[class];
		if (class == PR19262_ADMIT_CONVERTER) {
			VERIFY3U(c->p_bytes, >=, p_bytes);
			c->p_bytes -= p_bytes;
		}
		VERIFY3U(c->m_pages, >=, m_pages);
		VERIFY3U(c->j_frames, >=, j_frames);
		c->m_pages -= m_pages;
		c->j_frames -= j_frames;
	}
	mutex_exit(&engine.lock);
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
	memset(out, 0, sizeof (*out));
	mutex_enter(&engine.lock);
	VERIFY3P(spa, ==, engine.spa);
	engine_admit_refresh();
	out->free = zfs_range_tree_space(engine.free);
	out->sync_bytes = ENGINE_ADMIT_SYNC_BYTES;
	out->log_bytes = ENGINE_ADMIT_LOG_BYTES;
	out->reserve = engine.admit_reserve;
	out->reserve_growth = engine.admit_reserve_growth;
	out->reserve_legacy = engine.admit_reserve_legacy;
	out->reserve_weak = engine.admit_reserve_weak;
	out->batch = engine.admit_batch;
	out->parity = engine.admit_parity;
	out->pages_total = engine_admit_pages_total();
	out->frames_total = engine_admit_frames_total();
	out->pages_free = engine.persistent ? engine_admit_free_pages() : 0;
	out->frames_free = engine.persistent ? engine_admit_free_frames() : 0;
	out->growth = engine_admit_growth(out->pages_total, out->frames_total);
	out->growth_peak = engine.admit_growth_peak;
	out->levels = engine_pages.levels;
	out->converter_bytes = engine.admit[PR19262_ADMIT_CONVERTER].p_bytes;
	out->space_rejections = engine.admit_space_rejections;
	for (uint_t c = 0; c < PR19262_ADMIT_CLASSES; c++)
		out->classes[c] = engine.admit[c];
	out->protected = ENGINE_ADMIT_SYNC_BYTES + ENGINE_ADMIT_LOG_BYTES +
	    engine.admit_reserve + out->growth;
	mutex_exit(&engine.lock);
	(void) printf("PR19262_ADMIT label=%s free=%llu protected=%llu "
	    "sync=%llu log=%llu reserve=%llu reserve_growth=%llu "
	    "reserve_legacy=%llu reserve_weak=%llu batch=%llu parity=%llu "
	    "growth=%llu growth_peak=%llu pages_total=%llu pages_free=%llu "
	    "frames_total=%llu frames_free=%llu levels=%llu "
	    "converter_bytes=%llu space_rejections=%llu "
	    "client=%llu/%llu/%llu/%llu netfree=%llu/%llu "
	    "converter=%llu/%llu/%llu/%llu\n", label,
	    (u_longlong_t)out->free, (u_longlong_t)out->protected,
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
	    (u_longlong_t)out->classes[2].peak_j);
	(void) fflush(NULL);
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
	mutex_enter(&engine.lock);
	if (engine.spa != spa || spa == NULL || engine.vd == NULL) {
		mutex_exit(&engine.lock);
		return (0);
	}
	vdev_t *vd = engine.vd;
	uint64_t free = zfs_range_tree_space(engine.free);
	uint64_t unusable = zfs_range_tree_space(engine.unusable);
	uint64_t physical = engine.total - free - unusable;
	uint64_t virtual = vd->vdev_stat.vs_alloc;
	uint64_t hidden = physical > virtual ? physical - virtual : 0;
	uint64_t protected = engine_admit_protected(PR19262_ADMIT_CLIENT) +
	    engine_admit_growth(engine_admit_pages_total(),
	    engine_admit_frames_total());
	uint64_t ratio = vd->vdev_deflate_ratio;
	mutex_exit(&engine.lock);
	if (ratio == 0)
		return (0);
	uint64_t slop = spa->spa_dedup_dspace == ~0ULL ? 0 :
	    spa_get_slop_space(spa);
	uint64_t hidden_d = (hidden >> SPA_MINBLOCKSHIFT) * ratio;
	uint64_t protected_d = (protected >> SPA_MINBLOCKSHIFT) * ratio;
	return (hidden_d + (protected_d > slop ? protected_d - slop : 0));
}
