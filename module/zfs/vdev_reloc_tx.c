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
} probe_tx_credit_t;

typedef struct probe_tx_allocation {
	list_node_t link;
	vdev_t *vd;
	uint64_t offset;
	uint64_t size;
	uint64_t spent;
	probe_tx_budget_t *budget;
} probe_tx_allocation_t;

static struct {
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
} probe_tx;

static boolean_t
probe_tx_scope(spa_t *spa)
{
	return ((probe_tx.os != NULL && dmu_objset_spa(probe_tx.os) == spa) ||
	    pr19262_engine_active(spa));
}

void
pr19262_tx_fail_second(boolean_t fail)
{
	mutex_enter(&probe_tx.lock);
	probe_tx.fail_second = fail;
	mutex_exit(&probe_tx.lock);
}

boolean_t
pr19262_tx_fail_dva(spa_t *spa, int d)
{
	mutex_enter(&probe_tx.lock);
	boolean_t fail = probe_tx_scope(spa) &&
	    probe_tx.fail_second && d == 1;
	mutex_exit(&probe_tx.lock);
	return (fail);
}

static probe_tx_budget_t *
probe_tx_budget_find(uint64_t txg)
{
	ASSERT(MUTEX_HELD(&probe_tx.lock));
	for (probe_tx_budget_t *b = list_head(&probe_tx.budgets); b != NULL;
	    b = list_next(&probe_tx.budgets, b))
		if (b->txg == txg)
			return (b);
	return (NULL);
}

static void
probe_tx_budget_drop(probe_tx_budget_t *b)
{
	if (b->refs != 0)
		return;
	VERIFY0(b->open);
	VERIFY0(b->committed);
	VERIFY0(b->inflight);
	VERIFY0(b->pending_refs);
	list_remove(&probe_tx.budgets, b);
	kmem_free(b, sizeof (*b));
}

static int
probe_tx_admit(dmu_tx_t *tx, uint64_t bytes)
{
	int error = 0;
	mutex_enter(&probe_tx.lock);
	spa_t *spa = tx->tx_pool->dp_spa;
	boolean_t runtime = pr19262_engine_active(spa);
	if (bytes == 0 || (!runtime &&
	    (tx->tx_objset != probe_tx.os || probe_tx.os == NULL)))
		goto out;
	VERIFY3P(tx->tx_pr19262_physical_credit, ==, NULL);
	metaslab_class_t *mc = spa_normal_class(tx->tx_pool->dp_spa);
	uint64_t total = metaslab_class_get_space(mc);
	uint64_t allocated = metaslab_class_get_alloc(mc);
	uint64_t available = total > allocated ? total - allocated : 0;
	/* Native committed ALLOC does not yet include these allocations. */
	uint64_t limit = MIN(probe_tx.cap, available);
	limit = limit > probe_tx.inflight ? limit - probe_tx.inflight : 0;
	uint64_t protected = tx->tx_netfree ? 0 : probe_tx.emergency;
	limit = limit > protected ? limit - protected : 0;
	if (runtime)
		limit = pr19262_engine_limit(spa, tx->tx_netfree);
	if (probe_tx.reserved > limit || bytes > limit - probe_tx.reserved) {
		probe_tx.rejected++;
		error = ENOSPC;
		goto out;
	}
	probe_tx_budget_t *b = probe_tx_budget_find(tx->tx_txg);
	if (b == NULL) {
		b = kmem_zalloc(sizeof (*b), KM_SLEEP);
		b->txg = tx->tx_txg;
		list_insert_tail(&probe_tx.budgets, b);
	}
	probe_tx_credit_t *c = kmem_alloc(sizeof (*c), KM_SLEEP);
	c->bytes = bytes;
	c->txg = tx->tx_txg;
	c->spa = spa;
	c->budget = b;
	b->open += bytes;
	b->refs++;
	tx->tx_pr19262_physical_credit = c;
	probe_tx.reserved += bytes;
	probe_tx.granted++;
out:
	mutex_exit(&probe_tx.lock);
	return (error);
}

static void
probe_tx_unreserve(dmu_tx_t *tx)
{
	probe_tx_credit_t *c = tx->tx_pr19262_physical_credit;
	if (c == NULL)
		return;
	mutex_enter(&probe_tx.lock);
	probe_tx_budget_t *b = c->budget;
	VERIFY3U(b->open, >=, c->bytes);
	b->open -= c->bytes;
	probe_tx.reserved -= c->bytes;
	b->refs--;
	probe_tx_budget_drop(b);
	kmem_free(c, sizeof (*c));
	probe_tx.rolled_back++;
	tx->tx_pr19262_physical_credit = NULL;
	mutex_exit(&probe_tx.lock);
}

