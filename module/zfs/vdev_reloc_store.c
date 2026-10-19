/* SPDX-License-Identifier: CDDL-1.0 */
/* Private userspace persistence experiment, included by the normal engine. */
#define	ENGINE_STORE_MAGIC 0x31393236324d3031ULL
#define	ENGINE_STORE_PAGE 4096ULL
#define	ENGINE_STORE_BYTES (4ULL << 20)
#define	ENGINE_STORE_PAGES (ENGINE_STORE_BYTES / ENGINE_STORE_PAGE)
#define	ENGINE_JOURNAL_BYTES (1ULL << 20)

static uint64_t engine_store_crash_txg;
static void (*engine_store_test_hook)(spa_t *, int);

void
pr19262_engine_store_test_hook(void (*hook)(spa_t *, int))
{
	mutex_enter(&engine.lock);
	engine_store_test_hook = hook;
	mutex_exit(&engine.lock);
}

static void
engine_store_test_phase(spa_t *spa, int phase)
{
	mutex_enter(&engine.lock);
	void (*hook)(spa_t *, int) = engine_store_test_hook;
	mutex_exit(&engine.lock);
	if (hook != NULL)
		hook(spa, phase);
}

void
pr19262_engine_store_crash(uint64_t txg)
{
	engine_store_crash_txg = txg;
}

boolean_t
pr19262_engine_store_ub(const uberblock_t *ub)
{
	return (ub->ub_pr19262[0] == ENGINE_STORE_MAGIC);
}

boolean_t
pr19262_engine_persistent(spa_t *spa)
{
	mutex_enter(&engine.lock);
	boolean_t active = engine.spa == spa && spa != NULL &&
	    engine.persistent;
	mutex_exit(&engine.lock);
	return (active);
}

boolean_t
pr19262_engine_store_dirty(spa_t *spa)
{
	mutex_enter(&engine.lock);
	boolean_t dirty = engine.spa == spa && engine.persistent &&
	    engine.revision != engine.stored_revision;
	mutex_exit(&engine.lock);
	return (dirty);
}

static void
engine_store_pick(void *arg, uint64_t start, uint64_t size)
{
	uint64_t *found = arg;
	uint64_t stripe = engine.vd->vdev_children * ENGINE_STORE_PAGE;
	uint64_t bytes = engine.vd->vdev_children *
	    (engine.arena_bytes == 0 ? ENGINE_STORE_BYTES :
	    ENGINE_JOURNAL_BYTES);
	uint64_t p = roundup(start, stripe);
	if (p >= start && p - start <= size && bytes <= size - (p - start))
		*found = p;
}

void
pr19262_engine_store_enable(spa_t *spa)
{
	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_WRITER);
	mutex_enter(&engine.lock);
	VERIFY3P(spa, ==, engine.spa);
	VERIFY(!engine.persistent);
	/* The raw arena covers whole physical rows on every child. */
	VERIFY3U(engine.vd->vdev_ashift, ==, 12);
	uint64_t p = UINT64_MAX;
	zfs_range_tree_walk(engine.free, engine_store_pick, &p);
	VERIFY3U(p, !=, UINT64_MAX);
	engine.arena = p;
	engine.arena_bytes = ENGINE_STORE_BYTES;
	engine.bitmap = kmem_zalloc(ENGINE_STORE_PAGES, KM_SLEEP);
	engine.store_codec = 7;
	engine.bitmap[0] = engine.bitmap[1] = 1;
	engine.persistent = B_TRUE;
	engine.revision++;
	zfs_range_tree_remove(engine.free, p,
	    ENGINE_STORE_BYTES * engine.vd->vdev_children);
	engine.store_extents = zfs_range_tree_create(NULL,
	    ZFS_RANGE_SEG64, NULL, 0, 0);
	zfs_range_tree_add(engine.store_extents, p,
	    ENGINE_STORE_BYTES * engine.vd->vdev_children);
	p = UINT64_MAX;
	zfs_range_tree_walk(engine.free, engine_store_pick, &p);
	VERIFY3U(p, !=, UINT64_MAX);
	engine.journal_arena = p;
	engine.journal_bytes = ENGINE_JOURNAL_BYTES;
	zfs_range_tree_remove(engine.free, p,
	    ENGINE_JOURNAL_BYTES * engine.vd->vdev_children);
	VERIFY0(random_get_pseudo_bytes((void *)engine.journal_epoch,
	    sizeof (engine.journal_epoch)));
	engine.journal_root = p;
	memcpy(engine.journal_root_epoch, engine.journal_epoch,
	    sizeof (engine.journal_epoch));
	engine.journal_extents = zfs_range_tree_create(NULL,
	    ZFS_RANGE_SEG64, NULL, 0, 0);
	zfs_range_tree_add(engine.journal_extents, p,
	    ENGINE_JOURNAL_BYTES * engine.vd->vdev_children);
	engine.journal_segments = engine.journal_tail =
	    kmem_zalloc(sizeof (engine_j_segment_t), KM_SLEEP);
	engine.journal_tail->arena = p;
	engine.journal_current = engine.journal_tail;
	memcpy(engine.journal_tail->epoch, engine.journal_epoch,
	    sizeof (engine.journal_epoch));
	mutex_exit(&engine.lock);
	fnvlist_add_uint64(spa->spa_label_features,
	    PR19262_ENGINE_LABEL_FEATURE, 0);
	vdev_config_dirty(engine.vd);
	spa_config_exit(spa, SCL_CONFIG, FTAG);
	VERIFY0(engine_journal_format(spa));
	mutex_enter(&engine.lock);
	engine.journal_formatted = B_TRUE;
	cv_broadcast(&engine.changed);
	mutex_exit(&engine.lock);
	txg_wait_synced(spa->spa_dsl_pool, 0);
	mutex_enter(&engine.lock);
	VERIFY(engine.journal_ready);
	mutex_exit(&engine.lock);
}

