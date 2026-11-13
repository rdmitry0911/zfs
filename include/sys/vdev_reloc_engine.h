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
#define	pr19262_backtrace()	dump_stack()
#else
extern void libspl_backtrace(int);
#define	pr19262_backtrace()	libspl_backtrace(1)
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
#include <sys/dmu_traverse.h>
/* Private userspace experiment; no supported persistent/public ABI. */
#define	PR19262_ENGINE_LABEL_FEATURE "org.openzfs:pr19262_private_vp"
boolean_t pr19262_engine_persistent(spa_t *);
boolean_t pr19262_engine_store_dirty(spa_t *, uint64_t);
int pr19262_engine_store_enable(spa_t *);
int pr19262_engine_store_sync(spa_t *, uberblock_t *);
int pr19262_engine_store_load(spa_t *, const uberblock_t *);
int pr19262_engine_store_recover(spa_t *);
void pr19262_engine_store_rebind(spa_t *);
void pr19262_engine_store_unload(spa_t *);
void pr19262_engine_checkpoint(spa_t *, const uberblock_t *);
int pr19262_engine_checkpoint_admit(spa_t *);
uint64_t pr19262_engine_free_chunk_rows(spa_t *);
#define	PR19262_FREE_DEFER	0
#define	PR19262_FREE_ADMIT	1
#define	PR19262_FREE_AGED	2
/* L13: defer without forcing a txg (under a pool checkpoint). */
#define	PR19262_FREE_WAIT	3
#define	PR19262_FREE_AGE_TXGS	64
#define	PR19262_FREE_REAGE_TXGS	4
/* L13: the rows a long-free piece always may retire (reserve-funded). */
#define	PR19262_FREE_PIECE_ROWS	40
int pr19262_engine_free_pace(spa_t *, uint64_t, uint64_t, uint64_t,
    uint64_t);
int pr19262_engine_checkpoint_validate(spa_t *, const uberblock_t *);
int pr19262_engine_checkpoint_discard(vdev_t *, uint64_t, uint64_t,
    boolean_t *, uint64_t *);
struct zthr;
void pr19262_engine_checkpoint_discard_wait(spa_t *, struct zthr *);
boolean_t pr19262_engine_store_ub(const uberblock_t *);
/* Caller holds SCL_STATE; callbacks run after all reads complete. */
int pr19262_vdev_uberblocks(spa_t *, void *,
    int (*)(void *, const uberblock_t *));
int pr19262_vdev_uberblocks_r02(spa_t *, void *,
    int (*)(void *, const uberblock_t *), vdev_t *);
int pr19262_engine_store_inventory(spa_t *, uint64_t *, uint64_t *);
/*
 * A private, read-only exact-leaf closure for a later R02 reseed design.
 * It neither establishes replacement support nor authorizes any write.  The
 * entries are valid only for the duration of a visitor callback.  The
 * callback runs after all SPA and engine locks have been released, and gets
 * no engine-owned pointer or mutable allocator state.
 */
typedef enum pr19262_r02_manifest_kind {
	PR19262_R02_MANIFEST_ROOT,
	PR19262_R02_MANIFEST_CATALOG,
	PR19262_R02_MANIFEST_TREE,
	PR19262_R02_MANIFEST_CONTROL,
	PR19262_R02_MANIFEST_DIRECTORY,
	PR19262_R02_MANIFEST_ACTIVE_ARENA,
	PR19262_R02_MANIFEST_PENDING_ARENA,
	PR19262_R02_MANIFEST_JOURNAL_HEAD,
	PR19262_R02_MANIFEST_JOURNAL_BIND,
	PR19262_R02_MANIFEST_JOURNAL_FORWARD,
	PR19262_R02_MANIFEST_JOURNAL_PENDING
} pr19262_r02_manifest_kind_t;
typedef struct pr19262_r02_manifest_entry {
	pr19262_r02_manifest_kind_t kind;
	uint64_t physical;
	uint64_t pages;
	uint64_t generation;
	zio_cksum_t hash;
} pr19262_r02_manifest_entry_t;
typedef int (*pr19262_r02_manifest_visit_t)(void *,
    const pr19262_r02_manifest_entry_t *);
