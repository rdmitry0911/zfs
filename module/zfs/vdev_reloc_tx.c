/* SPDX-License-Identifier: CDDL-1.0 */
/*
 * Private userspace admission experiment, included by dmu_tx.c.
 * Native identity P allocations consume per-TXG committed credits.
 * No persistent dual allocator/catalog is implemented by this fixture.
 */
#include <sys/metaslab.h>
#include <sys/metaslab_impl.h>
#include <sys/spa_impl.h>

typedef struct probe_tx_budget {
	list_node_t link;
	uint64_t txg;
	uint64_t open;
	uint64_t committed;
	uint64_t inflight;
	uint64_t refs;
	uint64_t pending_refs;
} probe_tx_budget_t;

typedef struct probe_tx_credit {
	uint64_t bytes;
	uint64_t txg;
	spa_t *spa;
	probe_tx_budget_t *budget;
	/* Vector reserved with the engine: pages and frames, by class. */
	uint_t class;
	uint64_t m_pages;
	uint64_t j_frames;
} probe_tx_credit_t;

typedef struct probe_tx_allocation {
	list_node_t link;
	vdev_t *vd;
	uint64_t offset;
	uint64_t size;
	uint64_t spent;
	probe_tx_budget_t *budget;
} probe_tx_allocation_t;

typedef struct pr19262_tx_state {
	kmutex_t lock;
	kcondvar_t done;
	list_t budgets;
	list_t allocations;
	objset_t *os;
	uint64_t cap;
	uint64_t emergency;
	uint64_t reserved;
	uint64_t pending;
	uint64_t inflight;
	uint64_t consumed;
	uint64_t unallocations;
	uint64_t rejected;
	uint64_t granted;
	uint64_t rolled_back;
	uint64_t callbacks;
	boolean_t fail_second;
} probe_tx_state_t;

/*
 * The credit ledger lives from spa_add() through spa_remove().  A SPA's
 * txg callback taskq is drained by txg_fini() before spa_remove() frees it.
 */
void
pr19262_tx_init(spa_t *spa)
{
	probe_tx_state_t *pt = kmem_zalloc(sizeof (*pt), KM_SLEEP);
	mutex_init(&pt->lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&pt->done, NULL, CV_DEFAULT, NULL);
	list_create(&pt->budgets, sizeof (probe_tx_budget_t),
	    offsetof(probe_tx_budget_t, link));
	list_create(&pt->allocations, sizeof (probe_tx_allocation_t),
	    offsetof(probe_tx_allocation_t, link));
	spa->spa_pr19262_tx = pt;
}

void
pr19262_tx_fini(spa_t *spa)
{
	probe_tx_state_t *pt = spa->spa_pr19262_tx;
	VERIFY0(pt->reserved);
	VERIFY0(pt->pending);
	VERIFY0(pt->inflight);
	VERIFY(list_is_empty(&pt->budgets));
	VERIFY(list_is_empty(&pt->allocations));
	VERIFY3P(pt->os, ==, NULL);
	list_destroy(&pt->budgets);
	list_destroy(&pt->allocations);
	cv_destroy(&pt->done);
	mutex_destroy(&pt->lock);
	kmem_free(pt, sizeof (*pt));
	spa->spa_pr19262_tx = NULL;
}

static boolean_t
probe_tx_scope(spa_t *spa)
{
	probe_tx_state_t *pt = spa->spa_pr19262_tx;
	return ((pt->os != NULL && dmu_objset_spa(pt->os) == spa) ||
	    pr19262_engine_active(spa));
}

void
pr19262_tx_fail_second(spa_t *spa, boolean_t fail)
{
	probe_tx_state_t *pt = spa->spa_pr19262_tx;
	mutex_enter(&pt->lock);
	pt->fail_second = fail;
	mutex_exit(&pt->lock);
}