static int
engine_store_io_at(vdev_t *vd, uint64_t arena, uint64_t page,
    uint64_t pages, abd_t *abd, boolean_t write)
{
	uint64_t offset = VDEV_LABEL_START_SIZE +
	    arena / engine.vd->vdev_children + page * ENGINE_STORE_PAGE;
	uint64_t bytes = pages * ENGINE_STORE_PAGE;
	if (!vdev_readable(vd) || (write && !vdev_writeable(vd)))
		return (ENXIO);
	if (offset > vd->vdev_psize || bytes > vd->vdev_psize - offset)
		return (EINVAL);
	zio_flag_t flags = ZIO_FLAG_CANFAIL | ZIO_FLAG_DONT_RETRY;
	zio_t *zio = write ? zio_write_phys(NULL, vd, offset, bytes,
	    abd, ZIO_CHECKSUM_OFF, NULL, NULL, ZIO_PRIORITY_SYNC_WRITE,
	    flags, B_FALSE) : zio_read_phys(NULL, vd, offset, bytes,
	    abd, ZIO_CHECKSUM_OFF, NULL, NULL, ZIO_PRIORITY_SYNC_READ,
	    flags, B_FALSE);
	zio->io_pr19262_metadata_authority = &engine;
	return (zio_wait(zio));
}

static int
engine_store_io(vdev_t *vd, uint64_t page, uint64_t pages, abd_t *abd,
    boolean_t write)
{
	return (engine_store_io_at(vd, engine.arena, page, pages, abd, write));
}

#include "vdev_reloc_pages.c"

static boolean_t
engine_store_layout(const uint64_t *d)
{
	if (d[7] == 0)
		return (B_FALSE);
	if (d[15] == 5)
		return (d[5] != 0 && d[4] < ENGINE_STORE_PAGES &&
		    d[5] <= ENGINE_STORE_PAGES - d[4] &&
		    d[7] <= d[5] * ENGINE_STORE_PAGE);
	uint64_t stripe = d[13] * ENGINE_STORE_PAGE;
	return ((d[15] == 6 || d[15] == 7) && d[5] == 1 &&
	    d[7] <= ENGINE_STORE_BYTES &&
	    stripe != 0 && d[4] % stripe == 0 && d[4] < d[14] &&
	    stripe <= d[14] - d[4]);
}

typedef struct engine_ranges {
	uint64_t *values;
	uint_t count;
} engine_ranges_t;

static void
engine_store_range(void *arg, uint64_t start, uint64_t size)
{
	engine_ranges_t *r = arg;
	r->values[r->count++] = start;
	r->values[r->count++] = size;
}

static nvlist_t *
engine_store_snapshot(spa_t *spa, const uberblock_t *ub)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	nvlist_t *nv = fnvlist_alloc();
	fnvlist_add_uint64(nv, "txg", ub->ub_txg);
	fnvlist_add_uint64(nv, "pool", spa_guid(spa));
	fnvlist_add_uint64(nv, "top", engine.vd->vdev_guid);
	fnvlist_add_uint64_array(nv, "mos", (uint64_t *)&ub->ub_rootbp,
	    sizeof (blkptr_t) / sizeof (uint64_t));
	uint64_t geometry[] = { engine.total, engine.vd->vdev_ms_shift,
	    engine.vd->vdev_ashift, engine.vd->vdev_children,
	    engine.arena, engine.arena_bytes };
	fnvlist_add_uint64_array(nv, "geometry", geometry, 6);
	uint64_t journal[] = { engine.journal_root, engine.journal_bytes,
	    engine.journal_root_epoch[0], engine.journal_root_epoch[1] };
	fnvlist_add_uint64_array(nv, "journal", journal, 4);
	fnvlist_add_uint64_array(nv, "checkpoint",
	    (uint64_t *)&engine.checkpoint,
	    sizeof (engine.checkpoint) / sizeof (uint64_t));
	uint64_t state[] = { engine.sequence, engine.next_incarnation,
	    engine.retired_limit };
	fnvlist_add_uint64_array(nv, "state", state, 3);
	uint64_t max = avl_numnodes(&engine.owners);
	nvlist_t **rows = kmem_alloc(MAX(1, max) * sizeof (*rows), KM_SLEEP);
	uint_t n = 0;
	for (engine_owner_t *e = avl_first(&engine.owners); e != NULL;
	    e = AVL_NEXT(&engine.owners, e)) {
		/* New uncommitted V allocations are outside this MOS root. */
		if (e->aborted || e->catalog_txg > ub->ub_txg ||
		    e->journal_pending)
			continue;
		nvlist_t *row = rows[n++] = fnvlist_alloc();
		uint64_t identity[] = { e->v, e->size, e->allocation_txg,
		    e->incarnation, e->created, e->retired_at,
		    e->retired ? 1 : (e->checkpoint_only ? 2 : 0),
		    e->catalog_txg };
		fnvlist_add_uint64_array(row, "V", identity, 8);
		uint_t count = 1;
		for (engine_version_t *b = e->versions; b != NULL; b = b->next)
			count += b != e->current && b->retired;
		uint64_t *versions = kmem_alloc(count * 8 * sizeof (uint64_t),
		    KM_SLEEP);
		uint_t j = 0;
		engine_version_t *b = e->current;
		for (;;) {
			if (b == e->current || b->retired) {
				uint64_t values[] = { b->p, b->size, b->birth,
				    b->width, b->parity, b->ready, b->retired,
				    b->generation };
				memcpy(&versions[j], values, sizeof (values));
				j += 8;
			}
			/* Current first, then all other retired versions. */
			b = b == e->current ? e->versions : b->next;
			if (b == e->current)
				b = b->next;
			if (b == NULL)
				break;
		}
		VERIFY3U(j, ==, count * 8);
		fnvlist_add_uint64_array(row, "P", versions, j);
		kmem_free(versions, count * 8 * sizeof (uint64_t));
	}
	fnvlist_add_nvlist_array(nv, "owners",
	    (const nvlist_t * const *)rows, n);
	for (uint_t i = 0; i < n; i++)
		fnvlist_free(rows[i]);
	kmem_free(rows, MAX(1, max) * sizeof (*rows));
	uint64_t nr = zfs_range_tree_numsegs(engine.legacy) * 2;
	engine_ranges_t r = { kmem_alloc(MAX(1, nr) * sizeof (uint64_t),
	    KM_SLEEP), 0 };
	zfs_range_tree_walk(engine.legacy, engine_store_range, &r);
	fnvlist_add_uint64_array(nv, "legacy", r.values, r.count);
	kmem_free(r.values, MAX(1, nr) * sizeof (uint64_t));
	fnvlist_add_byte_array(nv, "bitmap", engine.bitmap, ENGINE_STORE_PAGES);
	return (nv);
}

