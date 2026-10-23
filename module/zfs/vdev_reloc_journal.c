/* SPDX-License-Identifier: CDDL-1.0 */
/* Private linked J segments, included by the same ownership engine. */
#define	ENGINE_J_MAGIC 0x31393236324a3035ULL
#define	ENGINE_J_BIND 1
#define	ENGINE_J_HEAD 2
#define	ENGINE_J_FORWARD 3
#define	ENGINE_J_NORMAL 0
#define	ENGINE_J_PREPARE 1
#define	ENGINE_J_ACTIVE 2
#define	ENGINE_J_WORDS (ENGINE_STORE_PAGE / sizeof (uint64_t))
#define	ENGINE_J_HASH (ENGINE_J_WORDS - 4)


static uint_t engine_journal_fault_stage;
static uint64_t engine_journal_fault_seq;
static uint_t engine_journal_roll_fault;
/* Test hook: a group of frames is cut after the first half landed. */
static uint_t engine_journal_group_fault;
static uint_t engine_journal_roll_phase;
static int engine_journal_repair(spa_t *);
static int engine_journal_read_head(spa_t *, abd_t *, boolean_t);

static int
engine_journal_empty(spa_t *spa, boolean_t all_io)
{
	abd_t *page = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	int error = engine_journal_read_head(spa, page, all_io);
	if (error == 0) {
		uint64_t *w = abd_to_buf(page);
		zio_cksum_t zero = { 0 };
		if (w[3] != 0 || w[31] != ENGINE_J_NORMAL ||
		    memcmp(w + 20, &zero, sizeof (zero)) != 0 ||
		    w[32] != 0 || w[33] != 0 || w[34] != 0 || w[35] != 0)
			error = ENGINE_EINVAL(8);
	} else if (error == ECKSUM) {
		/* No initialized header is expected before formatting. */
		error = 0;
	}
	abd_free(page);
	return (error);
}

void
pr19262_engine_journal_roll_fault(uint_t stage)
{
	mutex_enter(&engine.journal_lock);
	engine_journal_roll_fault = stage;
	mutex_exit(&engine.journal_lock);
}

static void
engine_journal_roll_stop(uint_t stage)
{
	if (engine_journal_roll_fault == stage) {
		pr19262_printf("PR19262_J_ROLL_CRASH stage=%u\n", stage);
		pr19262_flush();
		pr19262_exit(80 + stage);
	}
}

void
pr19262_engine_journal_group_fault(uint_t stage)
{
	mutex_enter(&engine.journal_lock);
	engine_journal_group_fault = stage;
	mutex_exit(&engine.journal_lock);
}

void
pr19262_engine_journal_fault(uint_t stage)
{
	mutex_enter(&engine.journal_lock);
	engine_journal_fault_stage = stage % 10;
	engine_journal_fault_seq = engine.journal_seq + (stage >= 10 ? 2 : 1);
	mutex_exit(&engine.journal_lock);
}

int
pr19262_engine_journal_wait_error(void)
{
	mutex_enter(&engine.lock);
	while (engine.journal_error == 0)
		cv_wait(&engine.changed, &engine.lock);
	int error = engine.journal_error;
	mutex_exit(&engine.lock);
	return (error);
}

static void
engine_journal_seal(abd_t *abd)
{
	uint64_t *w = abd_to_buf(abd);
	memset(w + ENGINE_J_HASH, 0, sizeof (zio_cksum_t));
	zio_cksum_t hash;
	abd_checksum_sha256(abd, ENGINE_STORE_PAGE, NULL, &hash);
	memcpy(w + ENGINE_J_HASH, &hash, sizeof (hash));
}

static boolean_t
engine_journal_valid(abd_t *abd, uint64_t kind, uint64_t seq)
{
	uint64_t *w = abd_to_buf(abd);
	if (w[0] != ENGINE_J_MAGIC ||
	    (kind != UINT64_MAX && w[1] != kind) ||
	    (w[1] != ENGINE_J_BIND && w[1] != ENGINE_J_HEAD &&
	    w[1] != ENGINE_J_FORWARD) ||
	    w[2] != engine.journal_epoch[0] || w[18] !=
	    engine.journal_epoch[1] || w[4] != spa_guid(engine.vd->vdev_spa) ||
	    w[5] != engine.vd->vdev_guid || (seq != UINT64_MAX && w[3] != seq))
		return (B_FALSE);
	zio_cksum_t saved, hash;
	memcpy(&saved, w + ENGINE_J_HASH, sizeof (saved));
	memset(w + ENGINE_J_HASH, 0, sizeof (saved));
	abd_checksum_sha256(abd, ENGINE_STORE_PAGE, NULL, &hash);
	memcpy(w + ENGINE_J_HASH, &saved, sizeof (saved));
	return (memcmp(&saved, &hash, sizeof (hash)) == 0);
}

static abd_t *
engine_journal_page(uint64_t kind, uint64_t seq)
{
	abd_t *abd = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	abd_zero(abd, ENGINE_STORE_PAGE);
	uint64_t *w = abd_to_buf(abd);
	w[0] = ENGINE_J_MAGIC; w[1] = kind;
	w[2] = engine.journal_epoch[0]; w[3] = seq;
	w[4] = spa_guid(engine.vd->vdev_spa);
	w[5] = engine.vd->vdev_guid;
	w[18] = engine.journal_epoch[1];
	return (abd);
}

/* Write consecutive frames to every member at once, then flush each. */
static int
engine_journal_write_pages(spa_t *spa, const engine_member_page_t *pv,
    uint_t n)
{
	engine_quorum_t q = { 0 };
	uint_t children = engine.vd->vdev_children;
	int *errors = kmem_alloc(children * sizeof (int), KM_SLEEP);
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	engine_members_write(spa, pv, n, B_TRUE, B_FALSE, errors, NULL);
	spa_config_exit(spa, SCL_ZIO, FTAG);
	for (uint_t c = 0; c < children; c++)
		engine_quorum_add(&q, errors[c]);
	kmem_free(errors, children * sizeof (int));
	return (engine_quorum_result(&q));
}

