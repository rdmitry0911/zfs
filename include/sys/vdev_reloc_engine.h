/* SPDX-License-Identifier: CDDL-1.0 */
#ifndef _VDEV_RELOC_ENGINE_H
#define	_VDEV_RELOC_ENGINE_H

/*
 * The engine prints its witness markers and reads its test hooks through
 * these: userspace (ztest) prints to stdout and reads the environment, the
 * kernel logs to zfs_dbgmsg and has no hooks (every cut point is dead).
 */
#ifdef _KERNEL
/* An expression (zfs_dbgmsg is an if statement): usable in a ternary. */
#define	pr19262_printf(...) \
	(zfs_dbgmsg_enable ? __dprintf(B_FALSE, __FILE__, __func__, __LINE__, \
	__VA_ARGS__) : (void)0)
#define	pr19262_env(name)	((const char *)NULL)
#define	pr19262_flush()		((void)0)
#define	pr19262_exit(code)	panic("pr19262 cut %d", (int)(code))
#define	pr19262_strtoull(s, e, b)	(0ULL)
#else
#define	pr19262_printf(...)	(void) printf(__VA_ARGS__)
#define	pr19262_env(name)	getenv(name)
#define	pr19262_flush()		(void) fflush(NULL)
#define	pr19262_exit(code)	_exit(code)
#define	pr19262_strtoull(s, e, b)	strtoull(s, e, b)
#endif

/*
 * Arrays sized by owners, rows or pages: the kernel takes them from the
 * virtual allocator (no "large kmem_alloc" warning); kmem_free handles both.
 */
#ifdef _KERNEL
#define	pr19262_alloc(size, flags)	vmem_alloc(size, flags)
#define	pr19262_zalloc(size, flags)	vmem_zalloc(size, flags)
#else
#define	pr19262_alloc(size, flags)	kmem_alloc(size, flags)
#define	pr19262_zalloc(size, flags)	kmem_zalloc(size, flags)
#endif

/* Every invalid-format verdict of the engine names its site in the log. */
#define	ENGINE_EINVAL(reason) \
	(pr19262_printf("PR19262_EINVAL file=%s reason=%d line=%d\n", \
	__FILE__, (reason), __LINE__), EINVAL)
#include <sys/uberblock.h>
#include <sys/range_tree.h>
/* Private userspace experiment; no supported persistent/public ABI. */
#define	PR19262_ENGINE_LABEL_FEATURE "org.openzfs:pr19262_private_vp"
boolean_t pr19262_engine_persistent(spa_t *);
boolean_t pr19262_engine_store_dirty(spa_t *);
void pr19262_engine_store_enable(spa_t *);
int pr19262_engine_store_sync(spa_t *, uberblock_t *);
int pr19262_engine_store_load(spa_t *, const uberblock_t *);
int pr19262_engine_store_recover(spa_t *);
void pr19262_engine_store_rebind(spa_t *);
void pr19262_engine_store_unload(spa_t *);
void pr19262_engine_checkpoint(spa_t *, const uberblock_t *);
int pr19262_engine_checkpoint_validate(spa_t *, const uberblock_t *);
int pr19262_engine_checkpoint_discard(vdev_t *, uint64_t, uint64_t,
    boolean_t *);
boolean_t pr19262_engine_store_ub(const uberblock_t *);
/* Caller holds SCL_STATE; callbacks run after all reads complete. */
int pr19262_vdev_uberblocks(spa_t *, void *,
    int (*)(void *, const uberblock_t *));
int pr19262_engine_store_inventory(spa_t *, uint64_t *, uint64_t *);
void pr19262_engine_store_test_hook(void (*)(spa_t *, int));
typedef struct pr19262_mgrowth_probe {
	uint64_t p;
	uint64_t pages;
	uint64_t uuid[2];
	uint64_t phase;
	uint64_t format_generation;
	uint64_t candidate[6];
	uint64_t directory[6];
	uint64_t serial;
	uint64_t control_serial;
	uint64_t capacity;
	uint64_t directory_bytes;
	uint64_t directory_hash[4];
	uint64_t bitmap_hash[4];
	uint64_t bitmap_canonical[4];
	uint64_t bitmap_used;
	uint64_t arenas;
	uint64_t floor;
	uint64_t certified;
} pr19262_mgrowth_probe_t;
int pr19262_engine_mgrowth_probe(spa_t *, boolean_t,
    pr19262_mgrowth_probe_t *);
int pr19262_engine_metadata_flush(vdev_t *);
void pr19262_engine_store_crash(uint64_t);
int pr19262_engine_journal_bind(spa_t *, const blkptr_t *);
void pr19262_engine_journal_gate(zio_t *, spa_t *, const blkptr_t *);
void pr19262_engine_journal_test_hook(void (*)(spa_t *, const blkptr_t *,
    int));