boolean_t
pr19262_tx_fail_dva(spa_t *spa, int d)
{
	probe_tx_state_t *pt = spa->spa_pr19262_tx;
	mutex_enter(&pt->lock);
	boolean_t fail = probe_tx_scope(spa) &&
	    pt->fail_second && d == 1;
	mutex_exit(&pt->lock);
	return (fail);
}

static probe_tx_budget_t *
probe_tx_budget_find(probe_tx_state_t *pt, uint64_t txg)
{
	ASSERT(MUTEX_HELD(&pt->lock));
	for (probe_tx_budget_t *b = list_head(&pt->budgets); b != NULL;
	    b = list_next(&pt->budgets, b))
		if (b->txg == txg)
			return (b);
	return (NULL);
}

static void
probe_tx_budget_drop(probe_tx_state_t *pt, probe_tx_budget_t *b)
{
	if (b->refs != 0)
		return;
	VERIFY0(b->open);
	VERIFY0(b->committed);
	VERIFY0(b->inflight);
	VERIFY0(b->pending_refs);
	list_remove(&pt->budgets, b);
	kmem_free(b, sizeof (*b));
}

/*
 * Catalog rows a transaction may add: one per block it may allocate,
 * bounded by its holds' block sizes; journal frames: one per log block
 * plus gang members a synchronous record of that size may need.
 */
static void
probe_tx_vector(dmu_tx_t *tx, uint64_t *rows, uint64_t *frames)
{
	*rows = 2;
	*frames = 1;
	for (dmu_tx_hold_t *txh = list_head(&tx->tx_holds); txh != NULL;
	    txh = list_next(&tx->tx_holds, txh)) {
		uint64_t towrite = zfs_refcount_count(&txh->txh_space_towrite);
		if (towrite == 0)
			continue;
		dnode_t *dn = txh->txh_dnode;
		if (dn != NULL) {
			uint64_t block = MAX(dn->dn_datablksz,
			    SPA_MINBLOCKSIZE);
			*rows += howmany(towrite, block) +
			    howmany(towrite, 1ULL << dn->dn_indblkshift) + 1;
		} else {
			*rows += howmany(towrite, SPA_MINBLOCKSIZE);
		}
		*frames += 4 * howmany(towrite, SPA_OLD_MAXBLOCKSIZE);
	}
}

static int
probe_tx_admit(dmu_tx_t *tx, uint64_t bytes)
{
	int error = 0;
	spa_t *spa = tx->tx_pool->dp_spa;
	probe_tx_state_t *pt = spa->spa_pr19262_tx;
	mutex_enter(&pt->lock);
	boolean_t runtime = pr19262_engine_active(spa);
	if (bytes == 0 || (!runtime &&
	    (tx->tx_objset != pt->os || pt->os == NULL)))
		goto out;
	VERIFY3P(tx->tx_pr19262_physical_credit, ==, NULL);
	uint_t class = tx->tx_netfree ? PR19262_ADMIT_NETFREE :
	    PR19262_ADMIT_CLIENT;
	uint64_t rows = 0, frames = 0, m_pages = 0;
	if (runtime) {
		probe_tx_vector(tx, &rows, &frames);
		m_pages = rows * pr19262_engine_admit_row_pages(spa);
		/* Physical bytes, pages and frames admitted together. */
		error = pr19262_engine_admit(spa, class, pt->reserved,
		    bytes, m_pages, frames);
		if (error != 0) {
			if (error == ENOSPC)
				pt->rejected++;
			goto out;
		}
	}
	metaslab_class_t *mc = spa_normal_class(tx->tx_pool->dp_spa);
	uint64_t total = metaslab_class_get_space(mc);
	uint64_t allocated = metaslab_class_get_alloc(mc);
	uint64_t available = total > allocated ? total - allocated : 0;
	/* Native committed ALLOC does not yet include these allocations. */
	uint64_t limit = MIN(pt->cap, available);
	limit = limit > pt->inflight ? limit - pt->inflight : 0;
	uint64_t protected = tx->tx_netfree ? 0 : pt->emergency;
	limit = limit > protected ? limit - protected : 0;
	if (!runtime && (pt->reserved > limit ||
	    bytes > limit - pt->reserved)) {
		pt->rejected++;
		error = ENOSPC;
		goto out;
	}
	probe_tx_budget_t *b = probe_tx_budget_find(pt, tx->tx_txg);
	if (b == NULL) {
		b = kmem_zalloc(sizeof (*b), KM_SLEEP);
		b->txg = tx->tx_txg;
		list_insert_tail(&pt->budgets, b);
	}
	probe_tx_credit_t *c = kmem_alloc(sizeof (*c), KM_SLEEP);
	c->bytes = bytes;
	c->txg = tx->tx_txg;
	c->spa = spa;
	c->budget = b;
	c->class = class;
	c->m_pages = m_pages;
	c->j_frames = frames;
	b->open += bytes;
	b->refs++;
	tx->tx_pr19262_physical_credit = c;
	pt->reserved += bytes;
	pt->granted++;
out:
	mutex_exit(&pt->lock);
	return (error);
}

