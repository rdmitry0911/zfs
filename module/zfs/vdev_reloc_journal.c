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

/* BIND, HEAD and FORWARD I/O; roll time includes its nested I/O. */

/* Test hook: a group of frames is cut after the first half landed. */

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
pr19262_engine_journal_roll_fault(spa_t *spa, uint_t stage)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->journal_lock);
	engine->journal_roll_fault = stage;
	mutex_exit(&engine->journal_lock);
}

static void
engine_journal_roll_stop(engine_t *engine, uint_t stage)
{
	if (engine->journal_roll_fault == stage) {
		pr19262_printf("PR19262_J_ROLL_CRASH stage=%u\n", stage);
		pr19262_flush();
		pr19262_exit(80 + stage);
	}
}

void
pr19262_engine_journal_group_fault(spa_t *spa, uint_t stage)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->journal_lock);
	engine->journal_group_fault = stage;
	mutex_exit(&engine->journal_lock);
}

void
pr19262_engine_journal_fault(spa_t *spa, uint_t stage)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->journal_lock);
	engine->journal_fault_stage = stage % 10;
	engine->journal_fault_seq = engine->journal_seq + (stage >= 10 ? 2 : 1);
	mutex_exit(&engine->journal_lock);
}

int
pr19262_engine_journal_wait_error(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	while (engine->journal_error == 0)
		cv_wait(&engine->changed, &engine->lock);
	int error = engine->journal_error;
	mutex_exit(&engine->lock);
	return (error);
}

static void
engine_journal_seal(engine_t *engine, abd_t *abd)
{
	hrtime_t start = gethrtime();
	uint64_t *w = abd_to_buf(abd);
	memset(w + ENGINE_J_HASH, 0, sizeof (zio_cksum_t));
	zio_cksum_t hash;
	abd_checksum_sha256(abd, ENGINE_STORE_PAGE, NULL, &hash);
	memcpy(w + ENGINE_J_HASH, &hash, sizeof (hash));
	atomic_add_64(&engine->journal_cost.seal, gethrtime() - start);
}

static boolean_t
engine_journal_valid(engine_t *engine, abd_t *abd, uint64_t kind, uint64_t seq)
{
	uint64_t *w = abd_to_buf(abd);
	if (w[0] != ENGINE_J_MAGIC ||
	    (kind != UINT64_MAX && w[1] != kind) ||
	    (w[1] != ENGINE_J_BIND && w[1] != ENGINE_J_HEAD &&
	    w[1] != ENGINE_J_FORWARD) ||
	    w[2] != engine->journal_epoch[0] || w[18] !=
	    engine->journal_epoch[1] || w[4] !=
	    spa_guid(engine->vd->vdev_spa) ||
	    w[5] != engine->vd->vdev_guid || (seq != UINT64_MAX && w[3] != seq))
		return (B_FALSE);
	zio_cksum_t saved, hash;
	memcpy(&saved, w + ENGINE_J_HASH, sizeof (saved));
	memset(w + ENGINE_J_HASH, 0, sizeof (saved));
	abd_checksum_sha256(abd, ENGINE_STORE_PAGE, NULL, &hash);
	memcpy(w + ENGINE_J_HASH, &saved, sizeof (saved));
	return (memcmp(&saved, &hash, sizeof (hash)) == 0);
}

/*
 * R02's manifest observer uses its own validator instead of the recovery
 * helper above.  In particular, the epoch comes from the immutable root
 * snapshot and every member read goes through engine_store_manifest_read(),
 * which deliberately has no error accounting side effect.
 */
static boolean_t
engine_r02_journal_valid(engine_t *engine, abd_t *abd,
    const uint64_t epoch[2], uint64_t kind, uint64_t seq)
{
	uint64_t *w = abd_to_buf(abd);
	if (w[0] != ENGINE_J_MAGIC ||
	    (kind != UINT64_MAX && w[1] != kind) ||
	    (w[1] != ENGINE_J_BIND && w[1] != ENGINE_J_HEAD &&
	    w[1] != ENGINE_J_FORWARD) || w[2] != epoch[0] ||
	    w[18] != epoch[1] || w[4] != spa_guid(engine->vd->vdev_spa) ||
	    w[5] != engine->vd->vdev_guid ||
	    (seq != UINT64_MAX && w[3] != seq))
		return (B_FALSE);
	zio_cksum_t saved, hash;
	memcpy(&saved, w + ENGINE_J_HASH, sizeof (saved));
	memset(w + ENGINE_J_HASH, 0, sizeof (saved));
	abd_checksum_sha256(abd, ENGINE_STORE_PAGE, NULL, &hash);
	memcpy(w + ENGINE_J_HASH, &saved, sizeof (saved));
	return (memcmp(&saved, &hash, sizeof (hash)) == 0);
}

/*
 * Select one logically consistent journal image without changing journal or
 * engine state.  The caller already owns SCL_ZIO, so this helper must not
 * enter it recursively.
 */
static int
engine_r02_journal_read(engine_r02_manifest_private_t *m,
    uint64_t arena, const uint64_t epoch[2], uint64_t page, uint64_t kind,
    uint64_t seq, const zio_cksum_t *previous, abd_t *out)
{
	engine_t *engine = m->spa->spa_reloc;
	int error = ECKSUM;
	abd_t *candidate = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	for (uint64_t child = 0; child < engine->vd->vdev_children; child++) {
		if (engine_store_manifest_read(engine->vd->vdev_child[child],
		    arena, page, candidate, m->mode) != 0 ||
		    !engine_r02_journal_valid(engine, candidate, epoch, kind, seq))
			continue;
		uint64_t *w = abd_to_buf(candidate);
		if (previous != NULL &&
		    memcmp(w + 20, previous, sizeof (*previous)) != 0)
			continue;
		if (error == 0) {
			uint64_t *selected = abd_to_buf(out);
			uint_t key = kind == ENGINE_J_HEAD ? 30 : 3;
			if (selected[key] == w[key] &&
			    memcmp(selected, w, ENGINE_STORE_PAGE) != 0) {
				error = ENGINE_EINVAL(101);
				break;
			}
			if (selected[key] >= w[key])
				continue;
		}
		abd_copy(out, candidate, ENGINE_STORE_PAGE);
		error = 0;
	}
	abd_free(candidate);
	return (error);
}