static int
engine_journal_write(spa_t *spa, uint64_t page, abd_t *abd)
{
	engine_quorum_t q = { 0 };
	/*
	 * Without a crash or fault hook armed, every member writes and then
	 * flushes at once.  The hooks act on the first member alone, so they
	 * keep the member-by-member order they were written for.
	 */
	if (engine_journal_fault_stage == 0 && engine_journal_roll_fault == 0) {
		engine_member_page_t pg = { engine.journal_arena, page, abd };
		return (engine_journal_write_pages(spa, &pg, 1));
	}
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	for (uint64_t i = 0; i < engine.vd->vdev_children; i++) {
		vdev_t *vd = engine.vd->vdev_child[i];
		uint64_t *w = abd_to_buf(abd);
		uint64_t fault_kind = engine_journal_fault_stage >= 4 ?
		    ENGINE_J_HEAD : ENGINE_J_BIND;
		boolean_t inject = i == 0 && w[1] == fault_kind &&
		    w[3] == engine_journal_fault_seq;
		uint_t roll = engine_journal_roll_fault;
		boolean_t roll_inject = i == 0 &&
		    ((engine_journal_roll_phase == 1 && roll <= 3) ||
		    (engine_journal_roll_phase == 2 &&
		    roll >= 4 && roll <= 6) ||
		    (engine_journal_roll_phase == 3 &&
		    roll >= 7 && roll <= 9) ||
		    (engine_journal_roll_phase == 4 &&
		    roll >= 11 && roll <= 12));
		int e = (inject && (engine_journal_fault_stage == 1 ||
		    engine_journal_fault_stage == 4)) ||
		    (roll_inject && (roll % 3 == 1 || roll == 11)) ? EIO :
		    engine_store_io_at(vd, engine.journal_arena, page,
		    1, abd, B_TRUE);
		if (e == 0)
			e = (inject && (engine_journal_fault_stage == 2 ||
			    engine_journal_fault_stage == 5)) ||
			    (roll_inject &&
			    (roll % 3 == 2 || roll == 12)) ? EIO :
			    pr19262_engine_metadata_flush(vd);
		engine_quorum_add(&q, e);
		if (e == 0 && i == 0 && engine_journal_roll_phase == 2 &&
		    roll == 10) {
			pr19262_printf("PR19262_J_SINGLE_FORWARD root=%llu "
			    "next=%llu seq=%llu\n",
			    (u_longlong_t)engine.journal_arena,
			    (u_longlong_t)w[6], (u_longlong_t)w[3]);
			pr19262_flush();
			pr19262_exit(90);
		}
		if (e == 0 && i == 0 &&
		    ((roll == 14 && engine_journal_roll_phase == 4) ||
		    (roll == 15 && engine_journal_roll_phase == 3) ||
		    (roll == 16 && engine_journal_roll_phase == 1))) {
			pr19262_printf("PR19262_J_MEMBER_CUT stage=%u "
			    "root=%llu next=%llu seq=%llu\n", roll,
			    (u_longlong_t)engine.journal_root,
			    (u_longlong_t)(engine.journal_pending != NULL ?
			    engine.journal_pending->arena : 0),
			    (u_longlong_t)w[3]);
			pr19262_flush();
			pr19262_exit(80 + roll);
		}
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	return (engine_quorum_result(&q));
}

static int
engine_journal_head(spa_t *spa, uint64_t seq, const zio_cksum_t *hash,
    uint64_t phase, engine_j_segment_t *target)
{
	if (engine.journal_serial == UINT64_MAX)
		return (EOVERFLOW);
	abd_t *page = engine_journal_page(ENGINE_J_HEAD, seq);
	uint64_t *w = abd_to_buf(page);
	memcpy(w + 20, hash, sizeof (*hash));
	w[30] = engine.journal_serial + 1;
	w[31] = phase;
	if (target != NULL) {
		w[32] = target->arena; w[33] = engine.journal_bytes;
		w[34] = target->epoch[0]; w[35] = target->epoch[1];
	}
	engine_journal_seal(page);
	int error = engine_journal_write(spa, w[30] % 2, page);
	if (error == 0) {
		mutex_enter(&engine.lock);
		engine_j_segment_t *s = engine.journal_current;
		engine.journal_serial = s->serial = w[30];
		engine.journal_seq = s->seq = seq;
		engine.journal_hash = s->hash = *hash;
		s->phase = phase;
		s->target = w[32];
		s->target_epoch[0] = w[34];
		s->target_epoch[1] = w[35];
		mutex_exit(&engine.lock);
	}
	abd_free(page);
	return (error);
}

static int
engine_journal_format(spa_t *spa)
{
	abd_t *head = engine_journal_page(ENGINE_J_HEAD, 0);
	((uint64_t *)abd_to_buf(head))[30] = 1;
	engine_journal_seal(head);
	int error = engine_journal_write(spa, 0, head);
	if (error == 0)
		error = engine_journal_write(spa, 1, head);
	if (error == 0) {
		mutex_enter(&engine.lock);
		engine.journal_serial = engine.journal_current->serial = 1;
		mutex_exit(&engine.lock);
	}
	abd_free(head);
	return (error);
}

static void
engine_journal_select(engine_j_segment_t *s)
{
	mutex_enter(&engine.lock);
	engine.journal_arena = s->arena;
	memcpy(engine.journal_epoch, s->epoch, sizeof (s->epoch));
	engine.journal_seq = s->seq;
	engine.journal_serial = s->serial;
	engine.journal_hash = s->hash;
	engine.journal_current = s;
	mutex_exit(&engine.lock);
}

static int
engine_journal_segment(uint64_t arena, const uint64_t epoch[2],
    boolean_t allocate, boolean_t link, engine_j_segment_t **result)
{
	mutex_enter(&engine.lock);
	if (allocate) {
		zfs_range_tree_walk(engine.free, engine_store_pick, &arena);
		if (arena == UINT64_MAX) {
			mutex_exit(&engine.lock);
			return (ENOSPC);
		}
	}
	uint64_t bytes = engine.journal_bytes * engine.vd->vdev_children;
	if (arena % (ENGINE_STORE_PAGE * engine.vd->vdev_children) != 0 ||
	    !engine_store_extent(arena, bytes)) {
		mutex_exit(&engine.lock);
		return (ENGINE_EINVAL(1));
	}
	engine_j_segment_t *s = kmem_zalloc(sizeof (*s), KM_SLEEP);
	s->arena = arena;
	memcpy(s->epoch, epoch, sizeof (s->epoch));
	s->index = ++engine.journal_segment_count;
	s->free_after = UINT64_MAX;
	zfs_range_tree_remove(engine.free, arena, bytes);
	zfs_range_tree_add(engine.journal_extents, arena, bytes);
	if (link) {
		VERIFY3P(engine.journal_tail->next, ==, NULL);
		engine.journal_tail->next = s;
		engine.journal_tail = s;
	} else {
		VERIFY3P(engine.journal_pending, ==, NULL);
		engine.journal_pending = s;
	}
	engine.revision++;
	*result = s;
	mutex_exit(&engine.lock);
	return (0);
}

static int
engine_journal_finish_prepare(spa_t *spa)
{
	ASSERT(MUTEX_HELD(&engine.journal_lock));
	engine_j_segment_t *old = engine.journal_tail;
	engine_j_segment_t *next = engine.journal_pending;
	VERIFY3P(next, !=, NULL);
	uint64_t arena = next->arena;
	engine_journal_select(next);
	int error = engine_journal_empty(spa, B_TRUE);
	if (error != 0) {
		engine_journal_select(old);
		return (error);
	}
	engine_journal_roll_phase = 1;
	error = engine_journal_format(spa);
	engine_journal_roll_phase = 0;
	engine_journal_select(old);
	if (error != 0)
		return (error);
	engine_journal_roll_stop(3);
	uint64_t seq = old->seq + 1;
	abd_t *page = engine_journal_page(ENGINE_J_FORWARD, seq);
	uint64_t *w = abd_to_buf(page);
	w[6] = arena; w[7] = engine.journal_bytes;
	w[8] = next->epoch[0]; w[9] = next->epoch[1];
	memcpy(w + 20, &old->hash, sizeof (old->hash));
	engine_journal_seal(page);
	zio_cksum_t hash;
	memcpy(&hash, w + ENGINE_J_HASH, sizeof (hash));
	engine_journal_roll_phase = 2;
	error = engine_journal_write(spa, seq + 1, page);
	engine_journal_roll_phase = 0;
	abd_free(page);
	if (error == 0)
		engine_journal_roll_stop(6);
	if (error == 0) {
		engine_journal_roll_phase = 3;
		error = engine_journal_head(spa, seq, &hash,
		    ENGINE_J_ACTIVE, next);
		engine_journal_roll_phase = 0;
	}
	if (error == 0) {
		engine_journal_roll_stop(9);
		mutex_enter(&engine.lock);
		old->next = next;
		engine.journal_tail = next;
		engine.journal_pending = NULL;
		mutex_exit(&engine.lock);
		engine_journal_select(next);
		pr19262_printf("PR19262_J_FORWARD from=%llu to=%llu seq=%llu\n",
		    (u_longlong_t)old->arena, (u_longlong_t)arena,
		    (u_longlong_t)seq);
	}
	/* Failed publication freezes J; both extents remain quarantined. */
	return (error);
}

static int
engine_journal_grow(spa_t *spa)
{
	ASSERT(MUTEX_HELD(&engine.journal_lock));
	uint64_t arena = UINT64_MAX, epoch[2];
	VERIFY0(random_get_pseudo_bytes((void *)epoch, sizeof (epoch)));
	engine_j_segment_t *next;
	int error = engine_journal_segment(arena, epoch,
	    B_TRUE, B_FALSE, &next);
	if (error != 0)
		return (error);
	engine_journal_roll_phase = 4;
	error = engine_journal_head(spa, engine.journal_seq,
	    &engine.journal_hash, ENGINE_J_PREPARE, next);
	engine_journal_roll_phase = 0;
	if (error == 0) {
		engine_journal_roll_stop(13);
		error = engine_journal_finish_prepare(spa);
	}
	return (error);
}

int
pr19262_engine_journal_roll(spa_t *spa)
{
	mutex_enter(&engine.journal_lock);
	int error = engine.journal_error;
	if (error == 0 && engine.journal_repair) {
		error = engine_journal_repair(spa);
		engine.journal_repair = error != 0;
	}
	if (error == 0)
		error = engine_journal_grow(spa);
	mutex_enter(&engine.lock);
	if (error != 0)
		engine.journal_error = error;
	cv_broadcast(&engine.changed);
	mutex_exit(&engine.lock);
	mutex_exit(&engine.journal_lock);
	return (error);
}

void
pr19262_engine_journal_ub_committed(spa_t *spa, const uberblock_t *ub)
{
	mutex_enter(&engine.lock);
	if (engine.spa == spa && engine.persistent) {
		engine.journal_ready = B_TRUE;
		if (pr19262_engine_store_ub(ub)) {
			engine.durable_ub_txg = ub->ub_txg;
			engine.durable_catalog_txg = ub->ub_pr19262[6];
			memcpy(engine.durable_desc, ub->ub_pr19262,
			    sizeof (engine.durable_desc));
		}
	}
	mutex_exit(&engine.lock);
}

static int
engine_journal_read(spa_t *spa, uint64_t page, uint64_t kind,
    uint64_t seq, const zio_cksum_t *previous, abd_t *out)
{
	int error = ECKSUM;
	abd_t *candidate = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	for (uint64_t i = 0; i < engine.vd->vdev_children; i++) {
		if (engine_store_io_at(engine.vd->vdev_child[i],
		    engine.journal_arena, page, 1, candidate, B_FALSE) != 0 ||
		    !engine_journal_valid(candidate, kind, seq))
			continue;
		uint64_t *w = abd_to_buf(candidate);
		if (previous != NULL &&
		    memcmp(w + 20, previous, sizeof (*previous)) != 0)
			continue;
		if (error == 0) {
			uint64_t *selected = abd_to_buf(out);
			uint_t key = kind == ENGINE_J_HEAD ? 30 : 3;
			if (selected[key] == w[key] && memcmp(selected, w,
			    ENGINE_STORE_PAGE) != 0) {
				error = ENGINE_EINVAL(9);
				break;
			}
			if (selected[key] >= w[key])
				continue;
		}
		abd_copy(out, candidate, ENGINE_STORE_PAGE);
		error = 0;
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	abd_free(candidate);
	return (error);
}

static int
engine_journal_read_head(spa_t *spa, abd_t *selected, boolean_t all_io)
{
	abd_t *page = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	int error = ECKSUM;
	engine_quorum_t io = { 0 };
	boolean_t conflict = B_FALSE;
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	for (uint_t slot = 0; slot < 2; slot++) {
		for (uint_t child = 0; child < engine.vd->vdev_children;
		    child++) {
			int e = engine_store_io_at(engine.vd->vdev_child[child],
			    engine.journal_arena, slot, 1, page, B_FALSE);
			if (e != 0) {
				if (slot == 0)
					engine_quorum_add(&io, e);
				continue;
			}
			if (!engine_journal_valid(page, ENGINE_J_HEAD,
			    UINT64_MAX))
				continue;
			uint64_t *w = abd_to_buf(page);
			uint64_t *best = abd_to_buf(selected);
			if (error == 0 && best[30] == w[30] &&
			    memcmp(best, w, ENGINE_STORE_PAGE) != 0)
				conflict = B_TRUE;
			if (error != 0 || w[30] > best[30]) {
				abd_copy(selected, page, ENGINE_STORE_PAGE);
				conflict = B_FALSE;
			}
			error = 0;
		}
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	abd_free(page);
	if (error == 0) {
		uint64_t *w = abd_to_buf(selected);
		if (conflict || w[30] == 0 || w[31] > ENGINE_J_ACTIVE)
			error = ENGINE_EINVAL(10);
	}
	/* A required read of every member tolerates the certified absences. */
	int required = engine_quorum_result(&io);
	return (all_io && required != 0 ? required : error);
}

static int
engine_journal_apply(uint64_t *w)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	uint64_t v = w[7], size = w[8];
	uint64_t sector = 1ULL << engine.vd->vdev_ashift;
	if (v >= engine.total || size == 0 || size > engine.total - v ||
	    !IS_P2ALIGNED(v, sector) ||
	    !IS_P2ALIGNED(size, sector) || w[6] == 0 ||
	    w[13] <= w[14] || w[13] > engine.vd->vdev_children ||
	    w[14] == 0 || w[14] > 3 || w[17] > 1 || w[15] == 0 ||
	    w[29] == 0 || w[29] > w[15] || w[27] <= w[28] ||
	    w[27] > engine.vd->vdev_children || w[28] == 0 || w[28] > 3)
		return (ENGINE_EINVAL(2));
	/* A released owner's bindings are folded into the catalog. */
	if (engine_tombstoned(v, w[6]))
		return (0);
	engine_owner_t *e = engine_find(v);
	if (e == NULL) {
		boolean_t legacy = zfs_range_tree_contains(engine.legacy,
		    v, size);
		if (legacy && (w[9] != 0 || w[24] != v || w[25] != size))
			return (ENGINE_EINVAL(3));
		e = engine_owner_alloc(KM_SLEEP);
		e->v = v; e->size = size; e->incarnation = w[6];
		e->allocation_txg = w[9]; e->created = w[16];
		e->catalog_txg = legacy ? 0 : UINT64_MAX;
		e->journal_pending = !legacy;
		avl_add(&engine.owners, e);
		if (legacy) {
			zfs_range_tree_remove(engine.legacy, v, size);
			engine_version_t *old = engine_version_new(e, v, size,
			    w[26], B_TRUE);
			old->width = w[27]; old->parity = w[28];
			old->generation = w[29];
			e->cur = old;
		}
	} else if (e->incarnation != w[6] || e->size != size ||
	    e->allocation_txg != w[9]) {
		return (ENGINE_EINVAL(4));
	}
	e->journal_touched = B_TRUE;
	engine_owner_dirty(e);
	engine.next_incarnation = MAX(engine.next_incarnation, w[6]);
	engine.sequence = MAX(engine.sequence, MAX(w[15], w[16]));
	/*
	 * A binding older than the catalog's current one is already folded
	 * into M; its backing may have been fenced, reclaimed and reused.
	 */
	if (e->cur != NULL && w[15] < e->cur->generation)
		return (0);
	for (engine_version_t *b = e->versions; b != NULL; b = b->next) {
		if (b->generation != w[15])
			continue;
		if (b->p != w[10] || b->size != w[11] || b->birth != w[12] ||
		    b->width != w[13] || b->parity != w[14])
			return (ENGINE_EINVAL(5));
		b->journal_seq = w[3];
		b->ready |= w[17];
		if (b->generation > e->cur->generation) {
			/* M may already quarantine a failed publication. */
			if (!b->retired)
				return (ENGINE_EINVAL(6));
			b->retired = B_FALSE;
			engine.retired -= b->size;
			e->cur->retired = B_TRUE;
			if (!e->retired)
				engine.retired += e->cur->size;
			e->cur = b;
			if (e->retired)
				engine.retired += b->size;
		}
		return (0);
	}
	if (!engine_store_extent(w[10], w[11]))
		return (ENGINE_EINVAL(7));
	engine_version_t *b = engine_version_new(e, w[10], w[11], w[12], w[17]);
	b->width = w[13]; b->parity = w[14]; b->generation = w[15];
	b->journal_seq = w[3];
	zfs_range_tree_remove(engine.free, b->p, b->size);
	if (e->cur == NULL || b->generation > e->cur->generation) {
		if (e->cur != NULL) {
			e->cur->retired = B_TRUE;
			if (!e->retired)
				engine.retired += e->cur->size;
		}
		e->cur = b;
		if (e->retired)
			engine.retired += b->size;
	} else {
		b->retired = B_TRUE;
		engine.retired += b->size;
	}
	return (0);
}

static int
engine_journal_recover_segment(spa_t *spa, engine_j_segment_t **next)
{
	abd_t *page = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	int error = engine_journal_read_head(spa, page, B_FALSE);
	if (error != 0) {
		abd_free(page);
		return (error);
	}
	uint64_t *header = abd_to_buf(page);
	uint64_t head = header[3], phase = header[31];
	uint64_t target = header[32], bytes = header[33];
	uint64_t epoch[] = { header[34], header[35] };
	engine.journal_current->serial = engine.journal_serial = header[30];
	engine.journal_current->phase = phase;
	engine.journal_current->target = target;
	memcpy(engine.journal_current->target_epoch, epoch, sizeof (epoch));
	zio_cksum_t expected = { 0 }, previous = { 0 };
	memcpy(&expected, header + 20, sizeof (expected));
	uint64_t capacity = engine.journal_bytes / ENGINE_STORE_PAGE - 2;
	if (head > capacity ||
	    (head == 0 && memcmp(&expected, &previous, sizeof (expected)) != 0))
		error = ECKSUM;
	if (phase == ENGINE_J_NORMAL &&
	    (target != 0 || bytes != 0 || epoch[0] != 0 || epoch[1] != 0))
		error = ENGINE_EINVAL(11);
	if (phase != ENGINE_J_NORMAL) {
		uint64_t size = bytes * engine.vd->vdev_children;
		uint64_t stripe = ENGINE_STORE_PAGE * engine.vd->vdev_children;
		uint64_t msize = engine.arena_bytes * engine.vd->vdev_children;
		uint64_t found, span;
		if (bytes != engine.journal_bytes || target % stripe != 0 ||
		    target >= engine.total || size > engine.total - target ||
		    (epoch[0] == engine.journal_epoch[0] &&
		    epoch[1] == engine.journal_epoch[1]) ||
		    (target < engine.arena + msize &&
		    engine.arena < target + size) ||
		    zfs_range_tree_find_in(engine.journal_extents,
		    target, size, &found, &span))
			error = ENGINE_EINVAL(12);
	}
	uint64_t last = 0;
	for (uint64_t seq = 1; error == 0 && seq <= capacity; seq++) {
		int e = engine_journal_read(spa, seq + 1, UINT64_MAX, seq,
		    &previous, page);
		if (e != 0) {
			if (seq <= head)
				error = ECKSUM;
			break;
		}
		uint64_t *w = abd_to_buf(page);
		if (w[1] != ENGINE_J_BIND && w[1] != ENGINE_J_FORWARD) {
			error = ENGINE_EINVAL(13);
			break;
		}
		if (w[1] == ENGINE_J_FORWARD && seq > head &&
		    phase != ENGINE_J_PREPARE)
			break;
		memcpy(&previous, w + ENGINE_J_HASH, sizeof (previous));
		if (seq == head && memcmp(&previous, &expected,
		    sizeof (previous)) != 0)
			error = ECKSUM;
		last = seq;
		if (error != 0)
			break;
		if (w[1] == ENGINE_J_FORWARD) {
			if (head > seq || phase == ENGINE_J_NORMAL ||
			    w[6] != target || w[7] != bytes ||
			    w[8] != epoch[0] || w[9] != epoch[1] ||
			    (phase == ENGINE_J_ACTIVE && seq != head)) {
				error = ENGINE_EINVAL(14);
				break;
			}
			error = engine_journal_segment(target, epoch,
			    B_FALSE, B_TRUE, next);
			if (error == 0)
				engine.journal_current->phase = ENGINE_J_ACTIVE;
			break;
		}
		if (seq == capacity) {
			error = ENGINE_EINVAL(15);
			break;
		}
		mutex_enter(&engine.lock);
		error = engine_journal_apply(w);
		mutex_exit(&engine.lock);
	}
	if (error == 0 && phase == ENGINE_J_ACTIVE && *next == NULL)
		error = ECKSUM;
	if (error == 0 && phase == ENGINE_J_PREPARE && *next == NULL) {
		/* An intent reserves space without publishing log data. */
		mutex_enter(&engine.lock);
		boolean_t free = engine_store_extent(target,
		    bytes * engine.vd->vdev_children);
		mutex_exit(&engine.lock);
		if (free) {
			engine_j_segment_t *pending;
			error = engine_journal_segment(target, epoch,
			    B_FALSE, B_FALSE, &pending);
			if (error == 0) {
				engine_j_segment_t *old =
				    engine.journal_current;
				engine_journal_select(pending);
				error = engine_journal_empty(spa, B_FALSE);
				engine_journal_select(old);
			}
		}
	}
	if (error == 0) {
		mutex_enter(&engine.lock);
		error = engine_store_validate_virtual();
		engine.journal_seq = last;
		engine.journal_hash = previous;
		mutex_exit(&engine.lock);
		pr19262_printf("PR19262_J_RECOVER head=%llu prefix=%llu\n",
		    (u_longlong_t)head, (u_longlong_t)last);
	}
	abd_free(page);
	return (error);
}

static int
engine_journal_recover(spa_t *spa)
{
	engine_j_segment_t *s = engine.journal_segments;
	int error = 0;
	while (s != NULL && error == 0) {
		engine_journal_select(s);
		engine_j_segment_t *next = NULL;
		error = engine_journal_recover_segment(spa, &next);
		s->seq = engine.journal_seq;
		s->hash = engine.journal_hash;
		s = next;
	}
	if (error == 0) {
		mutex_enter(&engine.lock);
		engine.journal_formatted = engine.journal_ready = B_TRUE;
		engine.journal_repair = B_TRUE;
		/*
		 * Every replayed record is in RAM now; the catalog written
		 * after the claim barrier lists all of it.  Tombstones loaded
		 * from the catalog may be bound anywhere up to the tail.
		 */
		for (s = engine.journal_segments; s != NULL; s = s->next)
			s->fold_txg = MAX(s->fold_txg,
			    spa_last_synced_txg(spa) + 2);
		for (engine_tomb_t *t = avl_first(&engine.tombstones);
		    t != NULL; t = AVL_NEXT(&engine.tombstones, t))
			if (t->segment == UINT64_MAX)
				t->segment = engine.journal_tail->index;
		mutex_exit(&engine.lock);
	}
	return (error);
}

/*
 * A record is folded once a durable catalog lists the version it binds:
 * the owner must be catalogued (allocation txg) and the next catalog sync
 * after the binding must have happened (at most two txgs later).
 */
static void
engine_journal_fold(spa_t *spa, uint64_t catalog_txg)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	uint64_t visible = spa_last_synced_txg(spa) + 2;
	if (catalog_txg != UINT64_MAX)
		visible = MAX(visible, catalog_txg);
	if (engine.journal_tail != NULL)
		engine.journal_tail->fold_txg =
		    MAX(engine.journal_tail->fold_txg, visible);
}

/*
 * Cut folded segments from the chain head once the floor root's catalog
 * lists every version they bind, so that the next catalog records the new
 * root; free a cut segment at a later fence whose floor root's catalog
 * already names that root, so that no importable root can reach it.
 * Tombstones whose segments are all gone expire.  The caller holds
 * store_lock; no root can be written meanwhile.
 */
static void
engine_journal_gc(spa_t *spa, uint64_t catalog, uint64_t *segments,
    uint64_t *bytes)
{
	(void) spa;
	*segments = *bytes = 0;
	mutex_enter(&engine.journal_lock);
	mutex_enter(&engine.lock);
	uint64_t size = engine.journal_bytes * engine.vd->vdev_children;
	engine_j_segment_t *s;
	while ((s = engine.journal_segments) != NULL &&
	    s != engine.journal_tail && s != engine.journal_current &&
	    s->next != NULL && s->fold_txg <= catalog) {
		engine.journal_segments = s->next;
		s->next = NULL;
		s->free_after = UINT64_MAX;
		engine_j_segment_t **link = &engine.journal_retired;
		while (*link != NULL)
			link = &(*link)->next;
		*link = s;
		engine.journal_root = engine.journal_segments->arena;
		memcpy(engine.journal_root_epoch,
		    engine.journal_segments->epoch,
		    sizeof (engine.journal_root_epoch));
		engine.revision++;
		pr19262_printf("PR19262_J_RETIRE index=%llu arena=%llu "
		    "fold=%llu catalog=%llu root=%llu\n",
		    (u_longlong_t)s->index, (u_longlong_t)s->arena,
		    (u_longlong_t)s->fold_txg, (u_longlong_t)catalog,
		    (u_longlong_t)engine.journal_root);
	}
	engine_j_segment_t **link = &engine.journal_retired;
	while ((s = *link) != NULL) {
		if (s->free_after == UINT64_MAX || s->free_after > catalog) {
			link = &s->next;
			continue;
		}
		*link = s->next;
		VERIFY(zfs_range_tree_contains(engine.journal_extents,
		    s->arena, size));
		zfs_range_tree_remove(engine.journal_extents, s->arena, size);
		zfs_range_tree_verify_not_present(engine.free, s->arena, size);
		zfs_range_tree_add(engine.free, s->arena, size);
		engine.journal_freed_segments++;
		engine.journal_freed_bytes += size;
		(*segments)++;
		*bytes += size;
		engine.revision++;
		pr19262_printf("PR19262_J_FREE index=%llu arena=%llu "
		    "recorded=%llu catalog=%llu\n", (u_longlong_t)s->index,
		    (u_longlong_t)s->arena, (u_longlong_t)s->free_after,
		    (u_longlong_t)catalog);
		kmem_free(s, sizeof (*s));
	}
	uint64_t live = engine.journal_segments->index;
	for (engine_tomb_t *t = avl_first(&engine.tombstones); t != NULL;
	    t = AVL_NEXT(&engine.tombstones, t))
		if (t->segment < live)
			t->expired = B_TRUE;
	mutex_exit(&engine.lock);
	mutex_exit(&engine.journal_lock);
}

static int
engine_journal_repair_segment(spa_t *spa)
{
	abd_t *page = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	zio_cksum_t previous = { 0 };
	/* A returning member can hold a higher unpublished header serial. */
	int error = engine_journal_read_head(spa, page, B_TRUE);
	if (error == 0) {
		uint64_t serial = ((uint64_t *)abd_to_buf(page))[30];
		engine.journal_serial = MAX(engine.journal_serial, serial);
	}
	for (uint64_t seq = 1; error == 0 && seq <= engine.journal_seq; seq++) {
		error = engine_journal_read(spa, seq + 1, UINT64_MAX,
		    seq, &previous, page);
		if (error == 0) {
			memcpy(&previous, (uint64_t *)abd_to_buf(page) +
			    ENGINE_J_HASH, sizeof (previous));
			error = engine_journal_write(spa, seq + 1, page);
		}
	}
	abd_free(page);
	if (error == 0) {
		engine_j_segment_t *s = engine.journal_current;
		uint64_t phase = s->phase;
		engine_j_segment_t *target = s->next;
		if (phase == ENGINE_J_PREPARE) {
			target = engine.journal_pending;
			if (target == NULL)
				phase = ENGINE_J_NORMAL;
		}
		error = engine_journal_head(spa, engine.journal_seq,
		    &previous, phase, target);
	}
	return (error);
}

static int
engine_journal_repair(spa_t *spa)
{
	int error = 0;
	for (engine_j_segment_t *s = engine.journal_segments;
	    s != NULL && error == 0; s = s->next) {
		engine_journal_select(s);
		error = engine_journal_repair_segment(spa);
	}
	engine_journal_select(engine.journal_tail);
	if (error == 0 && engine.journal_pending != NULL)
		error = engine_journal_finish_prepare(spa);
	return (error);
}

static void (*engine_journal_observer)(spa_t *, const blkptr_t *, int);

void
pr19262_engine_journal_test_hook(void (*hook)(spa_t *, const blkptr_t *, int))
{
	/* Install/remove this private test hook only with all callers idle. */
	engine_journal_observer = hook;
}

static void
engine_journal_test_event(spa_t *spa, const blkptr_t *bp, int phase)
{
	if (engine_journal_observer != NULL)
		engine_journal_observer(spa, bp, phase);
}

/* A held identity survives lookup replacement until this decision completes. */
static int
engine_journal_admit_locked(spa_t *spa, engine_hold_t *h)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	if (h == NULL || engine.spa != spa || !engine.persistent)
		return (ENOENT);
	if (h->error != 0)
		return (h->error);
	for (uint_t i = 0; i < h->count; i++) {
		engine_owner_t *e = h->owner[i];
		if (e->aborted || engine_find(e->v) != e)
			return (ESTALE);
	}
	for (uint_t i = 0; i < h->count; i++)
		h->owner[i]->journal_touched = B_TRUE;
	return (0);
}