int pr19262_zio_walk_gang(spa_t *, const blkptr_t *, void *,
    int (*)(spa_t *, const blkptr_t *, void *), void *);
int pr19262_engine_journal_claim(spa_t *, uint64_t);
boolean_t pr19262_engine_journal_claimed(spa_t *, vdev_t *, uint64_t,
    uint64_t, uint64_t, uint64_t);
void pr19262_engine_journal_ub_committed(spa_t *, const uberblock_t *);
void pr19262_engine_journal_fault(uint_t);
int pr19262_engine_journal_roll(spa_t *);
void pr19262_engine_journal_roll_fault(uint_t);
int pr19262_engine_journal_wait_error(void);
void pr19262_engine_init(void);
void pr19262_engine_fini(void);
void pr19262_engine_start(spa_t *);
boolean_t pr19262_engine_active(spa_t *);
vdev_t *pr19262_engine_vdev(spa_t *);
boolean_t pr19262_engine_bp_foreign(spa_t *, const blkptr_t *);
uint64_t pr19262_engine_limit(spa_t *, boolean_t);
void pr19262_engine_block_new(boolean_t);
void pr19262_engine_fail_writes(boolean_t);
void pr19262_engine_retired_limit(uint64_t);
void pr19262_engine_alloc_fail_after(uint64_t);
int pr19262_engine_pair(spa_t *, vdev_t *, uint64_t, uint64_t, uint64_t);
typedef enum pr19262_engine_alloc_result {
	PR19262_ALLOC_INACTIVE,
	PR19262_ALLOC_OK,
	PR19262_ALLOC_NATIVE_FULL,
	PR19262_ALLOC_V_BLOCKED,
	PR19262_ALLOC_P_BLOCKED,
	PR19262_ALLOC_NOMEM
} pr19262_engine_alloc_result_t;
/* Caller holds SCL_ALLOC and the native metaslab lock. No I/O is issued. */
pr19262_engine_alloc_result_t pr19262_engine_alloc(spa_t *, vdev_t *,
    zfs_range_tree_t *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *,
    uint64_t *);
/* Returns true when J admission requires retaining native allocation. */
boolean_t pr19262_engine_abort(spa_t *, vdev_t *, uint64_t, uint64_t,
    uint64_t);
boolean_t pr19262_engine_retire(vdev_t *, uint64_t, uint64_t, boolean_t);
void *pr19262_engine_hold(spa_t *, const blkptr_t *, void *, boolean_t,
    boolean_t);
void pr19262_engine_rele(void *);
int pr19262_engine_dispatch(zio_t *);
void pr19262_engine_io_rele(zio_t *);
int pr19262_engine_copy_begin(spa_t *, const blkptr_t *, int, uint64_t,
    void **);
int pr19262_engine_copy_run(void *, boolean_t);
int pr19262_engine_rebind_unwritten(spa_t *, const blkptr_t *, int, uint64_t);
zio_t *pr19262_engine_physical_io(spa_t *, blkptr_t *, vdev_t *,
    uint64_t, abd_t *, uint64_t, zio_type_t, uint64_t, void *);
void pr19262_engine_park_next(void);
uint64_t pr19262_engine_waiting(boolean_t);
void pr19262_engine_unpark(void);
typedef struct pr19262_engine_info {
	uint64_t p;
	uint64_t vsize;
	uint64_t psize;
	uint64_t birth;
	uint64_t leases;
	uint64_t old_leases;
	uint64_t versions;
	uint64_t incarnation;
	uint64_t holds;
	uint64_t journal_seq;
	boolean_t aborted;
	boolean_t retired;
	boolean_t journal_touched;
	uint64_t queued_writers;
	boolean_t copying;
	boolean_t ready;
	uint64_t catalog_txg;
} pr19262_engine_info_t;
void pr19262_engine_inspect(spa_t *, const blkptr_t *, int,
    pr19262_engine_info_t *);
boolean_t pr19262_engine_free_range(spa_t *, uint64_t, uint64_t);
void pr19262_engine_check(spa_t *, const char *);
zio_t *pr19262_engine_read_bound(zio_t *, spa_t *, const blkptr_t *,
    abd_t *, uint64_t, zio_done_func_t *, void *, zio_priority_t,
    zio_flag_t, const zbookmark_phys_t *, void **);