/* Read the two redundant HEAD slots and record their selected logical pages. */
static int
engine_r02_journal_head(engine_r02_manifest_private_t *m, uint64_t arena,
    const uint64_t epoch[2], abd_t *selected)
{
	engine_t *engine = m->spa->spa_reloc;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	abd_t *page = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	int error = ECKSUM;
	boolean_t conflict = B_FALSE;
	for (uint_t slot = 0; slot < 2; slot++) {
		int e = engine_r02_journal_read(m, arena, epoch, slot,
		    ENGINE_J_HEAD, UINT64_MAX, NULL, page);
		if (e == ECKSUM)
			continue;
		if (e != 0) {
			error = e;
			break;
		}
		uint64_t *w = abd_to_buf(page);
		zio_cksum_t hash;
		memcpy(&hash, w + ENGINE_J_HASH, sizeof (hash));
		e = engine_r02_entry_add(m, PR19262_R02_MANIFEST_JOURNAL_HEAD,
		    arena + slot * stripe, 1, w[30], &hash);
		if (e != 0) {
			error = e;
			break;
		}
		m->journal_heads++;
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
	abd_free(page);
	if (error == 0) {
		uint64_t *w = abd_to_buf(selected);
		if (conflict || w[30] == 0 || w[31] > ENGINE_J_ACTIVE)
			error = ENGINE_EINVAL(102);
	}
	return (error);
}

/*
 * Validate one journal extent against the immutable manifest geometry and
 * the live journal reservation.  The caller owns journal_lock, which keeps
 * the reservation stable; this helper takes engine->lock only to copy and
 * inspect it.  It never changes a range tree.
 */
static int
engine_r02_journal_extent(engine_r02_manifest_private_t *m,
    uint64_t arena, uint64_t bytes, uint64_t *sizep)
{
	engine_t *engine = m->spa->spa_reloc;
	int error = 0;
	mutex_enter(&engine->lock);
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	if (engine->vd->vdev_children == 0 || stripe == 0 ||
	    bytes < 3 * ENGINE_STORE_PAGE || bytes % ENGINE_STORE_PAGE != 0 ||
	    bytes > engine->total / engine->vd->vdev_children ||
	    arena % stripe != 0 || arena >= engine->total) {
		error = ENGINE_EINVAL(103);
	} else {
		uint64_t size = bytes * engine->vd->vdev_children;
		uint64_t msize = engine->arena_bytes *
		    engine->vd->vdev_children;
		if (engine->arena > engine->total ||
		    msize > engine->total - engine->arena ||
		    size > engine->total - arena ||
		    (arena < engine->arena + msize &&
		    engine->arena < arena + size) ||
		    !zfs_range_tree_contains(engine->journal_extents, arena, size))
			error = ENGINE_EINVAL(104);
		else
			*sizep = size;
	}
	mutex_exit(&engine->lock);
	return (error);
}

/* Reserve an already validated journal extent in the private closure. */
static int
engine_r02_journal_reserve(engine_r02_manifest_private_t *m,
    uint64_t arena, uint64_t size)
{
	uint64_t found, span;
	if (zfs_range_tree_find_in(m->all, arena, size, &found, &span) ||
	    zfs_range_tree_find_in(m->journal, arena, size, &found, &span))
		return (ENGINE_EINVAL(105));
	zfs_range_tree_add(m->journal, arena, size);
	return (0);
}

/* Decode and validate the target named by a PREPARE or ACTIVE head. */
static int
engine_r02_journal_target(engine_r02_manifest_private_t *m,
    const uint64_t current_epoch[2], uint64_t bytes, const uint64_t *header,
    uint64_t *arena, uint64_t epoch[2], uint64_t *size)
{
	*arena = header[32];
	epoch[0] = header[34];
	epoch[1] = header[35];
	if (header[33] != bytes ||
	    (epoch[0] == current_epoch[0] && epoch[1] == current_epoch[1]))
		return (ENGINE_EINVAL(106));
	return (engine_r02_journal_extent(m, *arena, header[33], size));
}

/*
 * Read the durable journal chain without applying records or formatting any
 * arena.  A PREPARE with no FORWARD is a reservation only; it is emitted as
 * JOURNAL_PENDING and is deliberately not read as a successor.  An ACTIVE
 * head must name a matching FORWARD at its durable head.  Every successor is
 * range-reserved before it is read, so cycles and overlap fail closed.
 */
static int
engine_r02_journal_manifest(engine_r02_manifest_private_t *m)
{
	engine_t *engine = m->spa->spa_reloc;
	uint64_t arena, epoch[2], bytes;
	int error = 0;
	mutex_enter(&engine->lock);
	arena = engine->journal_root;
	memcpy(epoch, engine->journal_root_epoch, sizeof (epoch));
	bytes = engine->journal_bytes;
	mutex_exit(&engine->lock);

	abd_t *page = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	while (error == 0) {
		uint64_t size;
		error = engine_r02_journal_extent(m, arena, bytes, &size);
		if (error == 0)
			error = engine_r02_journal_reserve(m, arena, size);
		if (error != 0)
			break;
		error = engine_r02_journal_head(m, arena, epoch, page);
		if (error != 0)
			break;
		uint64_t *header = abd_to_buf(page);
		uint64_t head = header[3], phase = header[31];
		uint64_t capacity = bytes / ENGINE_STORE_PAGE - 2;
		uint64_t target = 0, target_epoch[2] = { 0 }, target_size = 0;
		boolean_t forward = B_FALSE;
		zio_cksum_t expected, previous = { 0 };
		memcpy(&expected, header + 20, sizeof (expected));
		if (head > capacity || (head == 0 &&
		    memcmp(&expected, &previous, sizeof (expected)) != 0))
			error = ECKSUM;
		if (error == 0 && phase == ENGINE_J_NORMAL &&
		    (header[32] != 0 || header[33] != 0 ||
		    header[34] != 0 || header[35] != 0))
			error = ENGINE_EINVAL(107);
		if (error == 0 && phase != ENGINE_J_NORMAL)
			error = engine_r02_journal_target(m, epoch, bytes, header,
		    &target, target_epoch, &target_size);
		for (uint64_t seq = 1; error == 0 && seq <= capacity; seq++) {
			int e = engine_r02_journal_read(m, arena, epoch, seq + 1,
			    UINT64_MAX, seq, &previous, page);
			if (e != 0) {
				if (e == ECKSUM && seq > head)
					break;
				error = e;
				break;
			}
			uint64_t *w = abd_to_buf(page);
			if (w[1] == ENGINE_J_FORWARD && seq > head &&
			    phase != ENGINE_J_PREPARE)
				break;
			zio_cksum_t hash;
			memcpy(&hash, w + ENGINE_J_HASH, sizeof (hash));
			memcpy(&previous, &hash, sizeof (previous));
			if (seq == head && memcmp(&previous, &expected,
			    sizeof (previous)) != 0) {
				error = ECKSUM;
				break;
			}
			if (w[1] == ENGINE_J_FORWARD) {
				if (head > seq || phase == ENGINE_J_NORMAL ||
				    w[6] != target || w[7] != bytes ||
				    w[8] != target_epoch[0] ||
				    w[9] != target_epoch[1] ||
				    (phase == ENGINE_J_ACTIVE && seq != head)) {
					error = ENGINE_EINVAL(108);
					break;
				}
				error = engine_r02_entry_add(m,
				    PR19262_R02_MANIFEST_JOURNAL_FORWARD,
				    arena + (seq + 1) *
				    ENGINE_STORE_PAGE * engine->vd->vdev_children,
				    1, seq, &hash);
				if (error == 0)
					m->journal_forwards++;
				forward = error == 0;
				break;
			}
			if (w[1] != ENGINE_J_BIND) {
				error = ENGINE_EINVAL(109);
				break;
			}
			error = engine_r02_entry_add(m,
			    PR19262_R02_MANIFEST_JOURNAL_BIND,
			    arena + (seq + 1) *
			    ENGINE_STORE_PAGE * engine->vd->vdev_children,
			    1, seq, &hash);
			if (error != 0)
				break;
			m->journal_binds++;
			if (seq == capacity)
				error = ENGINE_EINVAL(110);
		}
		if (error != 0)
			break;
		if (phase == ENGINE_J_ACTIVE && !forward) {
			error = ECKSUM;
			break;
		}
		if (phase == ENGINE_J_PREPARE && !forward) {
			error = engine_r02_journal_reserve(m, target, target_size);
			if (error == 0)
				error = engine_r02_entry_add(m,
				    PR19262_R02_MANIFEST_JOURNAL_PENDING, target,
				    bytes / ENGINE_STORE_PAGE, header[30], NULL);
			if (error == 0)
				m->journal_pending++;
			break;
		}
		if (!forward)
			break;
		arena = target;
		memcpy(epoch, target_epoch, sizeof (epoch));
	}
	abd_free(page);
	return (error);
}

static abd_t *
engine_journal_page(engine_t *engine, uint64_t kind, uint64_t seq)
{
	abd_t *abd = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	abd_zero(abd, ENGINE_STORE_PAGE);
	uint64_t *w = abd_to_buf(abd);
	w[0] = ENGINE_J_MAGIC; w[1] = kind;
	w[2] = engine->journal_epoch[0]; w[3] = seq;
	w[4] = spa_guid(engine->vd->vdev_spa);
	w[5] = engine->vd->vdev_guid;
	w[18] = engine->journal_epoch[1];
	return (abd);
}

/* Write consecutive frames to every member at once, then flush each. */
static int
engine_journal_write_pages(spa_t *spa, const engine_member_page_t *pv,
    uint_t n)
{
	engine_t *engine = spa->spa_reloc;
	engine_quorum_t q = { 0 };
	uint_t children = engine->vd->vdev_children;
	int *errors = kmem_alloc(children * sizeof (int), KM_SLEEP);
	engine_store_cost_t cost = { 0 };
	uint64_t kind = n == 0 ? 0 : ((uint64_t *)abd_to_buf(pv[0].abd))[1];
	uint_t at = kind == ENGINE_J_BIND ? 0 :
	    (kind == ENGINE_J_HEAD ? 1 : 2);
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	engine_members_write(spa, pv, n, B_TRUE, B_FALSE, errors, &cost);
	spa_config_exit(spa, SCL_ZIO, FTAG);
	for (uint_t c = 0; c < children; c++)
		engine_quorum_add(engine, &q, errors[c]);
	kmem_free(errors, children * sizeof (int));
	atomic_inc_64(&engine->journal_cost.calls[at]);
	atomic_add_64(&engine->journal_cost.pages[at], n);
	atomic_add_64(&engine->journal_cost.write[at], cost.write);
	atomic_add_64(&engine->journal_cost.flush[at], cost.flush);
	return (engine_quorum_result(engine, &q));
}

static int
engine_journal_write(spa_t *spa, uint64_t page, abd_t *abd)
{
	engine_t *engine = spa->spa_reloc;
	engine_quorum_t q = { 0 };
	/*
	 * Without a crash or fault hook armed, every member writes and then
	 * flushes at once.  The hooks act on the first member alone, so they
	 * keep the member-by-member order they were written for.
	 */
	if (engine->journal_fault_stage == 0 && engine->journal_roll_fault ==
	    0) {
		engine_member_page_t pg = { engine->journal_arena, page, abd };
		return (engine_journal_write_pages(spa, &pg, 1));
	}
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	for (uint64_t i = 0; i < engine->vd->vdev_children; i++) {
		vdev_t *vd = engine->vd->vdev_child[i];
		uint64_t *w = abd_to_buf(abd);
		uint64_t fault_kind = engine->journal_fault_stage >= 4 ?
		    ENGINE_J_HEAD : ENGINE_J_BIND;
		boolean_t inject = i == 0 && w[1] == fault_kind &&
		    w[3] == engine->journal_fault_seq;
		uint_t roll = engine->journal_roll_fault;
		boolean_t roll_inject = i == 0 &&
		    ((engine->journal_roll_phase == 1 && roll <= 3) ||
		    (engine->journal_roll_phase == 2 &&
		    roll >= 4 && roll <= 6) ||
		    (engine->journal_roll_phase == 3 &&
		    roll >= 7 && roll <= 9) ||
		    (engine->journal_roll_phase == 4 &&
		    roll >= 11 && roll <= 12));
		int e = (inject && (engine->journal_fault_stage == 1 ||
		    engine->journal_fault_stage == 4)) ||
		    (roll_inject && (roll % 3 == 1 || roll == 11)) ? EIO :
		    engine_store_io_at(vd, engine->journal_arena, page,
		    1, abd, B_TRUE);
		if (e == 0)
			e = (inject && (engine->journal_fault_stage == 2 ||
			    engine->journal_fault_stage == 5)) ||
			    (roll_inject &&
			    (roll % 3 == 2 || roll == 12)) ? EIO :
			    engine_store_flush(vd);
		engine_quorum_add(engine, &q, e);
		if (e == 0 && i == 0 && engine->journal_roll_phase == 2 &&
		    roll == 10) {
			pr19262_printf("PR19262_J_SINGLE_FORWARD root=%llu "
			    "next=%llu seq=%llu\n",
			    (u_longlong_t)engine->journal_arena,
			    (u_longlong_t)w[6], (u_longlong_t)w[3]);
			pr19262_flush();
			pr19262_exit(90);
		}
		if (e == 0 && i == 0 &&
		    ((roll == 14 && engine->journal_roll_phase == 4) ||
		    (roll == 15 && engine->journal_roll_phase == 3) ||
		    (roll == 16 && engine->journal_roll_phase == 1))) {
			pr19262_printf("PR19262_J_MEMBER_CUT stage=%u "
			    "root=%llu next=%llu seq=%llu\n", roll,
			    (u_longlong_t)engine->journal_root,
			    (u_longlong_t)(engine->journal_pending != NULL ?
			    engine->journal_pending->arena : 0),
			    (u_longlong_t)w[3]);
			pr19262_flush();
			pr19262_exit(80 + roll);
		}
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	return (engine_quorum_result(engine, &q));
}

static int
engine_journal_head(spa_t *spa, uint64_t seq, const zio_cksum_t *hash,
    uint64_t phase, engine_j_segment_t *target)
{
	engine_t *engine = spa->spa_reloc;
	if (engine->journal_serial == UINT64_MAX)
		return (EOVERFLOW);
	abd_t *page = engine_journal_page(engine, ENGINE_J_HEAD, seq);
	uint64_t *w = abd_to_buf(page);
	memcpy(w + 20, hash, sizeof (*hash));
	w[30] = engine->journal_serial + 1;
	w[31] = phase;
	if (target != NULL) {
		w[32] = target->arena; w[33] = engine->journal_bytes;
		w[34] = target->epoch[0]; w[35] = target->epoch[1];
	}
	engine_journal_seal(engine, page);
	int error = engine_journal_write(spa, w[30] % 2, page);
	if (error == 0) {
		mutex_enter(&engine->lock);
		engine_j_segment_t *s = engine->journal_current;
		engine->journal_serial = s->serial = w[30];
		engine->journal_seq = s->seq = seq;
		engine->journal_hash = s->hash = *hash;
		s->phase = phase;
		s->target = w[32];
		s->target_epoch[0] = w[34];
		s->target_epoch[1] = w[35];
		mutex_exit(&engine->lock);
	}
	abd_free(page);
	return (error);
}

static int
engine_journal_format(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	abd_t *head = engine_journal_page(engine, ENGINE_J_HEAD, 0);
	((uint64_t *)abd_to_buf(head))[30] = 1;
	engine_journal_seal(engine, head);
	int error = engine_journal_write(spa, 0, head);
	if (error == 0)
		error = engine_journal_write(spa, 1, head);
	if (error == 0) {
		mutex_enter(&engine->lock);
		engine->journal_serial = engine->journal_current->serial = 1;
		mutex_exit(&engine->lock);
	}
	abd_free(head);
	return (error);
}

static void
engine_journal_select(engine_t *engine, engine_j_segment_t *s)
{
	mutex_enter(&engine->lock);
	engine->journal_arena = s->arena;
	memcpy(engine->journal_epoch, s->epoch, sizeof (s->epoch));
	engine->journal_seq = s->seq;
	engine->journal_serial = s->serial;
	engine->journal_hash = s->hash;
	engine->journal_current = s;
	mutex_exit(&engine->lock);
}

static int
engine_journal_segment(engine_t *engine, uint64_t arena,
    const uint64_t epoch[2],
    boolean_t allocate, boolean_t link, engine_j_segment_t **result)
{
	mutex_enter(&engine->lock);
	if (allocate) {
		engine_store_pick_arg_t pick = {
			engine->vd->vdev_children * ENGINE_STORE_PAGE,
			engine->journal_bytes * engine->vd->vdev_children,
			UINT64_MAX };
		zfs_range_tree_walk(engine->free, engine_store_pick, &pick);
		boolean_t released = B_FALSE;
		if (pick.found == UINT64_MAX &&
		    engine_pages_standby_release_locked(engine)) {
			/* A journal roll outranks the store's standby run. */
			released = B_TRUE;
			pick.found = UINT64_MAX;
			zfs_range_tree_walk(engine->free, engine_store_pick,
			    &pick);
		}
		arena = pick.found;
		if (released && arena != UINT64_MAX) {
			/* Headroom is low from now, not from the next fill. */
			uint64_t bytes = engine->journal_bytes *
			    engine->vd->vdev_children;
			zfs_range_tree_remove(engine->free, arena, bytes);
			engine->store_headroom_low =
			    engine_pages_runs_locked(engine) == 0;
			zfs_range_tree_add(engine->free, arena, bytes);
		}
		if (arena == UINT64_MAX) {
			mutex_exit(&engine->lock);
			return (ENOSPC);
		}
	}
	uint64_t bytes = engine->journal_bytes * engine->vd->vdev_children;
	if (arena % (ENGINE_STORE_PAGE * engine->vd->vdev_children) != 0 ||
	    !engine_store_extent(engine, arena, bytes)) {
		mutex_exit(&engine->lock);
		return (ENGINE_EINVAL(1));
	}
	engine_j_segment_t *s = kmem_zalloc(sizeof (*s), KM_SLEEP);
	s->arena = arena;
	memcpy(s->epoch, epoch, sizeof (s->epoch));
	s->index = ++engine->journal_segment_count;
	s->free_after = UINT64_MAX;
	zfs_range_tree_remove(engine->free, arena, bytes);
	zfs_range_tree_add(engine->journal_extents, arena, bytes);
	if (link) {
		VERIFY3P(engine->journal_tail->next, ==, NULL);
		engine->journal_tail->next = s;
		engine->journal_tail = s;
	} else {
		VERIFY3P(engine->journal_pending, ==, NULL);
		engine->journal_pending = s;
	}
	engine->revision++;
	*result = s;
	mutex_exit(&engine->lock);
	return (0);
}

static int
engine_journal_finish_prepare(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->journal_lock));
	engine_j_segment_t *old = engine->journal_tail;
	engine_j_segment_t *next = engine->journal_pending;
	VERIFY3P(next, !=, NULL);
	uint64_t arena = next->arena;
	engine_journal_select(engine, next);
	int error = engine_journal_empty(spa, B_TRUE);
	if (error != 0) {
		engine_journal_select(engine, old);
		return (error);
	}
	engine->journal_roll_phase = 1;
	error = engine_journal_format(spa);
	engine->journal_roll_phase = 0;
	engine_journal_select(engine, old);
	if (error != 0)
		return (error);
	engine_journal_roll_stop(engine, 3);
	uint64_t seq = old->seq + 1;
	abd_t *page = engine_journal_page(engine, ENGINE_J_FORWARD, seq);
	uint64_t *w = abd_to_buf(page);
	w[6] = arena; w[7] = engine->journal_bytes;
	w[8] = next->epoch[0]; w[9] = next->epoch[1];
	memcpy(w + 20, &old->hash, sizeof (old->hash));
	engine_journal_seal(engine, page);
	zio_cksum_t hash;
	memcpy(&hash, w + ENGINE_J_HASH, sizeof (hash));
	engine->journal_roll_phase = 2;
	error = engine_journal_write(spa, seq + 1, page);
	engine->journal_roll_phase = 0;
	abd_free(page);
	if (error == 0)
		engine_journal_roll_stop(engine, 6);
	if (error == 0) {
		engine->journal_roll_phase = 3;
		error = engine_journal_head(spa, seq, &hash,
		    ENGINE_J_ACTIVE, next);
		engine->journal_roll_phase = 0;
	}
	if (error == 0) {
		engine_journal_roll_stop(engine, 9);
		mutex_enter(&engine->lock);
		old->next = next;
		engine->journal_tail = next;
		engine->journal_pending = NULL;
		mutex_exit(&engine->lock);
		engine_journal_select(engine, next);
		pr19262_printf("PR19262_J_FORWARD from=%llu to=%llu seq=%llu\n",
		    (u_longlong_t)old->arena, (u_longlong_t)arena,
		    (u_longlong_t)seq);
	}
	/* Failed publication freezes J; both extents remain quarantined. */
	return (error);
}

/*
 * Roll to a new segment.  A roll that finds no run in free or the standby
 * fails in engine_journal_segment() before any write: no segment, head,
 * sequence or pending record moved, and the bind that needed the roll has
 * neither written its frame nor taken a lease.  That ENOSPC is retryable
 * (*retry): the caller does not store it in journal_error, the roll stays
 * pending, and the next roll that may wait (a bind) is tried only once a txg
 * later than the failed one has synced; deferred runs return at uberblock
 * commit, before spa_ubsync advances.  Every other failure stays sticky.
 */
static int
engine_journal_grow(spa_t *spa, boolean_t may_wait, boolean_t *retry)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->journal_lock));
	*retry = B_FALSE;
	uint64_t synced = spa_last_synced_txg(spa);
	mutex_enter(&engine->lock);
	if (may_wait && engine->journal_roll_pending &&
	    synced <= engine->journal_roll_txg) {
		engine->journal_roll_waits++;
		mutex_exit(&engine->lock);
		*retry = B_TRUE;
		return (ENOSPC);
	}
	mutex_exit(&engine->lock);
	hrtime_t start = gethrtime();
	atomic_inc_64(&engine->journal_cost.rolls);
	uint64_t arena = UINT64_MAX, epoch[2];
	VERIFY0(random_get_pseudo_bytes((void *)epoch, sizeof (epoch)));
	engine_j_segment_t *next;
	int error = engine_journal_segment(engine, arena, epoch,
	    B_TRUE, B_FALSE, &next);
	if (error != 0) {
		atomic_add_64(&engine->journal_cost.roll, gethrtime() - start);
		if (error != ENOSPC)
			return (error);
		mutex_enter(&engine->lock);
		ASSERT3P(engine->journal_pending, ==, NULL);
		boolean_t first = !engine->journal_roll_pending;
		engine->journal_roll_pending = B_TRUE;
		engine->journal_roll_txg = synced;
		uint64_t failures = ++engine->journal_roll_enospc;
		uint64_t deferred = engine_reuse_bytes(engine);
		mutex_exit(&engine->lock);
		if (first)
			pr19262_printf("PR19262_J_ROLL_ENOSPC seq=%llu "
			    "synced=%llu failures=%llu deferred=%llu\n",
			    (u_longlong_t)engine->journal_seq,
			    (u_longlong_t)synced, (u_longlong_t)failures,
			    (u_longlong_t)deferred);
		*retry = B_TRUE;
		return (error);
	}
	engine->journal_roll_phase = 4;
	error = engine_journal_head(spa, engine->journal_seq,
	    &engine->journal_hash, ENGINE_J_PREPARE, next);
	engine->journal_roll_phase = 0;
	if (error == 0) {
		engine_journal_roll_stop(engine, 13);
		error = engine_journal_finish_prepare(spa);
	}
	atomic_add_64(&engine->journal_cost.roll, gethrtime() - start);
	if (error == 0) {
		mutex_enter(&engine->lock);
		boolean_t was = engine->journal_roll_pending;
		engine->journal_roll_pending = B_FALSE;
		if (was)
			engine->journal_roll_recovered++;
		uint64_t failures = engine->journal_roll_enospc;
		uint64_t waits = engine->journal_roll_waits;
		mutex_exit(&engine->lock);
		if (was)
			pr19262_printf("PR19262_J_ROLL_RECOVERED synced=%llu "
			    "failures=%llu waits=%llu\n", (u_longlong_t)synced,
			    (u_longlong_t)failures, (u_longlong_t)waits);
	}
	return (error);
}

/* A roll is pending and the segment has no frame left before it. */
static boolean_t
engine_journal_roll_needed_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t frames = engine->journal_bytes / ENGINE_STORE_PAGE;
	return (engine->journal_roll_pending &&
	    engine->journal_seq + 3 >= frames);
}

/*
 * Before a copy is admitted or staged: retry a pending roll the segment
 * needs once it is due, so that a converter that is the journal's only user
 * still retries it.  0: no roll is needed (or the retry rolled); EBUSY:
 * still pending, the copy waits as busy instead of staging one it could not
 * bind; else the sticky error.
 */
static int
engine_journal_roll_wait(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	boolean_t pending = engine->spa == spa && engine->persistent &&
	    engine->journal_error == 0 &&
	    engine_journal_roll_needed_locked(engine);
	mutex_exit(&engine->lock);
	if (!pending)
		return (0);
	boolean_t retry = B_FALSE;
	mutex_enter(&engine->journal_lock);
	mutex_enter(&engine->lock);
	int error = engine->journal_error;
	pending = engine_journal_roll_needed_locked(engine);
	mutex_exit(&engine->lock);
	/* A pending roll is only set after any repair has run. */
	if (error == 0 && pending && !engine->journal_repair)
		error = engine_journal_grow(spa, B_TRUE, &retry);
	mutex_enter(&engine->lock);
	if (error != 0 && !(retry && error == ENOSPC))
		engine->journal_error = error;
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
	mutex_exit(&engine->journal_lock);
	return (retry ? EBUSY : error);
}

/* A roll is pending: a bind's ENOSPC is a wait, not a failure. */
static boolean_t
engine_journal_roll_pending(engine_t *engine)
{
	mutex_enter(&engine->lock);
	boolean_t pending = engine->journal_roll_pending;
	mutex_exit(&engine->lock);
	return (pending);
}