static int
engine_store_sync(spa_t *spa, uberblock_t *ub)
{
	mutex_enter(&engine.lock);
	if (engine.spa != spa || !engine.persistent) {
		mutex_exit(&engine.lock);
		return (0);
	}
	while (!engine.journal_formatted)
		cv_wait(&engine.changed, &engine.lock);
	if (pr19262_engine_store_ub(ub) && ub->ub_pr19262[6] == ub->ub_txg) {
		mutex_exit(&engine.lock);
		return (0);
	}
	nvlist_t *nv = engine_store_snapshot(spa, ub);
	if (engine.store_codec >= 6) {
		uint64_t revision = engine.revision;
		mutex_exit(&engine.lock);
		uint64_t desc[16];
		int error = engine_pages_store(spa, ub, nv, desc);
		fnvlist_free(nv);
		if (error == 0) {
			mutex_enter(&engine.lock);
			engine.stored_revision = revision;
			mutex_exit(&engine.lock);
			memcpy(ub->ub_pr19262, desc, sizeof (desc));
			if (engine_store_crash_txg == ub->ub_txg) {
				(void) fflush(NULL);
				_exit(77);
			}
		}
		return (error);
	}
	size_t size = 0;
	char *packed = NULL;
	VERIFY0(nvlist_pack(nv, &packed, &size, NV_ENCODE_XDR, KM_SLEEP));
	uint64_t pages = howmany(size, ENGINE_STORE_PAGE);
	uint64_t first = 0, run = 0;
	for (uint64_t i = 0; i < ENGINE_STORE_PAGES; i++) {
		run = engine.bitmap[i] ? 0 : run + 1;
		if (run == pages) {
			first = i + 1 - pages;
			break;
		}
	}
	if (pages == 0 || run != pages) {
		kmem_free(packed, size);
		fnvlist_free(nv);
		mutex_exit(&engine.lock);
		return (ENOSPC);
	}
	/* Finite COW pages, including this root's own allocator bitmap. */
	memset(engine.bitmap + first, 1, pages);
	fnvlist_remove(nv, "bitmap");
	fnvlist_add_byte_array(nv, "bitmap", engine.bitmap, ENGINE_STORE_PAGES);
	kmem_free(packed, size);
	packed = NULL;
	size = 0;
	VERIFY0(nvlist_pack(nv, &packed, &size, NV_ENCODE_XDR, KM_SLEEP));
	VERIFY3U(howmany(size, ENGINE_STORE_PAGE), ==, pages);
	fnvlist_free(nv);
	abd_t *abd = abd_alloc_for_io(pages * ENGINE_STORE_PAGE, B_TRUE);
	abd_zero(abd, pages * ENGINE_STORE_PAGE);
	abd_copy_from_buf(abd, packed, size);
	kmem_free(packed, size);
	uint64_t desc[16] = { ENGINE_STORE_MAGIC, engine.vd->vdev_guid,
	    engine.arena, engine.arena_bytes, first, pages, ub->ub_txg,
	    size, 0, 0, 0, 0, engine.vd->vdev_ashift,
	    engine.vd->vdev_children, engine.total, 5 };
	zio_cksum_t hash;
	abd_checksum_sha256(abd, pages * ENGINE_STORE_PAGE, NULL, &hash);
	memcpy(&desc[8], &hash, sizeof (hash));
	uint64_t revision = engine.revision;
	mutex_exit(&engine.lock);
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	int error = 0;
	for (uint64_t i = 0; i < engine.vd->vdev_children; i++) {
		int e = engine_store_io(engine.vd->vdev_child[i], first,
		    pages, abd, B_TRUE);
		if (e != 0)
			error = e;
		e = pr19262_engine_metadata_flush(engine.vd->vdev_child[i]);
		if (e != 0)
			error = e;
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	abd_free(abd);
	if (error == 0) {
		mutex_enter(&engine.lock);
		engine.stored_revision = revision;
		mutex_exit(&engine.lock);
		memcpy(ub->ub_pr19262, desc, sizeof (desc));
		(void) printf("PR19262_STORE_SYNC txg=%llu page=%llu "
		    "pages=%llu bytes=%llu arena=%llu\n",
		    (u_longlong_t)ub->ub_txg,
		    (u_longlong_t)first, (u_longlong_t)pages,
		    (u_longlong_t)size, (u_longlong_t)engine.arena);
		if (engine_store_crash_txg == ub->ub_txg) {
			(void) fflush(NULL);
			_exit(77);
		}
	}
	return (error);
}

int
pr19262_engine_store_sync(spa_t *spa, uberblock_t *ub)
{
	engine_store_test_phase(spa, 2);
	mutex_enter(&engine.store_lock);
	int error = engine_store_sync(spa, ub);
	mutex_exit(&engine.store_lock);
	return (error);
}

static void
engine_store_clear(void)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	engine_pages_clear();
	engine_owner_t *e;
	while ((e = avl_first(&engine.owners)) != NULL) {
		VERIFY0(e->holds);
		VERIFY(!e->copying && e->pending == NULL);
		while (e->versions != NULL) {
			engine_version_t *b = e->versions;
			VERIFY0(b->leases);
			e->versions = b->next;
			kmem_free(b, sizeof (*b));
		}
		avl_remove(&engine.owners, e);
		kmem_free(e, sizeof (*e));
	}
	if (engine.free != NULL) {
		zfs_range_tree_vacate(engine.free, NULL, NULL);
		zfs_range_tree_destroy(engine.free);
	}
	if (engine.legacy != NULL) {
		zfs_range_tree_vacate(engine.legacy, NULL, NULL);
		zfs_range_tree_destroy(engine.legacy);
	}
	if (engine.bitmap != NULL)
		kmem_free(engine.bitmap, ENGINE_STORE_PAGES);
	if (engine.store_extents != NULL) {
		zfs_range_tree_vacate(engine.store_extents, NULL, NULL);
		zfs_range_tree_destroy(engine.store_extents);
		engine.store_extents = NULL;
	}
	if (engine.journal_extents != NULL) {
		zfs_range_tree_vacate(engine.journal_extents, NULL, NULL);
		zfs_range_tree_destroy(engine.journal_extents);
		engine.journal_extents = NULL;
	}
	while (engine.journal_segments != NULL) {
		engine_j_segment_t *s = engine.journal_segments;
		engine.journal_segments = s->next;
		kmem_free(s, sizeof (*s));
	}
	engine.journal_tail = NULL;
	engine.journal_current = NULL;
	if (engine.journal_pending != NULL) {
		kmem_free(engine.journal_pending, sizeof (engine_j_segment_t));
		engine.journal_pending = NULL;
	}
	engine.spa = NULL;
	engine.vd = NULL;
	engine.free = engine.legacy = NULL;
	engine.bitmap = NULL;
	engine.persistent = B_FALSE;
	engine.arena = engine.arena_bytes = engine.retired = 0;
	engine.store_codec = 0;
	engine.journal_arena = engine.journal_bytes = engine.journal_seq = 0;
	engine.journal_serial = 0;
	engine.journal_root = 0;
	memset(engine.journal_root_epoch, 0,
	    sizeof (engine.journal_root_epoch));
	memset(&engine.journal_hash, 0, sizeof (engine.journal_hash));
	engine.journal_error = 0;
	engine.journal_ready = engine.journal_formatted = B_FALSE;
	engine.journal_repair = B_FALSE;
	memset(&engine.checkpoint, 0, sizeof (engine.checkpoint));
}