typedef struct pr19262_r02_manifest {
	uint64_t roots;
	uint64_t catalog_pages;
	uint64_t tree_pages;
	uint64_t control_pages;
	uint64_t directory_pages;
	uint64_t active_arenas;
	uint64_t pending_arenas;
	uint64_t journal_heads;
	uint64_t journal_binds;
	uint64_t journal_forwards;
	uint64_t journal_pending;
} pr19262_r02_manifest_t;
int pr19262_engine_r02_manifest(spa_t *, pr19262_r02_manifest_t *);
int pr19262_engine_r02_manifest_walk(spa_t *, pr19262_r02_manifest_visit_t,
    void *, pr19262_r02_manifest_t *);
/*
 * R02 exact-new-leaf reseed (D70).  zfs_pr19262_r02_reseed = 1 (default)
 * admits one strict exact-new-leaf replacement; 0 retains the D64 refusal of
 * every new persistent-member replacement.
 */
extern int zfs_pr19262_r02_reseed;
boolean_t pr19262_engine_r02_topology_enter(spa_t *);
void pr19262_engine_r02_topology_exit(spa_t *);
int pr19262_engine_r02_attach_admit(spa_t *, vdev_t *, boolean_t);
int pr19262_engine_r02_attach_seed(spa_t *, vdev_t *, vdev_t *);
void pr19262_engine_r02_attach_commit(spa_t *, vdev_t *);
int pr19262_engine_r02_detach_check(spa_t *, vdev_t *, boolean_t);
void pr19262_engine_r02_detach_commit(spa_t *);
void pr19262_engine_r02_readmit(spa_t *, vdev_t *);
void pr19262_engine_member_readmit(spa_t *, vdev_t *);
void pr19262_engine_r02_test_cut(uint_t);
#ifndef _KERNEL
extern int zfs_pr19262_r02_reseed_cut;
extern uint_t zfs_pr19262_r02_new_endpoint_faults;
int pr19262_engine_r02_pair_audit(spa_t *, uint64_t *, uint64_t *,
    uint64_t *, uint64_t *);
int pr19262_engine_r02_member_audit(spa_t *, uint64_t *, uint64_t *);
int pr19262_engine_r02_raw_extents(spa_t *);
#endif
#ifndef _KERNEL
/*
 * ztest-only observer witnesses.  They take the same read gates as the
 * collector and only report whether the current topology is direct; the
 * reject probe exercises the shared no-I/O eligibility gates with synthetic
 * replacing/legacy-codec inputs.  Neither API changes the SPA or engine.
 */
typedef struct pr19262_r02_manifest_state {
	uint64_t codec;
	uint64_t io_errors;
	uint64_t revision;
	uint64_t stored_revision;
	uint64_t durable_ub_txg;
	uint64_t durable_catalog_txg;
	uint64_t arena;
	uint64_t arena_bytes;
	uint64_t journal_root;
	uint64_t journal_root_epoch[2];
	uint64_t journal_arena;
	uint64_t journal_epoch[2];
	uint64_t journal_bytes;
	uint64_t journal_seq;
	uint64_t journal_serial;
	zio_cksum_t journal_hash;
	boolean_t persistent;
	boolean_t direct;
	boolean_t replacing;
} pr19262_r02_manifest_state_t;
int pr19262_engine_r02_manifest_test_state(spa_t *,
    pr19262_r02_manifest_state_t *);
int pr19262_engine_r02_manifest_test_reject(spa_t *, boolean_t, uint64_t);
#endif
void pr19262_engine_store_test_hook(spa_t *, void (*)(spa_t *, int));
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
int pr19262_engine_metadata_flush_config_writer(vdev_t *);
void pr19262_engine_metadata_flush_nowait(zio_t *, vdev_t *);
void pr19262_engine_store_crash(spa_t *, uint64_t);
int pr19262_engine_journal_bind(spa_t *, const blkptr_t *);
void pr19262_engine_journal_gate(zio_t *, spa_t *, const blkptr_t *);
void pr19262_zio_resume(zio_t *);
void pr19262_engine_journal_test_hook(spa_t *, void (*)(spa_t *,
    const blkptr_t *,
    int));