static int
engine_journal_bind_impl(spa_t *spa, engine_hold_t *h,
    engine_version_t *staging)
{
	mutex_enter(&engine.lock);
	int error = engine_journal_admit_locked(spa, h);
	mutex_exit(&engine.lock);
	if (error != 0)
		return (error);
	mutex_enter(&engine.journal_lock);
	if (error == 0 && engine.journal_repair) {
		error = engine_journal_repair(spa);
		engine.journal_repair = error != 0;
	}
	for (uint_t i = 0; error == 0 && i < h->count; i++) {
		mutex_enter(&engine.lock);
		engine_owner_t *e = h->owner[i];
		while (e->copying && staging == NULL) {
			/* The copier needs this lock to publish. */
			mutex_exit(&engine.journal_lock);
			cv_wait(&engine.changed, &engine.lock);
			mutex_exit(&engine.lock);
			mutex_enter(&engine.journal_lock);
			mutex_enter(&engine.lock);
		}
		if (e->aborted || engine_find(e->v) != e) {
			error = ESTALE;
			mutex_exit(&engine.lock);
			break;
		}
		engine_version_t *b = staging != NULL ? staging : e->cur;
		ASSERT(staging == NULL || (h->count == 1 && e->copying));
		if (b->journal_seq != 0) {
			mutex_exit(&engine.lock);
			continue;
		}
		error = engine.journal_error;
		if (error == 0 && engine.journal_seq + 3 >=
		    engine.journal_bytes / ENGINE_STORE_PAGE) {
			mutex_exit(&engine.lock);
			error = engine_journal_grow(spa);
			mutex_enter(&engine.lock);
		}
		uint64_t seq = engine.journal_seq + 1;
		if (!engine.journal_ready)
			error = EAGAIN;
		if (seq + 2 > engine.journal_bytes / ENGINE_STORE_PAGE)
			error = ENOSPC;
		if (error != 0) {
			mutex_exit(&engine.lock);
			break;
		}
		abd_t *page = engine_journal_page(ENGINE_J_BIND, seq);
		uint64_t *w = abd_to_buf(page);
		w[6] = e->incarnation; w[7] = e->v; w[8] = e->size;
		w[9] = e->allocation_txg; w[10] = b->p; w[11] = b->size;
		w[12] = b->birth; w[13] = b->width; w[14] = b->parity;
		w[15] = b->generation; w[16] = e->created; w[17] = b->ready;
		memcpy(w + 20, &engine.journal_hash, sizeof (zio_cksum_t));
		engine_version_t *old = b;
		for (engine_version_t *p = e->versions; p != NULL; p = p->next)
			if (p->generation < old->generation)
				old = p;
		w[24] = old->p; w[25] = old->size; w[26] = old->birth;
		w[27] = old->width; w[28] = old->parity;
		w[29] = old->generation;
		e->journal_touched = B_TRUE;
		b->leases++;
		mutex_exit(&engine.lock);
		engine_journal_seal(page);
		zio_cksum_t hash;
		memcpy(&hash, w + ENGINE_J_HASH, sizeof (hash));
		error = engine_journal_write(spa, seq + 1, page);
		abd_free(page);
		if (error == 0 && seq == engine_journal_fault_seq &&
		    engine_journal_fault_stage == 3) {
			pr19262_printf("PR19262_J_CRASH_AFTER_BIND seq=%llu\n",
			    (u_longlong_t)seq);
			pr19262_flush();
			pr19262_exit(78);
		}
		if (error == 0) {
			error = engine_journal_head(spa, seq, &hash,
			    ENGINE_J_NORMAL, NULL);
		}
		if (error == 0 && seq == engine_journal_fault_seq &&
		    engine_journal_fault_stage == 6) {
			pr19262_printf("PR19262_J_CRASH_AFTER_HEAD seq=%llu\n",
			    (u_longlong_t)seq);
			pr19262_flush();
			pr19262_exit(79);
		}
		mutex_enter(&engine.lock);
		b->leases--;
		if (error == 0) {
			engine.journal_seq = b->journal_seq = seq;
			engine.journal_hash = hash;
			engine.journal_tail->seq = seq;
			engine.journal_tail->hash = hash;
			engine_journal_fold(spa, e->catalog_txg);
			pr19262_printf("PR19262_J_BIND seq=%llu v=%llu p=%llu "
			    "arena=%llu\n", (u_longlong_t)seq,
			    (u_longlong_t)e->v,
			    (u_longlong_t)b->p,
			    (u_longlong_t)engine.journal_arena);
		}
		mutex_exit(&engine.lock);
	}
	mutex_enter(&engine.lock);
	if (error != 0)
		engine.journal_error = error;
	cv_broadcast(&engine.changed);
	mutex_exit(&engine.lock);
	mutex_exit(&engine.journal_lock);
	return (error);
}