void
pr19262_engine_store_unload(spa_t *spa)
{
	mutex_enter(&engine.store_lock);
	mutex_enter(&engine.lock);
	if (engine.spa == spa && engine.persistent)
		engine_store_clear();
	mutex_exit(&engine.lock);
	mutex_exit(&engine.store_lock);
}

void
pr19262_engine_store_rebind(spa_t *spa)
{
	mutex_enter(&engine.lock);
	if (engine.spa == spa)
		engine.vd = spa->spa_root_vdev->vdev_child[0];
	mutex_exit(&engine.lock);
}

static boolean_t
engine_store_extent(uint64_t p, uint64_t size)
{
	return (size != 0 && IS_P2ALIGNED(p, ENGINE_STORE_PAGE) &&
	    IS_P2ALIGNED(size, ENGINE_STORE_PAGE) && p < engine.total &&
	    size <= engine.total - p &&
	    zfs_range_tree_contains(engine.free, p, size));
}

static int
engine_store_validate_virtual(void)
{
	zfs_range_tree_t *seen = zfs_range_tree_create(NULL,
	    ZFS_RANGE_SEG64, NULL, 0, 0);
	zfs_range_tree_walk(engine.legacy, zfs_range_tree_add, seen);
	int error = 0;
	for (engine_owner_t *e = avl_first(&engine.owners); e != NULL;
	    e = AVL_NEXT(&engine.owners, e)) {
		uint64_t start, size;
		if (!IS_P2ALIGNED(e->v, ENGINE_STORE_PAGE) ||
		    !IS_P2ALIGNED(e->size, ENGINE_STORE_PAGE) ||
		    zfs_range_tree_find_in(seen, e->v, e->size,
		    &start, &size)) {
			error = EINVAL;
			break;
		}
		zfs_range_tree_add(seen, e->v, e->size);
	}
	zfs_range_tree_vacate(seen, NULL, NULL);
	zfs_range_tree_destroy(seen);
	return (error);
}