int pr19262_zio_walk_gang(spa_t *, const blkptr_t *, void *,
    int (*)(spa_t *, const blkptr_t *, void *), void *);
int pr19262_engine_journal_claim(spa_t *, uint64_t);
boolean_t pr19262_engine_journal_claimed(spa_t *, vdev_t *, uint64_t,
	uint64_t, uint64_t, uint64_t);
boolean_t pr19262_engine_journal_reference(spa_t *, vdev_t *, uint64_t,
	uint64_t, uint64_t, uint64_t);
void pr19262_engine_journal_ub_committed(spa_t *, const uberblock_t *);
void pr19262_engine_journal_fault(spa_t *, uint_t);
int pr19262_engine_journal_roll(spa_t *);
void pr19262_engine_journal_roll_fault(spa_t *, uint_t);
void pr19262_engine_journal_group_fault(spa_t *, uint_t);
int pr19262_engine_journal_wait_error(spa_t *);
void pr19262_engine_spa_init(spa_t *);
void pr19262_engine_spa_fini(spa_t *);
void pr19262_engine_init(void);
void pr19262_engine_fini(void);
void pr19262_engine_start(spa_t *);
int pr19262_engine_start_persistent(spa_t *);
int pr19262_engine_legacy_recover(spa_t *);
boolean_t pr19262_engine_active(spa_t *);
vdev_t *pr19262_engine_vdev(spa_t *);
boolean_t pr19262_engine_size_fixed(vdev_t *);
void pr19262_engine_size_refused(vdev_t *, uint64_t, const char *);
boolean_t pr19262_engine_bp_foreign(spa_t *, const blkptr_t *);
uint64_t pr19262_engine_limit(spa_t *, boolean_t);
void pr19262_engine_block_new(spa_t *, boolean_t);
void pr19262_engine_fail_writes(spa_t *, boolean_t);
void pr19262_engine_retired_limit(spa_t *, uint64_t);
void pr19262_engine_alloc_fail_after(spa_t *, uint64_t);
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
/* Syncing context: the catalog rows an async-destroy free retires fit. */
boolean_t pr19262_engine_free_bulk(spa_t *, const blkptr_t *, boolean_t);
/* The last synced txg's async destroy waited for catalog room. */
boolean_t pr19262_engine_free_stalled(spa_t *);
void *pr19262_engine_hold(spa_t *, const blkptr_t *, void *, boolean_t,
    boolean_t);
void pr19262_engine_rele(void *);
int pr19262_engine_dispatch(zio_t *);
int pr19262_engine_defer_logical(zio_t *);
void pr19262_engine_repair_skipped(zio_t *);
boolean_t pr19262_engine_scan_backing(spa_t *, const blkptr_t *,
    int, dva_t *, uint64_t *);
void pr19262_engine_io_rele(zio_t *);
int pr19262_engine_copy_begin(spa_t *, const blkptr_t *, int, uint64_t,
    void **);
int pr19262_engine_copy_run(void *, boolean_t);
void pr19262_engine_copy_group(void **, uint_t, const boolean_t *, int *);
int pr19262_engine_rebind_unwritten(spa_t *, const blkptr_t *, int, uint64_t);
zio_t *pr19262_engine_physical_io(spa_t *, blkptr_t *, vdev_t *,
    uint64_t, abd_t *, uint64_t, zio_type_t, uint64_t, void *, zio_flag_t);