int
pr19262_engine_journal_roll(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->journal_lock);
	int error = engine->journal_error;
	boolean_t retry = B_FALSE;
	if (error == 0 && engine->journal_repair) {
		error = engine_journal_repair(spa);
		engine->journal_repair = error != 0;
	}
	if (error == 0)
		error = engine_journal_grow(spa, B_FALSE, &retry);
	mutex_enter(&engine->lock);
	if (error != 0 && !(retry && error == ENOSPC))
		engine->journal_error = error;
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
	mutex_exit(&engine->journal_lock);
	return (error);
}

void
pr19262_engine_journal_ub_committed(spa_t *spa, const uberblock_t *ub)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->spa == spa && engine->persistent) {
		engine->journal_ready = B_TRUE;
		if (pr19262_engine_store_ub(ub)) {
			engine->durable_ub_txg = ub->ub_txg;
			engine->durable_catalog_txg = ub->ub_pr19262[6];
			memcpy(engine->durable_desc, ub->ub_pr19262,
			    sizeof (engine->durable_desc));
			engine_reuse_release_locked(engine, ub->ub_txg,
			    engine->durable_catalog_txg);
		}
	}
	mutex_exit(&engine->lock);
}

static int
engine_journal_read(spa_t *spa, uint64_t page, uint64_t kind,
    uint64_t seq, const zio_cksum_t *previous, abd_t *out)
{
	engine_t *engine = spa->spa_reloc;
	int error = ECKSUM;
	abd_t *candidate = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	for (uint64_t i = 0; i < engine->vd->vdev_children; i++) {
		if (engine_store_io_at(engine->vd->vdev_child[i],
		    engine->journal_arena, page, 1, candidate, B_FALSE) != 0 ||
		    !engine_journal_valid(engine, candidate, kind, seq))
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
	engine_t *engine = spa->spa_reloc;
	abd_t *page = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	int error = ECKSUM;
	engine_quorum_t io = { 0 };
	boolean_t conflict = B_FALSE;
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	for (uint_t slot = 0; slot < 2; slot++) {
		for (uint_t child = 0; child < engine->vd->vdev_children;
		    child++) {
			int e =
			    engine_store_io_at(engine->vd->vdev_child[child],
			    engine->journal_arena, slot, 1, page, B_FALSE);
			if (e != 0) {
				if (slot == 0)
					engine_quorum_add(engine, &io, e);
				continue;
			}
			if (!engine_journal_valid(engine, page, ENGINE_J_HEAD,
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
	int required = engine_quorum_result(engine, &io);
	return (all_io && required != 0 ? required : error);
}

static int
engine_journal_apply(engine_t *engine, uint64_t *w)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t v = w[7], size = w[8];
	uint64_t sector = 1ULL << engine->vd->vdev_ashift;
	if (v >= engine->total || size == 0 || size > engine->total - v ||
	    !IS_P2ALIGNED(v, sector) ||
	    !IS_P2ALIGNED(size, sector) || w[6] == 0 ||
	    w[13] <= w[14] || w[13] > UINT16_MAX ||
	    w[13] > engine->vd->vdev_children ||
	    w[14] == 0 || w[14] > 3 || w[17] > 1 || w[15] == 0 ||
	    w[29] == 0 || w[29] > w[15] || w[27] <= w[28] ||
	    w[27] > UINT16_MAX ||
	    w[27] > engine->vd->vdev_children || w[28] == 0 || w[28] > 3)
		return (ENGINE_EINVAL(2));
	/* A released owner's bindings are folded into the catalog. */
	if (engine_tombstoned(engine, v, w[6]))
		return (0);
	engine_owner_t *e = engine_find(engine, v);
	if (e == NULL) {
		boolean_t legacy = zfs_range_tree_contains(engine->legacy,
		    v, size);
		if (legacy && (w[9] != 0 || w[24] != v || w[25] != size))
			return (ENGINE_EINVAL(3));
		e = engine_owner_alloc(KM_SLEEP);
		e->v = v; e->size = size; e->incarnation = w[6];
		e->allocation_txg = w[9]; e->created = w[16];
		e->catalog_txg = legacy ? 0 : UINT64_MAX;
		e->journal_pending = !legacy;
		avl_add(&engine->owners, e);
		if (legacy) {
			zfs_range_tree_remove(engine->legacy, v, size);
			engine_version_t *old = engine_version_new(engine, e, v,
			    size,
			    w[26], B_TRUE);
			engine_version_geometry(old, w[27], w[28]);
			old->generation = w[29];
			e->cur = old;
		}
	} else if (e->incarnation != w[6] || e->size != size ||
	    e->allocation_txg != w[9]) {
		return (ENGINE_EINVAL(4));
	}
	e->journal_touched = B_TRUE;
	engine_owner_dirty(engine, e);
	engine->next_incarnation = MAX(engine->next_incarnation, w[6]);
	engine->sequence = MAX(engine->sequence, MAX(w[15], w[16]));
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
			engine->retired -= b->size;
			e->cur->retired = B_TRUE;
			if (!e->retired)
				engine->retired += e->cur->size;
			e->cur = b;
			if (e->retired)
				engine->retired += b->size;
		}
		return (0);
	}
	if (!engine_store_extent(engine, w[10], w[11]))
		return (ENGINE_EINVAL(7));
	engine_version_t *b = engine_version_new(engine, e, w[10], w[11], w[12],
	    w[17]);
	engine_version_geometry(b, w[13], w[14]);
	b->generation = w[15];
	b->journal_seq = w[3];
	zfs_range_tree_remove(engine->free, b->p, b->size);
	if (e->cur == NULL || b->generation > e->cur->generation) {
		if (e->cur != NULL) {
			e->cur->retired = B_TRUE;
			if (!e->retired)
				engine->retired += e->cur->size;
		}
		e->cur = b;
		if (e->retired)
			engine->retired += b->size;
	} else {
		b->retired = B_TRUE;
		engine->retired += b->size;
	}
	return (0);
}

static int
engine_journal_recover_segment(spa_t *spa, engine_j_segment_t **next)
{
	engine_t *engine = spa->spa_reloc;
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
	engine->journal_current->serial = engine->journal_serial = header[30];
	engine->journal_current->phase = phase;
	engine->journal_current->target = target;
	memcpy(engine->journal_current->target_epoch, epoch, sizeof (epoch));
	zio_cksum_t expected = { 0 }, previous = { 0 };
	memcpy(&expected, header + 20, sizeof (expected));
	uint64_t capacity = engine->journal_bytes / ENGINE_STORE_PAGE - 2;
	if (head > capacity ||
	    (head == 0 && memcmp(&expected, &previous, sizeof (expected)) != 0))
		error = ECKSUM;
	if (phase == ENGINE_J_NORMAL &&
	    (target != 0 || bytes != 0 || epoch[0] != 0 || epoch[1] != 0))
		error = ENGINE_EINVAL(11);
	if (phase != ENGINE_J_NORMAL) {
		uint64_t size = bytes * engine->vd->vdev_children;
		uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
		uint64_t msize =
		    engine->arena_bytes * engine->vd->vdev_children;
		uint64_t found, span;
		if (bytes != engine->journal_bytes || target % stripe != 0 ||
		    target >= engine->total || size > engine->total - target ||
		    (epoch[0] == engine->journal_epoch[0] &&
		    epoch[1] == engine->journal_epoch[1]) ||
		    (target < engine->arena + msize &&
		    engine->arena < target + size) ||
		    zfs_range_tree_find_in(engine->journal_extents,
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
			error = engine_journal_segment(engine, target, epoch,
			    B_FALSE, B_TRUE, next);
			if (error == 0)
				engine->journal_current->phase =
				    ENGINE_J_ACTIVE;
			break;
		}
		if (seq == capacity) {
			error = ENGINE_EINVAL(15);
			break;
		}
		mutex_enter(&engine->lock);
		error = engine_journal_apply(engine, w);
		mutex_exit(&engine->lock);
	}
	if (error == 0 && phase == ENGINE_J_ACTIVE && *next == NULL)
		error = ECKSUM;
	if (error == 0 && phase == ENGINE_J_PREPARE && *next == NULL) {
		/* An intent reserves space without publishing log data. */
		mutex_enter(&engine->lock);
		boolean_t free = engine_store_extent(engine, target,
		    bytes * engine->vd->vdev_children);
		mutex_exit(&engine->lock);
		if (free) {
			engine_j_segment_t *pending;
			error = engine_journal_segment(engine, target, epoch,
			    B_FALSE, B_FALSE, &pending);
			if (error == 0) {
				engine_j_segment_t *old =
				    engine->journal_current;
				engine_journal_select(engine, pending);
				error = engine_journal_empty(spa, B_FALSE);
				engine_journal_select(engine, old);
			}
		}
	}
	if (error == 0) {
		mutex_enter(&engine->lock);
		error = engine_store_validate_virtual(engine);
		engine->journal_seq = last;
		engine->journal_hash = previous;
		mutex_exit(&engine->lock);
		pr19262_printf("PR19262_J_RECOVER head=%llu prefix=%llu\n",
		    (u_longlong_t)head, (u_longlong_t)last);
	}
	abd_free(page);
	return (error);
}

static int
engine_journal_recover(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	engine_j_segment_t *s = engine->journal_segments;
	int error = 0;
	while (s != NULL && error == 0) {
		engine_journal_select(engine, s);
		engine_j_segment_t *next = NULL;
		error = engine_journal_recover_segment(spa, &next);
		s->seq = engine->journal_seq;
		s->hash = engine->journal_hash;
		s = next;
	}
	if (error == 0) {
		mutex_enter(&engine->lock);
		engine->journal_formatted = engine->journal_ready = B_TRUE;
		engine->journal_repair = B_TRUE;
		/*
		 * Every replayed record is in RAM now; the catalog written
		 * after the claim barrier lists all of it.  Tombstones loaded
		 * from the catalog may be bound anywhere up to the tail.
		 */
		for (s = engine->journal_segments; s != NULL; s = s->next)
			s->fold_txg = MAX(s->fold_txg,
			    spa_last_synced_txg(spa) + 2);
		for (engine_tomb_t *t = avl_first(&engine->tombstones);
		    t != NULL; t = AVL_NEXT(&engine->tombstones, t))
			if (t->segment == UINT64_MAX)
				t->segment = engine->journal_tail->index;
		mutex_exit(&engine->lock);
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
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t visible = spa_last_synced_txg(spa) + 2;
	if (catalog_txg != UINT64_MAX)
		visible = MAX(visible, catalog_txg);
	if (engine->journal_tail != NULL)
		engine->journal_tail->fold_txg =
		    MAX(engine->journal_tail->fold_txg, visible);
}

/*
 * Cut folded segments from the chain head once the floor root's catalog
 * lists every version they bind, so that the next catalog records the new
 * root; free a cut segment at a later fence whose floor root's catalog
 * already names that root, so that no importable root can reach it.
 * free_limit is that catalog txg, or 0 (free none) while a paged
 * checkpoint root is guarded.
 * Tombstones whose segments are all gone expire.  The caller holds
 * store_lock; no root can be written meanwhile.
 */
static void
engine_journal_gc(spa_t *spa, uint64_t catalog, uint64_t free_limit,
    uint64_t *segments, uint64_t *bytes)
{
	engine_t *engine = spa->spa_reloc;
	(void) spa;
	*segments = *bytes = 0;
	mutex_enter(&engine->journal_lock);
	mutex_enter(&engine->lock);
	uint64_t size = engine->journal_bytes * engine->vd->vdev_children;
	engine_j_segment_t *s;
	while ((s = engine->journal_segments) != NULL &&
	    s != engine->journal_tail && s != engine->journal_current &&
	    s->next != NULL && s->fold_txg <= catalog) {
		engine->journal_segments = s->next;
		s->next = NULL;
		s->free_after = UINT64_MAX;
		engine_j_segment_t **link = &engine->journal_retired;
		while (*link != NULL)
			link = &(*link)->next;
		*link = s;
		engine->journal_root = engine->journal_segments->arena;
		memcpy(engine->journal_root_epoch,
		    engine->journal_segments->epoch,
		    sizeof (engine->journal_root_epoch));
		engine->revision++;
		pr19262_printf("PR19262_J_RETIRE index=%llu arena=%llu "
		    "fold=%llu catalog=%llu root=%llu\n",
		    (u_longlong_t)s->index, (u_longlong_t)s->arena,
		    (u_longlong_t)s->fold_txg, (u_longlong_t)catalog,
		    (u_longlong_t)engine->journal_root);
	}
	engine_j_segment_t **link = &engine->journal_retired;
	while ((s = *link) != NULL) {
		if (s->free_after == UINT64_MAX || s->free_after > catalog ||
		    s->free_after > free_limit) {
			link = &s->next;
			continue;
		}
		*link = s->next;
		VERIFY(zfs_range_tree_contains(engine->journal_extents,
		    s->arena, size));
		zfs_range_tree_remove(engine->journal_extents, s->arena, size);
		/* The floor root's catalog quarantines it as cut. */
		engine_reuse_add(engine, s->arena, size);
		engine->journal_freed_segments++;
		engine->journal_freed_bytes += size;
		(*segments)++;
		*bytes += size;
		engine->revision++;
		pr19262_printf("PR19262_J_FREE index=%llu arena=%llu "
		    "recorded=%llu catalog=%llu\n", (u_longlong_t)s->index,
		    (u_longlong_t)s->arena, (u_longlong_t)s->free_after,
		    (u_longlong_t)catalog);
		kmem_free(s, sizeof (*s));
	}
	uint64_t live = engine->journal_segments->index;
	for (engine_tomb_t *t = avl_first(&engine->tombstones); t != NULL;
	    t = AVL_NEXT(&engine->tombstones, t))
		if (t->segment < live)
			t->expired = B_TRUE;
	mutex_exit(&engine->lock);
	mutex_exit(&engine->journal_lock);
}

static int
engine_journal_repair_segment(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	abd_t *page = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	zio_cksum_t previous = { 0 };
	/* A returning member can hold a higher unpublished header serial. */
	int error = engine_journal_read_head(spa, page, B_TRUE);
	if (error == 0) {
		uint64_t serial = ((uint64_t *)abd_to_buf(page))[30];
		engine->journal_serial = MAX(engine->journal_serial, serial);
	}
	for (uint64_t seq = 1; error == 0 &&
	    seq <= engine->journal_seq; seq++) {
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
		engine_j_segment_t *s = engine->journal_current;
		uint64_t phase = s->phase;
		engine_j_segment_t *target = s->next;
		if (phase == ENGINE_J_PREPARE) {
			target = engine->journal_pending;
			if (target == NULL)
				phase = ENGINE_J_NORMAL;
		}
		error = engine_journal_head(spa, engine->journal_seq,
		    &previous, phase, target);
	}
	return (error);
}

static int
engine_journal_repair(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	int error = 0;
	for (engine_j_segment_t *s = engine->journal_segments;
	    s != NULL && error == 0; s = s->next) {
		engine_journal_select(engine, s);
		error = engine_journal_repair_segment(spa);
	}
	engine_journal_select(engine, engine->journal_tail);
	if (error == 0 && engine->journal_pending != NULL)
		error = engine_journal_finish_prepare(spa);
	return (error);
}

void
pr19262_engine_journal_test_hook(spa_t *spa, void (*hook)(spa_t *,
    const blkptr_t *, int))
{
	engine_t *engine = spa->spa_reloc;
	/* Install/remove this private test hook only with all callers idle. */
	engine->journal_observer = hook;
}

static void
engine_journal_test_event(spa_t *spa, const blkptr_t *bp, int phase)
{
	engine_t *engine = spa->spa_reloc;
	if (engine->journal_observer != NULL)
		engine->journal_observer(spa, bp, phase);
}

/* A held identity survives lookup replacement until this decision completes. */
static int
engine_journal_admit_locked(spa_t *spa, engine_hold_t *h)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->lock));
	if (h == NULL || engine->spa != spa || !engine->persistent)
		return (ENOENT);
	if (h->error != 0)
		return (h->error);
	/* Fall back to TXG sync before admitting any startup journal work. */
	if (!engine->journal_ready)
		return (EAGAIN);
	for (uint_t i = 0; i < h->count; i++) {
		engine_owner_t *e = h->owner[i];
		if (e->aborted || engine_find(engine, e->v) != e)
			return (ESTALE);
	}
	for (uint_t i = 0; i < h->count; i++)
		h->owner[i]->journal_touched = B_TRUE;
	return (0);
}

/*
 * L20 group commit.  A hold's bind (a ZIL block at its allocation, a block
 * the ZIL writes indirectly) queues a request; one binder at a time leads:
 * it takes journal_lock and every queued request, runs the checks of
 * engine_journal_bind_locked() for each in order, and writes the frames
 * it builds with one engine_journal_write_pages() and one head, as
 * engine_journal_bind_copies() does.  The frames, their sequence numbers
 * and their hash chain are the ones the single binds would have written
 * in the same order; only the heads between them are not written, and
 * recovery replays the valid frames past the head.  A group cut short
 * publishes a prefix of frames whose binds were not acknowledged, as a
 * crash between a single bind's frame and its head does.  The leader never
 * waits: a request that meets a copying owner, or any request while a crash
 * or fault hook is armed, is handed back to its binder, which finishes it
 * on the single path after the group (owners already bound are skipped
 * there).  Copies never queue: they bind through
 * engine_journal_bind_copies() or the single path.
 */
static int zfs_pr19262_journal_group = 1;
#define	ENGINE_J_GROUP_MAX 64

typedef struct engine_j_req {
	list_node_t node;
	engine_hold_t *hold;
	int error;
	boolean_t done;
	/* Handed back: the binder finishes it on the single path. */
	boolean_t single;
} engine_j_req_t;

static int engine_journal_group_bind(spa_t *, engine_hold_t *);

/* One hold's or copy's bind, frame by frame; the caller holds journal_lock. */
static int
engine_journal_bind_locked(spa_t *spa, engine_hold_t *h,
    engine_version_t *staging)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->journal_lock));
	int error = 0;
	boolean_t retry = B_FALSE;
	if (error == 0 && engine->journal_repair) {
		error = engine_journal_repair(spa);
		engine->journal_repair = error != 0;
	}
	for (uint_t i = 0; error == 0 && i < h->count; i++) {
		mutex_enter(&engine->lock);
		engine_owner_t *e = h->owner[i];
		while (e->copying && staging == NULL) {
			/* The copier needs this lock to publish. */
			mutex_exit(&engine->journal_lock);
			cv_wait(&engine->changed, &engine->lock);
			mutex_exit(&engine->lock);
			mutex_enter(&engine->journal_lock);
			mutex_enter(&engine->lock);
		}
		if (e->aborted || engine_find(engine, e->v) != e) {
			error = ESTALE;
			mutex_exit(&engine->lock);
			break;
		}
		engine_version_t *b = staging != NULL ? staging : e->cur;
		ASSERT(staging == NULL || (h->count == 1 && e->copying));
		if (b->journal_seq != 0) {
			mutex_exit(&engine->lock);
			continue;
		}
		error = engine->journal_error;
		if (error == 0 && engine->journal_seq + 3 >=
		    engine->journal_bytes / ENGINE_STORE_PAGE) {
			mutex_exit(&engine->lock);
			error = engine_journal_grow(spa, B_TRUE, &retry);
			mutex_enter(&engine->lock);
		}
		uint64_t seq = engine->journal_seq + 1;
		if (!engine->journal_ready)
			error = EAGAIN;
		if (seq + 2 > engine->journal_bytes / ENGINE_STORE_PAGE)
			error = ENOSPC;
		if (error != 0) {
			mutex_exit(&engine->lock);
			break;
		}
		abd_t *page = engine_journal_page(engine, ENGINE_J_BIND, seq);
		uint64_t *w = abd_to_buf(page);
		w[6] = e->incarnation; w[7] = e->v; w[8] = e->size;
		w[9] = e->allocation_txg; w[10] = b->p; w[11] = b->size;
		w[12] = b->birth; w[13] = b->width; w[14] = b->parity;
		w[15] = b->generation; w[16] = e->created; w[17] = b->ready;
		memcpy(w + 20, &engine->journal_hash, sizeof (zio_cksum_t));
		engine_version_t *old = b;
		for (engine_version_t *p = e->versions; p != NULL; p = p->next)
			if (p->generation < old->generation)
				old = p;
		w[24] = old->p; w[25] = old->size; w[26] = old->birth;
		w[27] = old->width; w[28] = old->parity;
		w[29] = old->generation;
		e->journal_touched = B_TRUE;
		b->leases++;
		mutex_exit(&engine->lock);
		engine_journal_seal(engine, page);
		zio_cksum_t hash;
		memcpy(&hash, w + ENGINE_J_HASH, sizeof (hash));
		error = engine_journal_write(spa, seq + 1, page);
		abd_free(page);
		if (error == 0 && seq == engine->journal_fault_seq &&
		    engine->journal_fault_stage == 3) {
			pr19262_printf("PR19262_J_CRASH_AFTER_BIND seq=%llu\n",
			    (u_longlong_t)seq);
			pr19262_flush();
			pr19262_exit(78);
		}
		if (error == 0) {
			error = engine_journal_head(spa, seq, &hash,
			    ENGINE_J_NORMAL, NULL);
		}
		if (error == 0 && seq == engine->journal_fault_seq &&
		    engine->journal_fault_stage == 6) {
			pr19262_printf("PR19262_J_CRASH_AFTER_HEAD seq=%llu\n",
			    (u_longlong_t)seq);
			pr19262_flush();
			pr19262_exit(79);
		}
		mutex_enter(&engine->lock);
		b->leases--;
		if (error == 0) {
			engine->journal_seq = b->journal_seq = seq;
			engine->journal_hash = hash;
			engine->journal_tail->seq = seq;
			engine->journal_tail->hash = hash;
			engine_journal_fold(spa, e->catalog_txg);
			pr19262_printf("PR19262_J_BIND seq=%llu v=%llu p=%llu "
			    "arena=%llu\n", (u_longlong_t)seq,
			    (u_longlong_t)e->v,
			    (u_longlong_t)b->p,
			    (u_longlong_t)engine->journal_arena);
		}
		mutex_exit(&engine->lock);
	}
	mutex_enter(&engine->lock);
	if (error != 0 && !(retry && error == ENOSPC))
		engine->journal_error = error;
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
	return (error);
}