static void
probe_tx_unreserve(dmu_tx_t *tx)
{
	probe_tx_credit_t *c = tx->tx_pr19262_physical_credit;
	if (c == NULL)
		return;
	probe_tx_state_t *pt = c->spa->spa_pr19262_tx;
	mutex_enter(&pt->lock);
	probe_tx_budget_t *b = c->budget;
	VERIFY3U(b->open, >=, c->bytes);
	b->open -= c->bytes;
	pt->reserved -= c->bytes;
	b->refs--;
	probe_tx_budget_drop(pt, b);
	pr19262_engine_admit_release(c->spa, c->class, 0, c->m_pages,
	    c->j_frames);
	kmem_free(c, sizeof (*c));
	pt->rolled_back++;
	tx->tx_pr19262_physical_credit = NULL;
	mutex_exit(&pt->lock);
}

/* Does this TXG carry client credit (as opposed to sync-context or ZIL)? */
boolean_t
pr19262_tx_client_txg(spa_t *spa, uint64_t txg)
{
	probe_tx_state_t *pt = spa->spa_pr19262_tx;
	mutex_enter(&pt->lock);
	probe_tx_budget_t *b = probe_tx_budget_find(pt, txg);
	boolean_t client = b != NULL && b->pending_refs != 0;
	mutex_exit(&pt->lock);
	return (client);
}

void
pr19262_tx_allocated(spa_t *spa, metaslab_class_t *mc, vdev_t *vd,
    uint64_t offset, uint64_t size, uint64_t txg)
{
	probe_tx_state_t *pt = spa->spa_pr19262_tx;
	/* Native vdevs beside the translated one spend no physical credit. */
	vdev_t *target = pr19262_engine_vdev(spa);
	mutex_enter(&pt->lock);
	if (!probe_tx_scope(spa) ||
	    mc != spa_normal_class(spa) ||
	    (target != NULL && vd != target))
		goto out;
	probe_tx_budget_t *b = probe_tx_budget_find(pt, txg);
	/* Open-context ZIL needs its own durable budget, not client credit. */
	if (b == NULL || b->pending_refs == 0)
		goto out;
	probe_tx_allocation_t *a = kmem_zalloc(sizeof (*a), KM_SLEEP);
	a->vd = vd;
	a->offset = offset;
	a->size = size;
	a->budget = b;
	a->spent = MIN(size, b->committed);
	b->committed -= a->spent;
	pt->reserved -= a->spent;
	pt->pending -= a->spent;
	b->inflight += size;
	pt->inflight += size;
	pt->consumed += a->spent;
	list_insert_tail(&pt->allocations, a);
out:
	mutex_exit(&pt->lock);
}