void pr19262_engine_park_next(spa_t *);
uint64_t pr19262_engine_waiting(spa_t *, boolean_t);
void pr19262_engine_unpark(spa_t *);
#ifdef _KERNEL
/* Private R-01 test control: no synthetic I/O lives in this API. */
#define	PR19262_R01_SELECTED	0
#define	PR19262_R01_COPYING	1
#define	PR19262_R01_PUBLISHED	2
#define	PR19262_R01_FINAL_FREE	3
#define	PR19262_R01_BOUNDARIES	4
#define	PR19262_R01_TEST_NONE	0
#define	PR19262_R01_TEST_EPOCH_STALE	1
#define	PR19262_R01_TEST_CONVERT_EIO	2
int pr19262_engine_r01_arm(spa_t *, uint_t);
int pr19262_engine_r01_release(spa_t *);
int pr19262_engine_r01_cancel(spa_t *);
boolean_t pr19262_engine_r01_scan_restart(spa_t *);
void pr19262_engine_r01_scan_reset_cancel(spa_t *);
boolean_t pr19262_engine_r01_scan_reset_busy(spa_t *);
int pr19262_engine_r01_test_arm(spa_t *, uint_t);
int pr19262_engine_r01_test_fire(spa_t *);
int pr19262_engine_r01_prebind(zio_t *);
void pr19262_engine_r01_prebind_publish(zio_t *);
int pr19262_engine_r01_scan_cancel_begin(spa_t *);
void pr19262_engine_r01_scan_cancel_finish(spa_t *, int);
int pr19262_engine_r01_advance(spa_t *);
int pr19262_engine_r01_prepare(spa_t *, uint64_t);
int pr19262_engine_r01_copy(spa_t *);
int pr19262_engine_r01_epoch_wait(spa_t *, uint64_t);
int pr19262_engine_r01_epoch_check(spa_t *, uint64_t, uint64_t);
int pr19262_engine_r01_convert_test_wait(spa_t *, uint64_t);
void pr19262_engine_r01_epoch_abort(spa_t *);
void pr19262_engine_r01_epoch_finish(spa_t *);
boolean_t pr19262_engine_r01_terminal(spa_t *);
zio_t *pr19262_engine_r01_scan_parent(spa_t *, const blkptr_t *, zio_flag_t,
    void *);
void pr19262_engine_r01_scan_submit_done(spa_t *, zio_t *);
boolean_t pr19262_engine_r01_scan_pending(spa_t *);
int pr19262_engine_r01_final_free_wait(spa_t *);
int pr19262_engine_r01_final_free_second_fence(spa_t *, uint64_t *,
    uint64_t *, uint64_t *, uint64_t *, boolean_t *);
void pr19262_engine_r01_final_free_finish(spa_t *, int, uint64_t);
boolean_t pr19262_engine_r01_watch(zio_t *, uint64_t *, uint64_t *);
void pr19262_engine_r01_repair_created(zio_t *, vdev_t *, uint64_t,
    uint64_t, uint64_t, zio_flag_t);
void pr19262_engine_r01_repair_decision(zio_t *, vdev_t *, boolean_t,
    boolean_t, boolean_t, boolean_t);
int pr19262_engine_r01_status(spa_t *, char *, size_t);
/* Private R-02 observer: records a real scan mapping and its decision. */
int pr19262_engine_r02_arm(spa_t *);
void pr19262_engine_r02_backing(spa_t *, const blkptr_t *, int,
    const dva_t *, uint64_t, boolean_t);
void pr19262_engine_r02_worker(spa_t *, const blkptr_t *, boolean_t);
void pr19262_engine_r02_decision(spa_t *, const blkptr_t *, uint64_t,
    uint64_t, uint64_t, boolean_t, boolean_t);
int pr19262_engine_r02_status(spa_t *, char *, size_t);
#endif
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
#ifndef _KERNEL
/* ztest only: prefer a verified free P extent for one real DMU allocation. */
int pr19262_engine_test_prefer_p(spa_t *, uint64_t, uint64_t);
/*
 * ztest only, growth-defer test: mark native extents (before activation);
 * later report the highest arena run of marked space that is no longer
 * legacy and where it is now; steer the next catalog growth onto a run
 * (force: even if it is not free).
 */
typedef struct pr19262_reuse_probe {
	uint64_t p;
	uint64_t bytes;
	uint64_t left;
	uint64_t in_free;
	uint64_t any_free;
	uint64_t in_legacy;
	uint64_t in_store;
	uint64_t in_journal;
	uint64_t owned;
	uint64_t elsewhere;
	uint64_t durable_ub;
	uint64_t durable_catalog;
	uint64_t stored_catalog;
	uint64_t free_bytes;
	uint64_t capacity;
	/* Last steer: 0 none, 1 honoured, 2 forced, 3 refused (not free). */
	uint64_t steer;
} pr19262_reuse_probe_t;
int pr19262_engine_test_mark(spa_t *, uint64_t, uint64_t);
int pr19262_engine_test_reuse_probe(spa_t *, pr19262_reuse_probe_t *);
int pr19262_engine_test_prefer_arena(spa_t *, uint64_t, boolean_t);
/* Run fn(spa, arg) once, inside the next engine start, before it scans. */
void pr19262_engine_test_start_hook(spa_t *, void (*)(spa_t *, void *),
    void *);
