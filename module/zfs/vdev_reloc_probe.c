/* SPDX-License-Identifier: CDDL-1.0 */
/*
 * Private userspace lifetime feasibility component, included by zio.c.
 * Enrollment is explicit and finite, from a test fixture. Neither final-free
 * hooks nor stable on-disk incarnation identities are implemented here.
 * Recovery-floor arguments below are supplied assumptions, not disk proofs.
 */
#include <sys/vdev_reloc_probe.h>
#include <sys/vdev_reloc_engine.h>

#define	PROBE_ENTRIES	64
#define	PROBE_BACKINGS	4

typedef struct probe_backing {
	pr19262_backing_desc_t desc;
	uint64_t readers;
	boolean_t reclaimed;
} probe_backing_t;

typedef struct probe_entry {
	spa_t *spa;
	uint64_t offset;
	uint64_t birth;
	uint64_t incarnation;
	uint64_t handles;
	uint64_t writers;
	boolean_t live;
	boolean_t copying;
	boolean_t fresh;
	uint_t cur;
	uint_t count;
	probe_backing_t backing[PROBE_BACKINGS];
	pr19262_backing_desc_t prepared;
} probe_entry_t;

typedef struct probe_handle {
	probe_entry_t *entry;
	uint64_t incarnation;
	probe_backing_t *leased;
} probe_handle_t;

static kmutex_t probe_lock;
static probe_entry_t probe_entries[PROBE_ENTRIES];
static uint_t probe_count;
#ifndef _KERNEL
static boolean_t probe_capture_done;
#endif
static boolean_t probe_block_new_holds;

void
pr19262_reloc_init(void)
{
	mutex_init(&probe_lock, NULL, MUTEX_DEFAULT, NULL);
	const char *p = pr19262_env("PR19262_PROBE_MAP");
	if (pr19262_env("PR19262_LIFETIME") == NULL || p == NULL)
		return;
	while (*p != '\0') {
		u_longlong_t src, birth, dst, newbirth, oldsize, newsize;
		int used = 0;
		VERIFY3S(sscanf(p, "%llx,%llu,%llx,%llu,%llx,%llx%n",
		    &src, &birth, &dst, &newbirth, &oldsize, &newsize,
		    &used), ==, 6);
		VERIFY3U(probe_count, <, PROBE_ENTRIES);
		probe_entry_t *e = &probe_entries[probe_count++];
		e->offset = src;
		e->birth = birth;
		e->incarnation = 1;
		e->live = B_TRUE;
		e->count = 1;
		e->backing[0].desc = (pr19262_backing_desc_t) {
			src, birth, oldsize };
		e->prepared = (pr19262_backing_desc_t) {
			dst, newbirth, newsize };
		p += used;
		if (*p == ';')
			p++;
		else
			VERIFY0(*p);
	}
}

void
pr19262_reloc_fini(void)
{
	for (uint_t i = 0; i < probe_count; i++) {
		VERIFY0(probe_entries[i].handles);
		VERIFY0(probe_entries[i].writers);
		VERIFY(!probe_entries[i].copying);
		for (uint_t j = 0; j < probe_entries[i].count; j++)
			VERIFY0(probe_entries[i].backing[j].readers);
	}
	mutex_destroy(&probe_lock);
}

void *
pr19262_reloc_hold(spa_t *spa, const blkptr_t *bp)
{
	if (bp == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) ||
	    BP_IS_GANG(bp) || BP_GET_DEDUP(bp) || BP_GET_NDVAS(bp) != 1 ||
	    DVA_GET_VDEV(&bp->blk_dva[0]) != 0)
		return (NULL);
	probe_handle_t *h = NULL;
	mutex_enter(&probe_lock);
	if (probe_block_new_holds) {
		mutex_exit(&probe_lock);
		return (NULL);
	}
	for (uint_t i = 0; i < probe_count; i++) {
		probe_entry_t *e = &probe_entries[i];
		if (e->offset != DVA_GET_OFFSET(&bp->blk_dva[0]) ||
		    e->birth != BP_GET_PHYSICAL_BIRTH(bp) ||
		    (e->spa != NULL && e->spa != spa))
			continue;
		/* Dead entries still get a handle, so reads fail closed. */
		e->spa = spa;
		h = kmem_zalloc(sizeof (*h), KM_SLEEP);
		h->entry = e;
		h->incarnation = e->incarnation;
		e->handles++;
#ifndef _KERNEL
		const char *capture = pr19262_env("PR19262_CAPTURE_BP");
		if (capture != NULL && !probe_capture_done) {
			FILE *f = fopen(capture, "wb");
			VERIFY3P(f, !=, NULL);
			VERIFY3U(fwrite(bp, sizeof (*bp), 1, f), ==, 1);
			VERIFY0(fclose(f));
			probe_capture_done = B_TRUE;
		}
#endif
		break;
	}
	mutex_exit(&probe_lock);
	return (h);
}

