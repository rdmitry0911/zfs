/* SPDX-License-Identifier: CDDL-1.0 */
/*
 * Private userspace lifetime feasibility component, included by zio.c.
 * Enrollment is explicit and finite, from a test fixture. Neither final-free
 * hooks nor stable on-disk incarnation identities are implemented here.
 * Recovery-floor arguments below are supplied assumptions, not disk proofs.
 */
#include <sys/vdev_reloc_probe.h>

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
	uint_t current;
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
static boolean_t probe_capture_done;
static boolean_t probe_block_new_holds;

void
pr19262_reloc_init(void)
{
	mutex_init(&probe_lock, NULL, MUTEX_DEFAULT, NULL);
	const char *p = getenv("PR19262_PROBE_MAP");
	if (getenv("PR19262_LIFETIME") == NULL || p == NULL)
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
		const char *capture = getenv("PR19262_CAPTURE_BP");
		if (capture != NULL && !probe_capture_done) {
			FILE *f = fopen(capture, "wb");
			VERIFY3P(f, !=, NULL);
			VERIFY3U(fwrite(bp, sizeof (*bp), 1, f), ==, 1);
			VERIFY0(fclose(f));
			probe_capture_done = B_TRUE;
		}
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
		h->leased = &e->backing[e->current];
		VERIFY(!h->leased->reclaimed);
		h->leased->readers++;
	}
	*desc = h->leased->desc;
	mutex_exit(&probe_lock);
	return (0);
}

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
		e->current = e->count++;
		e->backing[e->current].desc = desc;
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
	if (index >= e->count || (e->live && index == e->current) ||
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
			e->current = 0;
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
	const char *dir = getenv("PR19262_LIFETIME_GATES");
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

int
pr19262_reloc_test(spa_t *spa, const char *output)
{
	const char *file = getenv("PR19262_LIFETIME_BP");
	VERIFY3P(file, !=, NULL);
	FILE *f = fopen(file, "rb");
	VERIFY3P(f, !=, NULL);
	blkptr_t bp;
	VERIFY3U(fread(&bp, sizeof (bp), 1, f), ==, 1);
	VERIFY0(fclose(f));
	uint64_t size = BP_GET_PSIZE(&bp);
	abd_t *abd = abd_alloc_for_io(size, B_FALSE);
	zio_t *queued = probe_read(spa, &bp, abd);
	probe_handle_t *h = queued->io_pr19262_hold;
	VERIFY3P(h, !=, NULL);
	probe_entry_t *e = h->entry;
	VERIFY3U(e->handles, ==, 1);
	VERIFY3P(h->leased, ==, NULL);
	void *scan_queue = NULL;
	boolean_t scan_test = getenv("PR19262_SCAN_LIFETIME_TEST") != NULL;
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
	const char *next = getenv("PR19262_LIFETIME_NEXT");
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
	(void) fprintf(stderr, "PR19262_LIFETIME_PASS "
	    "queued_no_P_pin leased_reclaim_blocked stale_cancelled "
	    "incarnation_drained writer_exclusion floor_required\n");
	if (scan_test)
		(void) fprintf(stderr, "PR19262_SCAN_LIFETIME_PASS "
		    "duplicate_cancel_destroy transfer_dispatch final_free\n");
	abd_free(abd);
	return (0);
}