static int
engine_journal_bind_impl(spa_t *spa, engine_hold_t *h,
    engine_version_t *staging)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	int error = engine_journal_admit_locked(spa, h);
	mutex_exit(&engine->lock);
	if (error != 0)
		return (error);
	if (staging == NULL && zfs_pr19262_journal_group != 0)
		return (engine_journal_group_bind(spa, h));
	mutex_enter(&engine->journal_lock);
	error = engine_journal_bind_locked(spa, h, staging);
	mutex_exit(&engine->journal_lock);
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
	engine_t *engine = c->spa->spa_reloc;
	/* The copy handle already pins its owner and source version. */
	engine_hold_t h = { .spa = c->spa, .count = 1,
	    .owner = { c->owner } };
	return (engine_journal_bind_impl(engine->spa, &h, c->stage));
}

/* One copy's BIND frame at seq; the chain hash is set when sealed. */
static abd_t *
engine_journal_bind_frame(engine_t *engine, engine_owner_t *e,
    engine_version_t *b,
    uint64_t seq)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	abd_t *page = engine_journal_page(engine, ENGINE_J_BIND, seq);
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
	if (n == 0)
		return;
	engine_t *engine = cv[0]->spa->spa_reloc;
	spa_t *spa = engine->spa;
	for (uint_t i = 0; i < n; i++)
		VERIFY3P(cv[i]->spa, ==, spa);
	/* The crash and fault hooks act on single frames: keep them so. */
	if (engine->journal_fault_stage != 0 || engine->journal_roll_fault !=
	    0) {
		for (uint_t i = 0; i < n; i++)
			if (attempted[i])
				errors[i] = engine_journal_bind_copy(cv[i]);
		return;
	}
	uint_t m = 0;
	mutex_enter(&engine->lock);
	for (uint_t i = 0; i < n; i++) {
		if (!attempted[i])
			continue;
		engine_owner_t *e = cv[i]->owner;
		ASSERT(e->copying);
		if (!engine->persistent)
			errors[i] = ENOENT;
		else if (e->aborted || engine_find(engine, e->v) != e)
			errors[i] = ESTALE;
		else
			e->journal_touched = B_TRUE;
		m += errors[i] == 0;
	}
	mutex_exit(&engine->lock);
	if (m == 0)
		return;
	engine_member_page_t *pv = kmem_alloc(m * sizeof (*pv), KM_SLEEP);
	uint_t *index = kmem_alloc(m * sizeof (uint_t), KM_SLEEP);
	hrtime_t lock = gethrtime();
	mutex_enter(&engine->journal_lock);
	atomic_add_64(&engine->journal_cost.lock, gethrtime() - lock);
	int error = 0;
	boolean_t retry = B_FALSE;
	if (engine->journal_repair) {
		error = engine_journal_repair(spa);
		engine->journal_repair = error != 0;
	}
	uint_t next = 0;
	while (error == 0 && next < n) {
		mutex_enter(&engine->lock);
		error = engine->journal_error;
		if (error == 0 && engine->journal_seq + 3 >=
		    engine->journal_bytes / ENGINE_STORE_PAGE) {
			mutex_exit(&engine->lock);
			error = engine_journal_grow(spa, B_TRUE, &retry);
			mutex_enter(&engine->lock);
		}
		uint64_t last = engine->journal_bytes / ENGINE_STORE_PAGE;
		if (error == 0 && !engine->journal_ready)
			error = EAGAIN;
		/* A grown segment has room for a frame; never spin on none. */
		if (error == 0 && engine->journal_seq + 3 >= last)
			error = ENOSPC;
		if (error != 0) {
			mutex_exit(&engine->lock);
			break;
		}
		/* As many frames as this segment holds before it must grow. */
		uint64_t first = engine->journal_seq + 1, seq = first - 1;
		uint_t k = 0;
		for (; next < n && seq + 3 < last; next++) {
			if (!attempted[next] || errors[next] != 0)
				continue;
			engine_copy_t *c = cv[next];
			engine_owner_t *e = c->owner;
			if (e->aborted || engine_find(engine, e->v) != e) {
				errors[next] = ESTALE;
				continue;
			}
			VERIFY0(c->stage->journal_seq);
			seq++;
			pv[k].arena = engine->journal_arena;
			pv[k].page = seq + 1;
			pv[k].abd = engine_journal_bind_frame(engine, e,
			    c->stage, seq);
			c->stage->leases++;
			index[k++] = next;
		}
		zio_cksum_t hash = engine->journal_hash;
		mutex_exit(&engine->lock);
		if (k == 0)
			continue;
		for (uint_t j = 0; j < k; j++) {
			uint64_t *w = abd_to_buf(pv[j].abd);
			memcpy(w + 20, &hash, sizeof (hash));
			engine_journal_seal(engine, pv[j].abd);
			memcpy(&hash, w + ENGINE_J_HASH, sizeof (hash));
		}
		if (engine->journal_group_fault != 0 && k >= 2) {
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
		mutex_enter(&engine->lock);
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
			    (u_longlong_t)engine->journal_arena);
		}
		if (error == 0) {
			engine->journal_seq = seq;
			engine->journal_hash = hash;
			engine->journal_tail->seq = seq;
			engine->journal_tail->hash = hash;
		}
		mutex_exit(&engine->lock);
	}
	/* What the loop did not reach shares the error that stopped it. */
	for (uint_t i = next; i < n; i++)
		if (attempted[i] && errors[i] == 0)
			errors[i] = error;
	mutex_enter(&engine->lock);
	if (error != 0 && !(retry && error == ENOSPC))
		engine->journal_error = error;
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
	mutex_exit(&engine->journal_lock);
	kmem_free(index, m * sizeof (uint_t));
	kmem_free(pv, m * sizeof (*pv));
}

/* The frames a group leader has built and not yet written (L20). */
typedef struct engine_j_group {
	uint_t k;
	uint64_t first;
	engine_member_page_t pv[ENGINE_J_GROUP_MAX];
	engine_version_t *b[ENGINE_J_GROUP_MAX];
	engine_owner_t *e[ENGINE_J_GROUP_MAX];
	engine_j_req_t *req[ENGINE_J_GROUP_MAX];
} engine_j_group_t;

static boolean_t
engine_journal_group_has(const engine_j_group_t *g, const engine_version_t *b)
{
	for (uint_t j = 0; j < g->k; j++)
		if (g->b[j] == b)
			return (B_TRUE);
	return (B_FALSE);
}

/*
 * Write the built frames with one write of every member and one head, as
 * a single bind writes its frame and head; each request with a frame in
 * the group shares the result.  The caller holds journal_lock.
 */
static int
engine_journal_group_flush(spa_t *spa, engine_j_group_t *g)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->journal_lock));
	uint_t k = g->k;
	if (k == 0)
		return (0);
	uint64_t seq = g->first + k - 1;
	mutex_enter(&engine->lock);
	VERIFY3U(g->first, ==, engine->journal_seq + 1);
	zio_cksum_t hash = engine->journal_hash;
	mutex_exit(&engine->lock);
	for (uint_t j = 0; j < k; j++) {
		uint64_t *w = abd_to_buf(g->pv[j].abd);
		memcpy(w + 20, &hash, sizeof (hash));
		engine_journal_seal(engine, g->pv[j].abd);
		memcpy(&hash, w + ENGINE_J_HASH, sizeof (hash));
	}
	int error = engine_journal_write_pages(spa, g->pv, k);
	if (error == 0) {
		error = engine_journal_head(spa, seq, &hash,
		    ENGINE_J_NORMAL, NULL);
	}
	mutex_enter(&engine->lock);
	for (uint_t j = 0; j < k; j++) {
		engine_version_t *b = g->b[j];
		ASSERT3U(b->leases, >, 0);
		b->leases--;
		abd_free(g->pv[j].abd);
		g->pv[j].abd = NULL;
		if (error != 0) {
			if (g->req[j]->error == 0)
				g->req[j]->error = error;
			continue;
		}
		b->journal_seq = g->first + j;
		engine_journal_fold(spa, g->e[j]->catalog_txg);
		pr19262_printf("PR19262_J_BIND seq=%llu v=%llu p=%llu "
		    "arena=%llu\n", (u_longlong_t)(g->first + j),
		    (u_longlong_t)g->e[j]->v, (u_longlong_t)b->p,
		    (u_longlong_t)engine->journal_arena);
	}
	if (error == 0) {
		engine->journal_seq = seq;
		engine->journal_hash = hash;
		engine->journal_tail->seq = seq;
		engine->journal_tail->hash = hash;
	} else if (engine->journal_error == 0) {
		engine->journal_error = error;
	}
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
	g->k = 0;
	return (error);
}

/*
 * Bind every queued request in order with engine_journal_bind_locked()'s
 * checks, deferring the frame writes into groups.  A group is written
 * before anything that a single bind would have done after its previous
 * frame was durable: a hand-back, a version already in the group, a roll,
 * a repair, an error, a full group.  The caller holds journal_lock, which
 * is never dropped here.
 */