void
pr19262_tx_unallocated(spa_t *spa, vdev_t *vd, uint64_t offset,
    uint64_t size, uint64_t txg)
{
	probe_tx_state_t *pt = spa->spa_pr19262_tx;
	mutex_enter(&pt->lock);
	if (!probe_tx_scope(spa))
		goto out;
	for (probe_tx_allocation_t *a = list_head(&pt->allocations);
	    a != NULL; a = list_next(&pt->allocations, a)) {
		if (a->vd != vd || a->offset != offset || a->size != size ||
		    a->budget->txg != txg)
			continue;
		probe_tx_budget_t *b = a->budget;
		b->committed += a->spent;
		pt->reserved += a->spent;
		pt->pending += a->spent;
		VERIFY3U(b->inflight, >=, size);
		VERIFY3U(pt->inflight, >=, size);
		b->inflight -= size;
		pt->inflight -= size;
		pt->unallocations++;
		list_remove(&pt->allocations, a);
		kmem_free(a, sizeof (*a));
		break;
	}
out:
	mutex_exit(&pt->lock);
}

static void
probe_tx_synced(void *arg, int error)
{
	probe_tx_credit_t *c = arg;
	probe_tx_state_t *pt = c->spa->spa_pr19262_tx;
	VERIFY0(error);
	VERIFY3U(spa_last_synced_txg(c->spa), >=, c->txg);
	mutex_enter(&pt->lock);
	probe_tx_budget_t *b = c->budget;
	/* Physical ownership now remains charged by native metaslab ALLOC. */
	VERIFY3U(pt->inflight, >=, b->inflight);
	pt->inflight -= b->inflight;
	b->inflight = 0;
	probe_tx_allocation_t *a = list_head(&pt->allocations);
	while (a != NULL) {
		probe_tx_allocation_t *next =
		    list_next(&pt->allocations, a);
		if (a->budget == b) {
			list_remove(&pt->allocations, a);
			kmem_free(a, sizeof (*a));
		}
		a = next;
	}
	VERIFY3U(b->pending_refs, >, 0);
	b->pending_refs--;
	b->refs--;
	if (b->pending_refs == 0) {
		VERIFY0(b->open);
		pt->pending -= b->committed;
		pt->reserved -= b->committed;
		b->committed = 0;
	}
	probe_tx_budget_drop(pt, b);
	pt->callbacks++;
	/* The catalog of this txg is stored: its pages and frames return. */
	pr19262_engine_admit_release(c->spa, c->class, 0, c->m_pages,
	    c->j_frames);
	kmem_free(c, sizeof (*c));
	cv_broadcast(&pt->done);
	mutex_exit(&pt->lock);
}

static void
probe_tx_commit(dmu_tx_t *tx)
{
	probe_tx_credit_t *c = tx->tx_pr19262_physical_credit;
	if (c == NULL)
		return;
	probe_tx_state_t *pt = c->spa->spa_pr19262_tx;
	mutex_enter(&pt->lock);
	probe_tx_budget_t *b = c->budget;
	VERIFY3U(b->open, >=, c->bytes);
	b->open -= c->bytes;
	b->committed += c->bytes;
	b->pending_refs++;
	pt->pending += c->bytes;
	tx->tx_pr19262_physical_credit = NULL;
	mutex_exit(&pt->lock);
	dmu_tx_callback_register(tx, probe_tx_synced, c);
}

static void
probe_tx_limit(spa_t *spa, uint64_t cap, uint64_t emergency)
{
	probe_tx_state_t *pt = spa->spa_pr19262_tx;
	mutex_enter(&pt->lock);
	pt->cap = cap;
	pt->emergency = emergency;
	mutex_exit(&pt->lock);
}

static uint64_t
probe_tx_reserved(spa_t *spa)
{
	probe_tx_state_t *pt = spa->spa_pr19262_tx;
	mutex_enter(&pt->lock);
	uint64_t r = pt->reserved;
	mutex_exit(&pt->lock);
	return (r);
}

/* Outstanding unconsumed physical credit, for the engine's own admission. */
uint64_t
pr19262_tx_reserved(spa_t *spa)
{
	return (probe_tx_reserved(spa));
}

