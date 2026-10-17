/* SPDX-License-Identifier: CDDL-1.0 */
/*
 * Private userspace admission experiment, included by dmu_tx.c.
 * Exercise normal DMU assignment/rollback and native TXG callbacks.
 * A fixture capacity cap is NOT a persistent physical allocator/catalog.
 */
#include <sys/metaslab.h>
#include <sys/metaslab_impl.h>
#include <sys/spa_impl.h>

typedef struct probe_tx_credit {
	uint64_t bytes;
	uint64_t txg;
} probe_tx_credit_t;

static struct {
	kmutex_t lock;
	kcondvar_t done;
	objset_t *os;
	uint64_t cap;
	uint64_t emergency;
	uint64_t reserved;
	uint64_t pending;
	uint64_t rejected;
	uint64_t granted;
	uint64_t rolled_back;
	uint64_t callbacks;
} probe_tx;

static void
probe_tx_credit_free(probe_tx_credit_t *c)
{
	ASSERT(MUTEX_HELD(&probe_tx.lock));
	VERIFY3U(probe_tx.reserved, >=, c->bytes);
	probe_tx.reserved -= c->bytes;
	kmem_free(c, sizeof (*c));
}

static int
probe_tx_admit(dmu_tx_t *tx, uint64_t bytes)
{
	int error = 0;
	mutex_enter(&probe_tx.lock);
	if (tx->tx_objset != probe_tx.os || probe_tx.os == NULL || bytes == 0)
		goto out;
	VERIFY3P(tx->tx_pr19262_physical_credit, ==, NULL);
	metaslab_class_t *mc = spa_normal_class(tx->tx_pool->dp_spa);
	uint64_t total = metaslab_class_get_space(mc);
	uint64_t allocated = metaslab_class_get_alloc(mc);
	uint64_t available = total > allocated ? total - allocated : 0;
	uint64_t limit = MIN(probe_tx.cap, available);
	uint64_t protected = tx->tx_netfree ? 0 : probe_tx.emergency;
	limit = limit > protected ? limit - protected : 0;
	if (probe_tx.reserved > limit || bytes > limit - probe_tx.reserved) {
		probe_tx.rejected++;
		error = ENOSPC;
		goto out;
	}
	probe_tx_credit_t *c = kmem_alloc(sizeof (*c), KM_SLEEP);
	c->bytes = bytes;
	c->txg = tx->tx_txg;
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
	probe_tx_credit_free(c);
	probe_tx.rolled_back++;
	tx->tx_pr19262_physical_credit = NULL;
	mutex_exit(&probe_tx.lock);
}

static void
probe_tx_synced(void *arg, int error)
{
	probe_tx_credit_t *c = arg;
	VERIFY0(error);
	VERIFY3U(spa_last_synced_txg(dmu_objset_spa(probe_tx.os)), >=, c->txg);
	mutex_enter(&probe_tx.lock);
	VERIFY3U(probe_tx.pending, >=, c->bytes);
	probe_tx.pending -= c->bytes;
	probe_tx.callbacks++;
	probe_tx_credit_free(c);
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
	while (probe_tx.pending != 0) {
		VERIFY3S(cv_timedwait(&probe_tx.done, &probe_tx.lock,
		    ddi_get_lbolt() + SEC_TO_TICK(30)), !=, -1);
	}
	VERIFY0(probe_tx.reserved);
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
	(void) printf("PR19262_TX_PASS held=%llu rollback=%llu "
	    "callbacks=%llu rejected=%llu granted=%llu\n",
	    (u_longlong_t)held, (u_longlong_t)probe_tx.rolled_back,
	    (u_longlong_t)probe_tx.callbacks,
	    (u_longlong_t)probe_tx.rejected, (u_longlong_t)probe_tx.granted);
	mutex_enter(&probe_tx.lock);
	probe_tx.os = NULL;
	mutex_exit(&probe_tx.lock);
	dmu_objset_disown(os, B_TRUE, FTAG);
	kmem_free(data, 131072);
	kmem_free(readback, 131072);
}
