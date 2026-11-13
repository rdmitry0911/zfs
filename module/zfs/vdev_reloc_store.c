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
/* Private userspace persistence experiment, included by the normal engine. */
#define	ENGINE_STORE_MAGIC 0x31393236324d3031ULL
#define	ENGINE_STORE_PAGE 4096ULL
#define	ENGINE_STORE_BYTES (4ULL << 20)
#define	ENGINE_STORE_PAGES (ENGINE_STORE_BYTES / ENGINE_STORE_PAGE)
#define	ENGINE_JOURNAL_BYTES (1ULL << 20)

/* Refuse a root or record with a numbered reason, visible in the log. */
#define	ENGINE_REFUSE(reason) do { \
	pr19262_printf("PR19262_STORE_REFUSED reason=%d line=%d\n", \
	    (reason), __LINE__); \
	return (EINVAL); \
} while (0)

void
pr19262_engine_store_test_hook(spa_t *spa, void (*hook)(spa_t *, int))
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine->store_test_hook = hook;
	mutex_exit(&engine->lock);
}

static void
engine_store_test_phase(spa_t *spa, int phase)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	void (*hook)(spa_t *, int) = engine->store_test_hook;
	mutex_exit(&engine->lock);
	if (hook != NULL)
		hook(spa, phase);
}

void
pr19262_engine_store_crash(spa_t *spa, uint64_t txg)
{
	engine_t *engine = spa->spa_reloc;
	engine->store_crash_txg = txg;
}

boolean_t
pr19262_engine_store_ub(const uberblock_t *ub)
{
	return (ub->ub_pr19262[0] == ENGINE_STORE_MAGIC);
}

/* Native checkpoints have no catalog pages to pin in the engine graph. */
static boolean_t
engine_native_checkpoint(uberblock_t *ub)
{
	uint64_t absent[ARRAY_SIZE(ub->ub_pr19262)] = { 0 };
	return (uberblock_verify(ub) == 0 && ub->ub_txg != 0 &&
	    ub->ub_checkpoint_txg == ub->ub_txg &&
	    memcmp(ub->ub_pr19262, absent, sizeof (absent)) == 0);
}

boolean_t
pr19262_engine_persistent(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	boolean_t active = engine->spa == spa && spa != NULL &&
	    engine->persistent;
	mutex_exit(&engine->lock);
	return (active);
}

/*
 * Whether the uberblock of txg must be written although nothing native
 * changed.  Deferred space forces it only at the txgs that move the
 * deferral (engine_reuse_due_locked): the store that stages it, and the
 * uberblock that releases a batch, so that an idle pool still releases.
 */
boolean_t
pr19262_engine_store_dirty(spa_t *spa, uint64_t txg)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	boolean_t dirty = engine->spa == spa && engine->persistent &&
	    (engine->revision != engine->stored_revision ||
	    engine_reuse_due_locked(engine, txg));
	mutex_exit(&engine->lock);
	return (dirty);
}

typedef struct engine_store_pick_arg {
	uint64_t stripe;
	uint64_t bytes;
	uint64_t found;
} engine_store_pick_arg_t;

static void
engine_store_pick(void *arg, uint64_t start, uint64_t size)
{
	engine_store_pick_arg_t *pick = arg;
	uint64_t p = roundup(start, pick->stripe);
	if (p >= start && p - start <= size &&
	    pick->bytes <= size - (p - start))
		pick->found = p;
}

static int
engine_store_enable_locked(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(spa_config_held(spa, SCL_CONFIG, RW_WRITER));
	mutex_enter(&engine->lock);
	VERIFY(engine->spa == NULL || engine->spa == spa);
	VERIFY(!engine->persistent);
	/* Raw pages are whole sectors: ashift up to the page size. */
	VERIFY0(ENGINE_STORE_PAGE % (1ULL << engine->vd->vdev_ashift));
	uint64_t children = engine->vd->vdev_children;
	engine_store_pick_arg_t pick = { children * ENGINE_STORE_PAGE,
	    children * ENGINE_STORE_BYTES, UINT64_MAX };
	zfs_range_tree_walk(engine->free, engine_store_pick, &pick);
	if (pick.found == UINT64_MAX) {
		mutex_exit(&engine->lock);
		return (SET_ERROR(ENOSPC));
	}
	uint64_t p = pick.found;
	zfs_range_tree_remove(engine->free, p, pick.bytes);
	pick.bytes = children * ENGINE_JOURNAL_BYTES;
	pick.found = UINT64_MAX;
	zfs_range_tree_walk(engine->free, engine_store_pick, &pick);
	if (pick.found == UINT64_MAX) {
		zfs_range_tree_add(engine->free, p,
		    children * ENGINE_STORE_BYTES);
		mutex_exit(&engine->lock);
		return (SET_ERROR(ENOSPC));
	}
	uint64_t journal = pick.found;
	zfs_range_tree_remove(engine->free, journal, pick.bytes);
	engine->arena = p;
	engine->arena_bytes = ENGINE_STORE_BYTES;
	engine->bitmap = kmem_zalloc(ENGINE_STORE_PAGES, KM_SLEEP);
	engine->store_codec = 7;
	engine->bitmap[0] = engine->bitmap[1] = 1;
	engine->persistent = B_TRUE;
	engine->store_all = B_TRUE;
	engine->revision++;
	engine->store_extents = zfs_range_tree_create(NULL,
	    ZFS_RANGE_SEG64, NULL, 0, 0);
	zfs_range_tree_add(engine->store_extents, p,
	    ENGINE_STORE_BYTES * engine->vd->vdev_children);
	p = journal;
	engine->journal_arena = p;
	engine->journal_bytes = ENGINE_JOURNAL_BYTES;
	VERIFY0(random_get_pseudo_bytes((void *)engine->journal_epoch,
	    sizeof (engine->journal_epoch)));
	engine->journal_root = p;
	memcpy(engine->journal_root_epoch, engine->journal_epoch,
	    sizeof (engine->journal_epoch));
	engine->journal_extents = zfs_range_tree_create(NULL,
	    ZFS_RANGE_SEG64, NULL, 0, 0);
	zfs_range_tree_add(engine->journal_extents, p,
	    ENGINE_JOURNAL_BYTES * engine->vd->vdev_children);
	engine->journal_segments = engine->journal_tail =
	    kmem_zalloc(sizeof (engine_j_segment_t), KM_SLEEP);
	engine->journal_tail->arena = p;
	engine->journal_tail->index = engine->journal_segment_count = 1;
	engine->journal_tail->free_after = UINT64_MAX;
	engine->journal_current = engine->journal_tail;
	memcpy(engine->journal_tail->epoch, engine->journal_epoch,
	    sizeof (engine->journal_epoch));
	mutex_exit(&engine->lock);
	fnvlist_add_uint64(spa->spa_label_features,
	    PR19262_ENGINE_LABEL_FEATURE, 0);
	vdev_config_dirty(engine->vd);
	return (0);
}

static void
engine_store_enable_done(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	VERIFY0(engine_journal_format(spa));
	mutex_enter(&engine->lock);
	engine->journal_formatted = B_TRUE;
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
	txg_wait_synced(spa->spa_dsl_pool, 0);
	mutex_enter(&engine->lock);
	VERIFY(engine->journal_ready);
	mutex_exit(&engine->lock);
}

int
pr19262_engine_store_enable(spa_t *spa)
{
	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_WRITER);
	int error = engine_store_enable_locked(spa);
	spa_config_exit(spa, SCL_CONFIG, FTAG);
	if (error == 0)
		engine_store_enable_done(spa);
	return (error);
}

typedef enum engine_store_member_kind {
	ENGINE_STORE_MEMBER_UNSUPPORTED,
	ENGINE_STORE_MEMBER_LEAF,
	ENGINE_STORE_MEMBER_REPLACING
} engine_store_member_kind_t;

typedef struct engine_store_member {
	engine_store_member_kind_t kind;
	vdev_t *slot;
	/* For REPLACING these are its old and exact new leaves, in order. */
	vdev_t *leaf[2];
} engine_store_member_t;

/*
 * Classify a logical raw-store member without treating a replacing parent as
 * a leaf.  The REPLACING result is deliberately not I/O-capable yet: its
 * mirror parent can report a write/flush success when only its old child
 * completed, so it cannot prove a raw page or flush reached the exact new
 * leaf.  A later reseed implementation will consume both named leaves and
 * collect exact-leaf completion results before it permits this shape.
 */
static int
engine_store_member_classify(vdev_t *slot, engine_store_member_t *out)
{
	memset(out, 0, sizeof (*out));
	out->slot = slot;
	if (slot == NULL)
		return (ENXIO);
	if (slot->vdev_ops->vdev_op_leaf && slot->vdev_children == 0) {
		out->kind = ENGINE_STORE_MEMBER_LEAF;
		out->leaf[0] = slot;
		return (0);
	}
	if (slot->vdev_ops == &vdev_replacing_ops &&
	    slot->vdev_children == 2 && slot->vdev_child[0] != NULL &&
	    slot->vdev_child[1] != NULL &&
	    slot->vdev_child[0]->vdev_ops->vdev_op_leaf &&
	    slot->vdev_child[0]->vdev_children == 0 &&
	    slot->vdev_child[1]->vdev_ops->vdev_op_leaf &&
	    slot->vdev_child[1]->vdev_children == 0) {
		out->kind = ENGINE_STORE_MEMBER_REPLACING;
		out->leaf[0] = slot->vdev_child[0];
		out->leaf[1] = slot->vdev_child[1];
		return (0);
	}
	return (ENOTSUP);
}

/*
 * Return only the direct-leaf case.  Every existing raw-store data I/O and
 * flush goes through this gate, so an unexpected replacing topology fails
 * cleanly rather than tripping zio_{read,write}_phys()'s leaf assertion.
 */
static int
engine_store_member_direct(vdev_t *slot, vdev_t **leafp)
{
	engine_store_member_t member;
	int error = engine_store_member_classify(slot, &member);
	if (error != 0)
		return (error);
	if (member.kind != ENGINE_STORE_MEMBER_LEAF)
		return (ENOTSUP);
	*leafp = member.leaf[0];
	return (0);
}

/* Used during activation/recovery before raw I/O is made authoritative. */
static boolean_t
engine_store_direct_topology(vdev_t *vd)
{
	if (vd == NULL || vd->vdev_children == 0)
		return (B_FALSE);
	for (uint_t c = 0; c < vd->vdev_children; c++) {
		vdev_t *leaf;
		if (engine_store_member_direct(vd->vdev_child[c], &leaf) != 0)
			return (B_FALSE);
	}
	return (B_TRUE);
}

/* Both production collection and the ztest negative probe use this gate. */
static int
engine_r02_manifest_gate(spa_t *spa, engine_t *engine, boolean_t direct,
    uint64_t codec)
{
	mutex_enter(&engine->lock);
	boolean_t persistent = engine->spa == spa && engine->persistent;
	mutex_exit(&engine->lock);
	return (persistent && direct && codec >= 6 && codec <= 7 ? 0 :
	    ENOTSUP);
}

static boolean_t
engine_r02_manifest_codec_supported(uint64_t codec)
{
	return (codec >= 6 && codec <= 7);
}

#ifndef _KERNEL
static boolean_t
engine_store_has_replacing_topology(vdev_t *vd)
{
	if (vd == NULL)
		return (B_FALSE);
	for (uint_t c = 0; c < vd->vdev_children; c++) {
		engine_store_member_t member;
		int error = engine_store_member_classify(vd->vdev_child[c],
		    &member);
		if (error == 0 &&
		    member.kind == ENGINE_STORE_MEMBER_REPLACING)
			return (B_TRUE);
	}
	return (B_FALSE);
}

int
pr19262_engine_r02_manifest_test_state(spa_t *spa,
    pr19262_r02_manifest_state_t *out)
{
	if (out == NULL)
		return (EINVAL);
	memset(out, 0, sizeof (*out));
	engine_t *engine = spa->spa_reloc;
	rw_enter(&engine->r02_topology_lock, RW_READER);
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine->store_lock);
	out->direct = engine_store_direct_topology(engine->vd);
	out->replacing = engine_store_has_replacing_topology(engine->vd);
	/* The observer's lock order is store, journal, then engine state. */
	mutex_enter(&engine->journal_lock);
	mutex_enter(&engine->lock);
	out->persistent = engine->spa == spa && engine->persistent;
	out->codec = engine->store_codec;
	out->io_errors = atomic_load_64(&engine->io_errors);
	out->revision = engine->revision;
	out->stored_revision = engine->stored_revision;
	out->durable_ub_txg = engine->durable_ub_txg;
	out->durable_catalog_txg = engine->durable_catalog_txg;
	out->arena = engine->arena;
	out->arena_bytes = engine->arena_bytes;
	out->journal_root = engine->journal_root;
	memcpy(out->journal_root_epoch, engine->journal_root_epoch,
	    sizeof (out->journal_root_epoch));
	out->journal_arena = engine->journal_arena;
	memcpy(out->journal_epoch, engine->journal_epoch,
	    sizeof (out->journal_epoch));
	out->journal_bytes = engine->journal_bytes;
	out->journal_seq = engine->journal_seq;
	out->journal_serial = engine->journal_serial;
	memcpy(&out->journal_hash, &engine->journal_hash,
	    sizeof (out->journal_hash));
	mutex_exit(&engine->lock);
	mutex_exit(&engine->journal_lock);
	mutex_exit(&engine->store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	rw_exit(&engine->r02_topology_lock);
	return (0);
}

/*
 * The test never mutates vdev topology or an on-disk root.  It feeds the
 * same eligibility gate a synthetic replacing shape or codec 5, both of
 * which must be refused before the collector can issue its raw reads.
 */
int
pr19262_engine_r02_manifest_test_reject(spa_t *spa, boolean_t replacing,
    uint64_t codec)
{
	engine_t *engine = spa->spa_reloc;
	rw_enter(&engine->r02_topology_lock, RW_READER);
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine->store_lock);
	boolean_t direct = !replacing &&
	    engine_store_direct_topology(engine->vd);
	int error = engine_r02_manifest_gate(spa, engine, direct, codec);
	mutex_exit(&engine->store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	rw_exit(&engine->r02_topology_lock);
	return (error);
}
#endif