void
pr19262_reloc_rele(void *arg)
{
	probe_handle_t *h = arg;
	if (h == NULL)
		return;
	mutex_enter(&probe_lock);
	VERIFY3U(h->entry->incarnation, ==, h->incarnation);
	VERIFY3U(h->entry->handles, >, 0);
	h->entry->handles--;
	if (h->leased != NULL) {
		VERIFY3U(h->leased->readers, >, 0);
		h->leased->readers--;
	}
	mutex_exit(&probe_lock);
	kmem_free(h, sizeof (*h));
}

boolean_t
pr19262_reloc_known(spa_t *spa, const blkptr_t *bp)
{
	if (bp == NULL || BP_IS_EMBEDDED(bp) || BP_GET_NDVAS(bp) != 1 ||
	    DVA_GET_VDEV(&bp->blk_dva[0]) != 0)
		return (B_FALSE);
	boolean_t found = B_FALSE;
	mutex_enter(&probe_lock);
	for (uint_t i = 0; i < probe_count; i++) {
		probe_entry_t *e = &probe_entries[i];
		if (e->offset == DVA_GET_OFFSET(&bp->blk_dva[0]) &&
		    e->birth == BP_GET_PHYSICAL_BIRTH(bp) &&
		    (e->spa == NULL || e->spa == spa))
			found = B_TRUE;
	}
	mutex_exit(&probe_lock);
	return (found);
}

boolean_t
pr19262_reloc_fresh_write(zio_t *zio)
{
	probe_handle_t *h = zio->io_logical != NULL ?
	    zio->io_logical->io_pr19262_hold : NULL;
	return (h != NULL && h->entry->fresh &&
	    pr19262_env("PR19262_SPACE_TEST") != NULL);
}

int
pr19262_reloc_resolve(zio_t *zio, pr19262_backing_desc_t *desc)
{
	zio_t *logical = zio->io_logical;
	probe_handle_t *h = logical != NULL ?
	    logical->io_pr19262_hold : zio->io_pr19262_hold;
	if (h == NULL)
		return (ENOENT);
	mutex_enter(&probe_lock);
	probe_entry_t *e = h->entry;
	if (e->incarnation != h->incarnation ||
	    (!e->live && h->leased == NULL)) {
		mutex_exit(&probe_lock);
		return (ESTALE);
	}
	if (h->leased == NULL) {
		h->leased = &e->backing[e->cur];
		VERIFY(!h->leased->reclaimed);
		h->leased->readers++;
	}
	*desc = h->leased->desc;
	mutex_exit(&probe_lock);
	return (0);
}

#ifndef _KERNEL
/* The rest of this file is the userspace feasibility harness. */
static int
probe_copy_begin(probe_entry_t *e)
{
	int error = 0;
	mutex_enter(&probe_lock);
	if (!e->live)
		error = ESTALE;
	else if (e->copying || e->writers != 0)
		error = EBUSY;
	else
		e->copying = B_TRUE;
	mutex_exit(&probe_lock);
	return (error);
}

static int
probe_writer_enter(probe_entry_t *e)
{
	int error = 0;
	mutex_enter(&probe_lock);
	if (!e->live)
		error = ESTALE;
	else if (e->copying)
		error = EBUSY;
	else
		e->writers++;
	mutex_exit(&probe_lock);
	return (error);
}

static int
probe_publish(probe_entry_t *e, pr19262_backing_desc_t desc,
    boolean_t verified)
{
	int error = 0;
	mutex_enter(&probe_lock);
	if (!e->live || !e->copying)
		error = EINVAL;
	else if (!verified)
		error = EIO;
	else if (e->count == PROBE_BACKINGS)
		error = ENOSPC;
	else {
		e->cur = e->count++;
		e->backing[e->cur].desc = desc;
		e->copying = B_FALSE;
	}
	mutex_exit(&probe_lock);
	return (error);
}

static int
probe_reclaim(probe_entry_t *e, uint_t index, boolean_t recovery_floor)
{
	int error = 0;
	mutex_enter(&probe_lock);
	if (index >= e->count || (e->live && index == e->cur) ||
	    e->backing[index].reclaimed)
		error = EINVAL;
	else if (!recovery_floor)
		error = EAGAIN;
	else if (e->backing[index].readers != 0)
		error = EBUSY;
	else
		e->backing[index].reclaimed = B_TRUE;
	mutex_exit(&probe_lock);
	return (error);
}