static int
engine_journal_bind_hold(spa_t *spa, engine_hold_t *h)
{
	return (engine_journal_bind_impl(spa, h, NULL));
}

static int
engine_journal_bind_copy(engine_copy_t *c)
{
	/* The copy handle already pins its owner and source version. */
	engine_hold_t h = { .count = 1, .owner = { c->owner } };
	return (engine_journal_bind_impl(engine.spa, &h, c->stage));
}

/* One copy's BIND frame at seq; the chain hash is set when sealed. */
static abd_t *
engine_journal_bind_frame(engine_owner_t *e, engine_version_t *b,
    uint64_t seq)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	abd_t *page = engine_journal_page(ENGINE_J_BIND, seq);
	uint64_t *w = abd_to_buf(page);
	w[6] = e->incarnation; w[7] = e->v; w[8] = e->size;
	w[9] = e->allocation_txg; w[10] = b->p; w[11] = b->size;
	w[12] = b->birth; w[13] = b->width; w[14] = b->parity;
	w[15] = b->generation; w[16] = e->created; w[17] = b->ready;
	engine_version_t *old = b;
	for (engine_version_t *p = e->versions; p != NULL; p = p->next)
		if (p->generation < old->generation)
			old = p;
	w[24] = old->p; w[25] = old->size; w[26] = old->birth;
	w[27] = old->width; w[28] = old->parity;
	w[29] = old->generation;
	return (page);
}