static void
probe_tx_drain(dsl_pool_t *dp, uint64_t txg)
{
	probe_tx_state_t *pt = dp->dp_spa->spa_pr19262_tx;
	txg_wait_synced(dp, txg);
	mutex_enter(&pt->lock);
	while (!list_is_empty(&pt->budgets)) {
		VERIFY3S(cv_timedwait(&pt->done, &pt->lock,
		    ddi_get_lbolt() + SEC_TO_TICK(30)), !=, -1);
	}
	VERIFY0(pt->reserved);
	VERIFY0(pt->inflight);
	mutex_exit(&pt->lock);
}

static uint64_t
probe_tx_dsl_reserved(dsl_dir_t *dd)
{
	uint64_t result = 0;
	mutex_enter(&dd->dd_lock);
	for (int i = 0; i < TXG_SIZE; i++)
		result += dd->dd_tempreserved[i];
	mutex_exit(&dd->dd_lock);
	return (result);
}

void
pr19262_tx_check(spa_t *spa)
{
	probe_tx_state_t *pt = spa->spa_pr19262_tx;
	probe_tx_drain(spa->spa_dsl_pool, 0);
	mutex_enter(&pt->lock);
	VERIFY0(pt->reserved);
	VERIFY0(pt->inflight);
	VERIFY(list_is_empty(&pt->allocations));
	pr19262_printf("PR19262_ENGINE_BUDGET grants=%llu consumed=%llu "
	    "callbacks=%llu unallocations=%llu\n",
	    (u_longlong_t)pt->granted, (u_longlong_t)pt->consumed,
	    (u_longlong_t)pt->callbacks,
	    (u_longlong_t)pt->unallocations);
	mutex_exit(&pt->lock);
}