static void
engine_journal_group_run(spa_t *spa, list_t *batch, engine_j_group_t *g)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->journal_lock));
	for (engine_j_req_t *r = list_head(batch); r != NULL;
	    r = list_next(batch, r)) {
		engine_hold_t *h = r->hold;
		if (engine->journal_fault_stage != 0 ||
		    engine->journal_roll_fault != 0 ||
		    engine->journal_group_fault != 0) {
			/* The hooks act on single frames: hand it back. */
			r->error = engine_journal_group_flush(spa, g);
			r->single = r->error == 0;
			continue;
		}
		int error = 0;
		boolean_t retry = B_FALSE;
		if (engine->journal_repair) {
			error = engine_journal_group_flush(spa, g);
			if (error == 0) {
				error = engine_journal_repair(spa);
				engine->journal_repair = error != 0;
			}
		}
		for (uint_t i = 0; error == 0 && i < h->count; i++) {
			engine_owner_t *e = h->owner[i];
			mutex_enter(&engine->lock);
			if (e->copying) {
				/*
				 * Waiting here would hold every queued bind
				 * behind this copy: write what was built and
				 * let the binder wait on the single path.
				 */
				mutex_exit(&engine->lock);
				error = engine_journal_group_flush(spa, g);
				r->single = error == 0;
				break;
			}
			if (g->k != 0 && engine_journal_group_has(g, e->cur)) {
				mutex_exit(&engine->lock);
				error = engine_journal_group_flush(spa, g);
				if (error != 0)
					break;
				mutex_enter(&engine->lock);
			}
			if (e->aborted || engine_find(engine, e->v) != e) {
				error = ESTALE;
				mutex_exit(&engine->lock);
				break;
			}
			engine_version_t *b = e->cur;
			if (b->journal_seq != 0) {
				mutex_exit(&engine->lock);
				continue;
			}
			error = engine->journal_error;
			if (error == 0 && engine->journal_seq + g->k + 3 >=
			    engine->journal_bytes / ENGINE_STORE_PAGE) {
				mutex_exit(&engine->lock);
				error = engine_journal_group_flush(spa, g);
				if (error == 0)
					error = engine_journal_grow(spa, B_TRUE,
					    &retry);
				mutex_enter(&engine->lock);
			}
			uint64_t seq = engine->journal_seq + g->k + 1;
			/* As the single path, but keep an earlier failure. */
			if (error == 0 && !engine->journal_ready)
				error = EAGAIN;
			if (error == 0 && seq + 2 >
			    engine->journal_bytes / ENGINE_STORE_PAGE)
				error = ENOSPC;
			if (error != 0) {
				mutex_exit(&engine->lock);
				break;
			}
			uint_t j = g->k;
			if (j == 0)
				g->first = seq;
			g->pv[j].arena = engine->journal_arena;
			g->pv[j].page = seq + 1;
			g->pv[j].abd = engine_journal_bind_frame(engine, e, b,
			    seq);
			g->b[j] = b;
			g->e[j] = e;
			g->req[j] = r;
			g->k = j + 1;
			e->journal_touched = B_TRUE;
			b->leases++;
			mutex_exit(&engine->lock);
			if (g->k == ENGINE_J_GROUP_MAX)
				error = engine_journal_group_flush(spa, g);
		}
		if (error != 0) {
			/* A single bind wrote the frames before its failure. */
			(void) engine_journal_group_flush(spa, g);
			if (r->error == 0)
				r->error = error;
			mutex_enter(&engine->lock);
			/* The first latched failure stays (a dead journal). */
			if (engine->journal_error == 0 &&
			    !(retry && error == ENOSPC))
				engine->journal_error = error;
			cv_broadcast(&engine->changed);
			mutex_exit(&engine->lock);
		}
	}
	(void) engine_journal_group_flush(spa, g);
}

/*
 * Queue this hold's bind and wait until a leader has bound it; lead when
 * nobody does.  The leader flag, not journal_lock, elects the leader, so
 * other journal_lock holders (copies, rolls, stores) never take a turn.
 * A request handed back finishes on the single path here, after the group
 * and with no leadership held, so only this binder waits for its copy.
 */
static int
engine_journal_group_bind(spa_t *spa, engine_hold_t *h)
{
	engine_t *engine = spa->spa_reloc;
	engine_j_req_t req = { .hold = h, .error = 0, .done = B_FALSE,
	    .single = B_FALSE };
	list_link_init(&req.node);
	mutex_enter(&engine->lock);
	list_insert_tail(&engine->journal_reqs, &req);
	while (!req.done) {
		if (engine->journal_leader) {
			cv_wait(&engine->journal_group_cv, &engine->lock);
			continue;
		}
		engine->journal_leader = B_TRUE;
		mutex_exit(&engine->lock);
		list_t batch;
		list_create(&batch, sizeof (engine_j_req_t),
		    offsetof(engine_j_req_t, node));
		engine_j_group_t *g = kmem_zalloc(sizeof (*g), KM_SLEEP);
		mutex_enter(&engine->journal_lock);
		mutex_enter(&engine->lock);
		list_move_tail(&batch, &engine->journal_reqs);
		mutex_exit(&engine->lock);
		engine_journal_group_run(spa, &batch, g);
		mutex_enter(&engine->lock);
		cv_broadcast(&engine->changed);
		mutex_exit(&engine->lock);
		mutex_exit(&engine->journal_lock);
		mutex_enter(&engine->lock);
		engine_j_req_t *r;
		/* A request is its binder's: untouched once marked done. */
		while ((r = list_remove_head(&batch)) != NULL)
			r->done = B_TRUE;
		list_destroy(&batch);
		engine->journal_leader = B_FALSE;
		cv_broadcast(&engine->journal_group_cv);
		mutex_exit(&engine->lock);
		kmem_free(g, sizeof (*g));
		mutex_enter(&engine->lock);
	}
	int error = req.error;
	boolean_t single = req.single;
	mutex_exit(&engine->lock);
	if (error == 0 && single) {
		mutex_enter(&engine->journal_lock);
		error = engine_journal_bind_locked(spa, h, NULL);
		mutex_exit(&engine->journal_lock);
	}
	return (error);
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
	engine_t *engine = spa->spa_reloc;
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
	mutex_enter(&engine->lock);
	int error = engine_journal_admit_locked(spa, h);
	if (error != 0) {
		mutex_exit(&engine->lock);
		pr19262_engine_rele(h);
		return (error);
	}
	h->write = B_TRUE;
	for (uint_t i = 0; i < h->count; i++)
		h->owner[i]->queued_writers++;
	mutex_exit(&engine->lock);
	engine_j_dep_t *dep = kmem_alloc(sizeof (*dep), KM_SLEEP);
	dep->hold = h;
	dep->next = job->deps;
	job->deps = dep;
	return (0);
}

static void
engine_journal_worker(void *arg)
{
	engine_t *engine = ((engine_j_job_t *)arg)->spa->spa_reloc;
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
	if (error != 0 && engine->gate_prints++ < 8)
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
	engine_t *engine = spa->spa_reloc;
	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return;
	engine_j_job_t *job = kmem_zalloc(sizeof (*job), KM_SLEEP);
	job->spa = spa;
	job->bp = *bp;
	job->hold = pr19262_engine_hold(spa, bp, NULL, B_FALSE, B_FALSE);
	engine_journal_test_event(spa, bp, 1);
	mutex_enter(&engine->lock);
	int error = engine_journal_admit_locked(spa, job->hold);
	if (job->hold != NULL && error != 0)
		job->hold->error = error;
	if (error != 0 && error != ENOENT && engine->gate_prints++ < 8)
		pr19262_printf("PR19262_GATE_REFUSED error=%d vdev=%llu "
		    "v=%llu birth=%llu type=%u level=%u ndvas=%u\n", error,
		    (u_longlong_t)DVA_GET_VDEV(&bp->blk_dva[0]),
		    (u_longlong_t)DVA_GET_OFFSET(&bp->blk_dva[0]),
		    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(bp),
		    (uint_t)BP_GET_TYPE(bp), (uint_t)BP_GET_LEVEL(bp),
		    (uint_t)BP_GET_NDVAS(bp));
	mutex_exit(&engine->lock);
	job->gate = zio_null(parent, spa, NULL, NULL, NULL, ZIO_FLAG_CANFAIL);
	VERIFY3U(taskq_dispatch(engine_journal_tq, engine_journal_worker, job,
	    TQ_SLEEP), !=, TASKQID_INVALID);
}

int
pr19262_engine_journal_claim(spa_t *spa, uint64_t txg)
{
	engine_t *engine = spa->spa_reloc;
	if (!pr19262_engine_persistent(spa))
		return (0);
	int error = 0;
	mutex_enter(&engine->lock);
	uint64_t capacity = avl_numnodes(&engine->owners), count = 0;
	if (capacity > SIZE_MAX / sizeof (engine_owner_t *)) {
		mutex_exit(&engine->lock);
		return (EOVERFLOW);
	}
	engine_owner_t **pending = pr19262_alloc(capacity * sizeof (*pending),
	    KM_SLEEP);
	for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
	    e = AVL_NEXT(&engine->owners, e)) {
		if (!e->journal_pending)
			continue;
		e->holds++;
		pending[count++] = e;
	}
	mutex_exit(&engine->lock);
	boolean_t trace = pr19262_env("PR19262_J_CLAIM_TRACE") != NULL;
	if (trace)
		pr19262_printf("PR19262_J_CLAIM_BEGIN owners=%llu txg=%llu "
		    "sync_on=%d\n", (u_longlong_t)count, (u_longlong_t)txg,
		    spa->spa_sync_on);
	spa_config_enter(spa, SCL_ALLOC, FTAG, RW_READER);
	/* Validate every reservation before mutating the native V allocator. */
	for (uint64_t i = 0; i < count && error == 0; i++)
		error = metaslab_claim_impl(engine->vd, pending[i]->v,
		    pending[i]->size, 0);
	for (uint64_t i = 0; i < count && error == 0; i++) {
		engine_owner_t *e = pending[i];
		error = metaslab_claim_impl(engine->vd, e->v, e->size, txg);
		if (error != 0)
			break;
		/* A successful claim must change the real native free tree. */
		if (metaslab_claim_impl(engine->vd,
		    e->v, e->size, 0) != ENOENT) {
			error = EIO;
			break;
		}
		mutex_enter(&engine->lock);
		e->journal_pending = B_FALSE;
		e->journal_reserved = B_TRUE;
		e->claimed_txg = e->catalog_txg = txg;
		engine_owner_dirty(engine, e);
		engine->revision++;
		mutex_exit(&engine->lock);
		spa->spa_claim_max_txg = MAX(spa->spa_claim_max_txg,
		    e->allocation_txg);
		if (trace)
			pr19262_printf("PR19262_J_CLAIM_NATIVE v=%llu "
			    "size=%llu "
			    "txg=%llu native_free=0\n", (u_longlong_t)e->v,
			    (u_longlong_t)e->size, (u_longlong_t)txg);
	}
	spa_config_exit(spa, SCL_ALLOC, FTAG);
	mutex_enter(&engine->lock);
	if (error == 0) {
		for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
		    e = AVL_NEXT(&engine->owners, e)) {
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
	mutex_exit(&engine->lock);
	kmem_free(pending, capacity * sizeof (*pending));
	if (trace)
		pr19262_printf("PR19262_J_CLAIM_END owners=%llu error=%d "
		    "sync_on=%d\n", (u_longlong_t)count, error,
		    spa->spa_sync_on);
	return (error);
}

static boolean_t
engine_journal_claimed(spa_t *spa, vdev_t *vd, uint64_t v,
    uint64_t size, uint64_t birth, uint64_t txg, boolean_t claim)
{
	engine_t *engine = spa->spa_reloc;
	boolean_t known = B_FALSE;
	uint64_t incarnation = 0;
	mutex_enter(&engine->lock);
	if (engine->spa == spa && engine->persistent && vd == engine->vd) {
		engine_owner_t *e = engine_find(engine, v);
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
		if (known)
			incarnation = e->incarnation;
	}
	mutex_exit(&engine->lock);
	if (!known || metaslab_claim_impl(vd, v, size, 0) != ENOENT)
		return (B_FALSE);
	if (claim) {
		/* Only an actual claim proves a ZIL reference. */
		mutex_enter(&engine->lock);
		engine_owner_t *e = engine_find(engine, v);
		known = e != NULL && e->incarnation == incarnation;
		if (known && e->journal_reserved) {
			ASSERT(!spa->spa_sync_on);
			e->journal_reserved = B_FALSE;
		}
		mutex_exit(&engine->lock);
	}
	return (known);
}

boolean_t
pr19262_engine_journal_claimed(spa_t *spa, vdev_t *vd, uint64_t v,
    uint64_t size, uint64_t birth, uint64_t txg)
{
	return (engine_journal_claimed(spa, vd, v, size, birth, txg, B_FALSE));
}

boolean_t
pr19262_engine_journal_reference(spa_t *spa, vdev_t *vd, uint64_t v,
    uint64_t size, uint64_t birth, uint64_t txg)
{
	return (engine_journal_claimed(spa, vd, v, size, birth, txg, B_TRUE));
}

/*
 * R02 exact-new-leaf reseed (D70 candidate).
 *
 * A seed copies the complete validated closure of every importable root
 * (catalog/tree/directory pages, both allocator control slots of every
 * arena including a validated pending-arena slot, and the journal HEAD
 * slots plus the valid BIND/FORWARD prefix of every segment) onto one exact
 * concrete leaf.  Every page comes from an independently validated source
 * copy, the target is flushed synchronously, and every page is then read
 * back from the target alone and compared with that source.  An audit
 * re-reads the target only and validates each page against the closure.
 */
typedef struct engine_r02_seed_stats {
	uint64_t entries;
	uint64_t byte_entries;
	uint64_t copied;
	uint64_t verified;
	uint64_t journal_pending;
	uint64_t pending_arenas;
} engine_r02_seed_stats_t;

/* Copy order: tree/catalog pages, controls, journal frames, HEAD slots. */
static int
engine_r02_entry_rank(pr19262_r02_manifest_kind_t kind)
{
	switch (kind) {
	case PR19262_R02_MANIFEST_CATALOG:
	case PR19262_R02_MANIFEST_TREE:
	case PR19262_R02_MANIFEST_DIRECTORY:
		return (0);
	case PR19262_R02_MANIFEST_CONTROL:
		return (1);
	case PR19262_R02_MANIFEST_JOURNAL_BIND:
	case PR19262_R02_MANIFEST_JOURNAL_FORWARD:
		return (2);
	case PR19262_R02_MANIFEST_JOURNAL_HEAD:
		return (3);
	default:
		/* Roots, arena reservations and a PREPARE target: no bytes. */
		return (-1);
	}
}

/* Does this exact page carry the hash the validated closure recorded? */
static boolean_t
engine_r02_entry_valid(const pr19262_r02_manifest_entry_t *e, abd_t *abd)
{
	zio_cksum_t saved, hash;
	if (e->pages != 1)
		return (B_FALSE);
	if (e->kind == PR19262_R02_MANIFEST_JOURNAL_HEAD ||
	    e->kind == PR19262_R02_MANIFEST_JOURNAL_BIND ||
	    e->kind == PR19262_R02_MANIFEST_JOURNAL_FORWARD) {
		uint64_t *w = abd_to_buf(abd);
		if (w[0] != ENGINE_J_MAGIC)
			return (B_FALSE);
		memcpy(&saved, w + ENGINE_J_HASH, sizeof (saved));
		memset(w + ENGINE_J_HASH, 0, sizeof (saved));
		abd_checksum_sha256(abd, ENGINE_STORE_PAGE, NULL, &hash);
		memcpy(w + ENGINE_J_HASH, &saved, sizeof (saved));
	} else {
		engine_mnode_t *n = engine_mnode_alloc();
		abd_copy_to_buf(n->data, abd, ENGINE_STORE_PAGE);
		engine_mhash(n, &hash);
		saved = engine_mheader(n)->hash;
		engine_mnode_free(n);
	}
	return (memcmp(&saved, &e->hash, sizeof (saved)) == 0 &&
	    memcmp(&hash, &e->hash, sizeof (hash)) == 0);
}

/* Find an independently valid copy; the target itself is never a source. */
static int
engine_r02_entry_source(engine_r02_manifest_private_t *m,
    const pr19262_r02_manifest_entry_t *e, vdev_t *target, abd_t *abd)
{
	engine_t *engine = m->spa->spa_reloc;
	for (uint_t c = 0; c < engine->vd->vdev_children; c++) {
		engine_store_endpoints_t ep;
		if (engine_store_member_endpoints(engine,
		    engine->vd->vdev_child[c], B_FALSE, &ep) != 0)
			continue;
		for (uint_t k = 0; k < ep.count; k++) {
			if (ep.leaf[k] == target ||
			    engine_store_leaf_io(engine, ep.leaf[k],
			    e->physical, 0, 1, abd, B_FALSE, m->mode) != 0)
				continue;
			if (engine_r02_entry_valid(e, abd))
				return (0);
		}
	}
	return (SET_ERROR(ECKSUM));
}