/*
 * Bind a group of copies with one write of their frames and one head.
 * The frames take consecutive sequence numbers, each chained to the one
 * before, as single binds would have written them; recovery replays the
 * longest valid prefix past the head, so a group cut short publishes
 * the copies whose frames landed, and every copy's data was flushed
 * before any frame was written.  Only copies with attempted[i] are bound;
 * errors[i] receives each one's result.
 */
static void
engine_journal_bind_copies(engine_copy_t **cv, uint_t n,
    const boolean_t *attempted, int *errors)
{
	spa_t *spa = engine.spa;
	/* The crash and fault hooks act on single frames: keep them so. */
	if (engine_journal_fault_stage != 0 || engine_journal_roll_fault != 0) {
		for (uint_t i = 0; i < n; i++)
			if (attempted[i])
				errors[i] = engine_journal_bind_copy(cv[i]);
		return;
	}
	uint_t m = 0;
	mutex_enter(&engine.lock);
	for (uint_t i = 0; i < n; i++) {
		if (!attempted[i])
			continue;
		engine_owner_t *e = cv[i]->owner;
		ASSERT(e->copying);
		if (!engine.persistent)
			errors[i] = ENOENT;
		else if (e->aborted || engine_find(e->v) != e)
			errors[i] = ESTALE;
		else
			e->journal_touched = B_TRUE;
		m += errors[i] == 0;
	}
	mutex_exit(&engine.lock);
	if (m == 0)
		return;
	engine_member_page_t *pv = kmem_alloc(m * sizeof (*pv), KM_SLEEP);
	uint_t *index = kmem_alloc(m * sizeof (uint_t), KM_SLEEP);
	mutex_enter(&engine.journal_lock);
	int error = 0;
	if (engine.journal_repair) {
		error = engine_journal_repair(spa);
		engine.journal_repair = error != 0;
	}
	uint_t next = 0;
	while (error == 0 && next < n) {
		mutex_enter(&engine.lock);
		error = engine.journal_error;
		if (error == 0 && engine.journal_seq + 3 >=
		    engine.journal_bytes / ENGINE_STORE_PAGE) {
			mutex_exit(&engine.lock);
			error = engine_journal_grow(spa);
			mutex_enter(&engine.lock);
		}
		uint64_t last = engine.journal_bytes / ENGINE_STORE_PAGE;
		if (error == 0 && !engine.journal_ready)
			error = EAGAIN;
		/* A grown segment has room for a frame; never spin on none. */
		if (error == 0 && engine.journal_seq + 3 >= last)
			error = ENOSPC;
		if (error != 0) {
			mutex_exit(&engine.lock);
			break;
		}
		/* As many frames as this segment holds before it must grow. */
		uint64_t first = engine.journal_seq + 1, seq = first - 1;
		uint_t k = 0;
		for (; next < n && seq + 3 < last; next++) {
			if (!attempted[next] || errors[next] != 0)
				continue;
			engine_copy_t *c = cv[next];
			engine_owner_t *e = c->owner;
			if (e->aborted || engine_find(e->v) != e) {
				errors[next] = ESTALE;
				continue;
			}
			VERIFY0(c->stage->journal_seq);
			seq++;
			pv[k].arena = engine.journal_arena;
			pv[k].page = seq + 1;
			pv[k].abd = engine_journal_bind_frame(e, c->stage, seq);
			c->stage->leases++;
			index[k++] = next;
		}
		zio_cksum_t hash = engine.journal_hash;
		mutex_exit(&engine.lock);
		if (k == 0)
			continue;
		for (uint_t j = 0; j < k; j++) {
			uint64_t *w = abd_to_buf(pv[j].abd);
			memcpy(w + 20, &hash, sizeof (hash));
			engine_journal_seal(pv[j].abd);
			memcpy(&hash, w + ENGINE_J_HASH, sizeof (hash));
		}
		if (engine_journal_group_fault != 0 && k >= 2) {
			uint_t landed = (k + 1) / 2;
			error = engine_journal_write_pages(spa, pv, landed);
			pr19262_printf("PR19262_J_GROUP_CUT head=%llu "
			    "first=%llu landed=%u frames=%u error=%d\n",
			    (u_longlong_t)(first - 1), (u_longlong_t)first,
			    landed, k, error);
			pr19262_flush();
			pr19262_exit(97);
		}
		error = engine_journal_write_pages(spa, pv, k);
		if (error == 0) {
			error = engine_journal_head(spa, seq, &hash,
			    ENGINE_J_NORMAL, NULL);
		}
		mutex_enter(&engine.lock);
		for (uint_t j = 0; j < k; j++) {
			engine_copy_t *c = cv[index[j]];
			c->stage->leases--;
			abd_free(pv[j].abd);
			if (error != 0) {
				errors[index[j]] = error;
				continue;
			}
			c->stage->journal_seq = first + j;
			engine_journal_fold(spa, c->owner->catalog_txg);
			pr19262_printf("PR19262_J_BIND seq=%llu v=%llu p=%llu "
			    "arena=%llu\n", (u_longlong_t)(first + j),
			    (u_longlong_t)c->owner->v,
			    (u_longlong_t)c->stage->p,
			    (u_longlong_t)engine.journal_arena);
		}
		if (error == 0) {
			engine.journal_seq = seq;
			engine.journal_hash = hash;
			engine.journal_tail->seq = seq;
			engine.journal_tail->hash = hash;
		}
		mutex_exit(&engine.lock);
	}
	/* What the loop did not reach shares the error that stopped it. */
	for (uint_t i = next; i < n; i++)
		if (attempted[i] && errors[i] == 0)
			errors[i] = error;
	mutex_enter(&engine.lock);
	if (error != 0)
		engine.journal_error = error;
	cv_broadcast(&engine.changed);
	mutex_exit(&engine.lock);
	mutex_exit(&engine.journal_lock);
	kmem_free(index, m * sizeof (uint_t));
	kmem_free(pv, m * sizeof (*pv));
}