void
pr19262_tx_allocated(spa_t *spa, metaslab_class_t *mc, vdev_t *vd,
    uint64_t offset, uint64_t size, uint64_t txg)
{
	mutex_enter(&probe_tx.lock);
	if (!probe_tx_scope(spa) ||
	    mc != spa_normal_class(spa))
		goto out;
	probe_tx_budget_t *b = probe_tx_budget_find(txg);
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
	probe_tx.reserved -= a->spent;
	probe_tx.pending -= a->spent;
	b->inflight += size;
	probe_tx.inflight += size;
	probe_tx.consumed += a->spent;
	list_insert_tail(&probe_tx.allocations, a);
out:
	mutex_exit(&probe_tx.lock);
}

void
pr19262_tx_unallocated(spa_t *spa, vdev_t *vd, uint64_t offset,
    uint64_t size, uint64_t txg)
{
	mutex_enter(&probe_tx.lock);
	if (!probe_tx_scope(spa))
		goto out;
	for (probe_tx_allocation_t *a = list_head(&probe_tx.allocations);
	    a != NULL; a = list_next(&probe_tx.allocations, a)) {
		if (a->vd != vd || a->offset != offset || a->size != size ||
		    a->budget->txg != txg)
			continue;
		probe_tx_budget_t *b = a->budget;
		b->committed += a->spent;
		probe_tx.reserved += a->spent;
		probe_tx.pending += a->spent;
		VERIFY3U(b->inflight, >=, size);
		VERIFY3U(probe_tx.inflight, >=, size);
		b->inflight -= size;
		probe_tx.inflight -= size;
		probe_tx.unallocations++;
		list_remove(&probe_tx.allocations, a);
		kmem_free(a, sizeof (*a));
		break;
	}
out:
	mutex_exit(&probe_tx.lock);
}

static void
probe_tx_synced(void *arg, int error)
{
	probe_tx_credit_t *c = arg;
	VERIFY0(error);
	VERIFY3U(spa_last_synced_txg(c->spa), >=, c->txg);
	mutex_enter(&probe_tx.lock);
	probe_tx_budget_t *b = c->budget;
	/* Physical ownership now remains charged by native metaslab ALLOC. */
	VERIFY3U(probe_tx.inflight, >=, b->inflight);
	probe_tx.inflight -= b->inflight;
	b->inflight = 0;
	probe_tx_allocation_t *a = list_head(&probe_tx.allocations);
	while (a != NULL) {
		probe_tx_allocation_t *next =
		    list_next(&probe_tx.allocations, a);
		if (a->budget == b) {
			list_remove(&probe_tx.allocations, a);
			kmem_free(a, sizeof (*a));
		}
		a = next;
	}
	VERIFY3U(b->pending_refs, >, 0);
	b->pending_refs--;
	b->refs--;
	if (b->pending_refs == 0) {
		VERIFY0(b->open);
		probe_tx.pending -= b->committed;
		probe_tx.reserved -= b->committed;
		b->committed = 0;
	}
	probe_tx_budget_drop(b);
	probe_tx.callbacks++;
	kmem_free(c, sizeof (*c));
	cv_broadcast(&probe_tx.done);
	mutex_exit(&probe_tx.lock);
}

static void
probe_tx_commit(dmu_tx_t *tx)
{
	probe_tx_credit_t *c = tx->tx_pr19262_physical_credit;
	if (c == NULL)
		return;
	mutex_enter(&probe_tx.lock);
	probe_tx_budget_t *b = c->budget;
	VERIFY3U(b->open, >=, c->bytes);
	b->open -= c->bytes;
	b->committed += c->bytes;
	b->pending_refs++;
	probe_tx.pending += c->bytes;
	tx->tx_pr19262_physical_credit = NULL;
	mutex_exit(&probe_tx.lock);
	dmu_tx_callback_register(tx, probe_tx_synced, c);
}

static void
probe_tx_limit(uint64_t cap, uint64_t emergency)
{
	mutex_enter(&probe_tx.lock);
	probe_tx.cap = cap;
	probe_tx.emergency = emergency;
	mutex_exit(&probe_tx.lock);
}

