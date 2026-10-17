/* SPDX-License-Identifier: CDDL-1.0 */
/* Private linked J segments, included by the same ownership engine. */
#define	ENGINE_J_MAGIC 0x31393236324a3034ULL
#define	ENGINE_J_BIND 1
#define	ENGINE_J_HEAD 2
#define	ENGINE_J_FORWARD 3
#define	ENGINE_J_WORDS (ENGINE_STORE_PAGE / sizeof (uint64_t))
#define	ENGINE_J_HASH (ENGINE_J_WORDS - 4)

static uint_t engine_journal_fault_stage;
static uint64_t engine_journal_fault_seq;
static uint_t engine_journal_roll_fault;
static uint_t engine_journal_roll_phase;

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
		(void) printf("PR19262_J_ROLL_CRASH stage=%u\n", stage);
		(void) fflush(NULL);
		_exit(80 + stage);
	}
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

static int
engine_journal_write(spa_t *spa, uint64_t page, abd_t *abd)
{
	int error = 0;
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
		    (engine_journal_roll_phase == 3 && roll >= 7));
		int e = (inject && (engine_journal_fault_stage == 1 ||
		    engine_journal_fault_stage == 4)) ||
		    (roll_inject && roll % 3 == 1) ? EIO :
		    engine_store_io_at(vd, engine.journal_arena, page,
		    1, abd, B_TRUE);
		if (e == 0)
			e = (inject && (engine_journal_fault_stage == 2 ||
			    engine_journal_fault_stage == 5)) ||
			    (roll_inject && roll % 3 == 2) ? EIO :
			    pr19262_engine_metadata_flush(vd);
		if (e != 0)
			error = e;
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	return (error);
}

static int
engine_journal_format(spa_t *spa)
{
	abd_t *head = engine_journal_page(ENGINE_J_HEAD, 0);
	engine_journal_seal(head);
	int error = engine_journal_write(spa, 0, head);
	if (error == 0)
		error = engine_journal_write(spa, 1, head);
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
	engine.journal_hash = s->hash;
	mutex_exit(&engine.lock);
}

static int
engine_journal_segment(uint64_t arena, const uint64_t epoch[2],
    boolean_t allocate, engine_j_segment_t **result)
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
		return (EINVAL);
	}
	engine_j_segment_t *s = kmem_zalloc(sizeof (*s), KM_SLEEP);
	s->arena = arena;
	memcpy(s->epoch, epoch, sizeof (s->epoch));
	zfs_range_tree_remove(engine.free, arena, bytes);
	zfs_range_tree_add(engine.journal_extents, arena, bytes);
	VERIFY3P(engine.journal_tail->next, ==, NULL);
	engine.journal_tail->next = s;
	engine.journal_tail = s;
	engine.revision++;
	*result = s;
	mutex_exit(&engine.lock);
	return (0);
}

static int
engine_journal_grow(spa_t *spa)
{
	ASSERT(MUTEX_HELD(&engine.journal_lock));
	engine_j_segment_t *old = engine.journal_tail;
	uint64_t arena = UINT64_MAX, epoch[2];
	VERIFY0(random_get_pseudo_bytes((void *)epoch, sizeof (epoch)));
	engine_j_segment_t *next;
	int error = engine_journal_segment(arena, epoch, B_TRUE, &next);
	if (error != 0)
		return (error);
	arena = next->arena;
	engine_journal_select(next);
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
	w[8] = epoch[0]; w[9] = epoch[1];
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
		page = engine_journal_page(ENGINE_J_HEAD, seq);
		memcpy((uint64_t *)abd_to_buf(page) + 20, &hash, sizeof (hash));
		engine_journal_seal(page);
		engine_journal_roll_phase = 3;
		error = engine_journal_write(spa, seq % 2, page);
		engine_journal_roll_phase = 0;
		abd_free(page);
	}
	if (error == 0) {
		engine_journal_roll_stop(9);
		old->seq = seq;
		old->hash = hash;
		engine_journal_select(next);
		(void) printf("PR19262_J_FORWARD from=%llu to=%llu seq=%llu\n",
		    (u_longlong_t)old->arena, (u_longlong_t)arena,
		    (u_longlong_t)seq);
	}
	/* Failed publication freezes J; both extents remain quarantined. */
	return (error);
}