typedef enum engine_store_leaf_io_mode {
	ENGINE_STORE_LEAF_IO_NORMAL,
	ENGINE_STORE_LEAF_IO_CONFIG_WRITER
} engine_store_leaf_io_mode_t;

/*
 * Create one raw I/O for an already-classified concrete leaf.  This is the
 * R02 exact-endpoint primitive: it deliberately has no parent traversal and
 * consequently cannot turn a replacing-mirror completion into an assertion
 * about either child.  It also does not acquire an SPA config lock.  A normal
 * raw caller holds its usual SCL_ZIO reader; the future attach writer will
 * use this same primitive while it owns SCL_ALL and therefore must not enter
 * SCL_ZIO recursively.  That writer uses a synchronous (pio == NULL) ZIO.
 */
/*
 * The endpoint-independent checks of one exact raw I/O.  The R02 pair writer
 * runs them for every endpoint before it creates any child, so an endpoint
 * that cannot take the write is reported instead of half-issuing the pair.
 */
static int
engine_store_leaf_check(engine_t *engine, vdev_t *leaf, uint64_t arena,
    uint64_t page, uint64_t pages, boolean_t write)
{
	if (engine == NULL || leaf == NULL || !leaf->vdev_ops->vdev_op_leaf ||
	    leaf->vdev_children != 0)
		return (ENOTSUP);
	if (engine->vd == NULL || engine->vd->vdev_children == 0 ||
	    pages == 0 || pages > UINT64_MAX / ENGINE_STORE_PAGE)
		return (EINVAL);
	uint64_t offset = VDEV_LABEL_START_SIZE +
	    arena / engine->vd->vdev_children + page * ENGINE_STORE_PAGE;
	uint64_t bytes = pages * ENGINE_STORE_PAGE;
	if (!vdev_readable(leaf) || (write && !vdev_writeable(leaf)))
		return (ENXIO);
	if (offset > leaf->vdev_psize || bytes > leaf->vdev_psize - offset)
		ENGINE_REFUSE(1);
	return (0);
}

static int
engine_store_leaf_zio(zio_t *pio, engine_t *engine, vdev_t *leaf,
    uint64_t arena, uint64_t page, uint64_t pages, abd_t *abd,
    boolean_t write, engine_store_leaf_io_mode_t mode, zio_t **zp)
{
	if (abd == NULL || zp == NULL)
		return (ENOTSUP);
	if (mode != ENGINE_STORE_LEAF_IO_NORMAL &&
	    mode != ENGINE_STORE_LEAF_IO_CONFIG_WRITER)
		return (EINVAL);
	if (mode == ENGINE_STORE_LEAF_IO_CONFIG_WRITER && pio != NULL)
		return (EINVAL);
	int error = engine_store_leaf_check(engine, leaf, arena, page, pages,
	    write);
	if (error != 0)
		return (error);
	uint64_t offset = VDEV_LABEL_START_SIZE +
	    arena / engine->vd->vdev_children + page * ENGINE_STORE_PAGE;
	uint64_t bytes = pages * ENGINE_STORE_PAGE;
	/*
	 * An aggregate of adjacent pages would reach the dispatch as a new
	 * physical write without the engine's authority, and be refused.
	 */
	zio_flag_t flags = ZIO_FLAG_CANFAIL | ZIO_FLAG_DONT_RETRY |
	    ZIO_FLAG_DONT_AGGREGATE;
	if (mode == ENGINE_STORE_LEAF_IO_CONFIG_WRITER)
		flags |= ZIO_FLAG_CONFIG_WRITER;
	zio_t *zio = write ? zio_write_phys(pio, leaf, offset, bytes,
	    abd, ZIO_CHECKSUM_OFF, NULL, NULL, ZIO_PRIORITY_SYNC_WRITE,
	    flags, B_FALSE) : zio_read_phys(pio, leaf, offset, bytes,
	    abd, ZIO_CHECKSUM_OFF, NULL, NULL, ZIO_PRIORITY_SYNC_READ,
	    flags, B_FALSE);
	zio->io_pr19262_metadata_authority = engine;
	*zp = zio;
	return (0);
}

/* Wait for one exact leaf; no parent result can mask this status. */
static int
engine_store_leaf_io(engine_t *engine, vdev_t *leaf, uint64_t arena,
    uint64_t page, uint64_t pages, abd_t *abd, boolean_t write,
    engine_store_leaf_io_mode_t mode)
{
	zio_t *zio;
	int error = engine_store_leaf_zio(NULL, engine, leaf, arena, page,
	    pages, abd, write, mode, &zio);
	if (error == 0)
		error = zio_wait(zio);
	return (error);
}

/*
 * Reseed durability requires the synchronous, reportable exact-leaf flush.
 * Do not substitute engine_metadata_flush_nowait(): it returns no endpoint
 * failure and is only suitable for the existing multi-member publisher.
 */
static int
engine_store_leaf_flush(vdev_t *leaf, engine_store_leaf_io_mode_t mode)
{
	if (leaf == NULL || !leaf->vdev_ops->vdev_op_leaf ||
	    leaf->vdev_children != 0)
		return (ENOTSUP);
	if (mode == ENGINE_STORE_LEAF_IO_CONFIG_WRITER)
		return (pr19262_engine_metadata_flush_config_writer(leaf));
	if (mode != ENGINE_STORE_LEAF_IO_NORMAL)
		return (EINVAL);
	return (pr19262_engine_metadata_flush(leaf));
}

/*
 * The final part of one exact write contract.  It is intentionally separate
 * from ordinary store publication: attaching it there would silently change
 * the existing batch protocol.  The future manifest binder must use it after
 * a successful write and synchronous exact-leaf flush.
 */
static int __maybe_unused
engine_store_leaf_readback_equal(engine_t *engine, vdev_t *leaf,
    uint64_t arena, uint64_t page, uint64_t pages, abd_t *expected,
    engine_store_leaf_io_mode_t mode)
{
	if (expected == NULL || pages == 0 ||
	    pages > UINT64_MAX / ENGINE_STORE_PAGE)
		return (EINVAL);
	uint64_t bytes = pages * ENGINE_STORE_PAGE;
	if (bytes != abd_get_size(expected))
		return (EINVAL);
	abd_t *actual = abd_alloc_linear(bytes, B_TRUE);
	int error = engine_store_leaf_io(engine, leaf, arena, page, pages,
	    actual, B_FALSE, mode);
	if (error == 0 && abd_cmp(actual, expected) != 0)
		error = ECKSUM;
	abd_free(actual);
	return (error);
}

/*
 * R02 exact-new-leaf reseed (D70), on by default (B3).  One admits only the
 * staged protocol below: a single strict replacing pair of two concrete
 * leaves in one engine member slot.  Every other replacement of an engine
 * member (hot spare, sequential rebuild, same-path replace, a nested attach,
 * a second pair, a codec-5 store) is still refused before any mutation.
 * Zero restores the D64 refusal of every new admission; an existing strict
 * pair is recovered and completed whatever the value.
 */
int zfs_pr19262_r02_reseed = 1;
#ifndef _KERNEL
/* ztest only: one crash cut and synthetic exact-new-endpoint failures. */
int zfs_pr19262_r02_reseed_cut = 0;
uint_t zfs_pr19262_r02_new_endpoint_faults = 0;
#endif

#define	ENGINE_R02_PAIR_NONE		0ULL
#define	ENGINE_R02_PAIR_ADMITTED	1ULL
#define	ENGINE_R02_PAIR_FAULTED		2ULL

static void
engine_r02_cut(uint_t stage)
{
#ifndef _KERNEL
	if (zfs_pr19262_r02_reseed_cut == (int)stage) {
		pr19262_printf("PR19262_R02_RESEED_CUT stage=%u\n", stage);
		pr19262_flush();
		pr19262_exit(100 + stage);
	}
#else
	(void) stage;
#endif
}

/*
 * ztest only: make the next exact-new-endpoint write or flush fail.  The
 * knob belongs to the cell that armed it: a scheduled fence (growth or
 * admission, run from the fence task) never consumes it.
 */
static int
engine_r02_test_fault(engine_t *engine)
{
#ifndef _KERNEL
	/* Set and cleared by the fence task itself: exact without a lock. */
	if (engine->fence_task_thread == curthread)
		return (0);
	volatile uint_t *faults = &zfs_pr19262_r02_new_endpoint_faults;
	uint_t n = atomic_load_32(faults);
	while (n != 0) {
		uint_t seen = atomic_cas_32(faults, n, n - 1);
		if (seen == n) {
			pr19262_printf("PR19262_R02_TEST_ENDPOINT_FAULT\n");
			return (SET_ERROR(EIO));
		}
		n = seen;
	}
#else
	(void) engine;
#endif
	return (0);
}

static boolean_t
engine_r02_pair_known(engine_t *engine, const engine_store_member_t *m)
{
	return (m->kind == ENGINE_STORE_MEMBER_REPLACING &&
	    atomic_load_64(&engine->r02_pair_state) != ENGINE_R02_PAIR_NONE &&
	    engine->r02_pair_old_guid == m->leaf[0]->vdev_guid &&
	    engine->r02_pair_new_guid == m->leaf[1]->vdev_guid);
}

/* R1: no parent or old-leaf success may hide an exact-endpoint failure. */
static void
engine_r02_pair_fault(engine_t *engine, int error)
{
	atomic_inc_64(&engine->r02_pair_faults);
	if (atomic_cas_64(&engine->r02_pair_state, ENGINE_R02_PAIR_ADMITTED,
	    ENGINE_R02_PAIR_FAULTED) == ENGINE_R02_PAIR_ADMITTED)
		pr19262_printf("PR19262_R02_PAIR_FAULT error=%d\n", error);
}

/* The old endpoint is superseded: its failure only marks it stale. */
static void
engine_r02_old_stale(engine_t *engine, int error)
{
	if (atomic_cas_64(&engine->r02_old_stale, 0, 1) == 0)
		pr19262_printf("PR19262_R02_OLD_STALE error=%d\n", error);
}

#define	ENGINE_R02_ROLE_DIRECT	0
#define	ENGINE_R02_ROLE_NEW	1
#define	ENGINE_R02_ROLE_OLD	2

typedef struct engine_store_endpoints {
	uint_t count;
	boolean_t pair;
	/* Write only: the old leaf had missed no write when resolved. */
	boolean_t old_current;
	vdev_t *leaf[2];
	uint_t role[2];
} engine_store_endpoints_t;

/*
 * Resolve one logical member slot to exact concrete endpoints, never to a
 * replacing parent.  A direct leaf is its own endpoint.  For a strict pair,
 * a read gets both leaves in preference order and the caller validates the
 * content it accepts.  A write needs the pair this engine seeded and
 * verified.  It goes to the exact new leaf while the pair is admitted, and
 * always to the old leaf while that is writeable: once the new leaf has
 * faulted it is frozen (stale) and the old leaf carries the slot, provided
 * it has missed no write.  An unwriteable old leaf becomes stale.
 */
static int
engine_store_member_endpoints(engine_t *engine, vdev_t *slot,
    boolean_t write, engine_store_endpoints_t *ep)
{
	engine_store_member_t member;
	memset(ep, 0, sizeof (*ep));
	int error = engine_store_member_classify(slot, &member);
	if (error != 0)
		return (error);
	if (member.kind == ENGINE_STORE_MEMBER_LEAF) {
		ep->count = 1;
		ep->leaf[0] = member.leaf[0];
		ep->role[0] = ENGINE_R02_ROLE_DIRECT;
		return (0);
	}
	vdev_t *oleaf = member.leaf[0], *nleaf = member.leaf[1];
	boolean_t known = engine_r02_pair_known(engine, &member);
	uint64_t state = atomic_load_64(&engine->r02_pair_state);
	ep->pair = B_TRUE;
	if (!write) {
		boolean_t fresh = known && state == ENGINE_R02_PAIR_ADMITTED;
		ep->leaf[0] = fresh ? nleaf : oleaf;
		ep->leaf[1] = fresh ? oleaf : nleaf;
		ep->role[0] = fresh ? ENGINE_R02_ROLE_NEW : ENGINE_R02_ROLE_OLD;
		ep->role[1] = fresh ? ENGINE_R02_ROLE_OLD : ENGINE_R02_ROLE_NEW;
		ep->count = 2;
		return (0);
	}
	if (!known)
		return (SET_ERROR(ENOTSUP));
	if (state == ENGINE_R02_PAIR_ADMITTED) {
		ep->leaf[ep->count] = nleaf;
		ep->role[ep->count++] = ENGINE_R02_ROLE_NEW;
	}
	if (vdev_writeable(oleaf)) {
		ep->old_current =
		    atomic_load_64(&engine->r02_old_stale) == 0;
		ep->leaf[ep->count] = oleaf;
		ep->role[ep->count++] = ENGINE_R02_ROLE_OLD;
	} else {
		engine_r02_old_stale(engine, ENXIO);
	}
	return (ep->count == 0 ? SET_ERROR(ENXIO) : 0);
}

/*
 * One slot result from its exact endpoint results.  The admitted new leaf,
 * or an old leaf that has missed no write, can hold the slot.  A failed new
 * leaf freezes the pair (FAULTED: it is reseeded and audited before the old
 * leaf may be detached); a failed old leaf becomes stale (it is reseeded
 * before a cancel may collapse onto it).  Neither failure is hidden, and
 * the slot fails only when no eligible endpoint holds the data.
 */