static uint64_t
probe_tx_reserved(void)
{
	mutex_enter(&probe_tx.lock);
	uint64_t r = probe_tx.reserved;
	mutex_exit(&probe_tx.lock);
	return (r);
}

static void
probe_tx_drain(dsl_pool_t *dp, uint64_t txg)
{
	txg_wait_synced(dp, txg);
	mutex_enter(&probe_tx.lock);
	while (!list_is_empty(&probe_tx.budgets)) {
		VERIFY3S(cv_timedwait(&probe_tx.done, &probe_tx.lock,
		    ddi_get_lbolt() + SEC_TO_TICK(30)), !=, -1);
	}
	VERIFY0(probe_tx.reserved);
	VERIFY0(probe_tx.inflight);
	mutex_exit(&probe_tx.lock);
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
	probe_tx_drain(spa->spa_dsl_pool, 0);
	mutex_enter(&probe_tx.lock);
	VERIFY0(probe_tx.reserved);
	VERIFY0(probe_tx.inflight);
	VERIFY(list_is_empty(&probe_tx.allocations));
	(void) printf("PR19262_ENGINE_BUDGET grants=%llu consumed=%llu "
	    "callbacks=%llu unallocations=%llu\n",
	    (u_longlong_t)probe_tx.granted, (u_longlong_t)probe_tx.consumed,
	    (u_longlong_t)probe_tx.callbacks,
	    (u_longlong_t)probe_tx.unallocations);
	mutex_exit(&probe_tx.lock);
}