static int
probe_reuse(probe_entry_t *e)
{
	int error = 0;
	mutex_enter(&probe_lock);
	if (e->live || e->copying || e->writers != 0 || e->handles != 0)
		error = EBUSY;
	else {
		for (uint_t i = 0; i < e->count; i++)
			if (!e->backing[i].reclaimed)
				error = EBUSY;
		if (error == 0) {
			e->incarnation++;
			e->live = B_TRUE;
			e->count = 1;
			e->cur = 0;
			e->backing[0] = (probe_backing_t) {
				.desc = e->prepared };
		}
	}
	mutex_exit(&probe_lock);
	return (error);
}

/* Deterministic rendezvous with the disposable-VM fixture controller. */
static void
probe_gate(const char *name)
{
	char path[1024];
	const char *dir = pr19262_env("PR19262_LIFETIME_GATES");
	VERIFY3P(dir, !=, NULL);
	(void) snprintf(path, sizeof (path), "%s/%s.ready", dir, name);
	FILE *f = fopen(path, "w");
	VERIFY3P(f, !=, NULL);
	VERIFY0(fclose(f));
	(void) snprintf(path, sizeof (path), "%s/%s.continue", dir, name);
	for (int i = 0; i < 3000; i++) {
		if (access(path, F_OK) == 0)
			return;
		(void) usleep(10000);
	}
	VERIFY0(!"probe controller timeout");
}

static zio_t *
probe_read(spa_t *spa, const blkptr_t *bp, abd_t *abd)
{
	return (zio_read(NULL, spa, bp, abd, BP_GET_PSIZE(bp), NULL, NULL,
	    ZIO_PRIORITY_SYNC_READ, ZIO_FLAG_CANFAIL | ZIO_FLAG_RAW |
	    ZIO_FLAG_SPECULATIVE, NULL));
}

static void
probe_save_abd(abd_t *abd, uint64_t size, const char *path)
{
	void *buf = abd_borrow_buf_copy(abd, size);
	FILE *f = fopen(path, "wb");
	VERIFY3P(f, !=, NULL);
	VERIFY3U(fwrite(buf, size, 1, f), ==, 1);
	VERIFY0(fclose(f));
	abd_return_buf_copy(abd, buf, size);
}