static int
engine_r02_slot_result(engine_t *engine, const engine_store_endpoints_t *ep,
    const int *err)
{
	if (!ep->pair)
		return (err[0]);
	boolean_t held = B_FALSE, by_new = B_FALSE;
	int first = 0;
	for (uint_t i = 0; i < ep->count; i++) {
		if (err[i] == 0) {
			by_new |= ep->role[i] == ENGINE_R02_ROLE_NEW;
			held |= ep->role[i] == ENGINE_R02_ROLE_NEW ||
			    ep->old_current;
			continue;
		}
		if (first == 0)
			first = err[i];
		if (ep->role[i] == ENGINE_R02_ROLE_NEW)
			engine_r02_pair_fault(engine, err[i]);
		else
			engine_r02_old_stale(engine, err[i]);
	}
	if (held) {
		atomic_inc_64(&engine->r02_pair_writes);
		if (!by_new)
			atomic_inc_64(&engine->r02_pair_old_held);
		return (0);
	}
	return (first != 0 ? first : SET_ERROR(EIO));
}

/*
 * L23 (A35): a direct member whose raw write or flush failed (absent,
 * offline, faulted, an I/O error) may lack pages that every other member
 * holds.  Mark its slot until pr19262_engine_member_readmit() audits it
 * clean against the closure.  A pair slot is the R02 code's (old_stale,
 * FAULTED).  No lock: the pass clears a mark only while it excludes raw I/O.
 * A member that is still readable and writeable (an I/O error, not an
 * absence) gets no state exit, so a new mark asks the async thread for the
 * pass; an absent one is passed when it returns.
 */
static void
engine_member_missed(engine_t *engine, vdev_t *slot, int error)
{
	if (error == 0 || engine->vd == NULL || slot == NULL ||
	    slot->vdev_parent != engine->vd ||
	    !slot->vdev_ops->vdev_op_leaf || slot->vdev_children != 0 ||
	    slot->vdev_id >= ENGINE_MEMBER_BITS)
		return;
	uint64_t *w = &engine->member_missed[slot->vdev_id / 64];
	uint64_t bit = 1ULL << (slot->vdev_id % 64), old;
	do {
		old = atomic_load_64(w);
		if (old & bit)
			return;
	} while (atomic_cas_64(w, old, old | bit) != old);
	boolean_t usable = vdev_readable(slot) && vdev_writeable(slot);
	pr19262_printf("PR19262_MEMBER_MISSED slot=%llu error=%d usable=%d\n",
	    (u_longlong_t)slot->vdev_id, error, usable);
	if (usable) {
		atomic_store_64(&engine->member_readmit_retries, 0);
		spa_async_request(slot->vdev_spa, SPA_ASYNC_MEMBER_READMIT);
	}
}

/* Read a pair in preference order; the caller validates the content. */
static int
engine_store_endpoints_read(engine_t *engine,
    const engine_store_endpoints_t *ep, uint64_t arena, uint64_t page,
    uint64_t pages, abd_t *abd, engine_store_leaf_io_mode_t mode)
{
	int error = ENXIO;
	if (ep->pair)
		atomic_inc_64(&engine->r02_pair_reads);
	for (uint_t i = 0; i < ep->count; i++) {
		error = engine_store_leaf_io(engine, ep->leaf[i], arena, page,
		    pages, abd, B_FALSE, mode);
		if (error == 0)
			break;
	}
	return (error);
}

/* Write and wait for every exact endpoint, then combine their results. */
static int
engine_store_endpoints_write(engine_t *engine,
    const engine_store_endpoints_t *ep, uint64_t arena, uint64_t page,
    uint64_t pages, abd_t *abd)
{
	int err[2] = { 0, 0 };
	for (uint_t i = 0; i < ep->count; i++) {
		if (ep->role[i] == ENGINE_R02_ROLE_NEW)
			err[i] = engine_r02_test_fault(engine);
		if (err[i] == 0)
			err[i] = engine_store_leaf_io(engine, ep->leaf[i],
			    arena, page, pages, abd, B_TRUE,
			    ENGINE_STORE_LEAF_IO_NORMAL);
	}
	return (engine_r02_slot_result(engine, ep, err));
}

/* Create one raw member I/O as a child of pio (NULL: caller waits). */
static int
engine_store_zio(zio_t *pio, vdev_t *vd, uint64_t arena, uint64_t page,
    uint64_t pages, abd_t *abd, boolean_t write, zio_t **zp)
{
	engine_t *engine = vd->vdev_spa->spa_reloc;
	engine_store_endpoints_t ep;
	int error = engine_store_member_endpoints(engine, vd, write, &ep);
	if (error != 0)
		return (error);
	if (!ep.pair || !write) {
		vdev_t *leaf = ep.leaf[0];
		/*
		 * An asynchronous pair read (the control-slot reader that
		 * reached a replacing parent before D69) takes the first
		 * readable exact leaf.
		 */
		if (ep.pair)
			atomic_inc_64(&engine->r02_pair_async_reads);
		if (ep.pair && !vdev_readable(leaf))
			leaf = ep.leaf[1];
		return (engine_store_leaf_zio(pio, engine, leaf, arena, page,
		    pages, abd, write, ENGINE_STORE_LEAF_IO_NORMAL, zp));
	}
	/*
	 * No current caller issues an asynchronous raw write to a member
	 * slot (engine_members_write() resolves endpoints itself).  Refuse
	 * rather than let one parent ZIO mix the new and old endpoint results.
	 */
	return (SET_ERROR(ENOTSUP));
}

static int
engine_store_flush(vdev_t *vd)
{
	engine_t *engine = vd->vdev_spa->spa_reloc;
	engine_store_endpoints_t ep;
	int error = engine_store_member_endpoints(engine, vd, B_TRUE, &ep);
	if (error != 0) {
		engine_member_missed(engine, vd, error);
		return (error);
	}
	int err[2] = { 0, 0 };
	for (uint_t i = 0; i < ep.count; i++) {
		if (ep.role[i] == ENGINE_R02_ROLE_NEW)
			err[i] = engine_r02_test_fault(engine);
		if (err[i] == 0)
			err[i] = engine_store_leaf_flush(ep.leaf[i],
			    ENGINE_STORE_LEAF_IO_NORMAL);
	}
	error = engine_r02_slot_result(engine, &ep, err);
	if (!ep.pair)
		engine_member_missed(engine, vd, error);
	return (error);
}

static int
engine_store_io_at(vdev_t *vd, uint64_t arena, uint64_t page,
    uint64_t pages, abd_t *abd, boolean_t write)
{
	engine_t *engine = vd->vdev_spa->spa_reloc;
	engine_store_endpoints_t ep;
	int error = engine_store_member_endpoints(engine, vd, write, &ep);
	if (error == 0 && write)
		error = engine_store_endpoints_write(engine, &ep, arena, page,
		    pages, abd);
	else if (error == 0)
		error = engine_store_endpoints_read(engine, &ep, arena, page,
		    pages, abd, ENGINE_STORE_LEAF_IO_NORMAL);
	if (error != 0)
		atomic_inc_64(&engine->io_errors);
	if (write && !ep.pair)
		engine_member_missed(engine, vd, error);
	return (error);
}

/*
 * The R02 manifest observer must not use engine_store_io_at(): that helper
 * deliberately accounts every failure in engine->io_errors.  This exact-leaf
 * read is otherwise intentionally the same physical operation, but owns no
 * engine state and never issues a flush or a write.  The reseed writer
 * passes its config-writer mode because it already owns SCL_ALL.
 */
static int
engine_store_manifest_read(vdev_t *vd, uint64_t arena, uint64_t page,
    abd_t *abd, engine_store_leaf_io_mode_t mode)
{
	engine_t *engine = vd->vdev_spa->spa_reloc;
	engine_store_endpoints_t ep;
	int error = engine_store_member_endpoints(engine, vd, B_FALSE, &ep);
	return (error == 0 ? engine_store_endpoints_read(engine, &ep, arena,
	    page, 1, abd, mode) : error);
}

/* The exact leaf whose state stands for a member slot, or NULL. */
static vdev_t *
engine_store_r02_member_leaf(vdev_t *slot)
{
	engine_store_member_t member;
	if (engine_store_member_classify(slot, &member) != 0)
		return (NULL);
	return (member.kind == ENGINE_STORE_MEMBER_LEAF ? member.leaf[0] :
	    member.leaf[1]);
}

/*
 * Every member is a direct leaf except exactly one strict replacing pair,
 * which is the pair this engine knows.  writable additionally requires
 * that pair to be admitted rather than faulted.
 */
static boolean_t
engine_store_r02_topology(vdev_t *vd, boolean_t writable)
{
	if (vd == NULL || vd->vdev_children == 0)
		return (B_FALSE);
	engine_t *engine = vd->vdev_spa->spa_reloc;
	uint_t pairs = 0;
	for (uint_t c = 0; c < vd->vdev_children; c++) {
		engine_store_member_t member;
		if (engine_store_member_classify(vd->vdev_child[c],
		    &member) != 0)
			return (B_FALSE);
		if (member.kind == ENGINE_STORE_MEMBER_LEAF)
			continue;
		if (!engine_r02_pair_known(engine, &member) ||
		    engine->r02_pair_slot != c)
			return (B_FALSE);
		pairs++;
	}
	return (pairs == 1 && (!writable ||
	    atomic_load_64(&engine->r02_pair_state) ==
	    ENGINE_R02_PAIR_ADMITTED));
}

/* A member's result: the first error, which an absence does not mask. */
static void
engine_member_error(engine_t *engine, int *member, int error)
{
	if (error != 0)
		atomic_inc_64(&engine->io_errors);
	if (error != 0 && (*member == 0 || error != ENXIO))
		*member = error;
}

/* Pages per merged write: 256 KiB. */
#define	ENGINE_MEMBER_RUN	64

typedef struct engine_member_run {
	uint64_t arena;
	uint64_t page;
	uint64_t pages;
	abd_t *abd;
	boolean_t owned;
} engine_member_run_t;

static int
engine_member_order(const void *a, const void *b)
{
	const uint64_t *x = a, *y = b;
	for (int i = 0; i < 2; i++)
		if (x[i] != y[i])
			return (x[i] < y[i] ? -1 : 1);
	return (0);
}

/*
 * Would a raw write of these bytes at this member offset (below the
 * labels) be the engine's own, as pr19262_engine_dispatch checks?  A run
 * never crosses from one kind of extent into another.
 */
static boolean_t
engine_member_run_ok(engine_t *engine, uint64_t offset, uint64_t bytes)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t children = engine->vd->vdev_children;
	return ((engine->store_extents != NULL &&
	    zfs_range_tree_contains(engine->store_extents, offset * children,
	    bytes * children)) ||
	    (engine->journal_extents != NULL &&
	    zfs_range_tree_contains(engine->journal_extents, offset * children,
	    bytes * children)));
}

/*
 * The pages in member-offset order, cut into runs of consecutive pages
 * inside one kind of extent: a run of two or more gets one linear buffer,
 * so every member writes it once instead of page by page (a store's new
 * pages and a group's journal frames are mostly consecutive).  Each page
 * validates on its own, so a torn run is some pages landed and some not,
 * as with single writes.
 */
static uint_t
engine_member_runs(engine_t *engine, const engine_member_page_t *pv, uint_t n,
    engine_member_run_t *runs)
{
	uint64_t children = engine->vd->vdev_children;
	uint64_t *order = pr19262_zalloc(MAX(1, n) * 2 * sizeof (uint64_t),
	    KM_SLEEP);
	for (uint_t i = 0; i < n; i++) {
		order[2 * i] = pv[i].arena / children +
		    pv[i].page * ENGINE_STORE_PAGE;
		order[2 * i + 1] = i;
	}
	qsort(order, n, 2 * sizeof (uint64_t), engine_member_order);
	uint_t r = 0;
	mutex_enter(&engine->lock);
	for (uint_t i = 0; i < n; ) {
		uint_t j = i + 1;
		while (j < n && j - i < ENGINE_MEMBER_RUN &&
		    order[2 * j] == order[2 * i] + (j - i) *
		    ENGINE_STORE_PAGE &&
		    engine_member_run_ok(engine, order[2 * i],
		    (j - i + 1) * ENGINE_STORE_PAGE))
			j++;
		const engine_member_page_t *first = &pv[order[2 * i + 1]];
		engine_member_run_t *run = &runs[r++];
		run->arena = first->arena;
		run->page = first->page;
		run->pages = j - i;
		run->owned = j - i > 1;
		run->abd = first->abd;
		if (run->owned) {
			run->abd = abd_alloc_linear((j - i) * ENGINE_STORE_PAGE,
			    B_TRUE);
			for (uint_t k = i; k < j; k++)
				abd_copy_off(run->abd, pv[order[2 * k + 1]].abd,
				    (k - i) * ENGINE_STORE_PAGE, 0,
				    ENGINE_STORE_PAGE);
		}
		i = j;
	}
	mutex_exit(&engine->lock);
	kmem_free(order, MAX(1, n) * 2 * sizeof (uint64_t));
	return (r);
}

/*
 * Where the catalog stores spend their time, summed over every stored
 * root (the kernel status line reports it).
 */

/*
 * One raw step on every member of the engine's vdev at once: every
 * member's page writes are issued before any is waited for, then every
 * member's flush.  The serial form wrote and flushed one member after
 * the other, so each journal frame and each catalog batch cost two round
 * trips per member.  The order of the steps is kept: a member flushes
 * after its writes completed, and the call returns after every flush.
 * errors[c] receives each member's result for the caller's quorum; a
 * member whose writes failed is flushed only when flush_failed is set.
 * No pages: only the flushes.  The caller holds SCL_ZIO.  cost, when
 * set, receives the runs and the time of the writes and of the flushes.
 */