void
pr19262_tx_probe(spa_t *spa)
{
	probe_tx_state_t *pt = spa->spa_pr19262_tx;
	char name[ZFS_MAX_DATASET_NAME_LEN];
	(void) snprintf(name, sizeof (name), "%s/physical_credit",
	    spa_name(spa));
	VERIFY0(dmu_objset_create(name, DMU_OST_OTHER, 0, NULL, NULL, NULL));
	objset_t *os;
	VERIFY0(dmu_objset_own(name, DMU_OST_OTHER, B_FALSE, B_TRUE,
	    FTAG, &os));
	dsl_pool_t *dp = dmu_objset_pool(os);
	dmu_tx_t *tx = dmu_tx_create(os);
	dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	uint64_t object = dmu_object_alloc(os, DMU_OT_UINT64_OTHER, 131072,
	    DMU_OT_NONE, 0, tx);
	dmu_tx_commit(tx);
	txg_wait_synced(dp, 0);
	mutex_enter(&pt->lock);
	pt->os = os;
	mutex_exit(&pt->lock);
	probe_tx_limit(spa, 0, 0);
	tx = dmu_tx_create(os);
	dmu_tx_hold_write(tx, object, 0, 131072);
	dsl_dir_t *dd = tx->tx_dir;
	uint64_t dsl_before = probe_tx_dsl_reserved(dd);
	VERIFY3S(dmu_tx_assign(tx, DMU_TX_NOWAIT), ==, ENOSPC);
	VERIFY0(tx->tx_txg);
	VERIFY3P(tx->tx_tempreserve_cookie, ==, NULL);
	VERIFY0(probe_tx_reserved(spa));
	VERIFY3U(probe_tx_dsl_reserved(dd), ==, dsl_before);
	dmu_tx_abort(tx);
	/* DSL quota failure must return an already acquired P credit. */
	uint64_t used = dsl_dir_phys(dd)->dd_used_bytes;
	VERIFY3U(used, >, 0);
	VERIFY0(dsl_dir_set_quota(name, ZPROP_SRC_LOCAL, used));
	probe_tx_limit(spa, 64ULL << 20, 0);
	tx = dmu_tx_create(os);
	dmu_tx_hold_write(tx, object, 0, 131072);
	VERIFY3S(dmu_tx_assign(tx, DMU_TX_WAIT), ==, EDQUOT);
	VERIFY0(probe_tx_reserved(spa));
	VERIFY3P(tx->tx_pr19262_physical_credit, ==, NULL);
	VERIFY3P(tx->tx_tempreserve_cookie, ==, NULL);
	dmu_tx_abort(tx);
	VERIFY0(dsl_dir_set_quota(name, ZPROP_SRC_LOCAL, 0));
	/* A real assigned transaction fences this and all later TXGs. */
	dmu_tx_t *fence = dmu_tx_create(os);
	VERIFY0(dmu_tx_assign(fence, DMU_TX_WAIT));
	tx = dmu_tx_create(os);
	dmu_tx_hold_write(tx, object, 0, 131072);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	uint64_t txg = dmu_tx_get_txg(tx);
	uint64_t held = probe_tx_reserved(spa);
	VERIFY3U(held, >, 0);
	probe_tx_limit(spa, held, 0);
	dmu_tx_t *retry = dmu_tx_create(os);
	dmu_tx_hold_space(retry, 4096);
	VERIFY3S(dmu_tx_assign(retry, DMU_TX_NOWAIT), ==, ENOSPC);
	VERIFY3U(probe_tx_reserved(spa), ==, held);
	void *data = kmem_alloc(131072, KM_SLEEP);
	void *readback = kmem_alloc(131072, KM_SLEEP);
	for (uint64_t i = 0; i < 131072 / sizeof (uint64_t); i++)
		((uint64_t *)data)[i] = i * 0x9e3779b97f4a7c15ULL;
	dmu_write(os, object, 0, 131072, data, tx, 0);
	dmu_tx_commit(tx);
	VERIFY3U(probe_tx_reserved(spa), ==, held);
	VERIFY3U(pt->pending, ==, held);
	/* Retry the same failed tx after resources become available. */
	probe_tx_limit(spa, 64ULL << 20, 0);
	VERIFY0(dmu_tx_assign(retry, DMU_TX_WAIT));
	uint64_t retry_txg = dmu_tx_get_txg(retry);
	dmu_tx_commit(retry);
	VERIFY3U(probe_tx_reserved(spa), >, held);
	/* Native two-DVA allocation consumes budget; unalloc refunds it. */
	uint64_t before_alloc = probe_tx_reserved(spa);
	blkptr_t rollback = { 0 };
	BP_SET_LSIZE(&rollback, 4096);
	BP_SET_PSIZE(&rollback, 4096);
	BP_SET_TYPE(&rollback, DMU_OT_UINT64_OTHER);
	zio_alloc_list_t trace;
	metaslab_trace_init(&trace);
	mutex_enter(&pt->lock);
	pt->fail_second = B_TRUE;
	mutex_exit(&pt->lock);
	VERIFY3S(metaslab_alloc(spa, spa_normal_class(spa), 4096, &rollback,
	    2, retry_txg, NULL, 0, &trace, 0, FTAG), ==, ENOSPC);
	mutex_enter(&pt->lock);
	pt->fail_second = B_FALSE;
	mutex_exit(&pt->lock);
	VERIFY0(BP_GET_NDVAS(&rollback));
	VERIFY0(BP_GET_PHYSICAL_BIRTH(&rollback));
	VERIFY3U(probe_tx_reserved(spa), ==, before_alloc);
	VERIFY0(pt->inflight);
	VERIFY3U(pt->unallocations, ==, 1);
	VERIFY0(metaslab_alloc(spa, spa_normal_class(spa), 4096, &rollback,
	    2, retry_txg, NULL, 0, &trace, 0, FTAG));
	uint64_t physical = DVA_GET_ASIZE(&rollback.blk_dva[0]) +
	    DVA_GET_ASIZE(&rollback.blk_dva[1]);
	VERIFY3U(probe_tx_reserved(spa), ==, before_alloc - physical);
	VERIFY3U(pt->inflight, ==, physical);
	metaslab_free(spa, &rollback, retry_txg, B_TRUE);
	metaslab_trace_fini(&trace);
	VERIFY3U(probe_tx_reserved(spa), ==, before_alloc);
	VERIFY0(pt->inflight);
	VERIFY3U(pt->unallocations, ==, 3);
	dmu_tx_commit(fence);
	probe_tx_drain(dp, MAX(txg, retry_txg));
	VERIFY0(dmu_read(os, object, 0, 131072, readback,
	    DMU_READ_NO_PREFETCH));
	VERIFY0(memcmp(data, readback, 131072));
	/* A dedicated P headroom permits actual netfree under normal ENOSPC. */
	probe_tx_limit(spa, 8ULL << 20, 8ULL << 20);
	tx = dmu_tx_create(os);
	dmu_tx_hold_space(tx, 4096);
	VERIFY3S(dmu_tx_assign(tx, DMU_TX_NOWAIT), ==, ENOSPC);
	dmu_tx_abort(tx);
	tx = dmu_tx_create(os);
	dmu_tx_hold_free(tx, object, 0, DMU_OBJECT_END);
	dmu_tx_mark_netfree(tx);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	VERIFY3U(probe_tx_reserved(spa), >, 0);
	VERIFY0(dmu_object_free(os, object, tx));
	txg = dmu_tx_get_txg(tx);
	dmu_tx_commit(tx);
	probe_tx_drain(dp, txg);
	VERIFY3S(dmu_object_info(os, object, NULL), ==, ENOENT);
	VERIFY3U(pt->rolled_back, ==, 1);
	VERIFY3U(pt->callbacks, ==, 3);
	VERIFY3U(pt->rejected, ==, 3);
	VERIFY3U(pt->granted, ==, 4);
	VERIFY3U(pt->consumed, >, physical);
	pr19262_printf("PR19262_TX_PASS held=%llu rollback=%llu "
	    "callbacks=%llu rejected=%llu granted=%llu "
	    "consumed=%llu unallocations=%llu\n",
	    (u_longlong_t)held, (u_longlong_t)pt->rolled_back,
	    (u_longlong_t)pt->callbacks,
	    (u_longlong_t)pt->rejected, (u_longlong_t)pt->granted,
	    (u_longlong_t)pt->consumed,
	    (u_longlong_t)pt->unallocations);
	mutex_enter(&pt->lock);
	pt->os = NULL;
	mutex_exit(&pt->lock);
	dmu_objset_disown(os, B_TRUE, FTAG);
	kmem_free(data, 131072);
	kmem_free(readback, 131072);
}

