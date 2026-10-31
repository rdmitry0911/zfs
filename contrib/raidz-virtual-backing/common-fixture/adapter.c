// SPDX-License-Identifier: CDDL-1.0
/* Test-only adapter; the neutral generator is included without edits. */
#include <malloc.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/arc.h>
#include <sys/arc_impl.h>
#include <sys/vdev.h>
#include <sys/zfeature.h>
#include <sys/vdev_reloc_engine.h>

int common_fixture_main(int, char **);
#define	main common_fixture_main
#include "fixture.c"
#undef main

typedef struct heap_sample {
	boolean_t stop;
	uint64_t peak;
	uint64_t count;
} heap_sample_t;

typedef struct io_snapshot {
	uint64_t reads;
	uint64_t writes;
	uint64_t rops;
	uint64_t wops;
	uint64_t alloc;
	uint64_t space;
} io_snapshot_t;

typedef struct mover_stats {
	double activation_s;
	double epoch_s;
	double sweep_s;
	double fence_certify_s;
	double elapsed_s;
	double mover_total_s;
	double final_census_s;
	uint64_t epoch_birth;
	heap_sample_t heap;
	io_snapshot_t io;
	pr19262_engine_convert_stats_t convert;
} mover_stats_t;

static void *
heap_sampler(void *arg)
{
	heap_sample_t *s = arg;

	while (!__atomic_load_n(&s->stop, __ATOMIC_RELAXED)) {
		struct mallinfo2 m = mallinfo2();

		s->peak = MAX(s->peak, m.uordblks);
		s->count++;
		(void) usleep(1000);
	}
	return (NULL);
}

static double
seconds_since(hrtime_t start)
{
	return ((double)(gethrtime() - start) / 1e9);
}

static void
verify_payload(void)
{
	if (spec.head != 0)
		verify_object(POOL, spec.hobj, spec.head, 2, B_FALSE);
	if (spec.retained != 0) {
		verify_object(POOL, spec.robj, spec.retained, 1, B_TRUE);
		if (!spec.dropped)
			verify_object(POOL "@retained", spec.robj,
			    spec.retained, 1, B_FALSE);
	}
}

static void
leaf_stats(vdev_t *vd, uint64_t *reads, uint64_t *writes,
    uint64_t *rops, uint64_t *wops)
{
	if (vd->vdev_children != 0) {
		for (uint64_t i = 0; i < vd->vdev_children; i++)
			leaf_stats(vd->vdev_child[i], reads, writes,
			    rops, wops);
		return;
	}
	vdev_stat_t s;

	vdev_get_stats(vd, &s);
	*reads += s.vs_bytes[ZIO_TYPE_READ];
	*writes += s.vs_bytes[ZIO_TYPE_WRITE];
	*rops += s.vs_ops[ZIO_TYPE_READ];
	*wops += s.vs_ops[ZIO_TYPE_WRITE];
}

static void
snapshot_io(spa_t *spa, io_snapshot_t *io)
{
	vdev_stat_t s;

	memset(io, 0, sizeof (*io));
	vdev_get_stats(spa->spa_root_vdev, &s);
	io->alloc = s.vs_alloc;
	io->space = s.vs_space;
	leaf_stats(spa->spa_root_vdev, &io->reads, &io->writes,
	    &io->rops, &io->wops);
}