/* Whole-vdev converter, census, durable floor and certificate. */
typedef struct pr19262_engine_census {
	uint64_t owners;
	uint64_t legacy_bytes;
	uint64_t legacy_segments;
	/* Rows beyond the last metaslab: never allocatable in either domain. */
	uint64_t unusable_bytes;
	uint64_t current_versions;
	uint64_t current_bytes;
	uint64_t weak_current;
	uint64_t weak_current_bytes;
	uint64_t retired_versions;
	uint64_t retired_bytes;
	uint64_t weak_retired;
	uint64_t retired_owners;
	uint64_t free_bytes;
	uint64_t store_bytes;
	uint64_t journal_bytes;
	/* Live chain, cut segments awaiting a fence, bytes returned so far. */
	uint64_t journal_segments;
	uint64_t journal_retired;
	uint64_t journal_freed_bytes;
	uint64_t tombstones;
	uint64_t min_parity;
	uint64_t floor;
	uint64_t certified;
	uint64_t durable_txg;
	uint64_t journal_seq;
	uint64_t pages_used;
	uint64_t pages_capacity;
} pr19262_engine_census_t;
void pr19262_engine_census(spa_t *, uint64_t, pr19262_engine_census_t *);
typedef struct pr19262_engine_convert_stats {
	uint64_t passes;
	uint64_t visited;
	uint64_t gang_members;
	uint64_t zil_blocks;
	uint64_t candidates;
	uint64_t copied;
	/* Allocated but never written log blocks rebound without a copy. */
	uint64_t unwritten;
	uint64_t old_bytes;
	uint64_t new_bytes;
	uint64_t busy;
	uint64_t stale;
	/* Copies refused by admission: the pass fenced and retried. */
	uint64_t starved;
	uint64_t fences;
	uint64_t reclaimed_bytes;
	uint64_t released_owners;
	uint64_t peak_retired_bytes;
	uint64_t floor;
	/* The first physical extent any fence returned to the allocator. */
	uint64_t sample[2];
} pr19262_engine_convert_stats_t;
int pr19262_engine_convert(spa_t *, uint64_t, uint64_t, uint64_t,
    pr19262_engine_convert_stats_t *);
int pr19262_engine_fence(spa_t *, uint64_t *, uint64_t *, uint64_t *,
    uint64_t *);
int pr19262_engine_certify(spa_t *, uint64_t);
int pr19262_engine_epoch(spa_t *, uint64_t, uint64_t *);
uint64_t pr19262_engine_floor(spa_t *);
/* Incarnation of an owner overlapping [V, V + size), or 0 when none. */
uint64_t pr19262_engine_owner_incarnation(spa_t *, uint64_t, uint64_t);
/* Vector admission: physical bytes, catalog pages, journal frames. */
#define	PR19262_ADMIT_CLIENT 0
#define	PR19262_ADMIT_NETFREE 1
#define	PR19262_ADMIT_CONVERTER 2
#define	PR19262_ADMIT_CLASSES 3
typedef struct engine_admit_class {
	uint64_t p_bytes;
	uint64_t m_pages;
	uint64_t j_frames;
	uint64_t admitted;
	uint64_t rejected;
	uint64_t peak_p;
	uint64_t peak_m;
	uint64_t peak_j;
} engine_admit_class_t;
typedef struct pr19262_engine_admit_report {
	uint64_t free;
	uint64_t protected;
	uint64_t sync_bytes;
	uint64_t log_bytes;
	uint64_t reserve;
	uint64_t reserve_growth;
	uint64_t reserve_legacy;
	uint64_t reserve_weak;
	uint64_t batch;
	uint64_t parity;
	uint64_t growth;
	uint64_t growth_peak;
	uint64_t pages_total;
	uint64_t pages_free;
	uint64_t frames_total;
	uint64_t frames_free;
	uint64_t levels;
	uint64_t converter_bytes;
	uint64_t space_rejections;
	uint64_t restarts;
	/* Free space of other normal-class vdevs; guarded allocations. */
	uint64_t foreign;
	uint64_t guarded;
	engine_admit_class_t classes[PR19262_ADMIT_CLASSES];
} pr19262_engine_admit_report_t;
int pr19262_engine_admit(spa_t *, uint_t, uint64_t, uint64_t, uint64_t,
    uint64_t);
void pr19262_engine_admit_release(spa_t *, uint_t, uint64_t, uint64_t,
    uint64_t);
uint64_t pr19262_engine_admit_row_pages(spa_t *);
void pr19262_engine_plan(spa_t *, uint64_t, uint64_t);
void pr19262_engine_admit_report(spa_t *, const char *,
    pr19262_engine_admit_report_t *);
uint64_t pr19262_engine_hidden_dspace(spa_t *);
/* Per-member count of valid live raw pages (arrays of vdev_children). */
int pr19262_engine_raw_audit(spa_t *, const char *, uint64_t *, uint64_t *);
#endif