void
pr19262_tx_probe(spa_t *spa)
{
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
	mutex_enter(&probe_tx.lock);
	probe_tx.os = os;
	mutex_exit(&probe_tx.lock);
	probe_tx_limit(0, 0);
	tx = dmu_tx_create(os);
	dmu_tx_hold_write(tx, object, 0, 131072);
	dsl_dir_t *dd = tx->tx_dir;
	uint64_t dsl_before = probe_tx_dsl_reserved(dd);
	VERIFY3S(dmu_tx_assign(tx, DMU_TX_NOWAIT), ==, ENOSPC);
	VERIFY0(tx->tx_txg);
	VERIFY3P(tx->tx_tempreserve_cookie, ==, NULL);
	VERIFY0(probe_tx_reserved());
	VERIFY3U(probe_tx_dsl_reserved(dd), ==, dsl_before);
	dmu_tx_abort(tx);
	/* DSL quota failure must return an already acquired P credit. */
	uint64_t used = dsl_dir_phys(dd)->dd_used_bytes;
	VERIFY3U(used, >, 0);
	VERIFY0(dsl_dir_set_quota(name, ZPROP_SRC_LOCAL, used));
	probe_tx_limit(64ULL << 20, 0);
	tx = dmu_tx_create(os);
	dmu_tx_hold_write(tx, object, 0, 131072);
	VERIFY3S(dmu_tx_assign(tx, DMU_TX_WAIT), ==, EDQUOT);
	VERIFY0(probe_tx_reserved());
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
	uint64_t held = probe_tx_reserved();
	VERIFY3U(held, >, 0);
	probe_tx_limit(held, 0);
	dmu_tx_t *retry = dmu_tx_create(os);
	dmu_tx_hold_space(retry, 4096);
	VERIFY3S(dmu_tx_assign(retry, DMU_TX_NOWAIT), ==, ENOSPC);
	VERIFY3U(probe_tx_reserved(), ==, held);
	void *data = kmem_alloc(131072, KM_SLEEP);
	void *readback = kmem_alloc(131072, KM_SLEEP);
	for (uint64_t i = 0; i < 131072 / sizeof (uint64_t); i++)
		((uint64_t *)data)[i] = i * 0x9e3779b97f4a7c15ULL;
	dmu_write(os, object, 0, 131072, data, tx, 0);
	dmu_tx_commit(tx);
	VERIFY3U(probe_tx_reserved(), ==, held);
	VERIFY3U(probe_tx.pending, ==, held);
	/* Retry the same failed tx after resources become available. */
	probe_tx_limit(64ULL << 20, 0);
	VERIFY0(dmu_tx_assign(retry, DMU_TX_WAIT));
	uint64_t retry_txg = dmu_tx_get_txg(retry);
	dmu_tx_commit(retry);
	VERIFY3U(probe_tx_reserved(), >, held);
	/* Native two-DVA allocation consumes budget; unalloc refunds it. */
	uint64_t before_alloc = probe_tx_reserved();
	blkptr_t rollback = { 0 };
	BP_SET_LSIZE(&rollback, 4096);
	BP_SET_PSIZE(&rollback, 4096);
	BP_SET_TYPE(&rollback, DMU_OT_UINT64_OTHER);
	zio_alloc_list_t trace;
	metaslab_trace_init(&trace);
	mutex_enter(&probe_tx.lock);
	probe_tx.fail_second = B_TRUE;
	mutex_exit(&probe_tx.lock);
	VERIFY3S(metaslab_alloc(spa, spa_normal_class(spa), 4096, &rollback,
	    2, retry_txg, NULL, 0, &trace, 0, FTAG), ==, ENOSPC);
	mutex_enter(&probe_tx.lock);
	probe_tx.fail_second = B_FALSE;
	mutex_exit(&probe_tx.lock);
	VERIFY0(BP_GET_NDVAS(&rollback));
	VERIFY0(BP_GET_PHYSICAL_BIRTH(&rollback));
	VERIFY3U(probe_tx_reserved(), ==, before_alloc);
	VERIFY0(probe_tx.inflight);
	VERIFY3U(probe_tx.unallocations, ==, 1);
	VERIFY0(metaslab_alloc(spa, spa_normal_class(spa), 4096, &rollback,
	    2, retry_txg, NULL, 0, &trace, 0, FTAG));
	uint64_t physical = DVA_GET_ASIZE(&rollback.blk_dva[0]) +
	    DVA_GET_ASIZE(&rollback.blk_dva[1]);
	VERIFY3U(probe_tx_reserved(), ==, before_alloc - physical);
	VERIFY3U(probe_tx.inflight, ==, physical);
	metaslab_free(spa, &rollback, retry_txg, B_TRUE);
	metaslab_trace_fini(&trace);
	VERIFY3U(probe_tx_reserved(), ==, before_alloc);
	VERIFY0(probe_tx.inflight);
	VERIFY3U(probe_tx.unallocations, ==, 3);
	dmu_tx_commit(fence);
	probe_tx_drain(dp, MAX(txg, retry_txg));
	VERIFY0(dmu_read(os, object, 0, 131072, readback,
	    DMU_READ_NO_PREFETCH));
	VERIFY0(memcmp(data, readback, 131072));
	/* A dedicated P headroom permits actual netfree under normal ENOSPC. */
	probe_tx_limit(8ULL << 20, 8ULL << 20);
	tx = dmu_tx_create(os);
	dmu_tx_hold_space(tx, 4096);
	VERIFY3S(dmu_tx_assign(tx, DMU_TX_NOWAIT), ==, ENOSPC);
	dmu_tx_abort(tx);
	tx = dmu_tx_create(os);
	dmu_tx_hold_free(tx, object, 0, DMU_OBJECT_END);
	dmu_tx_mark_netfree(tx);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	VERIFY3U(probe_tx_reserved(), >, 0);
	VERIFY0(dmu_object_free(os, object, tx));
	txg = dmu_tx_get_txg(tx);
	dmu_tx_commit(tx);
	probe_tx_drain(dp, txg);
	VERIFY3S(dmu_object_info(os, object, NULL), ==, ENOENT);
	VERIFY3U(probe_tx.rolled_back, ==, 1);
	VERIFY3U(probe_tx.callbacks, ==, 3);
	VERIFY3U(probe_tx.rejected, ==, 3);
	VERIFY3U(probe_tx.granted, ==, 4);
	VERIFY3U(probe_tx.consumed, >, physical);
	(void) printf("PR19262_TX_PASS held=%llu rollback=%llu "
	    "callbacks=%llu rejected=%llu granted=%llu "
	    "consumed=%llu unallocations=%llu\n",
	    (u_longlong_t)held, (u_longlong_t)probe_tx.rolled_back,
	    (u_longlong_t)probe_tx.callbacks,
	    (u_longlong_t)probe_tx.rejected, (u_longlong_t)probe_tx.granted,
	    (u_longlong_t)probe_tx.consumed,
	    (u_longlong_t)probe_tx.unallocations);
	mutex_enter(&probe_tx.lock);
	probe_tx.os = NULL;
	mutex_exit(&probe_tx.lock);
	dmu_objset_disown(os, B_TRUE, FTAG);
	kmem_free(data, 131072);
	kmem_free(readback, 131072);
}