/*
 * Reuse deferral probes.  defer_free moves bytes of free into the deferral
 * and returns what is deferred (0 bytes: only ask; ENOTSUP without the
 * deferral).  free_holes carves a hole into every 8 MiB of the free tree, so
 * that it holds no arena run, or puts the holes back.
 */
int pr19262_engine_test_defer_free(spa_t *, uint64_t, uint64_t *);
int pr19262_engine_test_free_holes(spa_t *, boolean_t);
/* ztest only, floor-flake negative control: a wrong drain_releasable. */
uint64_t pr19262_engine_test_flag_covered(spa_t *, uint64_t, uint64_t,
    uint64_t *);
/*
 * ztest only, journal roll test (L10).  jroll_probe reports the journal and
 * its roll state (retry: the tree has the retryable roll; runs: disjoint
 * segment-sized, stripe-aligned runs in free).  jroll_squeeze leaves a roll
 * no run: the standby run returns to free, one segment-sized run of free
 * (*run) goes into the reuse deferral (op 1) or is held back (op 2, later
 * deferred by op 3), and every 2 MiB of the rest of free gets a hole; op 0
 * puts the holes (and a held run) back.
 */
typedef struct pr19262_jroll_probe {
	uint64_t seq;
	uint64_t frames;
	uint64_t segments;
	uint64_t rolls;
	int64_t error;
	uint64_t retry;
	uint64_t pending;
	uint64_t pending_txg;
	uint64_t enospc;
	uint64_t waits;
	uint64_t recovered;
	uint64_t deferred;
	uint64_t standby;
	uint64_t runs;
	uint64_t free_bytes;
	uint64_t synced;
} pr19262_jroll_probe_t;
int pr19262_engine_test_jroll_probe(spa_t *, pr19262_jroll_probe_t *);
int pr19262_engine_test_jroll_squeeze(spa_t *, int, uint64_t *);
/*
 * L14 test only (L11 r2(a) cell): the retired bytes clients are measured
 * against, the part of them owners loaded without drain_releasable hold
 * while a discard drains, and the client refusals by branch.
 */
typedef struct pr19262_retired_probe {
	uint64_t retired;
	uint64_t limit;
	uint64_t unflagged;
	uint64_t unflagged_owners;
	uint64_t draining;
	uint64_t stalled;
	int64_t fence_error;
	uint64_t client_rejected;
	uint64_t arena_rejections;
	uint64_t headroom_rejections;
	uint64_t fence_waits;
	uint64_t fence_requests;
} pr19262_retired_probe_t;
int pr19262_engine_test_retired_probe(spa_t *, pr19262_retired_probe_t *);
/* L14 test only (convert-busy8): a queued writer on bp's owner, or not. */
int pr19262_engine_test_jroll_busy(spa_t *, const blkptr_t *, int);
#endif
void pr19262_engine_check(spa_t *, const char *);
#ifndef _KERNEL
/* Test only (L13 witness driver). */
extern volatile int pr19262_l13_hold_release;
void pr19262_engine_l13_status(spa_t *, char *, size_t);
void pr19262_engine_l13_fence_dispatch(spa_t *);
#endif
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
	/*
	 * Slices of the sweep, the longest any held the open txg, and the
	 * most any ran past its time.
	 */
	uint64_t slices;
	uint64_t hold_ms_max;
	uint64_t slice_over_ms_max;
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
/* The MOS alone, resumable (dmu_traverse.c): the sweep walks in slices. */
int pr19262_traverse_mos_resume(spa_t *, zbookmark_phys_t *, int,
    blkptr_cb_t, void *);