static void
engine_members_write(spa_t *spa, const engine_member_page_t *pv,
    uint_t n, boolean_t flush, boolean_t flush_failed, int *errors,
    engine_store_cost_t *cost)
{
	engine_t *engine = spa->spa_reloc;
	hrtime_t start = gethrtime();
	uint_t children = engine->vd->vdev_children;
	/*
	 * One root and one result per exact endpoint: a pair's two leaves are
	 * independent results that engine_r02_slot_result() combines.
	 */
	uint_t slots = children * 2;
	zio_t **root = kmem_zalloc(slots * sizeof (zio_t *), KM_SLEEP);
	int *perr = kmem_zalloc(slots * sizeof (int), KM_SLEEP);
	engine_store_endpoints_t *ep = kmem_zalloc(children * sizeof (*ep),
	    KM_SLEEP);
	boolean_t *resolved = kmem_zalloc(children * sizeof (boolean_t),
	    KM_SLEEP);
	engine_member_run_t *runs = pr19262_zalloc(MAX(1, n) * sizeof (*runs),
	    KM_SLEEP);
	uint_t nr = n == 0 ? 0 : engine_member_runs(engine, pv, n, runs);
	for (uint_t c = 0; c < children; c++) {
		errors[c] = 0;
		int e = engine_store_member_endpoints(engine,
		    engine->vd->vdev_child[c], B_TRUE, &ep[c]);
		if (e != 0) {
			engine_member_error(engine, &errors[c], e);
			continue;
		}
		resolved[c] = B_TRUE;
		if (nr == 0)
			continue;
		for (uint_t k = 0; k < ep[c].count; k++) {
			uint_t r = 2 * c + k;
			if (ep[c].role[k] == ENGINE_R02_ROLE_NEW &&
			    (perr[r] = engine_r02_test_fault(engine)) != 0)
				continue;
			root[r] = zio_root(spa, NULL, NULL, ZIO_FLAG_CANFAIL);
			for (uint_t i = 0; i < nr; i++) {
				zio_t *zio;
				e = engine_store_leaf_zio(root[r], engine,
				    ep[c].leaf[k], runs[i].arena, runs[i].page,
				    runs[i].pages, runs[i].abd, B_TRUE,
				    ENGINE_STORE_LEAF_IO_NORMAL, &zio);
				if (e != 0 && !ep[c].pair)
					engine_member_error(engine, &errors[c],
					    e);
				else if (e != 0 && perr[r] == 0)
					perr[r] = e;
				else if (e == 0)
					zio_nowait(zio);
			}
		}
	}
	for (uint_t r = 0; r < slots; r++) {
		if (root[r] == NULL)
			continue;
		int e = zio_wait(root[r]);
		root[r] = NULL;
		if (!ep[r / 2].pair)
			engine_member_error(engine, &errors[r / 2], e);
		else if (e != 0 && perr[r] == 0)
			perr[r] = e;
	}
	for (uint_t i = 0; i < nr; i++)
		if (runs[i].owned)
			abd_free(runs[i].abd);
	kmem_free(runs, MAX(1, n) * sizeof (*runs));
	hrtime_t written = gethrtime();
	if (flush) {
		for (uint_t c = 0; c < children; c++) {
			if (!resolved[c] || (!ep[c].pair && errors[c] != 0 &&
			    !flush_failed))
				continue;
			for (uint_t k = 0; k < ep[c].count; k++) {
				uint_t r = 2 * c + k;
				if (ep[c].pair && perr[r] != 0 &&
				    !flush_failed)
					continue;
				root[r] = zio_root(spa, NULL, NULL,
				    ZIO_FLAG_CANFAIL);
				pr19262_engine_metadata_flush_nowait(root[r],
				    ep[c].leaf[k]);
			}
		}
		for (uint_t r = 0; r < slots; r++) {
			if (root[r] == NULL)
				continue;
			int e = zio_wait(root[r]);
			root[r] = NULL;
			if (!ep[r / 2].pair)
				engine_member_error(engine, &errors[r / 2], e);
			else if (e != 0 && perr[r] == 0)
				perr[r] = e;
		}
	}
	/* A pair slot's result is decided once, from both endpoints. */
	for (uint_t c = 0; c < children; c++) {
		if (resolved[c] && ep[c].pair)
			engine_member_error(engine, &errors[c],
			    engine_r02_slot_result(engine, &ep[c],
			    &perr[2 * c]));
		else if (nr != 0 || flush)
			engine_member_missed(engine, engine->vd->vdev_child[c],
			    errors[c]);
	}
	kmem_free(perr, slots * sizeof (int));
	kmem_free(root, slots * sizeof (zio_t *));
	kmem_free(resolved, children * sizeof (boolean_t));
	kmem_free(ep, children * sizeof (*ep));
	if (cost != NULL) {
		cost->runs += nr;
		cost->write += written - start;
		cost->flush += gethrtime() - written;
	}
}

/* Flush every member at once and count the results in q. */
static void
engine_members_flush(spa_t *spa, engine_quorum_t *q)
{
	engine_t *engine = spa->spa_reloc;
	uint_t children = engine->vd->vdev_children;
	int *errors = kmem_alloc(children * sizeof (int), KM_SLEEP);
	engine_members_write(spa, NULL, 0, B_TRUE, B_TRUE, errors, NULL);
	for (uint_t c = 0; c < children; c++)
		engine_quorum_add(engine, q, errors[c]);
	kmem_free(errors, children * sizeof (int));
}

static int
engine_store_io(vdev_t *vd, uint64_t page, uint64_t pages, abd_t *abd,
    boolean_t write)
{
	engine_t *engine = vd->vdev_spa->spa_reloc;
	return (engine_store_io_at(vd, engine->arena, page, pages, abd, write));
}

/*
 * The owners the dirty and gone lists name when a store takes its rows:
 * (V, incarnation, mark, listed) each, in key order.  Debug ztest checks
 * every row the store changes against them; a stored root settles them.
 */
typedef struct engine_dirty_snap {
	uint64_t *keys;
	uint_t count;
	uint_t gone;
	boolean_t all;
} engine_dirty_snap_t;

static int
engine_dirty_compare(const void *a, const void *b)
{
	const uint64_t *x = a, *y = b;
	for (int i = 0; i < 2; i++)
		if (x[i] != y[i])
			return (x[i] < y[i] ? -1 : 1);
	return (0);
}

/* Is the owner in a root of this txg? */
static boolean_t
engine_store_listed(const engine_owner_t *e, const uberblock_t *ub)
{
	return (!e->aborted && e->catalog_txg <= ub->ub_txg &&
	    !e->journal_pending);
}

static void
engine_dirty_snapshot(engine_t *engine, const uberblock_t *ub,
    engine_dirty_snap_t *s)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint_t n = 0;
	for (engine_owner_t *e = list_head(&engine->dirty); e != NULL;
	    e = list_next(&engine->dirty, e))
		n++;
	s->all = engine->store_all;
	s->gone = engine->gone_count;
	s->count = n + s->gone;
	s->keys = pr19262_zalloc(MAX(1, s->count) * 4 * sizeof (uint64_t),
	    KM_SLEEP);
	uint64_t *k = s->keys;
	for (engine_owner_t *e = list_head(&engine->dirty); e != NULL;
	    e = list_next(&engine->dirty, e), k += 4) {
		k[0] = e->v;
		k[1] = e->incarnation;
		k[2] = e->dirty_seq;
		/* Not listed: the owner stays marked until it is or goes. */
		k[3] = engine_store_listed(e, ub);
	}
	for (uint_t i = 0; i < s->gone; i++, k += 4) {
		k[0] = engine->gone[2 * i];
		k[1] = engine->gone[2 * i + 1];
		k[2] = 0;
		k[3] = 1;
	}
	qsort(s->keys, s->count, 4 * sizeof (uint64_t), engine_dirty_compare);
	/* An incremental store visits each owner once. */
	for (uint_t i = 1; i < s->count; i++)
		VERIFY(engine_dirty_compare(&s->keys[4 * (i - 1)],
		    &s->keys[4 * i]) < 0);
}

static void
engine_dirty_free(engine_dirty_snap_t *s)
{
	kmem_free(s->keys, MAX(1, s->count) * 4 * sizeof (uint64_t));
	s->keys = NULL;
}

/* The listed owner of a snapshot entry; NULL if gone or not listed. */
static engine_owner_t *
engine_dirty_owner(engine_t *engine, const engine_dirty_snap_t *s, uint_t i)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	const uint64_t *k = &s->keys[4 * i];
	if (k[2] == 0 || k[3] == 0)
		return (NULL);
	engine_owner_t *e = engine_find(engine, k[0]);
	VERIFY(e != NULL && e->incarnation == k[1]);
	return (e);
}

/* Does the snapshot name the owner of this row key? */
static boolean_t
engine_dirty_covers(const engine_dirty_snap_t *s, const uint64_t key[4])
{
	if (s->all)
		return (B_TRUE);
	uint64_t want[2] = { key[1], key[2] };
	uint_t lo = 0, hi = s->count;
	while (lo < hi) {
		uint_t mid = lo + (hi - lo) / 2;
		int order = engine_dirty_compare(want, &s->keys[4 * mid]);
		if (order == 0)
			return (B_TRUE);
		if (order < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return (B_FALSE);
}

/*
 * A stored root covers the snapshot: its listed owners leave the dirty
 * list unless marked again since, and its gone entries are done.
 */
static void
engine_dirty_settle(engine_t *engine, const engine_dirty_snap_t *s)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	for (uint_t i = 0; i < s->count; i++) {
		const uint64_t *k = &s->keys[4 * i];
		if (k[2] == 0 || k[3] == 0)
			continue;
		engine_owner_t *e = engine_find(engine, k[0]);
		if (e != NULL && e->incarnation == k[1] &&
		    e->dirty_seq == k[2] && list_link_active(&e->dirty_node)) {
			/* Settle its reserve before removing it. */
			engine_admit_owner_update(engine, e);
			engine_owner_unlist(engine, e);
		}
	}
	VERIFY3U(engine->gone_count, >=, s->gone);
	memmove(engine->gone, engine->gone + 2 * s->gone,
	    (engine->gone_count - s->gone) * 2 * sizeof (uint64_t));
	engine->gone_count -= s->gone;
	if (s->all)
		engine->store_all = B_FALSE;
}

#include "vdev_reloc_pages.c"

/* R02 pair discovery and import recovery live with the reseed code. */
static void engine_r02_pair_discover(engine_t *);
static int engine_r02_pair_recover(spa_t *);

static uint_t
engine_store_tolerated(engine_t *engine)
{
	uint64_t tolerance = engine->pages->certified != 0 ?
	    engine->pages->certified : engine->store_tolerance;
	return (MIN(tolerance, VDEV_RAIDZ_MAXPARITY));
}

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

static uint_t
engine_store_owner_count(const engine_owner_t *e)
{
	uint_t n = 1;
	for (engine_version_t *b = e->versions; b != NULL; b = b->next)
		n += b != e->cur && b->retired;
	return (n);
}

/*
 * A listed owner's rows: the current version, then every other retired
 * one.  The first root that lists a retirement stamps its txg; with stamp
 * false (the checking full rows after an incremental store's own) there
 * must be nothing left to stamp.
 */
static uint_t
engine_store_owner_rows(engine_t *engine, engine_owner_t *e,
    const uberblock_t *ub,
    boolean_t stamp, engine_mrow_t *rows)
{
	if (e->retired && e->retired_txg == 0) {
		ENGINE_STORE_CHECK(stamp);
		if (stamp)
			e->retired_txg = ub->ub_txg;
	}
	uint64_t identity[] = { e->v, e->size, e->allocation_txg,
	    e->incarnation, e->created, e->retired_at,
	    e->retired ? 1 : (e->checkpoint_only ? 2 : 0),
	    e->catalog_txg };
	uint_t at = 0;
	engine_version_t *b = e->cur;
	for (;;) {
		if (b == e->cur || b->retired) {
			if (b != e->cur && b->retired_txg == 0) {
				ENGINE_STORE_CHECK(stamp);
				if (stamp)
					b->retired_txg = ub->ub_txg;
			}
			uint64_t values[] = { b->p, b->size, b->birth,
			    b->width, b->parity, b->ready, b->retired,
			    b->generation };
			engine_mrow_fill(&rows[at++], identity, values,
			    b == e->cur);
		}
		/* Current first, then all other retired versions. */
		b = b == e->cur ? e->versions : b->next;
		if (b == e->cur)
			b = b->next;
		if (b == NULL)
			break;
	}
	return (at);
}

/*
 * The paged catalog's rows of this root, straight from the owners: every
 * listed owner's (all) or the listed owners' a snapshot names.
 */
static int
engine_store_rows(engine_t *engine, const uberblock_t *ub,
    const engine_dirty_snap_t *s,
    boolean_t stamp, engine_mrow_t **out, uint_t *count)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t total = 0;
	if (s == NULL) {
		for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
		    e = AVL_NEXT(&engine->owners, e))
			if (engine_store_listed(e, ub))
				total += engine_store_owner_count(e);
	} else {
		for (uint_t i = 0; i < s->count; i++) {
			engine_owner_t *e = engine_dirty_owner(engine, s, i);
			if (e != NULL)
				total += engine_store_owner_count(e);
		}
	}
	/* The arena limit is checked by the store, once it is open. */
	if (total > UINT_MAX)
		return (ENGINE_EINVAL(9));
	engine_mrow_t *rows = pr19262_zalloc(MAX(1, total) * sizeof (*rows),
	    KM_SLEEP);
	uint_t at = 0;
	if (s == NULL) {
		for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
		    e = AVL_NEXT(&engine->owners, e))
			if (engine_store_listed(e, ub))
				at += engine_store_owner_rows(engine, e, ub,
				    stamp,
				    rows + at);
	} else {
		for (uint_t i = 0; i < s->count; i++) {
			engine_owner_t *e = engine_dirty_owner(engine, s, i);
			if (e != NULL)
				at += engine_store_owner_rows(engine, e, ub,
				    stamp,
				    rows + at);
		}
	}
	VERIFY3U(at, ==, total);
	int error = engine_mrows_order(rows, total);
	if (error != 0) {
		kmem_free(rows, MAX(1, total) * sizeof (*rows));
		return (error);
	}
	*out = rows;
	*count = total;
	return (0);
}