static void
report_census(const pr19262_engine_census_t *c)
{
	boolean_t first = B_TRUE;

	(void) printf("{");
#define	CENSUS_FIELD(field) do { \
	(void) printf("%s\"" #field "\":%llu", first ? "" : ",", \
	    (u_longlong_t)c->field); \
	first = B_FALSE; \
} while (0)
	CENSUS_FIELD(owners);
	CENSUS_FIELD(legacy_bytes);
	CENSUS_FIELD(legacy_segments);
	CENSUS_FIELD(unusable_bytes);
	CENSUS_FIELD(current_versions);
	CENSUS_FIELD(current_bytes);
	CENSUS_FIELD(weak_current);
	CENSUS_FIELD(weak_current_bytes);
	CENSUS_FIELD(retired_versions);
	CENSUS_FIELD(retired_bytes);
	CENSUS_FIELD(weak_retired);
	CENSUS_FIELD(retired_owners);
	CENSUS_FIELD(free_bytes);
	CENSUS_FIELD(store_bytes);
	CENSUS_FIELD(journal_bytes);
	CENSUS_FIELD(journal_segments);
	CENSUS_FIELD(journal_retired);
	CENSUS_FIELD(journal_freed_bytes);
	CENSUS_FIELD(tombstones);
	CENSUS_FIELD(min_parity);
	CENSUS_FIELD(floor);
	CENSUS_FIELD(certified);
	CENSUS_FIELD(durable_txg);
	CENSUS_FIELD(journal_seq);
	CENSUS_FIELD(pages_used);
	CENSUS_FIELD(pages_capacity);
#undef CENSUS_FIELD
	(void) printf("}");
}

static void
report_convert(const pr19262_engine_convert_stats_t *s)
{
	boolean_t first = B_TRUE;

	(void) printf("{");
#define	CONVERT_FIELD(field) do { \
	(void) printf("%s\"" #field "\":%llu", first ? "" : ",", \
	    (u_longlong_t)s->field); \
	first = B_FALSE; \
} while (0)
	CONVERT_FIELD(passes);
	CONVERT_FIELD(slices);
	CONVERT_FIELD(hold_ms_max);
	CONVERT_FIELD(slice_over_ms_max);
	CONVERT_FIELD(visited);
	CONVERT_FIELD(gang_members);
	CONVERT_FIELD(zil_blocks);
	CONVERT_FIELD(candidates);
	CONVERT_FIELD(copied);
	CONVERT_FIELD(unwritten);
	CONVERT_FIELD(old_bytes);
	CONVERT_FIELD(new_bytes);
	CONVERT_FIELD(busy);
	CONVERT_FIELD(stale);
	CONVERT_FIELD(starved);
	CONVERT_FIELD(fences);
	CONVERT_FIELD(reclaimed_bytes);
	CONVERT_FIELD(released_owners);
	CONVERT_FIELD(peak_retired_bytes);
	CONVERT_FIELD(floor);
#undef CONVERT_FIELD
	(void) printf("}");
}