static int
engine_r02_seed_leaf(engine_r02_manifest_private_t *m, vdev_t *target,
    boolean_t copy, engine_r02_seed_stats_t *st)
{
	engine_t *engine = m->spa->spa_reloc;
	abd_t *src = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	abd_t *got = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	int error = 0;
	memset(st, 0, sizeof (*st));
	for (engine_r02_entry_t *e = list_head(&m->entries); e != NULL;
	    e = list_next(&m->entries, e)) {
		st->entries++;
		st->byte_entries += engine_r02_entry_rank(e->entry.kind) >= 0;
		st->journal_pending += e->entry.kind ==
		    PR19262_R02_MANIFEST_JOURNAL_PENDING;
		st->pending_arenas += e->entry.kind ==
		    PR19262_R02_MANIFEST_PENDING_ARENA;
	}
	for (int rank = 0; copy && error == 0 && rank <= 3; rank++) {
		for (engine_r02_entry_t *e = list_head(&m->entries);
		    error == 0 && e != NULL; e = list_next(&m->entries, e)) {
			if (engine_r02_entry_rank(e->entry.kind) != rank)
				continue;
			error = engine_r02_entry_source(m, &e->entry, target,
			    src);
			if (error == 0)
				error = engine_store_leaf_io(engine, target,
				    e->entry.physical, 0, 1, src, B_TRUE,
				    m->mode);
			if (error == 0 && st->copied++ == 0)
				engine_r02_cut(1);
		}
	}
	if (copy && error == 0) {
		engine_r02_cut(2);
		error = engine_store_leaf_flush(target, m->mode);
	}
	/* Fresh exact-target readback of every byte object in the closure. */
	for (engine_r02_entry_t *e = list_head(&m->entries);
	    error == 0 && e != NULL; e = list_next(&m->entries, e)) {
		if (engine_r02_entry_rank(e->entry.kind) < 0)
			continue;
		if (copy) {
			error = engine_r02_entry_source(m, &e->entry, target,
			    src);
			if (error == 0)
				error = engine_store_leaf_readback_equal(engine,
				    target, e->entry.physical, 0, 1, src,
				    m->mode);
		}
		if (error == 0)
			error = engine_store_leaf_io(engine, target,
			    e->entry.physical, 0, 1, got, B_FALSE, m->mode);
		if (error == 0 && !engine_r02_entry_valid(&e->entry, got))
			error = SET_ERROR(ECKSUM);
		if (error == 0)
			st->verified++;
	}
	abd_free(got);
	abd_free(src);
	return (error);
}

static void
engine_r02_seed_print(const char *what, int error,
    const engine_r02_seed_stats_t *st, const pr19262_r02_manifest_t *n)
{
	pr19262_printf("PR19262_R02_SEED op=%s error=%d entries=%llu "
	    "byte_entries=%llu copied=%llu verified=%llu roots=%llu "
	    "catalog=%llu tree=%llu control=%llu directory=%llu "
	    "journal_heads=%llu journal_binds=%llu journal_forwards=%llu "
	    "journal_pending=%llu pending_arenas=%llu\n", what, error,
	    (u_longlong_t)st->entries, (u_longlong_t)st->byte_entries,
	    (u_longlong_t)st->copied, (u_longlong_t)st->verified,
	    (u_longlong_t)n->roots, (u_longlong_t)n->catalog_pages,
	    (u_longlong_t)n->tree_pages, (u_longlong_t)n->control_pages,
	    (u_longlong_t)n->directory_pages, (u_longlong_t)n->journal_heads,
	    (u_longlong_t)n->journal_binds, (u_longlong_t)n->journal_forwards,
	    (u_longlong_t)n->journal_pending, (u_longlong_t)n->pending_arenas);
}

/*
 * Collect the closure and seed or audit target.  The caller owns the
 * locks engine_r02_collect_locked() documents.  copy: seed, then audit;
 * otherwise audit, and on any audit failure reseed and audit again.
 */
static int
engine_r02_seed(spa_t *spa, engine_store_leaf_io_mode_t mode,
    vdev_t *target, vdev_t *absent_ok, boolean_t copy, const char *what)
{
	engine_t *engine = spa->spa_reloc;
	engine_r02_manifest_private_t m;
	pr19262_r02_manifest_t counts;
	engine_r02_seed_stats_t st = { 0 };
	engine_r02_manifest_init(&m, spa, mode, B_TRUE);
	int error = engine_r02_collect_locked(&m, absent_ok);
	memset(&counts, 0, sizeof (counts));
	if (error == 0)
		engine_r02_manifest_counts(&m, &counts);
	char op[48];
	(void) strlcpy(op, what, sizeof (op));
	if (error == 0) {
		error = engine_r02_seed_leaf(&m, target, copy, &st);
		if (error != 0 && !copy) {
			engine_r02_seed_print("audit-failed", error, &st,
			    &counts);
			copy = B_TRUE;
			/* Never report a reseeded leaf as a clean audit. */
			(void) snprintf(op, sizeof (op), "%s-reseeded", what);
			error = engine_r02_seed_leaf(&m, target, B_TRUE, &st);
		}
	}
	engine_r02_seed_print(op, error, &st, &counts);
	if (error == 0) {
		mutex_enter(&engine->lock);
		engine->r02_pair_audits++;
		if (copy) {
			engine->r02_pair_seeds++;
			engine->r02_pair_copied += st.copied;
		}
		mutex_exit(&engine->lock);
	}
	engine_r02_manifest_fini(&m);
	return (error);
}

/*
 * spa_vdev_attach()/spa_vdev_detach() take the raw-topology writer before
 * spa_vdev_enter(): every R02 collector/observer enters it as a reader
 * before SCL_STATE, so no such reader can hold store_lock or journal_lock
 * while the attach/detach waits for SCL_ALL.
 */
boolean_t
pr19262_engine_r02_topology_enter(spa_t *spa)
{
	if (spa == NULL || spa->spa_reloc == NULL ||
	    !pr19262_engine_persistent(spa))
		return (B_FALSE);
	rw_enter(&((engine_t *)spa->spa_reloc)->r02_topology_lock, RW_WRITER);
	return (B_TRUE);
}

void
pr19262_engine_r02_topology_exit(spa_t *spa)
{
	rw_exit(&((engine_t *)spa->spa_reloc)->r02_topology_lock);
}

/*
 * Other raw-store users are not topology readers; under SCL_ALL they cannot
 * be issuing raw I/O, but one may hold store_lock or journal_lock while it
 * waits for SCL_ZIO.  The writer therefore never blocks on those mutexes:
 * a busy store fails the attach or detach cleanly (EBUSY, no mutation).
 */
static int
engine_r02_writer_locks(engine_t *engine)
{
	if (!mutex_tryenter(&engine->store_lock))
		return (SET_ERROR(EBUSY));
	if (!mutex_tryenter(&engine->journal_lock)) {
		mutex_exit(&engine->store_lock);
		return (SET_ERROR(EBUSY));
	}
	return (0);
}

static void
engine_r02_writer_unlock(engine_t *engine)
{
	mutex_exit(&engine->journal_lock);
	mutex_exit(&engine->store_lock);
}

/*
 * Staged admission instead of D64.  Returns 0 only when this replacement is
 * one the candidate fully covers; every other persistent-member replacement
 * keeps the D64 refusal (ENOTSUP) before any mutation.
 */
int
pr19262_engine_r02_attach_admit(spa_t *spa, vdev_t *oldvd, boolean_t gate)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(spa_config_held(spa, SCL_ALL, RW_WRITER) == SCL_ALL);
	if (!zfs_pr19262_r02_reseed || !gate || engine->vd == NULL ||
	    oldvd->vdev_parent != engine->vd ||
	    !oldvd->vdev_ops->vdev_op_leaf || oldvd->vdev_children != 0 ||
	    engine->store_codec < 6 || engine->store_codec > 7)
		return (SET_ERROR(ENOTSUP));
	/* One pair at a time, from a direct topology only. */
	if (!engine_store_direct_topology(engine->vd) ||
	    atomic_load_64(&engine->r02_pair_state) != ENGINE_R02_PAIR_NONE)
		return (SET_ERROR(ENOTSUP));
	return (0);
}

/*
 * The seed seam: newvd is created, labelled and geometry-checked but has no
 * parent in the pool; the topology is still direct and nothing is dirty.
 * A failure leaves an orphan candidate and the old configuration intact.
 */
int
pr19262_engine_r02_attach_seed(spa_t *spa, vdev_t *oldvd, vdev_t *newvd)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(RW_WRITE_HELD(&engine->r02_topology_lock));
	ASSERT(spa_config_held(spa, SCL_ALL, RW_WRITER) == SCL_ALL);
	if (newvd == NULL || !newvd->vdev_ops->vdev_op_leaf ||
	    newvd->vdev_children != 0 || newvd->vdev_isspare ||
	    newvd->vdev_ashift > engine->vd->vdev_ashift ||
	    newvd->vdev_psize < oldvd->vdev_psize)
		return (SET_ERROR(ENOTSUP));
	int error = engine_r02_writer_locks(engine);
	if (error != 0)
		return (error);
	error = engine_r02_seed(spa, ENGINE_STORE_LEAF_IO_CONFIG_WRITER, newvd,
	    vdev_readable(oldvd) ? NULL : oldvd, B_TRUE, "attach-seed");
	/*
	 * The old leaf may carry the slot later only if it holds the full
	 * closure itself; a direct member can already lack pages.  Audit it
	 * (reseeding it from the others if needed); otherwise it is stale.
	 */
	boolean_t old_current = B_FALSE;
	if (error == 0 && vdev_writeable(oldvd))
		old_current = engine_r02_seed(spa,
		    ENGINE_STORE_LEAF_IO_CONFIG_WRITER, oldvd, NULL, B_FALSE,
		    "attach-audit-old") == 0;
	atomic_store_64(&engine->r02_attach_old_current, old_current);
	engine_r02_writer_unlock(engine);
	if (error == 0)
		engine_r02_cut(3);
	return (error);
}

/* The verified seed now has its strict replacing parent in the config. */
void
pr19262_engine_r02_attach_commit(spa_t *spa, vdev_t *pvd)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(spa_config_held(spa, SCL_ALL, RW_WRITER) == SCL_ALL);
	VERIFY3P(pvd->vdev_ops, ==, &vdev_replacing_ops);
	VERIFY3P(pvd->vdev_parent, ==, engine->vd);
	VERIFY3U(pvd->vdev_children, ==, 2);
	mutex_enter(&engine->lock);
	engine->r02_pair_slot = pvd->vdev_id;
	engine->r02_pair_old_guid = pvd->vdev_child[0]->vdev_guid;
	engine->r02_pair_new_guid = pvd->vdev_child[1]->vdev_guid;
	atomic_store_64(&engine->r02_pair_writes, 0);
	atomic_store_64(&engine->r02_pair_old_held, 0);
	atomic_store_64(&engine->r02_pair_reads, 0);
	atomic_store_64(&engine->r02_pair_async_reads, 0);
	atomic_store_64(&engine->r02_old_stale,
	    atomic_load_64(&engine->r02_attach_old_current) == 0);
	atomic_store_64(&engine->r02_readmit_retries, 0);
	atomic_store_64(&engine->r02_pair_state, ENGINE_R02_PAIR_ADMITTED);
	mutex_exit(&engine->lock);
	pr19262_printf("PR19262_R02_PAIR_ADMITTED slot=%llu old=%llu "
	    "new=%llu\n", (u_longlong_t)pvd->vdev_id,
	    (u_longlong_t)pvd->vdev_child[0]->vdev_guid,
	    (u_longlong_t)pvd->vdev_child[1]->vdev_guid);
}

/*
 * The final detach gate, before the old leaf's labels are erased.  Returns
 * 0 for a detach that is not of an engine member.  Detaching the exact new
 * leaf cancels the replacement and is always admitted.  Detaching the old
 * leaf (explicitly or from resilver completion) requires a complete exact
 * audit of the new leaf, reseeding it first when the audit fails or the
 * pair was faulted; otherwise it is refused before label erasure.
 */
int
pr19262_engine_r02_detach_check(spa_t *spa, vdev_t *vd, boolean_t gate)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(spa_config_held(spa, SCL_ALL, RW_WRITER) == SCL_ALL);
	vdev_t *pvd = vd->vdev_parent;
	if (!pr19262_engine_persistent(spa) || engine->vd == NULL ||
	    pvd == NULL || pvd->vdev_parent != engine->vd)
		return (0);
	engine_store_member_t member;
	if (!gate || engine_store_member_classify(pvd, &member) != 0 ||
	    member.kind != ENGINE_STORE_MEMBER_REPLACING ||
	    !engine_r02_pair_known(engine, &member))
		return (SET_ERROR(ENOTSUP));
	int error;
	if (vd == member.leaf[1]) {
		/* Cancel: the old leaf must again hold the full closure. */
		if (atomic_load_64(&engine->r02_old_stale) == 0)
			return (0);
		if (!vdev_writeable(member.leaf[0]))
			return (SET_ERROR(EBUSY));
		error = engine_r02_writer_locks(engine);
		if (error != 0)
			return (error);
		error = engine_r02_seed(spa, ENGINE_STORE_LEAF_IO_CONFIG_WRITER,
		    member.leaf[0], NULL, B_TRUE, "cancel-reseed-old");
		engine_r02_writer_unlock(engine);
		return (error);
	}
	error = engine_r02_writer_locks(engine);
	if (error != 0)
		return (error);
	engine_r02_cut(11);
	boolean_t faulted = atomic_load_64(&engine->r02_pair_state) !=
	    ENGINE_R02_PAIR_ADMITTED;
	vdev_t *oldvd = member.leaf[0];
	error = engine_r02_seed(spa, ENGINE_STORE_LEAF_IO_CONFIG_WRITER,
	    member.leaf[1], vdev_readable(oldvd) ? NULL : oldvd, faulted,
	    faulted ? "detach-reseed" : "detach-audit");
	if (error == 0)
		atomic_store_64(&engine->r02_pair_state,
		    ENGINE_R02_PAIR_ADMITTED);
	engine_r02_writer_unlock(engine);
	if (error == 0)
		engine_r02_cut(12);
	return (error);
}

/* The replacing parent collapsed: the remaining leaf is a direct member. */
void
pr19262_engine_r02_detach_commit(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(spa_config_held(spa, SCL_ALL, RW_WRITER) == SCL_ALL);
	if (atomic_load_64(&engine->r02_pair_state) == ENGINE_R02_PAIR_NONE)
		return;
	pr19262_printf("PR19262_R02_PAIR_STATS state=%llu pair_writes=%llu "
	    "old_held=%llu old_stale=%llu faults=%llu reads=%llu "
	    "async_reads=%llu\n",
	    (u_longlong_t)atomic_load_64(&engine->r02_pair_state),
	    (u_longlong_t)atomic_load_64(&engine->r02_pair_writes),
	    (u_longlong_t)atomic_load_64(&engine->r02_pair_old_held),
	    (u_longlong_t)atomic_load_64(&engine->r02_old_stale),
	    (u_longlong_t)atomic_load_64(&engine->r02_pair_faults),
	    (u_longlong_t)atomic_load_64(&engine->r02_pair_reads),
	    (u_longlong_t)atomic_load_64(&engine->r02_pair_async_reads));
	mutex_enter(&engine->lock);
	atomic_store_64(&engine->r02_pair_state, ENGINE_R02_PAIR_NONE);
	engine->r02_pair_old_guid = engine->r02_pair_new_guid = 0;
	engine->r02_pair_slot = 0;
	atomic_store_64(&engine->r02_old_stale, 0);
	atomic_store_64(&engine->r02_readmit_retries, 0);
	atomic_store_64(&engine->r02_collapsed, 1);
	mutex_exit(&engine->lock);
	pr19262_printf("PR19262_R02_PAIR_COLLAPSED\n");
}

void
pr19262_engine_r02_test_cut(uint_t stage)
{
	engine_r02_cut(stage);
}

/*
 * Pre-MOS: remember an imported strict pair as known but frozen, so exact
 * reads and the read-only observer may use it.  Writes stay refused until
 * the writable recovery below reseeds and audits the exact new leaf.
 */
static void
engine_r02_pair_discover(engine_t *engine)
{
	vdev_t *vd = engine->vd;
	uint_t pairs = 0;
	engine_store_member_t found = { 0 };
	uint64_t slot = 0;
	for (uint_t c = 0; vd != NULL && c < vd->vdev_children; c++) {
		engine_store_member_t member;
		if (engine_store_member_classify(vd->vdev_child[c],
		    &member) != 0)
			return;
		if (member.kind == ENGINE_STORE_MEMBER_REPLACING) {
			found = member;
			slot = c;
			pairs++;
		}
	}
	if (pairs != 1)
		return;
	mutex_enter(&engine->lock);
	engine->r02_pair_slot = slot;
	engine->r02_pair_old_guid = found.leaf[0]->vdev_guid;
	engine->r02_pair_new_guid = found.leaf[1]->vdev_guid;
	atomic_store_64(&engine->r02_pair_state, ENGINE_R02_PAIR_FAULTED);
	mutex_exit(&engine->lock);
	pr19262_printf("PR19262_R02_PAIR_IMPORTED slot=%llu frozen=1\n",
	    (u_longlong_t)slot);
}