int
pr19262_engine_journal_bind(spa_t *spa, const blkptr_t *bp)
{
	if (!pr19262_engine_persistent(spa))
		return (0);
	engine_hold_t *h = pr19262_engine_hold(spa, bp, NULL, B_FALSE, B_FALSE);
	engine_journal_test_event(spa, bp, 2);
	/* A block on native vdevs only (a separate log) needs no binding. */
	int error = h == NULL && pr19262_engine_bp_foreign(spa, bp) ? 0 :
	    engine_journal_bind_hold(spa, h);
	pr19262_engine_rele(h);
	return (error);
}

typedef struct engine_j_dep {
	struct engine_j_dep *next;
	engine_hold_t *hold;
} engine_j_dep_t;

typedef struct engine_j_job {
	spa_t *spa;
	engine_hold_t *hold;
	zio_t *gate;
	blkptr_t bp;
	engine_j_dep_t *deps;
} engine_j_job_t;

static int
engine_journal_collect(spa_t *spa, const blkptr_t *bp, void *arg)
{
	engine_j_job_t *job = arg;
	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return (0);
	engine_hold_t *h = pr19262_engine_hold(spa, bp, job->hold,
	    B_TRUE, B_TRUE);
	if (h == NULL)
		return (pr19262_engine_bp_foreign(spa, bp) ? 0 : ENOENT);
	if (h->error != 0) {
		int error = h->error;
		pr19262_engine_rele(h);
		return (error);
	}
	mutex_enter(&engine.lock);
	int error = engine_journal_admit_locked(spa, h);
	if (error != 0) {
		mutex_exit(&engine.lock);
		pr19262_engine_rele(h);
		return (error);
	}
	h->write = B_TRUE;
	for (uint_t i = 0; i < h->count; i++)
		h->owner[i]->queued_writers++;
	mutex_exit(&engine.lock);
	engine_j_dep_t *dep = kmem_alloc(sizeof (*dep), KM_SLEEP);
	dep->hold = h;
	dep->next = job->deps;
	job->deps = dep;
	return (0);
}