int
pr19262_engine_journal_roll(spa_t *spa)
{
	mutex_enter(&engine.journal_lock);
	int error = engine.journal_error;
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
pr19262_engine_journal_ub_committed(spa_t *spa)
{
	mutex_enter(&engine.lock);
	if (engine.spa == spa && engine.persistent)
		engine.journal_ready = B_TRUE;
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
			if (selected[3] == w[3] && memcmp(selected, w,
			    ENGINE_STORE_PAGE) != 0) {
				error = EINVAL;
				break;
			}
			if (selected[3] >= w[3])
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
engine_journal_apply(uint64_t *w)
{
	ASSERT(MUTEX_HELD(&engine.lock));
	uint64_t v = w[7], size = w[8];
	if (v >= engine.total || size == 0 || size > engine.total - v ||
	    !IS_P2ALIGNED(v, ENGINE_STORE_PAGE) ||
	    !IS_P2ALIGNED(size, ENGINE_STORE_PAGE) || w[6] == 0 ||
	    w[13] <= w[14] || w[13] > engine.vd->vdev_children ||
	    w[14] == 0 || w[14] > 3 || w[17] > 1 || w[15] == 0 ||
	    w[29] == 0 || w[29] > w[15] || w[27] <= w[28] ||
	    w[27] > engine.vd->vdev_children || w[28] == 0 || w[28] > 3)
		return (EINVAL);
	engine_owner_t *e = engine_find(v);
	if (e == NULL) {
		boolean_t legacy = zfs_range_tree_contains(engine.legacy,
		    v, size);
		if (legacy && (w[9] != 0 || w[24] != v || w[25] != size))
			return (EINVAL);
		e = kmem_zalloc(sizeof (*e), KM_SLEEP);
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
			e->current = old;
		}
	} else if (e->incarnation != w[6] || e->size != size ||
	    e->allocation_txg != w[9]) {
		return (EINVAL);
	}
	e->journal_touched = B_TRUE;
	engine.next_incarnation = MAX(engine.next_incarnation, w[6]);
	engine.sequence = MAX(engine.sequence, MAX(w[15], w[16]));
	for (engine_version_t *b = e->versions; b != NULL; b = b->next) {
		if (b->generation != w[15])
			continue;
		if (b->p != w[10] || b->size != w[11] || b->birth != w[12] ||
		    b->width != w[13] || b->parity != w[14])
			return (EINVAL);
		b->journal_seq = w[3];
		b->ready |= w[17];
		if (b->generation > e->current->generation) {
			/* M may already quarantine a failed publication. */
			if (!b->retired)
				return (EINVAL);
			b->retired = B_FALSE;
			engine.retired -= b->size;
			e->current->retired = B_TRUE;
			if (!e->retired)
				engine.retired += e->current->size;
			e->current = b;
			if (e->retired)
				engine.retired += b->size;
		}
		return (0);
	}
	if (!engine_store_extent(w[10], w[11]))
		return (EINVAL);
	engine_version_t *b = engine_version_new(e, w[10], w[11], w[12], w[17]);
	b->width = w[13]; b->parity = w[14]; b->generation = w[15];
	b->journal_seq = w[3];
	zfs_range_tree_remove(engine.free, b->p, b->size);
	if (e->current == NULL || b->generation > e->current->generation) {
		if (e->current != NULL) {
			e->current->retired = B_TRUE;
			if (!e->retired)
				engine.retired += e->current->size;
		}
		e->current = b;
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
	uint64_t head = UINT64_MAX;
	zio_cksum_t expected = { 0 }, previous = { 0 };
	int error = 0;
	for (uint_t i = 0; i < 2; i++) {
		int e = engine_journal_read(spa, i, ENGINE_J_HEAD, UINT64_MAX,
		    NULL, page);
		if (e == EINVAL) {
			error = ECKSUM;
			break;
		}
		if (e != 0)
			continue;
		uint64_t *w = abd_to_buf(page);
		if (head == w[3] && memcmp(&expected, w + 20,
		    sizeof (expected)) != 0) {
			error = ECKSUM;
			break;
		}
		if (head == UINT64_MAX || w[3] > head) {
			head = w[3];
			memcpy(&expected, w + 20, sizeof (expected));
		}
	}
	uint64_t capacity = engine.journal_bytes / ENGINE_STORE_PAGE - 2;
	if (head == UINT64_MAX || head > capacity ||
	    (head == 0 && memcmp(&expected, &previous, sizeof (expected)) != 0))
		error = ECKSUM;
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
			error = EINVAL;
			break;
		}
		memcpy(&previous, w + ENGINE_J_HASH, sizeof (previous));
		if (seq == head && memcmp(&previous, &expected,
		    sizeof (previous)) != 0)
			error = ECKSUM;
		last = seq;
		if (error != 0)
			break;
		if (w[1] == ENGINE_J_FORWARD) {
			if (head > seq || w[7] != engine.journal_bytes ||
			    (w[8] == engine.journal_epoch[0] &&
			    w[9] == engine.journal_epoch[1])) {
				error = EINVAL;
				break;
			}
			uint64_t epoch[] = { w[8], w[9] };
			error = engine_journal_segment(w[6], epoch,
			    B_FALSE, next);
			break;
		}
		if (seq == capacity) {
			error = EINVAL;
			break;
		}
		mutex_enter(&engine.lock);
		error = engine_journal_apply(w);
		mutex_exit(&engine.lock);
	}
	if (error == 0) {
		mutex_enter(&engine.lock);
		error = engine_store_validate_virtual();
		engine.journal_seq = last;
		engine.journal_hash = previous;
		mutex_exit(&engine.lock);
		(void) printf("PR19262_J_RECOVER head=%llu prefix=%llu\n",
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
		mutex_exit(&engine.lock);
	}
	return (error);
}

static int
engine_journal_repair_segment(spa_t *spa)
{
	abd_t *page = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	zio_cksum_t previous = { 0 };
	int error = 0;
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
		page = engine_journal_page(ENGINE_J_HEAD, engine.journal_seq);
		memcpy((uint64_t *)abd_to_buf(page) + 20, &previous,
		    sizeof (previous));
		engine_journal_seal(page);
		error = engine_journal_write(spa, engine.journal_seq % 2, page);
		abd_free(page);
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
	return (error);
}

static int
engine_journal_bind_impl(spa_t *spa, engine_hold_t *h,
    engine_version_t *staging)
{
	if (h == NULL)
		return (0);
	int error = h->error;
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
		engine_version_t *b = staging != NULL ? staging : e->current;
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
			(void) printf("PR19262_J_CRASH_AFTER_BIND seq=%llu\n",
			    (u_longlong_t)seq);
			(void) fflush(NULL);
			_exit(78);
		}
		if (error == 0) {
			page = engine_journal_page(ENGINE_J_HEAD, seq);
			memcpy((uint64_t *)abd_to_buf(page) + 20, &hash,
			    sizeof (hash));
			engine_journal_seal(page);
			error = engine_journal_write(spa, seq % 2, page);
			abd_free(page);
		}
		if (error == 0 && seq == engine_journal_fault_seq &&
		    engine_journal_fault_stage == 6) {
			(void) printf("PR19262_J_CRASH_AFTER_HEAD seq=%llu\n",
			    (u_longlong_t)seq);
			(void) fflush(NULL);
			_exit(79);
		}
		mutex_enter(&engine.lock);
		b->leases--;
		if (error == 0) {
			engine.journal_seq = b->journal_seq = seq;
			engine.journal_hash = hash;
			engine.journal_tail->seq = seq;
			engine.journal_tail->hash = hash;
			(void) printf("PR19262_J_BIND seq=%llu v=%llu p=%llu\n",
			    (u_longlong_t)seq, (u_longlong_t)e->v,
			    (u_longlong_t)b->p);
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

int
pr19262_engine_journal_bind(spa_t *spa, const blkptr_t *bp)
{
	if (!pr19262_engine_persistent(spa))
		return (0);
	engine_hold_t *h = pr19262_engine_hold(spa, bp, NULL, B_FALSE, B_FALSE);
	int error = engine_journal_bind_hold(spa, h);
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
		return (ENOENT);
	if (h->error != 0) {
		int error = h->error;
		pr19262_engine_rele(h);
		return (error);
	}
	mutex_enter(&engine.lock);
	h->write = B_TRUE;
	for (uint_t i = 0; i < h->count; i++) {
		h->owner[i]->queued_writers++;
		h->owner[i]->journal_touched = B_TRUE;
	}
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
	int error = 0;
	if (BP_IS_GANG(&job->bp))
		error = pr19262_zio_walk_gang(job->spa, &job->bp, job->hold,
		    engine_journal_collect, job);
	if (error == 0)
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
		(void) printf("PR19262_J_GANG dependencies=%llu error=%d\n",
		    (u_longlong_t)dependencies, error);
	job->gate->io_error = error;
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
	mutex_enter(&engine.lock);
	if (job->hold != NULL)
		for (uint_t i = 0; i < job->hold->count; i++)
			job->hold->owner[i]->journal_touched = B_TRUE;
	mutex_exit(&engine.lock);
	job->gate = zio_null(parent, spa, NULL, NULL, NULL, ZIO_FLAG_CANFAIL);
	VERIFY3U(taskq_dispatch(system_taskq, engine_journal_worker, job,
	    TQ_SLEEP), !=, TASKQID_INVALID);
}

boolean_t
pr19262_engine_journal_unalloc(spa_t *spa, vdev_t *vd, uint64_t v,
    uint64_t size)
{
	boolean_t held = B_FALSE;
	mutex_enter(&engine.lock);
	if (engine.spa == spa && engine.persistent && vd == engine.vd) {
		engine_owner_t *e = engine_find(v);
		if (e != NULL && e->journal_touched) {
			VERIFY3U(e->size, ==, size);
			if (!e->retired) {
				e->retired = B_TRUE;
				e->retired_at = ++engine.sequence;
				engine.retired += e->current->size;
				engine.revision++;
			}
			held = B_TRUE;
		}
	}
	mutex_exit(&engine.lock);
	return (held);
}

int
pr19262_engine_journal_claim(spa_t *spa, uint64_t txg)
{
	if (!pr19262_engine_persistent(spa))
		return (0);
	int error = 0;
	mutex_enter(&engine.lock);
	uint64_t capacity = avl_numnodes(&engine.owners), count = 0;
	engine_owner_t **pending = kmem_alloc(capacity * sizeof (*pending),
	    KM_SLEEP);
	for (engine_owner_t *e = avl_first(&engine.owners); e != NULL;
	    e = AVL_NEXT(&engine.owners, e)) {
		if (!e->journal_pending)
			continue;
		e->holds++;
		pending[count++] = e;
	}
	mutex_exit(&engine.lock);
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
		mutex_enter(&engine.lock);
		e->journal_pending = B_FALSE;
		e->claimed_txg = e->catalog_txg = txg;
		engine.revision++;
		mutex_exit(&engine.lock);
		spa->spa_claim_max_txg = MAX(spa->spa_claim_max_txg,
		    e->allocation_txg);
	}
	spa_config_exit(spa, SCL_ALLOC, FTAG);
	mutex_enter(&engine.lock);
	for (uint64_t i = 0; i < count; i++) {
		VERIFY3U(pending[i]->holds, >, 0);
		pending[i]->holds--;
	}
	mutex_exit(&engine.lock);
	kmem_free(pending, capacity * sizeof (*pending));
	return (error);
}

boolean_t
pr19262_engine_journal_claimed(spa_t *spa, vdev_t *vd, uint64_t v,
    uint64_t size, uint64_t txg)
{
	boolean_t known = B_FALSE;
	mutex_enter(&engine.lock);
	if (engine.spa == spa && engine.persistent && vd == engine.vd) {
		engine_owner_t *e = engine_find(v);
		known = e != NULL && e->size == size && e->journal_touched &&
		    !e->journal_pending && (e->claimed_txg == txg ||
		    e->catalog_txg <= spa_last_synced_txg(spa));
	}
	mutex_exit(&engine.lock);
	return (known && metaslab_claim_impl(vd, v, size, 0) == ENOENT);
}