static int
engine_store_decode(spa_t *spa, const uberblock_t *ub, nvlist_t *nv)
{
	uint64_t txg, pool, top, *mos, *g, *state, *legacy, *journal;
	uint64_t *checkpoint;
	uint_t nm, ng, ns, nl, nr, nb, nj, nc;
	uchar_t *bitmap;
	nvlist_t **rows;
	if (nvlist_lookup_uint64(nv, "txg", &txg) != 0 ||
	    nvlist_lookup_uint64(nv, "pool", &pool) != 0 ||
	    nvlist_lookup_uint64(nv, "top", &top) != 0 ||
	    nvlist_lookup_uint64_array(nv, "mos", &mos, &nm) != 0 ||
	    nvlist_lookup_uint64_array(nv, "geometry", &g, &ng) != 0 ||
	    nvlist_lookup_uint64_array(nv, "state", &state, &ns) != 0 ||
	    nvlist_lookup_uint64_array(nv, "legacy", &legacy, &nl) != 0 ||
	    nvlist_lookup_nvlist_array(nv, "owners", &rows, &nr) != 0 ||
	    nvlist_lookup_byte_array(nv, "bitmap", &bitmap, &nb) != 0)
		return (EINVAL);
	if (nvlist_lookup_uint64_array(nv, "journal", &journal, &nj) != 0 ||
	    nj != 4 || journal[1] != ENGINE_JOURNAL_BYTES ||
	    journal[0] >= engine.total ||
	    journal[1] * engine.vd->vdev_children >
	    engine.total - journal[0] ||
	    journal[0] % (engine.vd->vdev_children * ENGINE_STORE_PAGE) != 0)
		return (EINVAL);
	if (nvlist_lookup_uint64_array(nv, "checkpoint", &checkpoint,
	    &nc) != 0 ||
	    nc != sizeof (engine.checkpoint) / sizeof (uint64_t))
		return (EINVAL);
	memcpy(&engine.checkpoint, checkpoint, sizeof (engine.checkpoint));
	if (engine.checkpoint.ub_txg != 0 &&
	    (engine.checkpoint.ub_checkpoint_txg != engine.checkpoint.ub_txg ||
	    engine.checkpoint.ub_txg > ub->ub_txg ||
	    !pr19262_engine_store_ub(&engine.checkpoint)))
		return (EINVAL);
	if (txg != ub->ub_pr19262[6] || pool != spa_guid(spa) ||
	    top != engine.vd->vdev_guid || nm != 16 || ng != 6 || ns != 3 ||
	    memcmp(mos, &ub->ub_rootbp, sizeof (blkptr_t)) != 0 ||
	    g[0] != engine.total || g[1] != engine.vd->vdev_ms_shift ||
	    g[2] != 12 || g[3] != engine.vd->vdev_children ||
	    g[4] != engine.arena || g[5] != engine.arena_bytes ||
	    nb != ENGINE_STORE_PAGES || nl % 2 != 0 ||
	    nr > (engine.store_codec >= 6 ? engine_pages_row_limit() : 50000))
		return (EINVAL);
	for (uint64_t i = 0; i < ENGINE_STORE_PAGES; i++)
		if (bitmap[i] > 1 || (ub->ub_pr19262[15] == 5 &&
		    i >= ub->ub_pr19262[4] &&
		    i < ub->ub_pr19262[4] + ub->ub_pr19262[5] && !bitmap[i]))
			return (EINVAL);
	engine.bitmap = kmem_alloc(ENGINE_STORE_PAGES, KM_SLEEP);
	memcpy(engine.bitmap, bitmap, ENGINE_STORE_PAGES);
	engine.sequence = state[0];
	engine.next_incarnation = state[1];
	engine.retired_limit = state[2];
	engine.free = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64, NULL, 0, 0);
	engine.legacy = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	zfs_range_tree_add(engine.free, 0, engine.total);
	zfs_range_tree_remove(engine.free, engine.arena,
	    engine.arena_bytes * engine.vd->vdev_children);
	engine.store_extents = zfs_range_tree_create(NULL,
	    ZFS_RANGE_SEG64, NULL, 0, 0);
	zfs_range_tree_add(engine.store_extents, engine.arena,
	    engine.arena_bytes * engine.vd->vdev_children);
	for (engine_marena_t *a = engine_pages.arenas; a != NULL;
	    a = a->next) {
		if (a->p == engine.arena || a->pending)
			continue;
		uint64_t bytes = a->pages * ENGINE_STORE_PAGE *
		    engine.vd->vdev_children;
		if (!engine_store_extent(a->p, bytes))
			return (EINVAL);
		zfs_range_tree_remove(engine.free, a->p, bytes);
		zfs_range_tree_add(engine.store_extents, a->p, bytes);
	}
	engine.journal_arena = journal[0];
	engine.journal_bytes = journal[1];
	engine.journal_epoch[0] = journal[2];
	engine.journal_epoch[1] = journal[3];
	engine.journal_root = journal[0];
	memcpy(engine.journal_root_epoch, engine.journal_epoch,
	    sizeof (engine.journal_epoch));
	uint64_t jsize = engine.journal_bytes * engine.vd->vdev_children;
	if (!engine_store_extent(engine.journal_arena, jsize))
		return (EINVAL);
	zfs_range_tree_remove(engine.free, engine.journal_arena, jsize);
	engine.journal_extents = zfs_range_tree_create(NULL,
	    ZFS_RANGE_SEG64, NULL, 0, 0);
	zfs_range_tree_add(engine.journal_extents, engine.journal_arena, jsize);
	engine.journal_segments = engine.journal_tail =
	    kmem_zalloc(sizeof (engine_j_segment_t), KM_SLEEP);
	engine.journal_tail->arena = engine.journal_arena;
	engine.journal_current = engine.journal_tail;
	memcpy(engine.journal_tail->epoch, engine.journal_epoch,
	    sizeof (engine.journal_epoch));
	for (uint_t i = 0; i < nl; i += 2) {
		if (!engine_store_extent(legacy[i], legacy[i + 1]))
			return (EINVAL);
		zfs_range_tree_remove(engine.free, legacy[i], legacy[i + 1]);
		zfs_range_tree_add(engine.legacy, legacy[i], legacy[i + 1]);
	}
	for (uint_t i = 0; i < nr; i++) {
		uint64_t *v, *p;
		uint_t nv, np;
		if (nvlist_lookup_uint64_array(rows[i], "V", &v, &nv) != 0 ||
		    nvlist_lookup_uint64_array(rows[i], "P", &p, &np) != 0 ||
		    nv != 8 || np == 0 || np % 8 != 0 || v[6] > 2 ||
		    v[0] >= engine.total || v[1] == 0 ||
		    v[1] > engine.total - v[0] || engine_find(v[0]) != NULL ||
		    v[7] > txg || v[3] > engine.next_incarnation)
			return (EINVAL);
		engine_owner_t *e = kmem_zalloc(sizeof (*e), KM_SLEEP);
		e->v = v[0]; e->size = v[1]; e->allocation_txg = v[2];
		e->incarnation = v[3]; e->created = v[4];
		e->catalog_txg = v[7];
		e->retired_at = v[5]; e->retired = v[6] == 1;
		e->checkpoint_only = v[6] == 2;
		avl_add(&engine.owners, e);
		for (uint_t j = 0; j < np; j += 8) {
			if (!engine_store_extent(p[j], p[j + 1]) ||
			    p[j + 3] <= p[j + 4] ||
			    p[j + 3] > engine.vd->vdev_children ||
			    p[j + 4] == 0 || p[j + 4] > 3 ||
			    p[j + 5] > 1 || p[j + 6] > 1)
				return (EINVAL);
			engine_version_t *b = engine_version_new(e, p[j],
			    p[j + 1], p[j + 2], B_TRUE);
			b->width = p[j + 3]; b->parity = p[j + 4];
			b->retired = p[j + 6];
			b->ready = p[j + 5]; b->generation = p[j + 7];
			if (j == 0)
				e->current = b;
			zfs_range_tree_remove(engine.free, b->p, b->size);
			if (b->retired || (e->retired && j == 0))
				engine.retired += b->size;
		}
	}
	return (engine_store_validate_virtual());
}