static int
probe_space_test(spa_t *spa, const char *output, const blkptr_t *bp)
{
	const char *guid = pr19262_env("PR19262_SPACE_RW_GUID");
	VERIFY3P(guid, !=, NULL);
	VERIFY3U(strtoull(guid, NULL, 10), ==, spa_guid(spa));
	VERIFY(!spa_writeable(spa));
	blkptr_t original = *bp;
	abd_t *old_data = abd_alloc_for_io(BP_GET_PSIZE(bp), B_FALSE);
	zio_t *queued = probe_read(spa, bp, old_data);
	probe_entry_t *e = ((probe_handle_t *)queued->io_pr19262_hold)->entry;
	void *space = pr19262_space_create(spa);
	uint64_t v0 = pr19262_space_available(space, B_FALSE);
	uint64_t p0 = pr19262_space_available(space, B_TRUE);
	VERIFY3U(v0, ==, p0);
	txg_handle_t txh;
	uint64_t fresh_birth = txg_hold_open(spa->spa_dsl_pool, &txh);
	txg_rele_to_quiesce(&txh);
	VERIFY3S(pr19262_space_reserve(space, p0 + 1), ==, ENOSPC);
	VERIFY3U(pr19262_space_available(space, B_TRUE), ==, p0);
	uint64_t size = 16 * 1024;
	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	vdev_t *vd = vdev_lookup_top(spa, 0);
	uint64_t asize = vdev_psize_to_asize_txg(vd, size,
	    fresh_birth);
	VERIFY3S(metaslab_claim_impl(vd, e->offset,
	    e->backing[0].desc.asize, 0), ==, ENOENT);
	spa_config_exit(spa, SCL_ALL, FTAG);
	VERIFY0(pr19262_space_reserve(space, asize));
	pr19262_space_pair_t pair;
	VERIFY3S(pr19262_space_allocate(space, asize, fresh_birth,
	    e->offset, &pair), ==, ENOSPC);
	VERIFY3U(pr19262_space_available(space, B_FALSE), ==, v0);
	pr19262_space_unreserve(space, asize);

	/* Verify the already-native stronger copy before publishing it. */
	blkptr_t target = *bp;
	DVA_SET_OFFSET(&target.blk_dva[0], e->prepared.offset);
	DVA_SET_ASIZE(&target.blk_dva[0], e->prepared.asize);
	BP_SET_PHYSICAL_BIRTH(&target, e->prepared.birth);
	abd_t *verify = abd_alloc_for_io(BP_GET_PSIZE(bp), B_FALSE);
	VERIFY0(zio_wait(probe_read(spa, &target, verify)));
	VERIFY0(probe_copy_begin(e));
	VERIFY0(probe_publish(e, e->prepared, B_TRUE));
	VERIFY0(probe_reclaim(e, 0, B_TRUE));
	VERIFY3S(pr19262_space_reclaim(space, e->offset,
	    e->backing[0].desc.asize, B_FALSE), ==, EAGAIN);
	VERIFY0(pr19262_space_reclaim(space, e->offset,
	    e->backing[0].desc.asize, B_TRUE));
	VERIFY3S(pr19262_space_reclaim(space, e->offset,
	    e->backing[0].desc.asize, B_TRUE), ==, EEXIST);
	VERIFY0(pr19262_space_reserve(space, asize));
	VERIFY0(pr19262_space_allocate(space, asize, fresh_birth,
	    e->offset, &pair));
	VERIFY3U(pair.virtual_offset, !=, e->offset);
	VERIFY3U(pair.physical_offset, ==, e->offset);
	VERIFY3U(pr19262_space_available(space, B_FALSE), ==, v0 - asize);
	VERIFY3U(pr19262_space_available(space, B_TRUE), ==,
	    p0 + e->backing[0].desc.asize - asize);
	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	VERIFY3S(metaslab_claim_impl(vd, e->offset,
	    e->backing[0].desc.asize, 0), ==, ENOENT);
	VERIFY3S(metaslab_claim_impl(vd, pair.virtual_offset,
	    asize, 0), ==, ENOENT);
	spa_config_exit(spa, SCL_ALL, FTAG);

	blkptr_t loan = *bp;
	BP_ZERO_DVAS(&loan);
	BP_SET_LSIZE(&loan, size);
	BP_SET_PSIZE(&loan, size);
	BP_SET_BIRTH(&loan, fresh_birth, 0);
	BP_SET_REWRITE(&loan, 0);
	DVA_SET_OFFSET(&loan.blk_dva[0], pair.virtual_offset);
	DVA_SET_ASIZE(&loan.blk_dva[0], asize);
	mutex_enter(&probe_lock);
	VERIFY3U(probe_count, <, PROBE_ENTRIES);
	probe_entry_t *n = &probe_entries[probe_count++];
	n->spa = spa;
	n->offset = pair.virtual_offset;
	n->birth = BP_GET_PHYSICAL_BIRTH(&loan);
	n->incarnation = 1;
	n->live = n->fresh = B_TRUE;
	n->count = 1;
	n->backing[0].desc = (pr19262_backing_desc_t) {
		pair.physical_offset, n->birth, asize };
	mutex_exit(&probe_lock);
	void *buf = kmem_alloc(size, KM_SLEEP);
	FILE *f = fopen(pr19262_env("PR19262_SPACE_INPUT"), "rb");
	VERIFY3P(f, !=, NULL);
	VERIFY3U(fread(buf, size, 1, f), ==, 1);
	VERIFY0(fclose(f));
	abd_t *new_data = abd_alloc_for_io(size, B_FALSE);
	abd_copy_from_buf(new_data, buf, size);
	/* Only this raw data ZIO is writable; no MOS TXG is started. */
	spa_mode_t mode = spa->spa_mode;
	boolean_t trusted = spa->spa_trust_config;
	spa->spa_mode |= SPA_MODE_WRITE;
	spa->spa_trust_config = B_TRUE;
	/* Re-probe readonly-imported leaves before the data write. */
	spa_config_enter(spa, SCL_ALL, FTAG, RW_WRITER);
	for (uint64_t i = 0; i < vd->vdev_children; i++) {
		VERIFY0(zio_wait(vdev_probe(vd->vdev_child[i], NULL)));
		VERIFY(vdev_writeable(vd->vdev_child[i]));
	}
	spa_config_exit(spa, SCL_ALL, FTAG);
	VERIFY0(zio_wait(zio_rewrite(NULL, spa, n->birth, &loan, new_data,
	    size, NULL, NULL, ZIO_PRIORITY_SYNC_WRITE,
	    ZIO_FLAG_CANFAIL | ZIO_FLAG_RAW, NULL)));
	spa->spa_mode = mode;
	spa->spa_trust_config = trusted;
	txg_rele_to_sync(&txh);
	VERIFY0(n->handles);
	abd_zero(new_data, size);
	VERIFY0(zio_wait(probe_read(spa, &loan, new_data)));
	VERIFY0(abd_cmp_buf(new_data, buf, size));
	char path[1024];
	(void) snprintf(path, sizeof (path), "%s.loan", output);
	probe_save_abd(new_data, size, path);
	VERIFY0(zio_wait(queued));
	VERIFY0(abd_cmp(old_data, verify));
	probe_save_abd(old_data, BP_GET_PSIZE(bp), output);
	VERIFY0(memcmp(bp, &original, sizeof (*bp)));
	mutex_enter(&probe_lock);
	n->live = B_FALSE;
	mutex_exit(&probe_lock);
	VERIFY0(probe_reclaim(n, 0, B_TRUE));
	pr19262_space_release(space, &pair);
	VERIFY3U(pr19262_space_available(space, B_FALSE), ==, v0);
	VERIFY3U(pr19262_space_available(space, B_TRUE), ==,
	    p0 + e->backing[0].desc.asize);
	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	VERIFY3S(metaslab_claim_impl(vd, e->offset,
	    e->backing[0].desc.asize, 0), ==, ENOENT);
	spa_config_exit(spa, SCL_ALL, FTAG);
	pr19262_printf("PR19262_SPACE_PASS %llu %llu %llu "
	    "%llu %llu %llu\n", (u_longlong_t)v0, (u_longlong_t)p0,
	    (u_longlong_t)pair.virtual_offset,
	    (u_longlong_t)pair.physical_offset,
	    (u_longlong_t)asize,
	    (u_longlong_t)e->backing[0].desc.asize);
	pr19262_space_destroy(space);
	abd_free(new_data);
	abd_free(verify);
	abd_free(old_data);
	kmem_free(buf, size);
	return (0);
}