/*
 * I1: writable import with a replacing pair.  The caller (store recovery)
 * holds SCL_STATE and store_lock; vdevs are open writeable.  Reseed and
 * audit the exact new leaf before any raw publication is admitted.
 */
static int
engine_r02_pair_recover(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->store_lock));
	if (atomic_load_64(&engine->r02_pair_state) == ENGINE_R02_PAIR_NONE)
		return (0);
	/*
	 * The tunable gates new admissions only, never an existing pair: an
	 * engine cannot become persistent over a pair, so a strict pair here
	 * was admitted by this candidate or created by a pre-D64 module whose
	 * seed never ran.  Reseeding the exact new leaf from validated members
	 * is the safe route for both; refusing would only block import.  This
	 * is an intended deviation from "D64 unchanged" when the tunable is 0.
	 */
	vdev_t *pvd = engine->vd->vdev_child[engine->r02_pair_slot];
	engine_store_member_t member;
	if (engine_store_member_classify(pvd, &member) != 0 ||
	    !engine_r02_pair_known(engine, &member) ||
	    !vdev_writeable(member.leaf[1]))
		return (SET_ERROR(ENXIO));
	mutex_enter(&engine->journal_lock);
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	int error = engine_r02_seed(spa, ENGINE_STORE_LEAF_IO_NORMAL,
	    member.leaf[1], NULL, B_TRUE, "import-reseed");
	/*
	 * Writes the old leaf may have missed before the crash are unknown.
	 * A writeable old leaf is audited (and reseeded if needed) so that
	 * it can carry the slot after a later new-leaf fault; at tolerance 0
	 * (uncertified import) that is the only way publication continues.
	 */
	boolean_t old_current = B_FALSE;
	if (error == 0 && vdev_writeable(member.leaf[0]))
		old_current = engine_r02_seed(spa, ENGINE_STORE_LEAF_IO_NORMAL,
		    member.leaf[0], NULL, B_FALSE, "import-audit-old") == 0;
	spa_config_exit(spa, SCL_ZIO, FTAG);
	mutex_exit(&engine->journal_lock);
	if (error == 0) {
		atomic_store_64(&engine->r02_old_stale, !old_current);
		atomic_store_64(&engine->r02_pair_state,
		    ENGINE_R02_PAIR_ADMITTED);
	}
	return (error);
}

/*
 * v3.2 runtime readmit.  Without it a pair whose old leaf was stale (for
 * example offline across an import) and whose new leaf then faulted has
 * no eligible endpoint at tolerance 0 until the next import: every store
 * sync fails and the pool stays suspended through zpool online and clear.
 *
 * spa_vdev_state_exit() calls this after it dropped the vdev state locks
 * (zpool online, offline, clear, fault, reopen, async remove), so a leaf
 * that returned is audited before zpool clear resumes a suspended sync.
 * Only what the slot needs is readmitted: a writeable stale old leaf is
 * audited and, on any audit failure, reseeded from the validated closure
 * (as import-audit-old does); a faulted writeable new leaf is reseeded and
 * readmitted only while the old leaf cannot hold the slot.  A faulted new
 * leaf beside a current old leaf keeps the v3 path (detach-reseed).  Like
 * import recovery this is not gated by zfs_pr19262_r02_reseed: it repairs
 * an existing pair and never admits a new one.
 *
 * Locks, in the attach/detach order: the raw-topology writer (try only:
 * this caller may run on the async thread, which an export waits for under
 * the namespace lock), a try SCL_VDEV reader for the cheap precheck, then
 * SCL_STATE_ALL as writer (it includes SCL_ZIO, so no normal raw I/O is in
 * flight or can start, and no vdev_config_sync is running), then
 * store_lock and journal_lock by trylock.  SCL_CONFIG is not taken (a
 * suspended spa_sync holds it as reader), so this can run between the
 * store attempt and the uberblock of a txg; it is refused while a stored
 * engine root is not yet durable, because the retry would publish that
 * root without rewriting its pages.  A failed store attempt leaves its
 * pages dirty and the retry writes them to every eligible endpoint.
 *
 * The SCL_STATE_ALL writer stalls pool I/O for one closure pass, so the
 * pass runs only when a pair leaf is eligible, and a failed pass is not
 * repeated for the same leaf states within 30 s unless the exit is for one
 * of the pair's leaves (vd).  A failed readmit leaves the state as before;
 * the operator repeats zpool online or zpool clear.  A readmit skipped on
 * a busy lock or a not yet durable root is retried by the async thread
 * after a later txg (engine_r02_readmit_retry(), bounded); one skipped on
 * the backoff is not, since it would repeat a failed pass.
 *
 * Cached slot serials (engine_pages_slots_cached()) may skip a control
 * write only under this invariant: while slot_serial[s] is nonzero, every
 * direct member holds the newest slots[s], and so does every eligible pair
 * leaf (an ADMITTED new leaf, or an old leaf with r02_old_stale == 0).  A
 * missed old-leaf write does not invalidate the cache (it only marks the
 * leaf stale, and zpool clear need not bump the config generation), so the
 * invariant is carried by the seed, not by invalidation: a leaf becomes
 * eligible again only after a pass returns 0, and a pass collects both
 * control slots of every arena (pending ones included) at their newest
 * serial from other leaves only and hash-verifies the target, even when
 * the audit needed no copy.  Equal serials with different bytes fail with
 * ECKSUM and refuse the readmit.  No control write can interleave, since
 * it needs SCL_ZIO and the pass holds the SCL_STATE_ALL writer, store_lock
 * and journal_lock.  This assumes no silent corruption on direct members.
 */
#define	ENGINE_R02_READMIT_BACKOFF	SEC2NSEC(30)
#define	ENGINE_R02_READMIT_RETRIES	8

/*
 * Ask the async thread (dispatched at the end of each spa_sync()) to call
 * the readmit again.  r02_readmit_retries counts the retried skips.  Once
 * the lock-free gate passes, it is zeroed by an exit for a vdev (vd !=
 * NULL) or on a suspended pool (zpool clear passes no vdev there); it is
 * also zeroed by a pass or a not-needed check and when a pair is admitted,
 * collapsed or cleared.  So at most ENGINE_R02_READMIT_RETRIES retries are
 * armed in between, and a suspended pool dispatches no async work, so its
 * reset cannot loop.  The bound is in txgs, not time: a topology writer
 * held across several txgs (an attach or detach waiting in spa_vdev_exit())
 * can outlast it.  A retry is an exit for no leaf: it takes the same try
 * locks and honors the backoff even when the skipped exit was for a pair
 * leaf (which bypasses it); the backoff only matches unchanged leaf
 * states.  A suspended pool completes no sync, so there zpool clear stays
 * the retry.
 */
static void
engine_r02_readmit_retry(spa_t *spa, engine_t *engine, const char *reason)
{
	uint64_t n = atomic_inc_64_nv(&engine->r02_readmit_retries);
	boolean_t armed = n <= ENGINE_R02_READMIT_RETRIES;
	if (armed)
		spa_async_request(spa, SPA_ASYNC_R02_READMIT);
	pr19262_printf("PR19262_R02_READMIT_RETRY reason=%s retry=%llu "
	    "armed=%d\n", reason, (u_longlong_t)n, armed);
}

static uint64_t
engine_r02_readmit_sig(engine_t *engine, vdev_t *oldvd, vdev_t *newvd)
{
	return (oldvd->vdev_guid ^ (newvd->vdev_guid << 1) ^
	    ((uint64_t)oldvd->vdev_state << 56) ^
	    ((uint64_t)newvd->vdev_state << 48) ^
	    (atomic_load_64(&engine->r02_pair_state) << 40) ^
	    (atomic_load_64(&engine->r02_old_stale) << 32));
}

/* Is there a returned pair leaf the slot needs?  Caller: SCL_VDEV+. */
static boolean_t
engine_r02_readmit_need(engine_t *engine, engine_store_member_t *member)
{
	uint64_t state = atomic_load_64(&engine->r02_pair_state);
	if (state == ENGINE_R02_PAIR_NONE || engine->vd == NULL ||
	    engine->r02_pair_slot >= engine->vd->vdev_children)
		return (B_FALSE);
	vdev_t *pvd = engine->vd->vdev_child[engine->r02_pair_slot];
	if (engine_store_member_classify(pvd, member) != 0 ||
	    !engine_r02_pair_known(engine, member) ||
	    atomic_load_64(&engine->r02_old_stale) == 0)
		return (B_FALSE);
	return (vdev_writeable(member->leaf[0]) ||
	    (state == ENGINE_R02_PAIR_FAULTED &&
	    vdev_writeable(member->leaf[1])));
}

void
pr19262_engine_r02_readmit(spa_t *spa, vdev_t *vd)
{
	engine_t *engine = spa->spa_reloc;
	if (engine == NULL || !spa_writeable(spa) ||
	    spa->spa_load_state != SPA_LOAD_NONE ||
	    spa_get_dsl(spa) == NULL ||
	    dsl_pool_sync_context(spa_get_dsl(spa)) ||
	    !pr19262_engine_persistent(spa))
		return;
	/* Lock-free gate: only a stale old leaf can leave the slot unheld. */
	if (atomic_load_64(&engine->r02_pair_state) == ENGINE_R02_PAIR_NONE ||
	    atomic_load_64(&engine->r02_old_stale) == 0)
		return;
	/* An explicit state exit restarts the bounded automatic retries. */
	if (vd != NULL || spa_suspended(spa))
		atomic_store_64(&engine->r02_readmit_retries, 0);
	if (!rw_tryenter(&engine->r02_topology_lock, RW_WRITER)) {
		pr19262_printf("PR19262_R02_READMIT result=skipped "
		    "reason=topology-busy\n");
		engine_r02_readmit_retry(spa, engine, "topology-busy");
		return;
	}
	engine_store_member_t member;
	if (!spa_config_tryenter(spa, SCL_VDEV, FTAG, RW_READER)) {
		rw_exit(&engine->r02_topology_lock);
		pr19262_printf("PR19262_R02_READMIT result=skipped "
		    "reason=config-busy\n");
		engine_r02_readmit_retry(spa, engine, "config-busy");
		return;
	}
	boolean_t need = engine_r02_readmit_need(engine, &member);
	uint64_t sig = need ?
	    engine_r02_readmit_sig(engine, member.leaf[0], member.leaf[1]) : 0;
	spa_config_exit(spa, SCL_VDEV, FTAG);
	if (!need) {
		atomic_store_64(&engine->r02_readmit_retries, 0);
		rw_exit(&engine->r02_topology_lock);
		return;
	}
	/* An explicit state change of a pair leaf always gets a pass. */
	if (vd != NULL && (vd == member.leaf[0] || vd == member.leaf[1]))
		engine->r02_readmit_fail_sig = 0;
	if (engine->r02_readmit_fail_sig == sig &&
	    gethrtime() - engine->r02_readmit_fail_time <
	    ENGINE_R02_READMIT_BACKOFF) {
		rw_exit(&engine->r02_topology_lock);
		pr19262_printf("PR19262_R02_READMIT result=skipped "
		    "reason=backoff\n");
		return;
	}
	int error = EBUSY;
	for (int attempt = 0; attempt < 10; attempt++) {
		spa_config_enter(spa, SCL_STATE_ALL, FTAG, RW_WRITER);
		error = engine_r02_writer_locks(engine);
		if (error == 0)
			break;
		spa_config_exit(spa, SCL_STATE_ALL, FTAG);
		delay(MSEC_TO_TICK(50));
	}
	if (error != 0) {
		rw_exit(&engine->r02_topology_lock);
		pr19262_printf("PR19262_R02_READMIT result=skipped "
		    "reason=store-busy\n");
		engine_r02_readmit_retry(spa, engine, "store-busy");
		return;
	}
	const char *skip = NULL;
	int old_error = 0, new_error = 0;
	const char *old_op = "none", *new_op = "none";
	const uberblock_t *ub = &spa->spa_uberblock;
	mutex_enter(&engine->lock);
	boolean_t unpublished = pr19262_engine_store_ub(ub) &&
	    ub->ub_pr19262[6] != engine->durable_catalog_txg;
	mutex_exit(&engine->lock);
	boolean_t retry = B_FALSE;
	if (!engine_r02_readmit_need(engine, &member)) {
		skip = "not-needed";
	} else if (unpublished) {
		skip = "unpublished-root";
		retry = B_TRUE;
	}
	if (!retry)
		atomic_store_64(&engine->r02_readmit_retries, 0);
	if (skip == NULL) {
		vdev_t *oldvd = member.leaf[0], *newvd = member.leaf[1];
		sig = engine_r02_readmit_sig(engine, oldvd, newvd);
		if (vdev_writeable(oldvd)) {
			old_error = engine_r02_seed(spa,
			    ENGINE_STORE_LEAF_IO_CONFIG_WRITER, oldvd,
			    vdev_readable(newvd) ? NULL : newvd, B_FALSE,
			    "readmit-audit-old");
			old_op = old_error == 0 ? "ok" : "failed";
			if (old_error == 0)
				atomic_store_64(&engine->r02_old_stale, 0);
		}
		if (atomic_load_64(&engine->r02_pair_state) ==
		    ENGINE_R02_PAIR_FAULTED &&
		    atomic_load_64(&engine->r02_old_stale) != 0 &&
		    vdev_writeable(newvd)) {
			new_error = engine_r02_seed(spa,
			    ENGINE_STORE_LEAF_IO_CONFIG_WRITER, newvd,
			    vdev_readable(oldvd) ? NULL : oldvd, B_TRUE,
			    "readmit-reseed-new");
			new_op = new_error == 0 ? "ok" : "failed";
			if (new_error == 0) {
				atomic_store_64(&engine->r02_pair_state,
				    ENGINE_R02_PAIR_ADMITTED);
				pr19262_printf("PR19262_R02_PAIR_READMITTED "
				    "slot=%llu new=%llu\n",
				    (u_longlong_t)engine->r02_pair_slot,
				    (u_longlong_t)newvd->vdev_guid);
			}
		}
		boolean_t failed = old_error != 0 || new_error != 0;
		engine->r02_readmit_fail_sig = failed ? sig : 0;
		engine->r02_readmit_fail_time = failed ? gethrtime() : 0;
	}
	pr19262_printf("PR19262_R02_READMIT result=%s reason=%s old_op=%s "
	    "old_error=%d new_op=%s new_error=%d state=%llu old_stale=%llu\n",
	    skip == NULL ? "done" : "skipped", skip == NULL ? "none" : skip,
	    old_op, old_error, new_op, new_error,
	    (u_longlong_t)atomic_load_64(&engine->r02_pair_state),
	    (u_longlong_t)atomic_load_64(&engine->r02_old_stale));
	engine_r02_writer_unlock(engine);
	spa_config_exit(spa, SCL_STATE_ALL, FTAG);
	rw_exit(&engine->r02_topology_lock);
	if (retry)
		engine_r02_readmit_retry(spa, engine, skip);
}

/*
 * L23 (A35): member return.  Every raw page goes to every member, and a
 * read takes any valid copy, so a direct member that missed raw writes
 * (offline, faulted or absent while the store synced, or a failed write or
 * flush; engine_member_missed()) comes back without the pages written
 * meanwhile, and the store keeps one copy fewer of them until a writable
 * import repairs present members (engine_pages_resilver_members(), journal
 * repair).  On a narrow vdev (width <= the tolerated absences plus the
 * parity) a later loss of the members that hold them leaves no copy.
 *
 * spa_vdev_state_exit() calls this after pr19262_engine_r02_readmit(), and
 * the async thread retries it: every marked direct member that is readable
 * and writeable again is audited against the validated R02 closure (every
 * retained root's catalog, tree and directory pages, both control slots of
 * every arena at their newest serial, the journal HEAD slots and BIND and
 * FORWARD prefix; the member audit's set) and, on any audit failure,
 * reseeded from the other members and audited again
 * (engine_r02_seed_leaf()).  Its mark is cleared only by a pass that
 * returned 0.  The pass writes only closure pages, each at its own place,
 * on the returning member; it changes no root, serial, allocation or
 * catalog row, so a cut at any point leaves that member with at most the
 * copies it lacked.  The locks, the order and the unpublished-root refusal
 * are the readmit's (see there): no raw I/O, store or control publication
 * interleaves, and a pair slot is left to the R02 code.  The closure reads
 * the labels of every leaf of the POOL (pr19262_vdev_uberblocks_r02()): a
 * cheap precheck under the SCL_VDEV reader skips the pass, without the
 * writer, while any such leaf is unreadable or not a leaf or strict pair,
 * and the member keeps its mark until a later state exit or the next
 * writable import.  A failed pass is not repeated for the same marks and
 * leaf states within ENGINE_R02_READMIT_BACKOFF unless the exit is for a
 * marked member.  A busy skip or an unpublished root arms
 * SPA_ASYNC_MEMBER_READMIT, at most ENGINE_R02_READMIT_RETRIES times
 * between resets; so does a new mark of a member that stays usable.
 */