typedef struct engine_root_graph {
	spa_t *spa;
	uberblock_t *roots;
	uint_t count;
	uint_t capacity;
	zfs_range_tree_t *occupied;
} engine_root_graph_t;

static int
engine_root_add(void *arg, const uberblock_t *ub)
{
	engine_root_graph_t *g = arg;
	const uint64_t *d = ub->ub_pr19262;
	if (!pr19262_engine_store_ub(ub) || !engine_store_layout(d) ||
	    d[1] != engine.vd->vdev_guid || d[2] != engine.arena ||
	    d[3] != engine.arena_bytes || d[12] != 12 ||
	    d[13] != engine.vd->vdev_children || d[14] != engine.total ||
	    d[6] == 0 ||
	    d[6] > ub->ub_txg ||
	    (d[6] != ub->ub_txg && (ub->ub_checkpoint_txg == 0 ||
	    d[6] > ub->ub_checkpoint_txg)))
		return (EINVAL);
	for (uint_t i = 0; i < g->count; i++) {
		if (memcmp(g->roots[i].ub_pr19262, d,
		    sizeof (ub->ub_pr19262)) != 0)
			continue;
		return (memcmp(&g->roots[i].ub_rootbp, &ub->ub_rootbp,
		    sizeof (ub->ub_rootbp)) == 0 ? 0 : EINVAL);
	}
	if (g->count == g->capacity) {
		uint64_t wanted = g->capacity == 0 ? 64 :
		    (uint64_t)g->capacity * 2;
		uint64_t limit = MIN(UINT_MAX, engine.total /
		    (ENGINE_STORE_PAGE * engine.vd->vdev_children));
		limit = MIN(limit, SIZE_MAX / sizeof (uberblock_t));
		uint_t capacity = MIN(wanted, limit);
		if (capacity == g->capacity)
			return (EOVERFLOW);
		uberblock_t *roots = kmem_alloc(capacity *
		    sizeof (*roots), KM_SLEEP);
		if (g->roots != NULL) {
			memcpy(roots, g->roots, g->count * sizeof (*roots));
			kmem_free(g->roots, g->capacity * sizeof (*roots));
		}
		g->roots = roots;
		g->capacity = capacity;
	}
	g->roots[g->count++] = *ub;
	return (0);
}