static void
probe_fixture_write(spa_t *spa, vdev_t *vd, uint64_t txg, blkptr_t *bp,
    abd_t *data, uint64_t size, boolean_t physical)
{
	spa_mode_t mode = spa->spa_mode;
	boolean_t trusted = spa->spa_trust_config;
	spa->spa_mode |= SPA_MODE_WRITE;
	spa->spa_trust_config = B_TRUE;
	spa_config_enter(spa, SCL_ALL, FTAG, RW_WRITER);
	for (uint64_t i = 0; i < vd->vdev_children; i++) {
		VERIFY0(zio_wait(vdev_probe(vd->vdev_child[i], NULL)));
		VERIFY(vdev_writeable(vd->vdev_child[i]));
	}
	spa_config_exit(spa, SCL_ALL, FTAG);
	zio_t *write = physical ?
	    pr19262_write_physical(spa, txg, bp, data, size) :
	    zio_rewrite(NULL, spa, txg, bp, data, size, NULL, NULL,
	    ZIO_PRIORITY_SYNC_WRITE, ZIO_FLAG_CANFAIL | ZIO_FLAG_RAW, NULL);
	VERIFY0(zio_wait(write));
	spa->spa_mode = mode;
	spa->spa_trust_config = trusted;
}

static int
probe_copy_test(spa_t *spa, const char *output, const blkptr_t *bp)
{
	const char *guid = pr19262_env("PR19262_SPACE_RW_GUID");
	VERIFY3P(guid, !=, NULL);
	VERIFY3U(strtoull(guid, NULL, 10), ==, spa_guid(spa));
	VERIFY(!spa_writeable(spa));
	blkptr_t original = *bp;
	uint64_t size = BP_GET_PSIZE(bp);
	abd_t *old = abd_alloc_for_io(size, B_FALSE);
	abd_t *verify = abd_alloc_for_io(size, B_FALSE);
	zio_t *queued = probe_read(spa, bp, old);
	probe_entry_t *e = ((probe_handle_t *)queued->io_pr19262_hold)->entry;
	void *scan = pr19262_scan_probe_create(spa, bp);
	VERIFY3U(e->handles, ==, 2);
	void *space = pr19262_space_create(spa);
	uint64_t v0 = pr19262_space_available(space, B_FALSE);
	uint64_t p0 = pr19262_space_available(space, B_TRUE);
	txg_handle_t txh;
	uint64_t birth = txg_hold_open(spa->spa_dsl_pool, &txh);
	txg_rele_to_quiesce(&txh);
	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	vdev_t *vd = vdev_lookup_top(spa, 0);
	uint64_t target_size = vdev_psize_to_asize_txg(vd, size, birth);
	uint64_t loan_size = 16 * 1024;
	uint64_t loan_asize = vdev_psize_to_asize_txg(vd, loan_size, birth);
	spa_config_exit(spa, SCL_ALL, FTAG);
	VERIFY3U(target_size, >, DVA_GET_ASIZE(&bp->blk_dva[0]));
	pr19262_stage_t stage;
	VERIFY3S(pr19262_space_stage_alloc(space, target_size, &stage),
	    ==, ENOSPC);
	VERIFY0(pr19262_space_reserve(space, target_size));
	VERIFY0(pr19262_space_stage_alloc(space, target_size, &stage));
	VERIFY3U(pr19262_space_available(space, B_FALSE), ==, v0);
	VERIFY3U(pr19262_space_available(space, B_TRUE), ==, p0 - target_size);
	pr19262_stage_t wrong = stage;
	wrong.owner++;
	VERIFY3S(pr19262_space_stage_free(space, &wrong, B_TRUE), ==, EINVAL);
	VERIFY0(probe_copy_begin(e));
	VERIFY0(zio_wait(probe_read(spa, bp, verify)));
	blkptr_t target = *bp;
	DVA_SET_OFFSET(&target.blk_dva[0], stage.offset);
	DVA_SET_ASIZE(&target.blk_dva[0], stage.asize);
	BP_SET_PHYSICAL_BIRTH(&target, birth);
	probe_fixture_write(spa, vd, birth, &target, verify, size, B_TRUE);
	VERIFY0(memcmp(&target.blk_cksum, &bp->blk_cksum,
	    sizeof (zio_cksum_t)));
	abd_t *physical = abd_alloc_for_io(size, B_FALSE);
	VERIFY0(zio_wait(pr19262_read_physical(spa, &target, physical, size)));
	VERIFY0(abd_cmp(physical, verify));
	pr19262_backing_desc_t desc = { stage.offset, birth, stage.asize };
	VERIFY3S(probe_publish(e, desc, B_FALSE), ==, EIO);
	VERIFY0(pr19262_space_stage_publish(space, &stage));
	VERIFY0(probe_publish(e, desc, B_TRUE));
	VERIFY3S(pr19262_space_stage_free(space, &stage, B_FALSE), ==, EAGAIN);
	VERIFY0(probe_reclaim(e, 0, B_TRUE));
	uint64_t old_asize = e->backing[0].desc.asize;
	VERIFY0(pr19262_space_reclaim(space, e->offset, old_asize, B_TRUE));
	VERIFY0(pr19262_space_reserve(space, loan_asize));
	pr19262_space_pair_t pair;
	VERIFY0(pr19262_space_allocate_at(space, loan_asize, birth,
	    stage.offset, e->offset, &pair));
	VERIFY3U(pair.virtual_offset, ==, stage.offset);
	VERIFY3U(pair.physical_offset, ==, e->offset);
	VERIFY3U(pr19262_space_available(space, B_FALSE), ==, v0 - loan_asize);
	VERIFY3U(pr19262_space_available(space, B_TRUE), ==,
	    p0 - target_size + old_asize - loan_asize);
	blkptr_t loan = *bp;
	BP_ZERO_DVAS(&loan);
	BP_SET_LSIZE(&loan, loan_size);
	BP_SET_PSIZE(&loan, loan_size);
	BP_SET_BIRTH(&loan, birth, 0);
	BP_SET_REWRITE(&loan, 0);
	DVA_SET_OFFSET(&loan.blk_dva[0], pair.virtual_offset);
	DVA_SET_ASIZE(&loan.blk_dva[0], pair.virtual_size);
	mutex_enter(&probe_lock);
	VERIFY3U(probe_count, <, PROBE_ENTRIES);
	probe_entry_t *n = &probe_entries[probe_count++];
	n->spa = spa;
	n->offset = pair.virtual_offset;
	n->birth = birth;
	n->incarnation = 1;
	n->live = n->fresh = B_TRUE;
	n->count = 1;
	n->backing[0].desc = (pr19262_backing_desc_t) {
		pair.physical_offset, birth, loan_asize };
	mutex_exit(&probe_lock);
	void *buf = kmem_alloc(loan_size, KM_SLEEP);
	FILE *f = fopen(pr19262_env("PR19262_SPACE_INPUT"), "rb");
	VERIFY3P(f, !=, NULL);
	VERIFY3U(fread(buf, loan_size, 1, f), ==, 1);
	VERIFY0(fclose(f));
	abd_t *data = abd_alloc_for_io(loan_size, B_FALSE);
	abd_copy_from_buf(data, buf, loan_size);
	probe_fixture_write(spa, vd, birth, &loan, data, loan_size, B_FALSE);
	txg_rele_to_sync(&txh);
	abd_zero(data, loan_size);
	VERIFY0(zio_wait(probe_read(spa, &loan, data)));
	VERIFY0(abd_cmp_buf(data, buf, loan_size));
	char path[1024];
	(void) snprintf(path, sizeof (path), "%s.loan", output);
	probe_save_abd(data, loan_size, path);
	/* The same number is now a live V token and another allocation's P. */
	VERIFY0(zio_wait(pr19262_read_physical(spa, &target, physical, size)));
	VERIFY0(abd_cmp(physical, verify));
	VERIFY3S(zio_wait(probe_read(spa, &target, physical)), ==, EINVAL);
	(void) snprintf(path, sizeof (path), "%s.copy", output);
	VERIFY0(zio_wait(pr19262_read_physical(spa, &target, physical, size)));
	probe_save_abd(physical, size, path);
	mutex_enter(&probe_lock);
	probe_block_new_holds = B_TRUE;
	mutex_exit(&probe_lock);
	VERIFY0(pr19262_scan_probe_issue(scan));
	mutex_enter(&probe_lock);
	probe_block_new_holds = B_FALSE;
	mutex_exit(&probe_lock);
	VERIFY3U(e->handles, ==, 1);
	VERIFY0(zio_wait(queued));
	VERIFY0(abd_cmp(old, verify));
	probe_save_abd(old, size, output);
	VERIFY0(memcmp(bp, &original, sizeof (*bp)));
	mutex_enter(&probe_lock);
	n->live = B_FALSE;
	mutex_exit(&probe_lock);
	VERIFY0(probe_reclaim(n, 0, B_TRUE));
	pr19262_space_release(space, &pair);
	/* Teardown all transient bindings before discarding this fixture. */
	mutex_enter(&probe_lock);
	e->live = B_FALSE;
	mutex_exit(&probe_lock);
	VERIFY0(probe_reclaim(e, e->cur, B_TRUE));
	VERIFY0(pr19262_space_stage_free(space, &stage, B_TRUE));
	VERIFY3S(pr19262_space_stage_free(space, &stage, B_TRUE), ==, EINVAL);
	VERIFY3U(pr19262_space_available(space, B_FALSE), ==, v0);
	VERIFY3U(pr19262_space_available(space, B_TRUE), ==, p0 + old_asize);
	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	VERIFY3S(metaslab_claim_impl(vd, e->offset, old_asize, 0), ==, ENOENT);
	spa_config_exit(spa, SCL_ALL, FTAG);
	pr19262_printf("PR19262_COPY_PASS %llu %llu %llu "
	    "%llu %llu %llu %llu\n", (u_longlong_t)v0, (u_longlong_t)p0,
	    (u_longlong_t)e->offset, (u_longlong_t)stage.offset,
	    (u_longlong_t)old_asize, (u_longlong_t)target_size,
	    (u_longlong_t)loan_asize);
	pr19262_space_destroy(space);
	abd_free(data);
	abd_free(physical);
	abd_free(verify);
	abd_free(old);
	kmem_free(buf, loan_size);
	return (0);
}