/* Two real pools: one callback must not drain the other's held credit. */
#ifndef _KERNEL
void
pr19262_tx_isolation(spa_t *first, spa_t *second)
{
	spa_t *spas[2] = { first, second };
	objset_t *sets[2];
	dmu_tx_t *fences[2];
	uint64_t objects[2], txgs[2], held[2];
	uint64_t size = 16384;
	uint64_t *data = kmem_alloc(size, KM_SLEEP);
	uint64_t *readback = kmem_alloc(size, KM_SLEEP);

	for (uint_t i = 0; i < 2; i++) {
		spa_t *spa = spas[i];
		probe_tx_state_t *pt = spa->spa_pr19262_tx;
		char name[ZFS_MAX_DATASET_NAME_LEN];
		(void) snprintf(name, sizeof (name), "%s/credit_isolation",
		    spa_name(spa));
		VERIFY0(dmu_objset_create(name, DMU_OST_OTHER, 0, NULL,
		    NULL, NULL));
		VERIFY0(dmu_objset_own(name, DMU_OST_OTHER, B_FALSE, B_TRUE,
		    FTAG, &sets[i]));
		dmu_tx_t *tx = dmu_tx_create(sets[i]);
		dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
		objects[i] = dmu_object_alloc(sets[i], DMU_OT_UINT64_OTHER,
		    size, DMU_OT_NONE, 0, tx);
		dmu_tx_commit(tx);
		txg_wait_synced(spa->spa_dsl_pool, 0);
		mutex_enter(&pt->lock);
		pt->os = sets[i];
		mutex_exit(&pt->lock);
		probe_tx_limit(spa, i == 0 ? 64ULL << 20 : 0, 0);
	}
	for (uint_t i = 0; i < 2; i++) {
		spa_t *spa = spas[i];
		/* This ordinary assigned tx keeps the callback from running. */
		fences[i] = dmu_tx_create(sets[i]);
		VERIFY0(dmu_tx_assign(fences[i], DMU_TX_WAIT));
		dmu_tx_t *tx = dmu_tx_create(sets[i]);
		dmu_tx_hold_write(tx, objects[i], 0, size);
		if (i == 1) {
			VERIFY3S(dmu_tx_assign(tx, DMU_TX_NOWAIT), ==, ENOSPC);
			VERIFY0(pr19262_tx_reserved(spa));
			VERIFY3U(pr19262_tx_reserved(first), ==, held[0]);
			probe_tx_limit(spa, 64ULL << 20, 0);
		}
		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
		txgs[i] = dmu_tx_get_txg(tx);
		for (uint_t w = 0; w < size / sizeof (*data); w++)
			data[w] = (i + 1ULL) * 0x192620000000ULL + w;
		dmu_write(sets[i], objects[i], 0, size, data, tx, 0);
		dmu_tx_commit(tx);
		held[i] = pr19262_tx_reserved(spa);
		VERIFY3U(held[i], >, 0);
		VERIFY(pr19262_tx_client_txg(spa, txgs[i]));
		if (i == 0) {
			/* The same numeric txg has no peer credit. */
			VERIFY(!pr19262_tx_client_txg(second, txgs[0]));
			VERIFY0(pr19262_tx_reserved(second));
			pr19262_tx_fail_second(first, B_TRUE);
			VERIFY(pr19262_tx_fail_dva(first, 1));
			VERIFY(!pr19262_tx_fail_dva(second, 1));
			pr19262_tx_fail_second(first, B_FALSE);
		}
	}
	dmu_tx_commit(fences[0]);
	probe_tx_drain(first->spa_dsl_pool, txgs[0]);
	VERIFY0(pr19262_tx_reserved(first));
	VERIFY(!pr19262_tx_client_txg(first, txgs[1]));
	VERIFY3U(pr19262_tx_reserved(second), ==, held[1]);
	VERIFY(pr19262_tx_client_txg(second, txgs[1]));
	(void) printf("PR19262_TX_ISOLATED first_txg=%llu second_txg=%llu "
	    "first_drained=1 peer_held=%llu\n", (u_longlong_t)txgs[0],
	    (u_longlong_t)txgs[1], (u_longlong_t)held[1]);
	dmu_tx_commit(fences[1]);
	probe_tx_drain(second->spa_dsl_pool, txgs[1]);
	for (uint_t i = 0; i < 2; i++) {
		probe_tx_state_t *pt = spas[i]->spa_pr19262_tx;
		VERIFY0(dmu_read(sets[i], objects[i], 0, size,
		    readback, DMU_READ_NO_PREFETCH));
		for (uint_t w = 0; w < size / sizeof (*data); w++)
			VERIFY3U(readback[w], ==,
			    (i + 1ULL) * 0x192620000000ULL + w);
		mutex_enter(&pt->lock);
		pt->os = NULL;
		mutex_exit(&pt->lock);
		dmu_objset_disown(sets[i], B_TRUE, FTAG);
		pr19262_tx_check(spas[i]);
	}
	kmem_free(data, size);
	kmem_free(readback, size);
	(void) printf("PR19262_TX_ISOLATION_PASS\n");
}
#endif