static void
report(spa_t *spa, const char *phase, boolean_t readonly,
    const mover_stats_t *m, const pr19262_engine_census_t *frozen)
{
	boolean_t active = pr19262_engine_active(spa);
	pr19262_engine_census_t census;
	struct rusage usage;
	struct mallinfo2 heap = mallinfo2();
	io_snapshot_t io;
	double census_s = 0;

	VERIFY0(getrusage(RUSAGE_SELF, &usage));
	if (m == NULL)
		snapshot_io(spa, &io);
	else
		io = m->io;
	(void) printf("VB_JSON {\"phase\":\"%s\",\"readonly\":%s,"
	    "\"engine_active\":%s,\"physical_rewrite_enabled\":%s,"
	    "\"physical_rewrite_active\":%s,\"dropped\":%u,"
	    "\"record_bytes\":%llu,\"retained_payload_bytes\":%llu,"
	    "\"head_payload_bytes\":%llu,\"native_alloc_bytes\":%llu,"
	    "\"native_space_bytes\":%llu,\"vdev_read_bytes\":%llu,"
	    "\"vdev_write_bytes\":%llu,\"vdev_read_ops\":%llu,"
	    "\"vdev_write_ops\":%llu,\"heap_uordblks_bytes\":%llu,"
	    "\"process_ru_maxrss_kib\":%llu,\"census\":",
	    phase, readonly ? "true" : "false", active ? "true" : "false",
	    spa_feature_is_enabled(spa, SPA_FEATURE_PHYSICAL_REWRITE) ?
	    "true" : "false",
	    spa_feature_is_active(spa, SPA_FEATURE_PHYSICAL_REWRITE) ?
	    "true" : "false", spec.dropped,
	    (u_longlong_t)spec.record, (u_longlong_t)spec.retained,
	    (u_longlong_t)spec.head, (u_longlong_t)io.alloc,
	    (u_longlong_t)io.space, (u_longlong_t)io.reads,
	    (u_longlong_t)io.writes, (u_longlong_t)io.rops,
	    (u_longlong_t)io.wops,
	    (u_longlong_t)heap.uordblks, (u_longlong_t)usage.ru_maxrss);
	if (active) {
		if (frozen == NULL) {
			hrtime_t start = gethrtime();

			pr19262_engine_census(spa, 2, &census);
			census_s = seconds_since(start);
			frozen = &census;
		} else if (m != NULL) {
			census_s = m->final_census_s;
		}
		report_census(frozen);
	} else {
		(void) printf("null");
	}
	(void) printf(",\"allocation_census_s\":%.9f", census_s);
	if (m != NULL) {
		(void) printf(",\"activation_s\":%.9f,\"epoch_s\":%.9f,"
		    "\"sweep_s\":%.9f,\"fence_certify_s\":%.9f,"
		    "\"elapsed_s\":%.9f,\"mover_total_s\":%.9f,"
		    "\"epoch_birth\":%llu,"
		    "\"heap_max_observed_uordblks_bytes\":%llu,"
		    "\"heap_sample_count\":%llu,"
		    "\"heap_sample_interval_us\":1000,\"stats\":",
		    m->activation_s, m->epoch_s, m->sweep_s,
		    m->fence_certify_s, m->elapsed_s, m->mover_total_s,
		    (u_longlong_t)m->epoch_birth,
		    (u_longlong_t)m->heap.peak, (u_longlong_t)m->heap.count);
		report_convert(&m->convert);
	}
	(void) printf("}\n");
	(void) fflush(NULL);
}

static void
require_complete(spa_t *spa, pr19262_engine_census_t *out)
{
	pr19262_engine_census_t c;

	VERIFY(pr19262_engine_persistent(spa));
	pr19262_engine_census(spa, 2, &c);
	VERIFY0(c.legacy_bytes);
	VERIFY0(c.weak_current);
	VERIFY0(c.weak_retired);
	VERIFY3U(c.certified, ==, 2);
	VERIFY3U(c.min_parity, >=, 2);
	if (out != NULL)
		*out = c;
}

static void
convert_pool(spa_t *spa)
{
	mover_stats_t m = { 0 };
	pr19262_engine_census_t final;
	pthread_t sampler;
	uint64_t floor, reclaimed, released;
	hrtime_t overall, start;

	VERIFY(!pr19262_engine_active(spa));
	VERIFY(spa_feature_is_enabled(spa, SPA_FEATURE_PHYSICAL_REWRITE));
	report(spa, "before-mover", B_FALSE, NULL, NULL);
	overall = start = gethrtime();
	m.heap.peak = mallinfo2().uordblks;
	VERIFY0(pthread_create(&sampler, NULL, heap_sampler, &m.heap));
	VERIFY0(pr19262_engine_start_persistent(spa));
	m.activation_s = seconds_since(start);
	start = gethrtime();
	VERIFY0(pr19262_engine_epoch(spa, 2, &m.epoch_birth));
	m.epoch_s = seconds_since(start);
	report(spa, "before-convert", B_FALSE, NULL, NULL);
	start = gethrtime();
	VERIFY0(pr19262_engine_convert(spa, m.epoch_birth, 8ULL << 20,
	    4096, &m.convert));
	m.sweep_s = seconds_since(start);
	start = gethrtime();
	VERIFY0(pr19262_engine_fence(spa, &floor, &reclaimed,
	    &released, NULL));
	VERIFY0(pr19262_engine_certify(spa, 2));
	txg_wait_synced(spa_get_dsl(spa), 0);
	m.fence_certify_s = seconds_since(start);
	m.elapsed_s = m.sweep_s + m.fence_certify_s;
	start = gethrtime();
	require_complete(spa, &final);
	m.final_census_s = seconds_since(start);
	snapshot_io(spa, &m.io);
	m.mover_total_s = seconds_since(overall);
	__atomic_store_n(&m.heap.stop, B_TRUE, __ATOMIC_RELAXED);
	VERIFY0(pthread_join(sampler, NULL));
	m.heap.peak = MAX(m.heap.peak, mallinfo2().uordblks);
	report(spa, "convert", B_FALSE, &m, &final);
	/* Byte verification is deliberately outside every mover timer. */
	verify_payload();
}