static int
engine_root_read(engine_root_graph_t *g, const uberblock_t *ub)
{
	const uint64_t *d = ub->ub_pr19262;
	int error = ECKSUM;
	nvlist_t *nv = NULL;
	if (d[15] >= 6)
		error = engine_pages_read(g->spa, ub, &nv,
		    g->occupied, B_FALSE);
	abd_t *abd = d[15] == 5 ?
	    abd_alloc_linear(d[5] * ENGINE_STORE_PAGE, B_TRUE) : NULL;
	for (uint_t c = 0; d[15] == 5 && c < engine.vd->vdev_children; c++) {
		if (engine_store_io(engine.vd->vdev_child[c], d[4], d[5],
		    abd, B_FALSE) != 0)
			continue;
		zio_cksum_t hash;
		abd_checksum_sha256(abd, d[5] * ENGINE_STORE_PAGE, NULL, &hash);
		if (memcmp(&hash, d + 8, sizeof (hash)) != 0)
			continue;
		error = nvlist_unpack(abd_to_buf(abd), d[7], &nv, KM_SLEEP);
		break;
	}
	if (abd != NULL)
		abd_free(abd);
	if (error != 0) {
		return (error);
	}
	uint64_t txg, pool, top, *mos, *checkpoint;
	uint_t nm, nc;
	if (nvlist_lookup_uint64(nv, "txg", &txg) != 0 ||
	    nvlist_lookup_uint64(nv, "pool", &pool) != 0 ||
	    nvlist_lookup_uint64(nv, "top", &top) != 0 ||
	    nvlist_lookup_uint64_array(nv, "mos", &mos, &nm) != 0 ||
	    nvlist_lookup_uint64_array(nv, "checkpoint", &checkpoint,
	    &nc) != 0 || nm != sizeof (blkptr_t) / sizeof (uint64_t) ||
	    nc != sizeof (uberblock_t) / sizeof (uint64_t) || txg != d[6] ||
	    pool != spa_guid(g->spa) || top != d[1] ||
	    memcmp(mos, &ub->ub_rootbp, sizeof (blkptr_t)) != 0) {
		error = EINVAL;
	} else {
		uberblock_t pin;
		memcpy(&pin, checkpoint, sizeof (pin));
		if (pin.ub_txg != 0) {
			if (uberblock_verify(&pin) != 0 || pin.ub_txg > txg ||
			    pin.ub_checkpoint_txg != pin.ub_txg ||
			    pin.ub_pr19262[6] >= d[6])
				error = EINVAL;
			else
				error = engine_root_add(g, &pin);
		}
		if (error == 0) {
			if (d[15] == 5)
				engine_mmark(g->occupied,
				    engine.arena + d[4] * ENGINE_STORE_PAGE *
				    engine.vd->vdev_children,
				    d[5] * ENGINE_STORE_PAGE *
				    engine.vd->vdev_children);
			(void) printf("PR19262_M_PIN catalog=%llu first=%llu "
			    "pages=%llu cp=%llu cp_catalog=%llu\n",
			    (u_longlong_t)d[6],
			    (u_longlong_t)d[4], (u_longlong_t)d[5],
			    (u_longlong_t)pin.ub_txg,
			    (u_longlong_t)pin.ub_pr19262[6]);
		}
	}
	nvlist_free(nv);
	return (error);
}

/* Validated disk-root closure only; this does not authorize reclamation. */
int
pr19262_engine_store_inventory(spa_t *spa, uint64_t *roots, uint64_t *pages)
{
	*roots = *pages = 0;
	/*
	 * The caller holds no config locks.  Sync already holds STATE when
	 * taking store_lock; a queued STATE writer must not complete a cycle
	 * through this reader.  Take ZIO only after acquiring store_lock.
	 */
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine.store_lock);
	engine_store_test_phase(spa, 1);
	if (!pr19262_engine_persistent(spa)) {
		mutex_exit(&engine.store_lock);
		spa_config_exit(spa, SCL_STATE, FTAG);
		return (ENOTSUP);
	}
	engine_root_graph_t *g = kmem_zalloc(sizeof (*g), KM_SLEEP);
	g->spa = spa;
	g->occupied = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	int error = pr19262_vdev_uberblocks(spa, g, engine_root_add);
	for (uint_t i = 0; error == 0 && i < g->count; i++)
		error = engine_root_read(g, &g->roots[i]);
	spa_config_exit(spa, SCL_ZIO, FTAG);
	if (error == 0) {
		*roots = g->count;
		*pages = zfs_range_tree_space(g->occupied) /
		    (ENGINE_STORE_PAGE * engine.vd->vdev_children);
	}
	zfs_range_tree_vacate(g->occupied, NULL, NULL);
	zfs_range_tree_destroy(g->occupied);
	if (g->roots != NULL)
		kmem_free(g->roots, g->capacity * sizeof (uberblock_t));
	kmem_free(g, sizeof (*g));
	mutex_exit(&engine.store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	return (error);
}