static void
engine_journal_worker(void *arg)
{
	engine_j_job_t *job = arg;
	/* A log block on a native vdev (separate log) has nothing to bind. */
	boolean_t foreign = job->hold == NULL &&
	    pr19262_engine_bp_foreign(job->spa, &job->bp);
	int error = foreign ? 0 :
	    job->hold == NULL ? ENOENT : job->hold->error;
	if (error == 0 && !foreign && BP_IS_GANG(&job->bp))
		error = pr19262_zio_walk_gang(job->spa, &job->bp, job->hold,
		    engine_journal_collect, job);
	if (error == 0 && !foreign)
		error = engine_journal_bind_hold(job->spa, job->hold);
	uint64_t dependencies = 0;
	while (job->deps != NULL) {
		engine_j_dep_t *dep = job->deps;
		job->deps = dep->next;
		if (error == 0)
			error = engine_journal_bind_hold(job->spa, dep->hold);
		pr19262_engine_rele(dep->hold);
		kmem_free(dep, sizeof (*dep));
		dependencies++;
	}
	if (BP_IS_GANG(&job->bp))
		pr19262_printf("PR19262_J_GANG dependencies=%llu error=%d\n",
		    (u_longlong_t)dependencies, error);
	job->gate->io_error = error;
	if (error != 0 && engine.gate_prints++ < 8)
		pr19262_printf("PR19262_GATE_FAILED error=%d vdev=%llu v=%llu "
		    "birth=%llu type=%u level=%u\n", error,
		    (u_longlong_t)DVA_GET_VDEV(&job->bp.blk_dva[0]),
		    (u_longlong_t)DVA_GET_OFFSET(&job->bp.blk_dva[0]),
		    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(&job->bp),
		    (uint_t)BP_GET_TYPE(&job->bp),
		    (uint_t)BP_GET_LEVEL(&job->bp));
	pr19262_engine_rele(job->hold);
	zio_nowait(job->gate);
	kmem_free(job, sizeof (*job));
}