int
main(int argc, char **argv)
{
	boolean_t enable, convert, inspect, drop;
	spa_t *spa;
	char cache[4096];
	uint64_t floor, reclaimed, released;

	if (argc != 3) {
		(void) fprintf(stderr, "usage: %s "
		    "enable|convert|inspect|drop-retained DIR\n", argv[0]);
		return (2);
	}
	enable = strcmp(argv[1], "enable") == 0;
	convert = strcmp(argv[1], "convert") == 0;
	inspect = strcmp(argv[1], "inspect") == 0;
	drop = strcmp(argv[1], "drop-retained") == 0;
	if (!enable && !convert && !inspect && !drop)
		return (2);
	dir = argv[2];
	if (dir[0] != '/' || strlen(dir) > 3900)
		return (2);
	load_spec();
	path_for(cache, sizeof (cache), "adapter.cache");
	spa_config_path = cache;
	kernel_init(inspect ? SPA_MODE_READ :
	    SPA_MODE_READ | SPA_MODE_WRITE);
	/* Explicit, identical per-process limit for both build profiles. */
	arc_c_max = 256ULL << 20;
	arc_c_min = MIN(arc_c_min, arc_c_max / 2);
	arc_c = MIN(arc_c, arc_c_max);
	spa = import_pool();
	if (enable) {
		nvlist_t *props = fnvlist_alloc();

		VERIFY(!pr19262_engine_active(spa));
		fnvlist_add_uint64(props, "feature@physical_rewrite", 0);
		VERIFY0(spa_prop_set(spa, props));
		fnvlist_free(props);
		txg_wait_synced(spa_get_dsl(spa), 0);
		VERIFY(spa_feature_is_enabled(spa,
		    SPA_FEATURE_PHYSICAL_REWRITE));
	} else if (convert) {
		convert_pool(spa);
	} else if (drop) {
		require_complete(spa, NULL);
		if (spec.retained != 0 && !spec.dropped) {
			VERIFY0(dsl_destroy_snapshot(POOL "@retained",
			    B_FALSE));
			spec.dropped = 1;
		}
		/* Drain native deferred frees through their ordinary txgs. */
		for (uint_t i = 0; i < TXG_DEFER_SIZE + 2; i++)
			txg_wait_synced(spa_get_dsl(spa), 0);
		VERIFY0(pr19262_engine_fence(spa, &floor, &reclaimed,
		    &released, NULL));
		VERIFY0(pr19262_engine_certify(spa, 2));
		txg_wait_synced(spa_get_dsl(spa), 0);
		require_complete(spa, NULL);
	} else if (pr19262_engine_active(spa)) {
		require_complete(spa, NULL);
	}
	if (!convert) {
		report(spa, argv[1], inspect, NULL, NULL);
		verify_payload();
	}
	spa_close(spa, FTAG);
	if (!inspect) {
		VERIFY0(spa_export(POOL, NULL, B_FALSE, B_FALSE));
		if (drop)
			save_spec();
	}
	kernel_fini();
	return (0);
}