static nvlist_t *
engine_store_snapshot(spa_t *spa, const uberblock_t *ub, boolean_t owners)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->lock));
	nvlist_t *nv = fnvlist_alloc();
	fnvlist_add_uint64(nv, "txg", ub->ub_txg);
	fnvlist_add_uint64(nv, "pool", spa_guid(spa));
	fnvlist_add_uint64(nv, "top", engine->vd->vdev_guid);
	fnvlist_add_uint64_array(nv, "mos", (uint64_t *)&ub->ub_rootbp,
	    sizeof (blkptr_t) / sizeof (uint64_t));
	uint64_t geometry[] = { engine->total, engine->vd->vdev_ms_shift,
	    engine->vd->vdev_ashift, engine->vd->vdev_children,
	    engine->arena, engine->arena_bytes };
	fnvlist_add_uint64_array(nv, "geometry", geometry, 6);
	uint64_t journal[] = { engine->journal_root, engine->journal_bytes,
	    engine->journal_root_epoch[0], engine->journal_root_epoch[1] };
	fnvlist_add_uint64_array(nv, "journal", journal, 4);
	/*
	 * This catalog names the current root: cut segments may go at a
	 * fence whose floor root's catalog is at least this one.  Until then
	 * they stay quarantined, so that a resume from this catalog cannot
	 * reuse an extent an older importable root still reads.
	 */
	uint_t retired = 0;
	for (engine_j_segment_t *s = engine->journal_retired; s != NULL;
	    s = s->next) {
		if (s->free_after == UINT64_MAX)
			s->free_after = ub->ub_txg;
		retired++;
	}
	uint64_t *cut = kmem_alloc(MAX(1, retired) * 5 * sizeof (uint64_t),
	    KM_SLEEP);
	uint_t at = 0;
	for (engine_j_segment_t *s = engine->journal_retired; s != NULL;
	    s = s->next) {
		cut[at++] = s->arena;
		cut[at++] = engine->journal_bytes;
		cut[at++] = s->epoch[0];
		cut[at++] = s->epoch[1];
		cut[at++] = s->free_after;
	}
	fnvlist_add_uint64_array(nv, "journal-retired", cut, at);
	kmem_free(cut, MAX(1, retired) * 5 * sizeof (uint64_t));
	fnvlist_add_uint64_array(nv, "checkpoint",
	    (uint64_t *)&engine->checkpoint,
	    sizeof (engine->checkpoint) / sizeof (uint64_t));
	uint64_t state[] = { engine->sequence, engine->next_incarnation,
	    engine->retired_limit };
	fnvlist_add_uint64_array(nv, "state", state, 3);
	uint64_t max = owners ? avl_numnodes(&engine->owners) : 0;
	nvlist_t **rows = pr19262_alloc(MAX(1, max) * sizeof (*rows),
	    KM_SLEEP);
	uint_t n = 0;
	/* The paged store takes its rows from engine_store_rows instead. */
	for (engine_owner_t *e = owners ? avl_first(&engine->owners) : NULL;
	    e != NULL; e = AVL_NEXT(&engine->owners, e)) {
		/* New uncommitted V allocations are outside this MOS root. */
		if (e->aborted || e->catalog_txg > ub->ub_txg ||
		    e->journal_pending)
			continue;
		nvlist_t *row = rows[n++] = fnvlist_alloc();
		if (e->retired && e->retired_txg == 0)
			e->retired_txg = ub->ub_txg;
		uint64_t identity[] = { e->v, e->size, e->allocation_txg,
		    e->incarnation, e->created, e->retired_at,
		    e->retired ? 1 : (e->checkpoint_only ? 2 : 0),
		    e->catalog_txg };
		fnvlist_add_uint64_array(row, "V", identity, 8);
		uint_t count = 1;
		for (engine_version_t *b = e->versions; b != NULL; b = b->next)
			count += b != e->cur && b->retired;
		uint64_t *versions = pr19262_alloc(
		    count * 8 * sizeof (uint64_t),
		    KM_SLEEP);
		uint_t j = 0;
		engine_version_t *b = e->cur;
		for (;;) {
			if (b == e->cur || b->retired) {
				if (b != e->cur && b->retired_txg == 0)
					b->retired_txg = ub->ub_txg;
				uint64_t values[] = { b->p, b->size, b->birth,
				    b->width, b->parity, b->ready, b->retired,
				    b->generation };
				memcpy(&versions[j], values, sizeof (values));
				j += 8;
			}
			/* Current first, then all other retired versions. */
			b = b == e->cur ? e->versions : b->next;
			if (b == e->cur)
				b = b->next;
			if (b == NULL)
				break;
		}
		VERIFY3U(j, ==, count * 8);
		fnvlist_add_uint64_array(row, "P", versions, j);
		kmem_free(versions, count * 8 * sizeof (uint64_t));
	}
	if (owners)
		fnvlist_add_nvlist_array(nv, "owners",
		    (const nvlist_t * const *)rows, n);
	for (uint_t i = 0; i < n; i++)
		fnvlist_free(rows[i]);
	kmem_free(rows, MAX(1, max) * sizeof (*rows));
	uint64_t nr = zfs_range_tree_numsegs(engine->legacy) * 2;
	engine_ranges_t r = { pr19262_alloc(MAX(1, nr) * sizeof (uint64_t),
	    KM_SLEEP), 0 };
	zfs_range_tree_walk(engine->legacy, engine_store_range, &r);
	fnvlist_add_uint64_array(nv, "legacy", r.values, r.count);
	kmem_free(r.values, MAX(1, nr) * sizeof (uint64_t));
	fnvlist_add_byte_array(nv, "bitmap", engine->bitmap,
	    ENGINE_STORE_PAGES);
	return (nv);
}

/*
 * A paged catalog root of this txg.  Entered under engine.lock, which it
 * drops.  Out of line, so the page store runs on a small frame in the
 * txg sync thread.
 */
noinline static int
engine_store_paged(spa_t *spa, uberblock_t *ub, nvlist_t *nv)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_dirty_snap_t snap;
	engine_dirty_snapshot(engine, ub, &snap);
	/*
	 * After engage or import every listed owner's rows; otherwise only
	 * those of the owners the snapshot names.  Debug ztest, or a kernel
	 * with store_verify set, builds the full rows as well and checks the
	 * tree against them.
	 */
	engine_mrow_t *rows = NULL, *full = NULL;
	uint_t count = 0, full_count = 0;
	int error = engine_store_rows(engine, ub, snap.all ? NULL : &snap,
	    B_TRUE,
	    &rows, &count);
	if (error == 0 && !snap.all &&
	    (ENGINE_MIRROR || zfs_pr19262_store_verify != 0)) {
		error = engine_store_rows(engine, ub, NULL, B_FALSE, &full,
		    &full_count);
		if (error != 0)
			kmem_free(rows, MAX(1, count) * sizeof (*rows));
	}
	uint64_t revision = engine->revision;
	mutex_exit(&engine->lock);
	if (error != 0) {
		engine_dirty_free(&snap);
		fnvlist_free(nv);
		return (error);
	}
	uint64_t desc[16];
	error = engine_pages_store(spa, ub, nv, rows, count, full, full_count,
	    &snap, desc);
	fnvlist_free(nv);
	if (error == 0) {
		mutex_enter(&engine->lock);
		engine_dirty_settle(engine, &snap);
		engine->stored_revision = revision;
		memcpy(engine->stored_desc, desc, sizeof (desc));
		mutex_exit(&engine->lock);
		memcpy(ub->ub_pr19262, desc, sizeof (desc));
		if (engine->store_crash_txg == ub->ub_txg) {
			pr19262_flush();
			pr19262_exit(77);
		}
	}
	engine_dirty_free(&snap);
	return (error);
}

/*
 * A packed (codec 5) catalog root of this txg.  Entered under engine.lock,
 * which it drops.
 */
noinline static int
engine_store_packed(spa_t *spa, uberblock_t *ub, nvlist_t *nv)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->lock));
	size_t size = 0;
	char *packed = NULL;
	VERIFY0(nvlist_pack(nv, &packed, &size, NV_ENCODE_XDR, KM_SLEEP));
	uint64_t pages = howmany(size, ENGINE_STORE_PAGE);
	uint64_t first = 0, run = 0;
	for (uint64_t i = 0; i < ENGINE_STORE_PAGES; i++) {
		run = engine->bitmap[i] ? 0 : run + 1;
		if (run == pages) {
			first = i + 1 - pages;
			break;
		}
	}
	if (pages == 0 || run != pages) {
		kmem_free(packed, size);
		fnvlist_free(nv);
		mutex_exit(&engine->lock);
		return (ENOSPC);
	}
	/* Finite COW pages, including this root's own allocator bitmap. */
	memset(engine->bitmap + first, 1, pages);
	fnvlist_remove(nv, "bitmap");
	fnvlist_add_byte_array(nv, "bitmap", engine->bitmap,
	    ENGINE_STORE_PAGES);
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
	uint64_t desc[16] = { ENGINE_STORE_MAGIC, engine->vd->vdev_guid,
	    engine->arena, engine->arena_bytes, first, pages, ub->ub_txg,
	    size, 0, 0, 0, 0, engine->vd->vdev_ashift,
	    engine->vd->vdev_children, engine->total, 5 };
	zio_cksum_t hash;
	abd_checksum_sha256(abd, pages * ENGINE_STORE_PAGE, NULL, &hash);
	memcpy(&desc[8], &hash, sizeof (hash));
	uint64_t revision = engine->revision;
	mutex_exit(&engine->lock);
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	int error = 0;
	for (uint64_t i = 0; i < engine->vd->vdev_children; i++) {
		int e = engine_store_io(engine->vd->vdev_child[i], first,
		    pages, abd, B_TRUE);
		if (e != 0)
			error = e;
		e = engine_store_flush(engine->vd->vdev_child[i]);
		if (e != 0)
			error = e;
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	abd_free(abd);
	if (error == 0) {
		mutex_enter(&engine->lock);
		engine->stored_revision = revision;
		memcpy(engine->stored_desc, desc, sizeof (desc));
		mutex_exit(&engine->lock);
		memcpy(ub->ub_pr19262, desc, sizeof (desc));
		pr19262_printf("PR19262_STORE_SYNC txg=%llu page=%llu "
		    "pages=%llu bytes=%llu arena=%llu\n",
		    (u_longlong_t)ub->ub_txg,
		    (u_longlong_t)first, (u_longlong_t)pages,
		    (u_longlong_t)size, (u_longlong_t)engine->arena);
		if (engine->store_crash_txg == ub->ub_txg) {
			pr19262_flush();
			pr19262_exit(77);
		}
	}
	return (error);
}

static int
engine_store_sync(spa_t *spa, uberblock_t *ub)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->spa != spa || !engine->persistent) {
		mutex_exit(&engine->lock);
		return (0);
	}
	while (!engine->journal_formatted)
		cv_wait(&engine->changed, &engine->lock);
	if (pr19262_engine_store_ub(ub) && ub->ub_pr19262[6] == ub->ub_txg) {
		mutex_exit(&engine->lock);
		return (0);
	}
	boolean_t paged = engine->store_codec >= 6;
	/* This snapshot records every deferred free as gone. */
	engine_reuse_stage_locked(engine, ub->ub_txg);
	nvlist_t *nv = engine_store_snapshot(spa, ub, !paged);
	if (paged)
		return (engine_store_paged(spa, ub, nv));
	return (engine_store_packed(spa, ub, nv));
}

static void engine_fence_growth_check(spa_t *);

int
pr19262_engine_store_sync(spa_t *spa, uberblock_t *ub)
{
	engine_t *engine = spa->spa_reloc;
	engine_store_test_phase(spa, 2);
	mutex_enter(&engine->store_lock);
	int error = engine_store_sync(spa, ub);
	mutex_exit(&engine->store_lock);
	/*
	 * Test cuts between this store sync and the uberblock: A4 after the
	 * first replacing-config sync, D4 after the collapsing detach sync.
	 */
	if (error == 0 && atomic_load_64(&engine->r02_pair_state) ==
	    ENGINE_R02_PAIR_ADMITTED)
		engine_r02_cut(5);
	if (error == 0 && atomic_load_64(&engine->r02_collapsed) != 0) {
		engine_r02_cut(14);
		atomic_store_64(&engine->r02_collapsed, 0);
	}
	/* Syncing context: may only dispatch a fence, never run one. */
	if (error == 0)
		engine_fence_growth_check(spa);
	return (error);
}