int
pr19262_engine_mgrowth_probe(spa_t *spa, boolean_t advance,
    pr19262_mgrowth_probe_t *out)
{
	memset(out, 0, sizeof (*out));
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine.store_lock);
	int error = pr19262_engine_persistent(spa) ?
	    engine_pages_growth_probe(spa, advance, out) : ENOTSUP;
	mutex_exit(&engine.store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	return (error);
}

int
pr19262_engine_store_load(spa_t *spa, const uberblock_t *ub)
{
	if (!pr19262_engine_store_ub(ub))
		return (0);
	const uint64_t *d = ub->ub_pr19262;
	vdev_t *root = spa->spa_root_vdev;
	if (root->vdev_children != 1 || engine.spa != NULL)
		return (ENOTSUP);
	vdev_t *vd = root->vdev_child[0];
	/* Older M bitmaps cannot allocate over newer still-selectable roots. */
	if ((spa->spa_mode & SPA_MODE_WRITE) &&
	    ub->ub_txg < spa->spa_load_latest_ub_txg)
		return (ENOTSUP);
	if (vd->vdev_ops != &vdev_raidz_ops || d[1] != vd->vdev_guid ||
	    d[3] != ENGINE_STORE_BYTES || d[12] != 12 ||
	    d[13] != vd->vdev_children || d[14] != vd->vdev_asize ||
	    d[6] > ub->ub_txg ||
	    (d[6] != ub->ub_txg && (ub->ub_checkpoint_txg == 0 ||
	    d[6] > ub->ub_checkpoint_txg)) ||
	    !engine_store_layout(d) ||
	    d[2] >= vd->vdev_asize ||
	    d[3] * d[13] > vd->vdev_asize - d[2] ||
	    d[2] % (vd->vdev_children * ENGINE_STORE_PAGE) != 0)
		return (EINVAL);
	engine.vd = vd;
	engine.total = vd->vdev_asize;
	engine.arena = d[2]; engine.arena_bytes = d[3];
	engine.store_codec = d[15];
	abd_t *abd = d[15] == 5 ?
	    abd_alloc_for_io(d[5] * ENGINE_STORE_PAGE, B_TRUE) : NULL;
	int error = ECKSUM;
	nvlist_t *nv = NULL;
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	if (d[15] >= 6) {
		error = engine_pages_read(spa, ub, &nv, NULL, B_TRUE);
	}
	for (uint64_t i = 0; d[15] == 5 && i < vd->vdev_children; i++) {
		if (engine_store_io(vd->vdev_child[i], d[4], d[5],
		    abd, B_FALSE) != 0)
			continue;
		zio_cksum_t hash;
		abd_checksum_sha256(abd, d[5] * ENGINE_STORE_PAGE, NULL, &hash);
		if (memcmp(&hash, &d[8], sizeof (hash)) != 0)
			continue;
		void *data = abd_borrow_buf_copy(abd, d[7]);
		error = nvlist_unpack(data, d[7], &nv, KM_SLEEP);
		abd_return_buf(abd, data, d[7]);
		break;
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	if (abd != NULL)
		abd_free(abd);
	mutex_enter(&engine.lock);
	if (error == 0)
		error = engine_store_decode(spa, ub, nv);
	if (error == 0 && d[15] >= 6)
		error = engine_pages_install(nv);
	nvlist_free(nv);
	if (error == 0) {
		engine.persistent = B_TRUE;
		engine.spa = spa;
		engine.stored_revision = engine.revision;
		/* Open native space maps even for a read-only SPA. */
		spa->spa_read_spacemaps = B_TRUE;
		(void) printf("PR19262_STORE_LOAD pre-MOS txg=%llu "
		    "owners=%llu catalog=%llu\n", (u_longlong_t)ub->ub_txg,
		    (u_longlong_t)avl_numnodes(&engine.owners),
		    (u_longlong_t)d[6]);
	} else {
		engine_store_clear();
	}
	mutex_exit(&engine.lock);
	if (error == 0)
		error = engine_journal_recover(spa);
	/*
	 * Only classify and exclude a pending arena here: vdevs opened
	 * before spa_trust_config are not writeable, so the finishing
	 * writes wait for pr19262_engine_store_recover().
	 */
	if (error == 0 && d[15] >= 6)
		error = engine_pages_reconcile(spa, B_FALSE);
	if (error != 0)
		pr19262_engine_store_unload(spa);
	if (error == 0 &&
	    getenv("PR19262_ENGINE_BOOTSTRAP_NO_LABEL_EPOCHS") != NULL) {
		/* Read MOS through stored layout descriptors. */
		vdev_raidz_parity_epochs_install(vd, NULL, 0);
		(void) printf("PR19262_STORE_BOOTSTRAP without-label-epochs\n");
	}
	return (error);
}

/*
 * Finish or cancel a pending raw arena operation.  spa_load calls this in
 * its writable branch, after spa_ld_trusted_config() reopened the vdevs
 * writeable and before the J claim barrier and any MOS allocation.
 */
int
pr19262_engine_store_recover(spa_t *spa)
{
	if (!pr19262_engine_persistent(spa) || !spa_writeable(spa))
		return (0);
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine.store_lock);
	int error = engine_pages_reconcile(spa, B_TRUE);
	mutex_exit(&engine.store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	return (error);
}