void
pr19262_engine_journal_gate(zio_t *parent, spa_t *spa, const blkptr_t *bp)
{
	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return;
	engine_j_job_t *job = kmem_zalloc(sizeof (*job), KM_SLEEP);
	job->spa = spa;
	job->bp = *bp;
	job->hold = pr19262_engine_hold(spa, bp, NULL, B_FALSE, B_FALSE);
	engine_journal_test_event(spa, bp, 1);
	mutex_enter(&engine.lock);
	int error = engine_journal_admit_locked(spa, job->hold);
	if (job->hold != NULL && error != 0)
		job->hold->error = error;
	if (error != 0 && error != ENOENT && engine.gate_prints++ < 8)
		pr19262_printf("PR19262_GATE_REFUSED error=%d vdev=%llu "
		    "v=%llu birth=%llu type=%u level=%u ndvas=%u\n", error,
		    (u_longlong_t)DVA_GET_VDEV(&bp->blk_dva[0]),
		    (u_longlong_t)DVA_GET_OFFSET(&bp->blk_dva[0]),
		    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(bp),
		    (uint_t)BP_GET_TYPE(bp), (uint_t)BP_GET_LEVEL(bp),
		    (uint_t)BP_GET_NDVAS(bp));
	mutex_exit(&engine.lock);
	job->gate = zio_null(parent, spa, NULL, NULL, NULL, ZIO_FLAG_CANFAIL);
	VERIFY3U(taskq_dispatch(system_taskq, engine_journal_worker, job,
	    TQ_SLEEP), !=, TASKQID_INVALID);
}

int
pr19262_engine_journal_claim(spa_t *spa, uint64_t txg)
{
	if (!pr19262_engine_persistent(spa))
		return (0);
	int error = 0;
	mutex_enter(&engine.lock);
	uint64_t capacity = avl_numnodes(&engine.owners), count = 0;
	if (capacity > SIZE_MAX / sizeof (engine_owner_t *)) {
		mutex_exit(&engine.lock);
		return (EOVERFLOW);
	}
	engine_owner_t **pending = pr19262_alloc(capacity * sizeof (*pending),
	    KM_SLEEP);
	for (engine_owner_t *e = avl_first(&engine.owners); e != NULL;
	    e = AVL_NEXT(&engine.owners, e)) {
		if (!e->journal_pending)
			continue;
		e->holds++;
		pending[count++] = e;
	}
	mutex_exit(&engine.lock);
	boolean_t trace = pr19262_env("PR19262_J_CLAIM_TRACE") != NULL;
	if (trace)
		pr19262_printf("PR19262_J_CLAIM_BEGIN owners=%llu txg=%llu "
		    "sync_on=%d\n", (u_longlong_t)count, (u_longlong_t)txg,
		    spa->spa_sync_on);
	spa_config_enter(spa, SCL_ALLOC, FTAG, RW_READER);
	/* Validate every reservation before mutating the native V allocator. */
	for (uint64_t i = 0; i < count && error == 0; i++)
		error = metaslab_claim_impl(engine.vd, pending[i]->v,
		    pending[i]->size, 0);
	for (uint64_t i = 0; i < count && error == 0; i++) {
		engine_owner_t *e = pending[i];
		error = metaslab_claim_impl(engine.vd, e->v, e->size, txg);
		if (error != 0)
			break;
		/* A successful claim must change the real native free tree. */
		if (metaslab_claim_impl(engine.vd,
		    e->v, e->size, 0) != ENOENT) {
			error = EIO;
			break;
		}
		mutex_enter(&engine.lock);
		e->journal_pending = B_FALSE;
		e->claimed_txg = e->catalog_txg = txg;
		engine_owner_dirty(e);
		engine.revision++;
		mutex_exit(&engine.lock);
		spa->spa_claim_max_txg = MAX(spa->spa_claim_max_txg,
		    e->allocation_txg);
		if (trace)
			pr19262_printf("PR19262_J_CLAIM_NATIVE v=%llu "
			    "size=%llu "
			    "txg=%llu native_free=0\n", (u_longlong_t)e->v,
			    (u_longlong_t)e->size, (u_longlong_t)txg);
	}
	spa_config_exit(spa, SCL_ALLOC, FTAG);
	mutex_enter(&engine.lock);
	if (error == 0) {
		for (engine_owner_t *e = avl_first(&engine.owners); e != NULL;
		    e = AVL_NEXT(&engine.owners, e)) {
			if (e->journal_pending) {
				error = EBUSY;
				break;
			}
		}
	}
	for (uint64_t i = 0; i < count; i++) {
		VERIFY3U(pending[i]->holds, >, 0);
		pending[i]->holds--;
	}
	mutex_exit(&engine.lock);
	kmem_free(pending, capacity * sizeof (*pending));
	if (trace)
		pr19262_printf("PR19262_J_CLAIM_END owners=%llu error=%d "
		    "sync_on=%d\n", (u_longlong_t)count, error,
		    spa->spa_sync_on);
	return (error);
}

boolean_t
pr19262_engine_journal_claimed(spa_t *spa, vdev_t *vd, uint64_t v,
    uint64_t size, uint64_t birth, uint64_t txg)
{
	boolean_t known = B_FALSE;
	mutex_enter(&engine.lock);
	if (engine.spa == spa && engine.persistent && vd == engine.vd) {
		engine_owner_t *e = engine_find(v);
		boolean_t identity = e != NULL && birth != 0 &&
		    e->allocation_txg == birth;
		/* A legacy V retains its original allocation birth. */
		if (e != NULL && e->allocation_txg == 0 && birth != 0) {
			for (engine_version_t *b = e->versions; b != NULL;
			    b = b->next) {
				if (b->p == e->v && b->size == e->size &&
				    b->birth == birth) {
					identity = B_TRUE;
					break;
				}
			}
		}
		/*
		 * The native not-free check below proves the V is still
		 * allocated.  The catalog only has to know this identity by
		 * the claim txg: restored, J-claimed, or still open.
		 */
		known = identity && e->size == size && !e->aborted &&
		    e->journal_touched && !e->journal_pending &&
		    e->catalog_txg <= txg;
	}
	mutex_exit(&engine.lock);
	return (known && metaslab_claim_impl(vd, v, size, 0) == ENOENT);
}