static void
engine_store_clear(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	atomic_store_64(&engine->r02_pair_state, ENGINE_R02_PAIR_NONE);
	engine->r02_pair_old_guid = engine->r02_pair_new_guid = 0;
	engine->r02_pair_slot = 0;
	atomic_store_64(&engine->r02_collapsed, 0);
	atomic_store_64(&engine->r02_old_stale, 0);
	atomic_store_64(&engine->r02_attach_old_current, 0);
	atomic_store_64(&engine->r02_readmit_retries, 0);
	memset(engine->member_missed, 0, sizeof (engine->member_missed));
	atomic_store_64(&engine->member_readmit_retries, 0);
	engine_pages_clear(engine);
	engine_owner_t *e;
	while ((e = avl_first(&engine->owners)) != NULL) {
		VERIFY0(e->holds);
		VERIFY(!e->copying && e->pending == NULL);
		while (e->versions != NULL) {
			engine_version_t *b = e->versions;
			VERIFY0(b->leases);
			e->versions = b->next;
			kmem_free(b, sizeof (*b));
		}
		engine_owner_unlist(engine, e);
		avl_remove(&engine->owners, e);
		kmem_free(e, sizeof (*e));
	}
	/* Whatever comes next starts from a whole store. */
	engine->gone_count = 0;
	engine->store_all = B_TRUE;
	engine_tombstones_clear(engine);
	if (engine->free != NULL) {
		zfs_range_tree_vacate(engine->free, NULL, NULL);
		zfs_range_tree_destroy(engine->free);
	}
	if (engine->legacy != NULL) {
		zfs_range_tree_vacate(engine->legacy, NULL, NULL);
		zfs_range_tree_destroy(engine->legacy);
	}
	if (engine->unusable != NULL) {
		zfs_range_tree_vacate(engine->unusable, NULL, NULL);
		zfs_range_tree_destroy(engine->unusable);
	}
	/* Deferred reuse is RAM-only: a load rebuilds free from a root. */
	if (engine->reuse_open != NULL) {
		zfs_range_tree_vacate(engine->reuse_open, NULL, NULL);
		zfs_range_tree_destroy(engine->reuse_open);
		engine->reuse_open = NULL;
		for (uint_t i = 0; i < ENGINE_REUSE_BATCHES; i++) {
			zfs_range_tree_vacate(engine->reuse_batch[i], NULL,
			    NULL);
			zfs_range_tree_destroy(engine->reuse_batch[i]);
			engine->reuse_batch[i] = NULL;
			engine->reuse_batch_txg[i] = 0;
		}
	}
	engine->reuse_deferred = engine->reuse_released = 0;
	if (engine->bitmap != NULL)
		kmem_free(engine->bitmap, ENGINE_STORE_PAGES);
	/* Reserved runs are RAM-only, inside store_extents: a load has none. */
	if (engine->store_reserved != NULL) {
		zfs_range_tree_vacate(engine->store_reserved, NULL, NULL);
		zfs_range_tree_destroy(engine->store_reserved);
		engine->store_reserved = NULL;
	}
	if (engine->store_extents != NULL) {
		zfs_range_tree_vacate(engine->store_extents, NULL, NULL);
		zfs_range_tree_destroy(engine->store_extents);
		engine->store_extents = NULL;
	}
	if (engine->journal_extents != NULL) {
		zfs_range_tree_vacate(engine->journal_extents, NULL, NULL);
		zfs_range_tree_destroy(engine->journal_extents);
		engine->journal_extents = NULL;
	}
	while (engine->journal_segments != NULL) {
		engine_j_segment_t *s = engine->journal_segments;
		engine->journal_segments = s->next;
		kmem_free(s, sizeof (*s));
	}
	while (engine->journal_retired != NULL) {
		engine_j_segment_t *s = engine->journal_retired;
		engine->journal_retired = s->next;
		kmem_free(s, sizeof (*s));
	}
	engine->journal_segment_count = 0;
	engine->journal_freed_segments = engine->journal_freed_bytes = 0;
	engine->journal_tail = NULL;
	engine->journal_current = NULL;
	if (engine->journal_pending != NULL) {
		kmem_free(engine->journal_pending, sizeof (engine_j_segment_t));
		engine->journal_pending = NULL;
	}
	engine->spa = NULL;
	engine->vd = NULL;
	engine->free = engine->legacy = engine->unusable = NULL;
	engine->bitmap = NULL;
	engine->persistent = B_FALSE;
	engine->arena = engine->arena_bytes = engine->retired = 0;
	engine->store_codec = 0;
	engine->journal_arena = engine->journal_bytes = engine->journal_seq = 0;
	engine->journal_serial = 0;
	engine->journal_root = 0;
	memset(engine->journal_root_epoch, 0,
	    sizeof (engine->journal_root_epoch));
	memset(&engine->journal_hash, 0, sizeof (engine->journal_hash));
	engine->journal_error = 0;
	engine->journal_roll_pending = B_FALSE;
	engine->journal_roll_txg = engine->journal_roll_enospc = 0;
	engine->journal_roll_waits = engine->journal_roll_recovered = 0;
	engine->journal_ready = engine->journal_formatted = B_FALSE;
	engine->journal_repair = B_FALSE;
	memset(&engine->checkpoint, 0, sizeof (engine->checkpoint));
	engine_ckpt_unpin_locked(engine);
	engine->ckpt_session_txg = 0;
	engine->ckpt_builds = engine->ckpt_build_errors = 0;
	engine->ckpt_unpins = 0;
	engine->durable_ub_txg = engine->durable_catalog_txg = 0;
	memset(engine->stored_desc, 0, sizeof (engine->stored_desc));
	memset(engine->durable_desc, 0, sizeof (engine->durable_desc));
	engine->certified = 0;
	engine->store_tolerance = 0;
	engine->release_thread = NULL;
	memset(engine->admit, 0, sizeof (engine->admit));
	engine->admit_parity = engine->admit_reserve = 0;
	engine->admit_reserve_growth = engine->admit_reserve_legacy = 0;
	engine->admit_reserve_weak = engine->admit_growth_peak = 0;
	engine->admit_space_rejections = 0;
	engine->admit_batch = ENGINE_ADMIT_BATCH;
	engine->admit_refreshed = UINT64_MAX;
	engine->admit_cached_parity = UINT64_MAX;
	engine->admit_first = NULL;
	VERIFY(!engine->fence_pending);
	engine->fence_quiesced = engine->fence_stalled = B_FALSE;
	engine->fence_timing = B_FALSE;
	engine->fence_timing_mark = engine->fence_timing_waits = 0;
	engine->fence_returns = 0;
	engine->retired_pinned_valid = engine->retired_pinned_draining =
	    B_FALSE;
	engine->retired_pinned = engine->retired_pinned_v = 0;
	engine->retired_pinned_ckpt = engine->retired_pinned_after = 0;
	engine->retired_pinned_txg = engine->retired_pinned_counts = 0;
	engine->retired_pinned_passes = engine->retired_pinned_prints = 0;
#ifndef _KERNEL
	engine->export_fence_test = NULL;
	VERIFY3P(engine->fence_task_thread, ==, NULL);
#endif
	engine->fence_error = 0;
	engine->fence_requests = engine->fence_auto = 0;
	engine->fence_growth_pages_mark = engine->fence_growth_retired_mark = 0;
	engine->fence_growth_triggers = 0;
	engine->fence_growth_dispatch_failures = 0;
	engine->admit_arena_rejections = 0;
	engine->fence_completions = engine->fence_suspended = 0;
	engine->admit_arena_wait_gen = engine->admit_arena_wait_deficit = 0;
	engine->admit_defer_txg = 0;
	engine->fence_growth_seeded = engine->fence_growth_running = B_FALSE;
	engine->admit_fence_waits = 0;
	/* store_extents, which held the standby, is already destroyed. */
	engine->store_standby = 0;
	engine->store_standby_valid = B_FALSE;
	engine->store_headroom_low = engine->store_headroom_warned = B_FALSE;
	engine->store_standby_fills = engine->store_standby_uses = 0;
	engine->store_grow_enospc = engine->admit_headroom_rejections = 0;
	engine->store_standby_releases = 0;
	engine->store_reserve_fills = engine->store_reserve_uses = 0;
	engine->store_reserve_returns = engine->store_reserve_peak = 0;
	engine->netfree_wait_mark = engine->netfree_wait_last = 0;
	engine->admit_held_waits = engine->copy_held_waits = 0;
	engine->admit_held_takes = 0;
	engine->checkpoint_headroom_refusals = 0;
	/* L19: the reserve itself went with engine_ckpt_unpin_locked above. */
	engine->ckres_churn_x16 = 0;
	engine->ckres_conv_refused = B_FALSE;
	engine->ckres_min_available = engine->ckres_arms = 0;
	engine->ckres_releases = engine->ckres_refusals = 0;
	engine->ckres_rejections = engine->ckres_disc_draws = 0;
	engine->ckres_rel_txg = engine->ckres_rel_left = 0;
	engine->ckres_rel_pages = 0;
	engine->free_runs_cached = 0;
	engine->free_runs_known = B_FALSE;
	memset(engine->free_pace_txg, 0, sizeof (engine->free_pace_txg));
	memset(engine->free_pace_rows, 0, sizeof (engine->free_pace_rows));
	engine->free_paced_rows = engine->free_deferrals = 0;
	engine->free_deferrals_mark = engine->free_aged = 0;
	engine->bulk_pot_fresh = engine->bulk_rows = 0;
	engine->bulk_destroy_charged = engine->release_carry = B_FALSE;
	engine->bulk_pathed = engine->bulk_destroy_asked = B_FALSE;
	engine->bulk_stall_txg = engine->bulk_runs_cached = 0;
	engine->free_budget_synced = engine->free_budget_pages = 0;
	engine->free_budget_admitted = 0;
	engine->free_ckpt_asked = B_FALSE;
	engine->bulk_waits = engine->bulk_waits_mark = 0;
	engine->bulk_destroy_rows = engine->bulk_destroy_pauses = 0;
	engine->bulk_release_cuts = engine->bulk_release_rounds = 0;
	engine->bulk_prints = 0;
	engine->ckpt_discard_txg = engine->ckpt_discard_budget = 0;
	engine->ckpt_discard_need = engine->ckpt_discard_rows = 0;
	engine->ckpt_discard_cuts = engine->ckpt_discard_stalls = 0;
	engine->ckpt_discard_waits = engine->ckpt_discard_fences = 0;
	engine->ckpt_discard_prints = 0;
	engine->ckpt_discard_warned = B_FALSE;
	engine->ckpt_drain_flags_loaded = B_FALSE;
	engine->ckpt_drain_flagged = 0;
}

void
pr19262_engine_store_unload(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->store_lock);
	mutex_enter(&engine->lock);
	if (engine->spa == spa)
		engine_store_clear(engine);
	mutex_exit(&engine->lock);
	mutex_exit(&engine->store_lock);
#ifdef _KERNEL
	engine_kernel_cache(spa);
#endif
}

void
pr19262_engine_store_rebind(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	boolean_t rebound = engine->spa == spa;
	if (rebound) {
		/* The config was reloaded: find the target by its guid. */
		engine->vd = engine_target_vdev(spa, engine->vd_guid);
		VERIFY3P(engine->vd, !=, NULL);
		engine->foreign_normal = engine_foreign_normal(spa, engine->vd);
		/* R02: a pair is known only from the trusted tree. */
		atomic_store_64(&engine->r02_pair_state, ENGINE_R02_PAIR_NONE);
		engine->r02_pair_old_guid = engine->r02_pair_new_guid = 0;
		engine->r02_pair_slot = 0;
	}
	mutex_exit(&engine->lock);
	if (rebound)
		engine_r02_pair_discover(engine);
}

static boolean_t
engine_store_extent(engine_t *engine, uint64_t p, uint64_t size)
{
	/* Backing extents are sector-aligned; arenas are checked by callers. */
	uint64_t sector = 1ULL << engine->vd->vdev_ashift;
	return (size != 0 && IS_P2ALIGNED(p, sector) &&
	    IS_P2ALIGNED(size, sector) && p < engine->total &&
	    size <= engine->total - p &&
	    zfs_range_tree_contains(engine->free, p, size));
}