int pr19262_engine_fence(spa_t *, uint64_t *, uint64_t *, uint64_t *,
    uint64_t *);
int pr19262_engine_certify(spa_t *, uint64_t);
void pr19262_engine_fence_schedule(spa_t *);
void pr19262_engine_zil_prepare(spa_t *);
void pr19262_engine_zil_finish(spa_t *);
void pr19262_engine_export_prepare(spa_t *);
void pr19262_engine_export_abort(spa_t *);
void pr19262_engine_unload_prepare(spa_t *);
void pr19262_engine_describe(spa_t *, const blkptr_t *, const char *,
    uint64_t, uint64_t);
void pr19262_engine_quiesce(spa_t *);
#ifndef _KERNEL
/* Deterministic scheduled-fence/export witness, owned by ztest. */
typedef struct pr19262_export_fence_test {
	kmutex_t lock;
	kcondvar_t changed;
	boolean_t paused;
	boolean_t release;
	boolean_t quiescing;
	boolean_t task_done;
	boolean_t exported;
	int fence_error;
	int export_error;
	/* L15 test: hold a scheduled fence after its catalog (verdict). */
	boolean_t hold_end;
	boolean_t paused_end;
	boolean_t release_end;
	uint64_t reclaimed;
	uint64_t released;
} pr19262_export_fence_test_t;
void pr19262_engine_export_fence_arm(spa_t *,
    pr19262_export_fence_test_t *);
void pr19262_engine_fence_idle(spa_t *);
/* L21 test: the checkpoint-pinned retired count and its counters. */
void pr19262_engine_retired_pinned_report(spa_t *, uint64_t *, uint64_t *,
    uint64_t *, uint64_t *, uint64_t *);
#endif
void pr19262_engine_hold_report(spa_t *, void *);
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
	/* Fences the admission scheduled, completed; clients restarted. */
	uint64_t fence_requests;
	uint64_t fence_auto;
	uint64_t fence_waits;
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
uint64_t pr19262_engine_admit_txg_pages(spa_t *, uint64_t, uint64_t,
    uint64_t, uint64_t *);
void pr19262_engine_plan(spa_t *, uint64_t, uint64_t);
void pr19262_engine_admit_report(spa_t *, const char *,
    pr19262_engine_admit_report_t *);
uint64_t pr19262_engine_hidden_dspace(spa_t *);
/* L18: the room a conversion has on top of the native slop. */
typedef struct pr19262_engine_room {
	int64_t room;		/* avail - delta, deflated bytes */
	int64_t avail;		/* native AVAIL unclamped, deflated */
	int64_t delta;		/* hidden increase: plan, legacy on top */
	uint64_t space;		/* dspace - checkpoint - deferred frees */
	uint64_t used;
	uint64_t slop;
	uint64_t growth;	/* planned weak growth (raw) */
	uint64_t legacy;	/* legacy worst case (raw) */
	uint64_t transient;	/* sync + log + legacy + batch + growth term */
	uint64_t free;		/* engine free (raw) */
	uint64_t store;		/* catalog extents (raw) */
	uint64_t journal;	/* journal extents (raw) */
	uint64_t protected;	/* client-protected amount (raw) */
	uint64_t growth_term;	/* its arena/segment growth forecast */
	uint64_t retired;	/* retired backing, back at a fence (raw) */
	uint64_t ratio;		/* the engine vdev's deflate ratio */
	uint64_t runs;		/* one catalog arena + one journal segment */
	uint64_t weak;
	uint64_t parity;
	uint64_t planned;	/* the plan in place is this parity and batch */
} pr19262_engine_room_t;
void pr19262_engine_convert_room(spa_t *, uint64_t, uint64_t,
    pr19262_engine_room_t *);
void pr19262_engine_admit_priority(spa_t *, boolean_t);
boolean_t pr19262_engine_admit_conv_alone(spa_t *, uint64_t);
void pr19262_engine_admit_converting(spa_t *, boolean_t);
/* Per-member count of valid live raw pages (arrays of vdev_children). */
int pr19262_engine_raw_audit(spa_t *, const char *, uint64_t *, uint64_t *);
#endif