static void
engine_member_readmit_retry(spa_t *spa, engine_t *engine, const char *reason)
{
	uint64_t n = atomic_inc_64_nv(&engine->member_readmit_retries);
	boolean_t armed = n <= ENGINE_R02_READMIT_RETRIES;
	if (armed)
		spa_async_request(spa, SPA_ASYNC_MEMBER_READMIT);
	pr19262_printf("PR19262_MEMBER_READMIT result=skipped reason=%s "
	    "retry=%llu armed=%d\n", reason, (u_longlong_t)n, armed);
}

/*
 * Mirror the closure's label walk (pr19262_vdev_uberblocks_impl() with
 * pairs): NULL when every leaf it needs is readable, else why not.
 */
static const char *
engine_member_closure_blocked(spa_t *spa)
{
	vdev_t *root = spa->spa_root_vdev;
	for (uint_t t = 0; t < root->vdev_children; t++) {
		vdev_t *top = root->vdev_child[t];
		/* A hole has no labels; the walk skips it too. */
		if (top->vdev_ishole)
			continue;
		if (top->vdev_ops->vdev_op_leaf) {
			if (!vdev_readable(top))
				return ("leaf-unreadable");
			continue;
		}
		for (uint_t c = 0; c < top->vdev_children; c++) {
			vdev_t *leaf = top->vdev_child[c];
			/* A strict pair: the old leaf is optional. */
			if (leaf->vdev_ops == &vdev_replacing_ops &&
			    leaf->vdev_children == 2 &&
			    leaf->vdev_child[0]->vdev_ops->vdev_op_leaf &&
			    leaf->vdev_child[0]->vdev_children == 0 &&
			    leaf->vdev_child[1]->vdev_ops->vdev_op_leaf &&
			    leaf->vdev_child[1]->vdev_children == 0)
				leaf = leaf->vdev_child[1];
			if (!leaf->vdev_ops->vdev_op_leaf)
				return ("topology");
			if (!vdev_readable(leaf))
				return ("leaf-unreadable");
		}
	}
	return (NULL);
}

/* The marks and every engine-member leaf state, for the backoff. */
static uint64_t
engine_member_readmit_sig(engine_t *engine, const uint64_t *want)
{
	uint64_t sig = 0;
	for (uint_t i = 0; i < ENGINE_MEMBER_WORDS; i++)
		sig ^= want[i] * (2 * i + 1);
	for (uint_t c = 0; c < engine->vd->vdev_children; c++) {
		engine_store_member_t m;
		if (engine_store_member_classify(engine->vd->vdev_child[c],
		    &m) != 0)
			continue;
		for (uint_t k = 0; k < 2 && m.leaf[k] != NULL; k++)
			sig ^= (m.leaf[k]->vdev_guid << (c % 7)) ^
			    ((uint64_t)m.leaf[k]->vdev_state << (40 + k * 8));
	}
	return (sig);
}

/*
 * The marked direct members that can take a pass now.  Caller: an SCL_VDEV
 * reader, or the SCL_STATE_ALL writer with the raw-topology writer.
 */
static boolean_t
engine_member_readmit_need(engine_t *engine, uint64_t *want)
{
	boolean_t need = B_FALSE;
	memset(want, 0, ENGINE_MEMBER_WORDS * sizeof (uint64_t));
	if (engine->vd == NULL)
		return (B_FALSE);
	for (uint_t c = 0; c < engine->vd->vdev_children &&
	    c < ENGINE_MEMBER_BITS; c++) {
		uint64_t bit = 1ULL << (c % 64);
		vdev_t *leaf;
		if ((atomic_load_64(&engine->member_missed[c / 64]) & bit) ==
		    0 || engine_store_member_direct(engine->vd->vdev_child[c],
		    &leaf) != 0 || !vdev_readable(leaf) ||
		    !vdev_writeable(leaf))
			continue;
		want[c / 64] |= bit;
		need = B_TRUE;
	}
	return (need);
}

void
pr19262_engine_member_readmit(spa_t *spa, vdev_t *vd)
{
	engine_t *engine = spa->spa_reloc;
	if (engine == NULL || !spa_writeable(spa) ||
	    spa->spa_load_state != SPA_LOAD_NONE ||
	    spa_get_dsl(spa) == NULL ||
	    dsl_pool_sync_context(spa_get_dsl(spa)) ||
	    !pr19262_engine_persistent(spa))
		return;
	/* Lock-free gate: no direct member missed a raw write. */
	uint64_t any = 0;
	for (uint_t i = 0; i < ENGINE_MEMBER_WORDS; i++)
		any |= atomic_load_64(&engine->member_missed[i]);
	if (any == 0 ||
	    !engine_r02_manifest_codec_supported(engine->store_codec))
		return;
	if (vd != NULL || spa_suspended(spa))
		atomic_store_64(&engine->member_readmit_retries, 0);
	if (!rw_tryenter(&engine->r02_topology_lock, RW_WRITER)) {
		engine_member_readmit_retry(spa, engine, "topology-busy");
		return;
	}
	uint64_t want[ENGINE_MEMBER_WORDS];
	if (!spa_config_tryenter(spa, SCL_VDEV, FTAG, RW_READER)) {
		rw_exit(&engine->r02_topology_lock);
		engine_member_readmit_retry(spa, engine, "config-busy");
		return;
	}
	boolean_t need = engine_member_readmit_need(engine, want);
	const char *blocked = need ? engine_member_closure_blocked(spa) : NULL;
	uint64_t sig = need ? engine_member_readmit_sig(engine, want) : 0;
	boolean_t marked_exit = B_FALSE;
	if (vd != NULL && vd->vdev_parent == engine->vd &&
	    vd->vdev_id < ENGINE_MEMBER_BITS)
		marked_exit = (want[vd->vdev_id / 64] &
		    (1ULL << (vd->vdev_id % 64))) != 0;
	spa_config_exit(spa, SCL_VDEV, FTAG);
	if (!need) {
		atomic_store_64(&engine->member_readmit_retries, 0);
		rw_exit(&engine->r02_topology_lock);
		return;
	}
	if (blocked != NULL || (!marked_exit &&
	    engine->member_readmit_fail_sig == sig &&
	    gethrtime() - engine->member_readmit_fail_time <
	    ENGINE_R02_READMIT_BACKOFF)) {
		rw_exit(&engine->r02_topology_lock);
		pr19262_printf("PR19262_MEMBER_READMIT result=skipped "
		    "reason=%s\n", blocked != NULL ? blocked : "backoff");
		return;
	}
	int error = EBUSY;
	for (int attempt = 0; attempt < 10; attempt++) {
		spa_config_enter(spa, SCL_STATE_ALL, FTAG, RW_WRITER);
		error = engine_r02_writer_locks(engine);
		if (error == 0)
			break;
		spa_config_exit(spa, SCL_STATE_ALL, FTAG);
		delay(MSEC_TO_TICK(50));
	}
	if (error != 0) {
		rw_exit(&engine->r02_topology_lock);
		engine_member_readmit_retry(spa, engine, "store-busy");
		return;
	}
	const char *skip = NULL;
	const uberblock_t *ub = &spa->spa_uberblock;
	mutex_enter(&engine->lock);
	boolean_t unpublished = pr19262_engine_store_ub(ub) &&
	    ub->ub_pr19262[6] != engine->durable_catalog_txg;
	mutex_exit(&engine->lock);
	boolean_t retry = B_FALSE;
	if (!engine_member_readmit_need(engine, want)) {
		skip = "not-needed";
	} else if (unpublished) {
		skip = "unpublished-root";
		retry = B_TRUE;
	}
	if (!retry)
		atomic_store_64(&engine->member_readmit_retries, 0);
	uint_t passed = 0, failed = 0;
	if (skip == NULL) {
		engine_r02_manifest_private_t m;
		engine_r02_manifest_init(&m, spa,
		    ENGINE_STORE_LEAF_IO_CONFIG_WRITER, B_TRUE);
		error = engine_r02_collect_locked(&m, NULL);
		for (uint_t c = 0; c < engine->vd->vdev_children &&
		    c < ENGINE_MEMBER_BITS; c++) {
			uint64_t bit = 1ULL << (c % 64);
			if ((want[c / 64] & bit) == 0)
				continue;
			engine_r02_seed_stats_t st = { 0 };
			vdev_t *leaf = engine->vd->vdev_child[c];
			const char *op = "audit";
			int e = error;
			if (e == 0)
				e = engine_r02_seed_leaf(&m, leaf, B_FALSE,
				    &st);
			if (e != 0 && error == 0) {
				op = "reseed";
				e = engine_r02_seed_leaf(&m, leaf, B_TRUE,
				    &st);
			}
			if (e == 0) {
				atomic_and_64(&engine->member_missed[c / 64],
				    ~bit);
				passed++;
			} else {
				failed++;
			}
			pr19262_printf("PR19262_MEMBER_RESEED slot=%u op=%s "
			    "error=%d entries=%llu copied=%llu verified=%llu\n",
			    c, op, e, (u_longlong_t)st.entries,
			    (u_longlong_t)st.copied, (u_longlong_t)st.verified);
		}
		engine_r02_manifest_fini(&m);
		/* Under the topology writer, as the backoff check. */
		engine->member_readmit_fail_sig = failed != 0 ? sig : 0;
		engine->member_readmit_fail_time = failed != 0 ?
		    gethrtime() : 0;
	}
	pr19262_printf("PR19262_MEMBER_READMIT result=%s reason=%s "
	    "collect_error=%d passed=%u failed=%u\n",
	    skip == NULL ? "done" : "skipped", skip == NULL ? "none" : skip,
	    skip == NULL ? error : 0, passed, failed);
	engine_r02_writer_unlock(engine);
	spa_config_exit(spa, SCL_STATE_ALL, FTAG);
	rw_exit(&engine->r02_topology_lock);
	if (retry)
		engine_member_readmit_retry(spa, engine, skip);
}

#ifndef _KERNEL
static void
engine_r02_extent_print(void *arg, uint64_t start, uint64_t size)
{
	pr19262_printf("PR19262_R02_RAW_EXTENT kind=%s start=%llu size=%llu\n",
	    (const char *)arg, (u_longlong_t)start, (u_longlong_t)size);
}

/*
 * ztest: the engine's raw-store reservations (allocator state, not the
 * closure walker), so a host check can erase them on chosen members.
 */
int
pr19262_engine_r02_raw_extents(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	int error = engine->persistent ? 0 : ENOTSUP;
	if (error == 0) {
		pr19262_printf("PR19262_R02_RAW_GEOMETRY children=%llu "
		    "label_start=%llu\n",
		    (u_longlong_t)engine->vd->vdev_children,
		    (u_longlong_t)VDEV_LABEL_START_SIZE);
		for (uint_t c = 0; c < engine->vd->vdev_children; c++) {
			engine_store_member_t member;
			if (engine_store_member_classify(
			    engine->vd->vdev_child[c], &member) != 0)
				continue;
			for (uint_t k = 0; k < 2 && member.leaf[k] != NULL;
			    k++)
				pr19262_printf("PR19262_R02_RAW_MEMBER slot=%u "
				    "leaf=%u path=%s\n", c, k,
				    member.leaf[k]->vdev_path);
		}
		zfs_range_tree_walk(engine->store_extents,
		    engine_r02_extent_print, (void *)"store");
		zfs_range_tree_walk(engine->journal_extents,
		    engine_r02_extent_print, (void *)"journal");
	}
	mutex_exit(&engine->lock);
	return (error);
}

/*
 * ztest: independent per-member completeness.  Every concrete leaf of every
 * member slot is read on its own (no first-valid-copy selection) against
 * each byte object of the validated closure.
 */
int
pr19262_engine_r02_member_audit(spa_t *spa, uint64_t *missing_total,
    uint64_t *checked_total)
{
	engine_t *engine = spa->spa_reloc;
	*missing_total = *checked_total = 0;
	rw_enter(&engine->r02_topology_lock, RW_READER);
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine->store_lock);
	mutex_enter(&engine->journal_lock);
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	engine_r02_manifest_private_t m;
	engine_r02_manifest_init(&m, spa, ENGINE_STORE_LEAF_IO_NORMAL, B_TRUE);
	int error = engine_r02_collect_locked(&m, NULL);
	abd_t *got = abd_alloc_linear(ENGINE_STORE_PAGE, B_TRUE);
	for (uint_t c = 0; error == 0 && c < engine->vd->vdev_children; c++) {
		engine_store_member_t member;
		error = engine_store_member_classify(engine->vd->vdev_child[c],
		    &member);
		uint_t leaves = member.kind == ENGINE_STORE_MEMBER_LEAF ? 1 : 2;
		for (uint_t k = 0; error == 0 && k < leaves; k++) {
			vdev_t *leaf = member.leaf[k];
			uint64_t checked = 0, missing = 0;
			for (engine_r02_entry_t *e = list_head(&m.entries);
			    e != NULL; e = list_next(&m.entries, e)) {
				if (engine_r02_entry_rank(e->entry.kind) < 0)
					continue;
				checked++;
				if (engine_store_leaf_io(engine, leaf,
				    e->entry.physical, 0, 1, got, B_FALSE,
				    ENGINE_STORE_LEAF_IO_NORMAL) != 0 ||
				    !engine_r02_entry_valid(&e->entry, got))
					missing++;
			}
			pr19262_printf("PR19262_R02_MEMBER_AUDIT slot=%u "
			    "leaf=%u path=%s checked=%llu missing=%llu\n", c, k,
			    leaf->vdev_path != NULL ? leaf->vdev_path : "-",
			    (u_longlong_t)checked, (u_longlong_t)missing);
			*missing_total += missing;
			*checked_total += checked;
		}
	}
	abd_free(got);
	engine_r02_manifest_fini(&m);
	spa_config_exit(spa, SCL_ZIO, FTAG);
	mutex_exit(&engine->journal_lock);
	mutex_exit(&engine->store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	rw_exit(&engine->r02_topology_lock);
	return (error);
}

/* ztest: exact audit of the new leaf without changing any state. */
int
pr19262_engine_r02_pair_audit(spa_t *spa, uint64_t *state,
    uint64_t *faults, uint64_t *seeds, uint64_t *audits)
{
	engine_t *engine = spa->spa_reloc;
	rw_enter(&engine->r02_topology_lock, RW_READER);
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	mutex_enter(&engine->store_lock);
	mutex_enter(&engine->journal_lock);
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	int error = ENOENT;
	*state = atomic_load_64(&engine->r02_pair_state);
	if (*state != ENGINE_R02_PAIR_NONE) {
		engine_store_member_t member;
		vdev_t *pvd = engine->vd->vdev_child[engine->r02_pair_slot];
		error = engine_store_member_classify(pvd, &member);
		if (error == 0 && !engine_r02_pair_known(engine, &member))
			error = ENOTSUP;
		if (error == 0) {
			engine_r02_manifest_private_t m;
			engine_r02_seed_stats_t st;
			pr19262_r02_manifest_t counts = { 0 };
			engine_r02_manifest_init(&m, spa,
			    ENGINE_STORE_LEAF_IO_NORMAL, B_TRUE);
			error = engine_r02_collect_locked(&m, NULL);
			if (error == 0) {
				engine_r02_manifest_counts(&m, &counts);
				error = engine_r02_seed_leaf(&m,
				    member.leaf[1], B_FALSE, &st);
				engine_r02_seed_print("test-audit", error, &st,
				    &counts);
			}
			engine_r02_manifest_fini(&m);
		}
	}
	*faults = atomic_load_64(&engine->r02_pair_faults);
	*seeds = engine->r02_pair_seeds;
	*audits = engine->r02_pair_audits;
	spa_config_exit(spa, SCL_ZIO, FTAG);
	mutex_exit(&engine->journal_lock);
	mutex_exit(&engine->store_lock);
	spa_config_exit(spa, SCL_STATE, FTAG);
	rw_exit(&engine->r02_topology_lock);
	return (error);
}
#endif