static int
engine_store_validate_virtual(engine_t *engine)
{
	zfs_range_tree_t *seen = zfs_range_tree_create(NULL,
	    ZFS_RANGE_SEG64, NULL, 0, 0);
	zfs_range_tree_walk(engine->legacy, zfs_range_tree_add, seen);
	int error = 0;
	for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
	    e = AVL_NEXT(&engine->owners, e)) {
		uint64_t start, size;
		uint64_t sector = 1ULL << engine->vd->vdev_ashift;
		if (!IS_P2ALIGNED(e->v, sector) ||
		    !IS_P2ALIGNED(e->size, sector) ||
		    zfs_range_tree_find_in(seen, e->v, e->size,
		    &start, &size)) {
			error = ENGINE_EINVAL(102);
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
	engine_t *engine = spa->spa_reloc;
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
		ENGINE_REFUSE(2);
	if (nvlist_lookup_uint64_array(nv, "journal", &journal, &nj) != 0 ||
	    nj != 4 || journal[1] != ENGINE_JOURNAL_BYTES ||
	    journal[0] >= engine->total ||
	    journal[1] * engine->vd->vdev_children >
	    engine->total - journal[0] ||
	    journal[0] % (engine->vd->vdev_children * ENGINE_STORE_PAGE) != 0)
		ENGINE_REFUSE(3);
	if (nvlist_lookup_uint64_array(nv, "checkpoint", &checkpoint,
	    &nc) != 0 ||
	    nc != sizeof (engine->checkpoint) / sizeof (uint64_t))
		ENGINE_REFUSE(4);
	memcpy(&engine->checkpoint, checkpoint, sizeof (engine->checkpoint));
	if (engine->checkpoint.ub_txg != 0 &&
	    (engine->checkpoint.ub_checkpoint_txg !=
	    engine->checkpoint.ub_txg ||
	    engine->checkpoint.ub_txg > ub->ub_txg ||
	    (!pr19262_engine_store_ub(&engine->checkpoint) &&
	    !engine_native_checkpoint(&engine->checkpoint))))
		ENGINE_REFUSE(5);
	if (txg != ub->ub_pr19262[6] || pool != spa_guid(spa) ||
	    top != engine->vd->vdev_guid || nm != 16 || ng != 6 || ns != 3 ||
	    memcmp(mos, &ub->ub_rootbp, sizeof (blkptr_t)) != 0 ||
	    g[0] != engine->total || g[1] != engine->vd->vdev_ms_shift ||
	    g[2] != engine->vd->vdev_ashift ||
	    g[3] != engine->vd->vdev_children ||
	    g[4] != engine->arena || g[5] != engine->arena_bytes ||
	    nb != ENGINE_STORE_PAGES || nl % 2 != 0 ||
	    nr > (engine->store_codec >= 6 ?
	    engine_pages_row_limit(engine) : 50000))
		ENGINE_REFUSE(6);
	for (uint64_t i = 0; i < ENGINE_STORE_PAGES; i++)
		if (bitmap[i] > 1 || (ub->ub_pr19262[15] == 5 &&
		    i >= ub->ub_pr19262[4] &&
		    i < ub->ub_pr19262[4] + ub->ub_pr19262[5] && !bitmap[i]))
			ENGINE_REFUSE(7);
	boolean_t at_floor = engine->store_codec >= 6 &&
	    engine->pages->floor != 0 && ub->ub_txg == engine->pages->floor;
	uint64_t dropped = 0;
	engine->bitmap = kmem_alloc(ENGINE_STORE_PAGES, KM_SLEEP);
	memcpy(engine->bitmap, bitmap, ENGINE_STORE_PAGES);
	engine->sequence = state[0];
	engine->next_incarnation = state[1];
	engine->retired_limit = state[2];
	engine->free = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64, NULL, 0, 0);
	engine->legacy = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	zfs_range_tree_add(engine->free, 0, engine->total);
	zfs_range_tree_remove(engine->free, engine->arena,
	    engine->arena_bytes * engine->vd->vdev_children);
	engine->store_extents = zfs_range_tree_create(NULL,
	    ZFS_RANGE_SEG64, NULL, 0, 0);
	zfs_range_tree_add(engine->store_extents, engine->arena,
	    engine->arena_bytes * engine->vd->vdev_children);
	for (engine_marena_t *a = engine->pages->arenas; a != NULL;
	    a = a->next) {
		if (a->p == engine->arena || a->pending)
			continue;
		uint64_t bytes = a->pages * ENGINE_STORE_PAGE *
		    engine->vd->vdev_children;
		if (!engine_store_extent(engine, a->p, bytes))
			ENGINE_REFUSE(8);
		zfs_range_tree_remove(engine->free, a->p, bytes);
		zfs_range_tree_add(engine->store_extents, a->p, bytes);
	}
	engine->journal_arena = journal[0];
	engine->journal_bytes = journal[1];
	engine->journal_epoch[0] = journal[2];
	engine->journal_epoch[1] = journal[3];
	engine->journal_root = journal[0];
	memcpy(engine->journal_root_epoch, engine->journal_epoch,
	    sizeof (engine->journal_epoch));
	uint64_t jsize = engine->journal_bytes * engine->vd->vdev_children;
	if (!engine_store_extent(engine, engine->journal_arena, jsize))
		ENGINE_REFUSE(9);
	zfs_range_tree_remove(engine->free, engine->journal_arena, jsize);
	engine->journal_extents = zfs_range_tree_create(NULL,
	    ZFS_RANGE_SEG64, NULL, 0, 0);
	zfs_range_tree_add(engine->journal_extents, engine->journal_arena,
	    jsize);
	/* Cut segments stay quarantined until a fence frees them. */
	uint64_t *cut;
	uint_t ncut;
	if (nvlist_lookup_uint64_array(nv, "journal-retired", &cut,
	    &ncut) == 0) {
		if (ncut % 5 != 0)
			ENGINE_REFUSE(10);
		engine_j_segment_t **link = &engine->journal_retired;
		for (uint_t i = 0; i < ncut; i += 5) {
			if (cut[i + 1] != engine->journal_bytes ||
			    cut[i] % (engine->vd->vdev_children *
			    ENGINE_STORE_PAGE) != 0 ||
			    !engine_store_extent(engine, cut[i], jsize) ||
			    cut[i + 4] == UINT64_MAX || cut[i + 4] > txg)
				ENGINE_REFUSE(11);
			zfs_range_tree_remove(engine->free, cut[i], jsize);
			zfs_range_tree_add(engine->journal_extents, cut[i],
			    jsize);
			engine_j_segment_t *s = kmem_zalloc(sizeof (*s),
			    KM_SLEEP);
			s->arena = cut[i];
			s->epoch[0] = cut[i + 2];
			s->epoch[1] = cut[i + 3];
			s->free_after = cut[i + 4];
			*link = s;
			link = &s->next;
		}
	}
	engine->journal_segments = engine->journal_tail =
	    kmem_zalloc(sizeof (engine_j_segment_t), KM_SLEEP);
	engine->journal_tail->arena = engine->journal_arena;
	engine->journal_tail->index = engine->journal_segment_count = 1;
	engine->journal_tail->free_after = UINT64_MAX;
	engine->journal_current = engine->journal_tail;
	memcpy(engine->journal_tail->epoch, engine->journal_epoch,
	    sizeof (engine->journal_epoch));
	for (uint_t i = 0; i < nl; i += 2) {
		if (!engine_store_extent(engine, legacy[i], legacy[i + 1]))
			ENGINE_REFUSE(12);
		zfs_range_tree_remove(engine->free, legacy[i], legacy[i + 1]);
		zfs_range_tree_add(engine->legacy, legacy[i], legacy[i + 1]);
	}
	/* Released owner identities: their journal bindings are folded. */
	uint64_t *tombs;
	uint_t nt;
	if (nvlist_lookup_uint64_array(nv, "tombstones", &tombs, &nt) == 0) {
		if (nt % 2 != 0)
			ENGINE_REFUSE(13);
		for (uint_t i = 0; i < nt; i += 2) {
			engine_tomb_t *t = kmem_zalloc(sizeof (*t), KM_SLEEP);
			t->v = tombs[i];
			t->incarnation = tombs[i + 1];
			/* Bound to the tail once the chain is recovered. */
			t->segment = UINT64_MAX;
			avl_index_t where;
			if (avl_find(&engine->tombstones, t, &where) != NULL) {
				kmem_free(t, sizeof (*t));
				ENGINE_REFUSE(14);
			}
			avl_insert(&engine->tombstones, t, where);
		}
	}
	/* Older images list the rows past the last metaslab as legacy. */
	engine->unusable = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	if (engine->vd->vdev_ms_shift == 0)
		ENGINE_REFUSE(15);
	uint64_t covered = P2ALIGN_TYPED(engine->total,
	    1ULL << engine->vd->vdev_ms_shift, uint64_t);
	if (covered < engine->total) {
		zfs_range_tree_clear(engine->free, covered,
		    engine->total - covered);
		zfs_range_tree_clear(engine->legacy, covered,
		    engine->total - covered);
		zfs_range_tree_add(engine->unusable, covered,
		    engine->total - covered);
	}
	for (uint_t i = 0; i < nr; i++) {
		uint64_t *v, *p;
		uint_t nv, np;
		if (nvlist_lookup_uint64_array(rows[i], "V", &v, &nv) != 0 ||
		    nvlist_lookup_uint64_array(rows[i], "P", &p, &np) != 0 ||
		    nv != 8 || np == 0 || np % 8 != 0 || v[6] > 2 ||
		    v[0] >= engine->total || v[1] == 0 ||
		    v[1] > engine->total - v[0] || engine_find(engine, v[0]) !=
		    NULL ||
		    v[7] > txg || v[3] > engine->next_incarnation)
			ENGINE_REFUSE(16);
		engine_owner_t *e = engine_owner_alloc(KM_SLEEP);
		e->v = v[0]; e->size = v[1]; e->allocation_txg = v[2];
		e->incarnation = v[3]; e->created = v[4];
		e->catalog_txg = v[7];
		e->retired_at = v[5]; e->retired = v[6] == 1;
		e->checkpoint_only = v[6] == 2;
		if (e->retired)
			e->retired_txg = txg;
		avl_add(&engine->owners, e);
		for (uint_t j = 0; j < np; j += 8) {
			if (p[j + 3] <= p[j + 4] ||
			    p[j + 3] > UINT16_MAX ||
			    p[j + 3] > engine->vd->vdev_children ||
			    p[j + 4] == 0 || p[j + 4] > 3 ||
			    p[j + 5] > 1 || p[j + 6] > 1 ||
			    (j != 0 && p[j + 6] != 1))
				ENGINE_REFUSE(17);
			/*
			 * The floor root itself was fenced: every retired
			 * version older than its current binding was returned
			 * to the physical allocator and may be in use again.
			 */
			if (j != 0 && at_floor && p[j + 7] < p[7]) {
				dropped++;
				continue;
			}
			if (!engine_store_extent(engine, p[j], p[j + 1]))
				ENGINE_REFUSE(18);
			engine_version_t *b = engine_version_new(engine, e,
			    p[j],
			    p[j + 1], p[j + 2], B_TRUE);
			engine_version_geometry(b, p[j + 3], p[j + 4]);
			b->retired = p[j + 6];
			b->ready = p[j + 5]; b->generation = p[j + 7];
			if (j != 0)
				b->retired_txg = txg;
			if (j == 0)
				e->cur = b;
			zfs_range_tree_remove(engine->free, b->p, b->size);
			if (b->retired || (e->retired && j == 0))
				engine->retired += b->size;
		}
	}
	if (dropped != 0)
		pr19262_printf("PR19262_FLOOR_DROP root=%llu catalog=%llu "
		    "floor=%llu versions=%llu\n", (u_longlong_t)ub->ub_txg,
		    (u_longlong_t)txg, (u_longlong_t)engine->pages->floor,
		    (u_longlong_t)dropped);
	return (engine_store_validate_virtual(engine));
}

typedef struct engine_root_graph {
	spa_t *spa;
	uberblock_t *roots;
	uint_t count;
	uint_t capacity;
	zfs_range_tree_t *occupied;
	/* The R02 observer obtains its floor from private raw control pages. */
	boolean_t manifest;
	boolean_t floor_valid;
	uint64_t floor;
} engine_root_graph_t;