int
pr19262_reloc_test(spa_t *spa, const char *output)
{
	const char *file = pr19262_env("PR19262_LIFETIME_BP");
	VERIFY3P(file, !=, NULL);
	FILE *f = fopen(file, "rb");
	VERIFY3P(f, !=, NULL);
	blkptr_t bp;
	VERIFY3U(fread(&bp, sizeof (bp), 1, f), ==, 1);
	VERIFY0(fclose(f));
	if (pr19262_env("PR19262_COPY_TEST") != NULL)
		return (probe_copy_test(spa, output, &bp));
	if (pr19262_env("PR19262_SPACE_TEST") != NULL)
		return (probe_space_test(spa, output, &bp));
	uint64_t size = BP_GET_PSIZE(&bp);
	abd_t *abd = abd_alloc_for_io(size, B_FALSE);
	zio_t *queued = probe_read(spa, &bp, abd);
	probe_handle_t *h = queued->io_pr19262_hold;
	VERIFY3P(h, !=, NULL);
	probe_entry_t *e = h->entry;
	VERIFY3U(e->handles, ==, 1);
	VERIFY3P(h->leased, ==, NULL);
	void *scan_queue = NULL;
	boolean_t scan_test = pr19262_env("PR19262_SCAN_LIFETIME_TEST") != NULL;
	if (scan_test) {
		scan_queue = pr19262_scan_probe_create(spa, &bp);
		pr19262_scan_probe_insert(scan_queue, &bp);
		VERIFY3U(e->handles, ==, 2);
		pr19262_scan_probe_cancel(scan_queue, &bp);
		VERIFY3U(e->handles, ==, 1);
		pr19262_scan_probe_destroy(scan_queue);
		scan_queue = pr19262_scan_probe_create(spa, &bp);
		pr19262_scan_probe_destroy(scan_queue);
		VERIFY3U(e->handles, ==, 1);
		scan_queue = pr19262_scan_probe_create(spa, &bp);
		VERIFY3U(e->handles, ==, 2);
		VERIFY0(e->backing[0].readers);
	}

	/* Writer exclusion is tested at the API, not DDT/ZIL hooks. */
	VERIFY0(probe_writer_enter(e));
	VERIFY3S(probe_copy_begin(e), ==, EBUSY);
	mutex_enter(&probe_lock);
	e->writers--;
	mutex_exit(&probe_lock);
	VERIFY0(probe_copy_begin(e));
	VERIFY3S(probe_writer_enter(e), ==, EBUSY);
	VERIFY3S(probe_publish(e, e->prepared, B_FALSE), ==, EIO);
	VERIFY0(probe_publish(e, e->prepared, B_TRUE));
	VERIFY3S(probe_reclaim(e, 0, B_FALSE), ==, EAGAIN);
	VERIFY0(probe_reclaim(e, 0, B_TRUE));
	probe_gate("old-backing-reclaim");
	if (scan_test) {
		/* Issuing must use the queued binding, without a new lookup. */
		mutex_enter(&probe_lock);
		probe_block_new_holds = B_TRUE;
		mutex_exit(&probe_lock);
		VERIFY0(pr19262_scan_probe_issue(scan_queue));
		mutex_enter(&probe_lock);
		probe_block_new_holds = B_FALSE;
		mutex_exit(&probe_lock);
		VERIFY3U(e->handles, ==, 1);
		VERIFY0(e->backing[0].readers);
	}
	VERIFY0(zio_wait(queued));
	VERIFY0(e->handles);
	void *data = abd_borrow_buf_copy(abd, size);
	f = fopen(output, "wb");
	VERIFY3P(f, !=, NULL);
	VERIFY3U(fwrite(data, size, 1, f), ==, 1);
	VERIFY0(fclose(f));
	abd_return_buf_copy(abd, data, size);

	/* Bind a physical lease before the next publication. */
	queued = probe_read(spa, &bp, abd);
	pr19262_backing_desc_t leased;
	VERIFY0(pr19262_reloc_resolve(queued, &leased));
	VERIFY3U(leased.offset, ==, e->prepared.offset);
	const char *next = pr19262_env("PR19262_LIFETIME_NEXT");
	u_longlong_t off, birth, asize;
	VERIFY3P(next, !=, NULL);
	VERIFY3S(sscanf(next, "%llx,%llu,%llx", &off, &birth,
	    &asize), ==, 3);
	pr19262_backing_desc_t desc = { off, birth, asize };
	VERIFY0(probe_copy_begin(e));
	VERIFY0(probe_publish(e, desc, B_TRUE));
	VERIFY3S(probe_reclaim(e, 1, B_TRUE), ==, EBUSY);
	VERIFY0(zio_wait(queued));
	VERIFY0(e->handles);
	VERIFY0(probe_reclaim(e, 1, B_TRUE));
	probe_gate("leased-backing-reclaim");
	VERIFY0(zio_wait(probe_read(spa, &bp, abd)));

	/* Final free cancels unissued reads; no fallback to reused storage. */
	queued = probe_read(spa, &bp, abd);
	if (scan_test)
		scan_queue = pr19262_scan_probe_create(spa, &bp);
	uint64_t incarnation = e->incarnation;
	mutex_enter(&probe_lock);
	e->live = B_FALSE;
	mutex_exit(&probe_lock);
	VERIFY3S(probe_reuse(e), ==, EBUSY);
	if (scan_test) {
		pr19262_scan_probe_cancel(scan_queue, &bp);
		pr19262_scan_probe_destroy(scan_queue);
		VERIFY3U(e->handles, ==, 1);
		VERIFY3S(probe_reuse(e), ==, EBUSY);
	}
	VERIFY3S(zio_wait(queued), ==, ESTALE);
	VERIFY0(e->handles);
	VERIFY0(probe_reclaim(e, 2, B_TRUE));
	VERIFY0(probe_reuse(e));
	VERIFY3U(e->incarnation, ==, incarnation + 1);
	pr19262_printf("PR19262_LIFETIME_PASS "
	    "queued_no_P_pin leased_reclaim_blocked stale_cancelled "
	    "incarnation_drained writer_exclusion floor_required\n");
	if (scan_test)
		pr19262_printf("PR19262_SCAN_LIFETIME_PASS "
		    "duplicate_cancel_destroy transfer_dispatch final_free\n");
	abd_free(abd);
	return (0);
}
#endif