static int
engine_root_add(void *arg, const uberblock_t *ub)
{
	engine_t *engine = ((engine_root_graph_t *)arg)->spa->spa_reloc;
	engine_root_graph_t *g = arg;
	const uint64_t *d = ub->ub_pr19262;
	/*
	 * Roots below the durable floor are not importable: skip them.  The
	 * guarded checkpoint root is, and its pages stay pinned.
	 */
	if (g->manifest && pr19262_engine_store_ub(ub) &&
	    !engine_r02_manifest_codec_supported(d[15]))
		return (ENOTSUP);
	if (pr19262_engine_store_ub(ub) &&
	    (g->floor_valid ? ub->ub_txg < g->floor :
	    (!g->manifest && ub->ub_txg < engine->pages->floor)) &&
	    !engine_ckpt_exempt(engine, ub))
		return (0);
	if (!pr19262_engine_store_ub(ub) || !engine_store_layout(d) ||
	    d[1] != engine->vd->vdev_guid || d[2] != engine->arena ||
	    d[3] != engine->arena_bytes || d[12] != engine->vd->vdev_ashift ||
	    d[13] != engine->vd->vdev_children || d[14] != engine->total ||
	    d[6] == 0 ||
	    d[6] > ub->ub_txg ||
	    (d[6] != ub->ub_txg && (ub->ub_checkpoint_txg == 0 ||
	    d[6] > ub->ub_checkpoint_txg)))
		ENGINE_REFUSE(19);
	for (uint_t i = 0; i < g->count; i++) {
		if (memcmp(g->roots[i].ub_pr19262, d,
		    sizeof (ub->ub_pr19262)) != 0)
			continue;
		return (memcmp(&g->roots[i].ub_rootbp, &ub->ub_rootbp,
		    sizeof (ub->ub_rootbp)) == 0 ? 0 : ENGINE_EINVAL(199));
	}
	if (g->count == g->capacity) {
		uint64_t wanted = g->capacity == 0 ? 64 :
		    (uint64_t)g->capacity * 2;
		uint64_t limit = MIN(UINT_MAX, engine->total /
		    (ENGINE_STORE_PAGE * engine->vd->vdev_children));
		limit = MIN(limit, SIZE_MAX / sizeof (uberblock_t));
		uint_t capacity = MIN(wanted, limit);
		if (capacity == g->capacity)
			return (EOVERFLOW);
		uberblock_t *roots = pr19262_alloc(capacity *
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
	engine_t *engine = g->spa->spa_reloc;
	const uint64_t *d = ub->ub_pr19262;
	int error = ECKSUM;
	nvlist_t *nv = NULL;
	if (d[15] >= 6)
		error = engine_pages_read(g->spa, ub, &nv,
		    g->occupied, B_FALSE);
	abd_t *abd = d[15] == 5 ?
	    abd_alloc_linear(d[5] * ENGINE_STORE_PAGE, B_TRUE) : NULL;
	for (uint_t c = 0; d[15] == 5 && c < engine->vd->vdev_children; c++) {
		if (engine_store_io(engine->vd->vdev_child[c], d[4], d[5],
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
		error = ENGINE_EINVAL(103);
	} else {
		uberblock_t pin;
		memcpy(&pin, checkpoint, sizeof (pin));
		if (pin.ub_txg != 0) {
			if (uberblock_verify(&pin) != 0 || pin.ub_txg > txg ||
			    pin.ub_checkpoint_txg != pin.ub_txg ||
			    pin.ub_pr19262[6] >= d[6])
				error = ENGINE_EINVAL(104);
			else if (!engine_native_checkpoint(&pin))
				error = engine_root_add(g, &pin);
		}
		if (error == 0) {
			if (d[15] == 5)
				engine_mmark(g->occupied,
				    engine->arena + d[4] * ENGINE_STORE_PAGE *
				    engine->vd->vdev_children,
				    d[5] * ENGINE_STORE_PAGE *
				    engine->vd->vdev_children);
			pr19262_printf("PR19262_M_PIN catalog=%llu first=%llu "
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
	engine_t *engine = spa->spa_reloc;
	*roots = *pages = 0;
	/*
	 * The caller holds no config locks.  Sync already holds STATE when
	 * taking store_lock; a queued STATE writer must not complete a cycle
	 * through this reader.  Take ZIO only after acquiring store_lock.
	 */
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine->store_lock);
	engine_store_test_phase(spa, 1);
	if (!pr19262_engine_persistent(spa)) {
		mutex_exit(&engine->store_lock);
		spa_config_exit(spa, SCL_STATE, FTAG);
		return (ENOTSUP);
	}
	/*
	 * This observer has no endpoint vector.  It must not let a successful
	 * read through a replacing parent stand in for an exact-leaf manifest.
	 * Keep the existing per-I/O classifier as the backstop, but refuse the
	 * whole inspection before collecting any result from that topology.
	 */
	if (!engine_store_direct_topology(engine->vd) &&
	    !engine_store_r02_topology(engine->vd, B_FALSE)) {
		mutex_exit(&engine->store_lock);
		spa_config_exit(spa, SCL_STATE, FTAG);
		return (ENOTSUP);
	}
	engine_root_graph_t *g = kmem_zalloc(sizeof (*g), KM_SLEEP);
	g->spa = spa;
	g->occupied = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	int error = pr19262_vdev_uberblocks_r02(spa, g, engine_root_add, NULL);
	for (uint_t i = 0; error == 0 && i < g->count; i++)
		error = engine_root_read(g, &g->roots[i]);
	spa_config_exit(spa, SCL_ZIO, FTAG);
	if (error == 0) {
		*roots = g->count;
		*pages = zfs_range_tree_space(g->occupied) /
		    (ENGINE_STORE_PAGE * engine->vd->vdev_children);
	}
	zfs_range_tree_vacate(g->occupied, NULL, NULL);
	zfs_range_tree_destroy(g->occupied);
	if (g->roots != NULL)
		kmem_free(g->roots, g->capacity * sizeof (uberblock_t));
	kmem_free(g, sizeof (*g));
	mutex_exit(&engine->store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	return (error);
}

/*
 * A read-only R02 prerequisite.  The lock order is deliberate: the private
 * topology gate precedes STATE, then store_lock, journal_lock, then ZIO.
 * The journal lock freezes the persisted root while this read-only closure
 * follows its HEAD/BIND prefix; it never calls journal recovery or apply.
 */
static int engine_r02_journal_manifest(engine_r02_manifest_private_t *);

/* The uberblock enumeration order is not a visitor-order contract. */
static int
engine_r02_manifest_ub_compare(const uberblock_t *left,
    const uberblock_t *right)
{
	if (left->ub_txg != right->ub_txg)
		return (left->ub_txg < right->ub_txg ? -1 : 1);
	for (uint_t i = 0; i < sizeof (left->ub_pr19262) /
	    sizeof (left->ub_pr19262[0]); i++) {
		if (left->ub_pr19262[i] != right->ub_pr19262[i])
			return (left->ub_pr19262[i] < right->ub_pr19262[i] ?
			    -1 : 1);
	}
	return (memcmp(&left->ub_rootbp, &right->ub_rootbp,
	    sizeof (left->ub_rootbp)));
}

static void
engine_r02_manifest_root_order(engine_root_graph_t *graph)
{
	/* The retained root set is small; an insertion sort keeps this scratch-only. */
	for (uint_t i = 1; i < graph->count; i++) {
		uberblock_t held = graph->roots[i];
		uint_t at = i;
		while (at != 0 && engine_r02_manifest_ub_compare(&held,
		    &graph->roots[at - 1]) < 0) {
			graph->roots[at] = graph->roots[at - 1];
			at--;
		}
		graph->roots[at] = held;
	}
}

static void
engine_r02_manifest_init(engine_r02_manifest_private_t *m, spa_t *spa,
    engine_store_leaf_io_mode_t mode, boolean_t reseed)
{
	memset(m, 0, sizeof (*m));
	m->spa = spa;
	m->mode = mode;
	m->reseed = reseed;
	m->all = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64, NULL, 0, 0);
	m->catalog = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64, NULL, 0, 0);
	m->tree = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64, NULL, 0, 0);
	m->control_pages = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64, NULL,
	    0, 0);
	m->directory = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64, NULL, 0,
	    0);
	m->journal = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64, NULL, 0, 0);
	list_create(&m->entries, sizeof (engine_r02_entry_t),
	    offsetof(engine_r02_entry_t, node));
}

/*
 * The validated closure itself.  The caller owns SCL_STATE (reader, or the
 * SCL_ALL writer), store_lock and journal_lock, and either an SCL_ZIO reader
 * (normal mode) or SCL_ALL writer (config-writer mode); this helper requests
 * no lock.  absent_ok names the one leaf whose labels may be unreadable: the
 * old leaf being superseded by an R02 reseed.  The topology must be direct
 * or carry exactly the strict pair this engine knows.
 */
static int
engine_r02_collect_locked(engine_r02_manifest_private_t *m,
    vdev_t *absent_ok)
{
	spa_t *spa = m->spa;
	engine_t *engine = spa->spa_reloc;
	boolean_t shaped = engine_store_direct_topology(engine->vd) ||
	    engine_store_r02_topology(engine->vd, B_FALSE);
	if (engine_r02_manifest_gate(spa, engine, shaped, 6) != 0)
		return (SET_ERROR(ENOTSUP));
	engine_root_graph_t graph = { .spa = spa, .manifest = B_TRUE };
	/* Do not issue raw I/O before root_add() rejects codec 5. */
	int error = pr19262_vdev_uberblocks_r02(spa, &graph, engine_root_add,
	    absent_ok);
	if (error == 0) {
		engine_r02_manifest_root_order(&graph);
		error = engine_r02_manifest_open(m);
	}
	if (error == 0) {
		graph.floor = m->control.floor;
		graph.floor_valid = B_TRUE;
	}
	for (uint_t i = 0; error == 0 && i < graph.count; i++) {
		if (graph.roots[i].ub_txg < graph.floor &&
		    !engine_ckpt_exempt(engine, &graph.roots[i]))
			continue;
		error = engine_r02_root(m, &graph.roots[i], &graph,
		    engine_root_add);
	}
	if (error == 0)
		error = engine_r02_journal_manifest(m);
	if (error == 0)
		error = engine_r02_manifest_check(m);
	if (graph.roots != NULL)
		kmem_free(graph.roots, graph.capacity * sizeof (*graph.roots));
	return (error);
}

int
pr19262_engine_r02_manifest_walk(spa_t *spa,
    pr19262_r02_manifest_visit_t visit, void *arg,
    pr19262_r02_manifest_t *out)
{
	if (out == NULL)
		return (EINVAL);
	memset(out, 0, sizeof (*out));
	engine_t *engine = spa->spa_reloc;
	rw_enter(&engine->r02_topology_lock, RW_READER);
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine->store_lock);
	boolean_t shaped = engine_store_direct_topology(engine->vd) ||
	    engine_store_r02_topology(engine->vd, B_FALSE);
	if (engine_r02_manifest_gate(spa, engine, shaped, 6) != 0) {
		mutex_exit(&engine->store_lock);
		spa_config_exit(spa, SCL_STATE, FTAG);
		rw_exit(&engine->r02_topology_lock);
		return (ENOTSUP);
	}
	engine_r02_manifest_private_t manifest;
	engine_r02_manifest_init(&manifest, spa, ENGINE_STORE_LEAF_IO_NORMAL,
	    B_FALSE);
	mutex_enter(&engine->journal_lock);
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	int error = engine_r02_collect_locked(&manifest, NULL);
	if (error == 0)
		engine_r02_manifest_counts(&manifest, out);
	spa_config_exit(spa, SCL_ZIO, FTAG);
	mutex_exit(&engine->journal_lock);
	mutex_exit(&engine->store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	rw_exit(&engine->r02_topology_lock);
	if (error == 0 && visit != NULL) {
		for (engine_r02_entry_t *e = list_head(&manifest.entries);
		    e != NULL; e = list_next(&manifest.entries, e)) {
			error = visit(arg, &e->entry);
			if (error != 0)
				break;
		}
	}
	engine_r02_manifest_fini(&manifest);
	return (error);
}

int
pr19262_engine_r02_manifest(spa_t *spa, pr19262_r02_manifest_t *out)
{
	return (pr19262_engine_r02_manifest_walk(spa, NULL, NULL, out));
}

int
pr19262_engine_mgrowth_probe(spa_t *spa, boolean_t advance,
    pr19262_mgrowth_probe_t *out)
{
	engine_t *engine = spa->spa_reloc;
	memset(out, 0, sizeof (*out));
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine->store_lock);
	int error = pr19262_engine_persistent(spa) ?
	    engine_pages_growth_probe(spa, advance, out) : ENOTSUP;
	mutex_exit(&engine->store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	return (error);
}

int
pr19262_engine_store_load(spa_t *spa, const uberblock_t *ub)
{
	engine_t *engine = spa->spa_reloc;
	if (!pr19262_engine_store_ub(ub))
		return (0);
	const uint64_t *d = ub->ub_pr19262;
	if (engine->spa != NULL)
		return (ENOTSUP);
	/* The trailer names the translated vdev; others stay native. */
	vdev_t *vd = engine_target_vdev(spa, d[1]);
	if (vd == NULL)
		return (ENOTSUP);
	/* Older M bitmaps cannot allocate over newer still-selectable roots. */
	if ((spa->spa_mode & SPA_MODE_WRITE) &&
	    ub->ub_txg < spa->spa_load_latest_ub_txg) {
		pr19262_printf("PR19262_REWIND_REFUSED ub=%llu latest=%llu "
		    "(writable; import read-only to read this root)\n",
		    (u_longlong_t)ub->ub_txg,
		    (u_longlong_t)spa->spa_load_latest_ub_txg);
		return (ENOTSUP);
	}
	if (vd->vdev_ops != &vdev_raidz_ops || d[1] != vd->vdev_guid ||
	    d[3] != ENGINE_STORE_BYTES || d[12] != vd->vdev_ashift ||
	    d[13] != vd->vdev_children || d[14] != vd->vdev_asize ||
	    d[6] > ub->ub_txg ||
	    (d[6] != ub->ub_txg && (ub->ub_checkpoint_txg == 0 ||
	    d[6] > ub->ub_checkpoint_txg)) ||
	    !engine_store_layout(d) ||
	    d[2] >= vd->vdev_asize ||
	    d[3] * d[13] > vd->vdev_asize - d[2] ||
	    d[2] % (vd->vdev_children * ENGINE_STORE_PAGE) != 0)
		ENGINE_REFUSE(20);
	engine->vd = vd;
	engine->vd_guid = vd->vdev_guid;
	engine_r02_pair_discover(engine);
	engine->foreign_normal = engine_foreign_normal(spa, vd);
	engine->total = vd->vdev_asize;
	engine->arena = d[2]; engine->arena_bytes = d[3];
	engine->store_codec = d[15];
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
	mutex_enter(&engine->lock);
	/* Roots below the durable floor may reference reused backing. */
	if (error == 0 && d[15] >= 6 && ub->ub_txg < engine->pages->floor) {
		pr19262_printf("PR19262_FLOOR_REFUSED ub=%llu floor=%llu\n",
		    (u_longlong_t)ub->ub_txg,
		    (u_longlong_t)engine->pages->floor);
		error = SET_ERROR(ESTALE);
	}
	/* The label certificate is published after the control record. */
	if (error == 0 && d[15] >= 6 &&
	    pr19262_vdev_certified_parity(vd) > engine->pages->certified)
		error = ENGINE_EINVAL(101);
	if (error == 0)
		error = engine_store_decode(spa, ub, nv);
	if (error == 0 && d[15] >= 6)
		error = engine_pages_install(engine, nv);
	nvlist_free(nv);
	if (error == 0) {
		engine->persistent = B_TRUE;
		engine->store_all = B_TRUE;
		engine->spa = spa;
		engine->stored_revision = engine->revision;
		engine->durable_ub_txg = ub->ub_txg;
		engine->durable_catalog_txg = d[6];
		/* Open-context allocations may carry the next txgs. */
		engine->ckpt_session_txg = ub->ub_txg + TXG_CONCURRENT_STATES;
		memcpy(engine->stored_desc, d, sizeof (engine->stored_desc));
		memcpy(engine->durable_desc, d, sizeof (engine->durable_desc));
		engine->certified = engine->pages->certified;
		/*
		 * Import trusts the on-disk certificate; it never trusts RAM.
		 */
		engine->store_tolerance = 0;
		engine->admit_batch = ENGINE_ADMIT_BATCH;
		engine->admit_refreshed = UINT64_MAX;
		engine->admit_cached_parity = UINT64_MAX;
		vdev_raidz_layout_for_birth(vd, ub->ub_txg + 1, NULL,
		    &engine->admit_parity);
		/* Open native space maps even for a read-only SPA. */
		spa->spa_read_spacemaps = B_TRUE;
		pr19262_printf("PR19262_STORE_LOAD pre-MOS txg=%llu "
		    "owners=%llu catalog=%llu\n", (u_longlong_t)ub->ub_txg,
		    (u_longlong_t)avl_numnodes(&engine->owners),
		    (u_longlong_t)d[6]);
	} else {
		engine_store_clear(engine);
	}
	mutex_exit(&engine->lock);
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
	    pr19262_env("PR19262_ENGINE_BOOTSTRAP_NO_LABEL_EPOCHS") != NULL) {
		/* Read MOS through stored layout descriptors. */
		vdev_raidz_parity_epochs_install(vd, NULL, 0);
		pr19262_printf("PR19262_STORE_BOOTSTRAP "
		    "without-label-epochs\n");
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
	engine_t *engine = spa->spa_reloc;
	if (!pr19262_engine_persistent(spa) || !spa_writeable(spa))
		return (0);
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine->store_lock);
	/* I1: an imported replacing pair is reseeded before any publication. */
	int error = engine_r02_pair_recover(spa);
	/* A pre-certificate runtime quorum is not durable import authority. */
	boolean_t missing = engine->pages->certified == 0 &&
	    !engine_store_members_ready(engine->vd);
	if (error == 0)
		error = missing ? ENXIO : engine_pages_reconcile(spa, B_TRUE);
	if (missing)
		pr19262_printf("PR19262_RECOVER_REFUSED certified=0 members\n");
	/* Members that returned after missing writes get the live pages. */
	if (error == 0)
		error = engine_pages_resilver_members(spa);
	/* L23: a member absent now may lack anything stored before it. */
	for (uint_t c = 0; error == 0 && c < engine->vd->vdev_children; c++) {
		vdev_t *m = engine->vd->vdev_child[c];
		if (!vdev_readable(m) || !vdev_writeable(m))
			engine_member_missed(engine, m, ENXIO);
	}
	mutex_exit(&engine->store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	return (error);
}

/* Audit which members hold every live raw page; private witness. */
int
pr19262_engine_raw_audit(spa_t *spa, const char *label, uint64_t *ok,
    uint64_t *missing)
{
	engine_t *engine = spa->spa_reloc;
	if (!pr19262_engine_persistent(spa))
		return (ENOTSUP);
	if (spa_writeable(spa))
		txg_wait_synced(spa->spa_dsl_pool, 0);
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine->store_lock);
	uint64_t written;
	int error = engine_pages_members(spa, -1, ok, missing, &written);
	if (error == 0)
		engine_pages_audit_print(spa, label, ok, missing);
	mutex_exit(&engine->store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	return (error);
}
