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
/*
 * Private normal writable-SPA V/P ownership engine, included by metaslab.c.
 * All new allocations, including MOS/ditto, are bound automatically. Native
 * V metadata remains allocated on final free; no supplied floor permits reuse.
 * The default backend is RAM-only. The optional private store pairs catalog
 * checkpoints with native uberblocks; it is not a supported disk feature.
 */
#include <sys/vdev_reloc_engine.h>
#include <sys/vdev_raidz.h>
#include <sys/zio_checksum.h>
#include <sys/dmu_tx.h>

typedef struct pr19262_engine engine_t;
typedef struct engine_owner engine_owner_t;

typedef struct engine_version {
	struct engine_version *next;
	engine_owner_t *owner;
	uint64_t p;
	uint64_t size;
	uint64_t birth;
	uint16_t width;
	uint16_t parity;
	uint64_t leases;
	uint64_t writers;
	uint64_t generation;
	uint64_t journal_seq;
	/* First durable catalog txg that listed this version as retired. */
	uint64_t retired_txg;
	boolean_t ready;
	boolean_t retired;
} engine_version_t;

_Static_assert(sizeof (engine_version_t) <= 96,
	"engine version must remain in the 96-byte kmem class");

typedef struct engine_pending {
	struct engine_pending *next;
	zio_t *zio;
	boolean_t start;
} engine_pending_t;

#ifdef _KERNEL
typedef enum engine_r01_state {
	ENGINE_R01_IDLE,
	ENGINE_R01_ARMED,
	ENGINE_R01_PREBINDING,
	ENGINE_R01_PARKED,
	ENGINE_R01_RELEASED,
	ENGINE_R01_DRAINED,
	ENGINE_R01_CANCELLED
} engine_r01_state_t;

/*
 * One private observer per SPA.  Pointers exist only while the selected zio
 * still owns its inherited hold and selected version lease; status exports
 * scalar identity only.
 */
typedef struct engine_r01 {
	engine_r01_state_t state;
	/* A private scan cancellation owns no R01 ZIO or engine lock. */
	boolean_t scan_cancel_inflight;
	uint64_t scan_cancel_gen;
	int scan_cancel_error;
	uint64_t generation;
	/* Detached natural scan root; release/cancel owns its final wait. */
	zio_t *scan_root;
	uint64_t scan_root_waits;
	int scan_root_error;
	/* Scan restarts that left a released R-01 alone, or were postponed. */
	uint64_t restart_kept;
	uint64_t restart_deferred;
	/* Scan cancels refused (EBUSY) because the reset would have waited. */
	uint64_t stop_busy;
	boolean_t scan_routed;
	boolean_t scan_submitting;
	boolean_t scan_waiting;
	boolean_t scan_done;
	/* Preallocated while the selected ZIO is non-releasably prebinding. */
	engine_pending_t *prebind_pending;
	boolean_t prebind_handoff;
	/*
	 * Pin scan-parent P until the bound natural read takes this lease.
	 */
	boolean_t bound_lease;
	/* The logical scan ZIO is parked before it enters SCL_ZIO. */
	zio_t *prebound;
	zio_t *selected;
	engine_owner_t *owner;
	engine_version_t *version;
	uint64_t v;
	uint64_t incarnation;
	uint64_t b;
	uint64_t p;
	uint64_t t;
	uint64_t p_offset;
	uint64_t p_size;
	uint64_t width;
	uint64_t parity;
	uint64_t lease_parked;
	uint64_t holds_parked;
	uint64_t writers_parked;
	uint64_t lease_terminal;
	uint64_t holds_terminal;
	uint64_t writers_terminal;
	uint64_t leaf_guid;
	uint64_t leaf_id;
	uint64_t leaf_offset;
	uint64_t leaf_size;
	uint64_t column;
	uint64_t second_leaf_guid;
	uint64_t second_leaf_id;
	uint64_t second_leaf_offset;
	uint64_t second_leaf_size;
	uint64_t second_column;
	uint64_t repair_created;
	uint64_t repair_forwarded;
	uint64_t repair_bypassed;
	uint64_t second_repair_forwarded;
	uint64_t second_repair_bypassed;
	/* First natural repair whose own leaf has a DTL tuple for B/P/T. */
	uint64_t dtl_target_guid;
	uint64_t dtl_target_id;
	uint64_t dtl_target_offset;
	uint64_t dtl_target_size;
	uint64_t dtl_target_column;
	uint64_t dtl_target_forwarded;
	uint64_t dtl_target_bypassed;
	uint64_t seen_top;
	uint64_t seen_no_owner;
	uint64_t seen_mapped;
	uint64_t seen_level0;
	uint64_t seen_data;
	uint64_t seen_b;
	uint64_t seen_p;
	uint64_t copy_boundary_calls;
	uint64_t copy_boundary_v;
	uint64_t copy_boundary_incarnation;
	blkptr_t copy_bp;
	uint_t boundary;
	uint_t boundary_reached;
	uint_t repair_boundary;
	uint_t copy_boundary_state;
	/* Ephemeral one-shot R-01 error hook; it never reaches the store. */
	uint_t test_mode;
	boolean_t test_armed;
	boolean_t test_ready;
	boolean_t test_fire;
	boolean_t test_used;
	uint64_t test_birth;
	int test_error;
	int copy_dva;
	int copy_error;
	uint64_t copy_birth;
	uint64_t copy_parity;
	boolean_t self_heal;
	boolean_t scrub;
	boolean_t resilver;
	boolean_t dtl_p;
	boolean_t dtl_b;
	boolean_t dtl_t;
	boolean_t second_self_heal;
	boolean_t second_scrub;
	boolean_t second_resilver;
	boolean_t second_dtl_p;
	boolean_t second_dtl_b;
	boolean_t second_dtl_t;
	boolean_t dtl_target_seen;
	boolean_t dtl_target_self_heal;
	boolean_t dtl_target_scrub;
	boolean_t dtl_target_resilver;
	boolean_t dtl_target_p;
	boolean_t dtl_target_b;
	boolean_t dtl_target_t;
	boolean_t tuple_distinct;
	boolean_t boundary_ready;
	boolean_t boundary_waiting;
	boolean_t boundary_advance;
	/* Test-only P lease retained across native root and public epoch. */
	boolean_t epoch_waiting;
	boolean_t observer_lease;
	boolean_t repair_boundary_valid;
	boolean_t copy_requested;
	boolean_t copy_prepared;
	boolean_t copy_running;
	boolean_t copy_done;
	boolean_t copy_owner_match;
	boolean_t parent_done;
	boolean_t final_fence_done;
	boolean_t final_fence_running;
	/*
	 * The thread executing the one R-01 final fence.  Only that fence may
	 * unlink the selected version, and only it records final_p_*; every
	 * other fence (converter passes, admission or growth-triggered) skips
	 * the version and owner R-01 still references.
	 */
	kthread_t *final_fence_thread;
	boolean_t drained;
	boolean_t prebind;
	boolean_t prebind_no_scl;
	uint64_t prebind_attempts;
	uint64_t prebind_bad_shape;
	uint64_t prebind_scl;
	uint64_t prebind_unheld;
	uint64_t prebind_nomatch;
	int parent_error;
	int final_fence_error;
	uint64_t final_reclaimed;
	uint64_t final_p_reclaims;
	uint64_t final_p_bytes;
	uint64_t final_p_birth;
	uint64_t final_p_offset;
	uint64_t final_p_size;
	uint64_t final_p_v;
	uint64_t final_p_incarnation;
} engine_r01_t;

/*
 * The R-02 observer only records the existing scan path.  It neither queues
 * I/O nor changes the scan's DTL verdict, so it may be armed before a normal
 * zpool online resilver without creating a synthetic scan.
 */
typedef struct engine_r02 {
	uint64_t generation;
	uint64_t backing_calls;
	uint64_t indirect_seen;
	uint64_t snapshot_worker;
	uint64_t decisions;
	uint64_t queued;
	uint64_t b;
	uint64_t p;
	uint64_t original_vdev;
	uint64_t original_offset;
	uint64_t original_size;
	uint64_t backing_vdev;
	uint64_t backing_offset;
	uint64_t backing_size;
	uint64_t min;
	uint64_t max;
	boolean_t armed;
	boolean_t mapped;
	boolean_t p_in_window;
	boolean_t resilver;
	boolean_t needs_io;
} engine_r02_t;
#endif

typedef struct engine_owner {
	avl_node_t avl;
	uint64_t v;
	/* Immutable V size; each backing owns its independent P size. */
	uint64_t size;
	uint64_t allocation_txg;
	uint64_t catalog_txg;
	uint64_t claimed_txg;
	uint64_t queued_writers;
	engine_version_t *versions;
	engine_version_t *cur;
	engine_pending_t *pending;
	uint64_t incarnation;
	uint64_t holds;
	uint64_t created;
	uint64_t retired_at;
	/* First durable catalog txg that listed the owner as retired. */
	uint64_t retired_txg;
	/* On engine.dirty while its catalog rows may differ from the tree. */
	list_node_t dirty_node;
	uint64_t dirty_seq;
	uint64_t admit_growth;
	uint8_t journal_pending;
	/* Import-only native reservation, not yet referenced by ZIL. */
	uint8_t journal_reserved;
	uint8_t journal_touched;
	uint8_t checkpoint_only;
	/*
	 * No checkpoint space map entry names this V, so a fence may release
	 * it while a discard still drains (L11): retired by an ordinary free
	 * (a live V is never checkpointed), or by a discard step whose
	 * processed prefix holds the whole V.  RAM only: after a load, the
	 * first release of a draining discard sets it again for every
	 * retired owner the remaining checkpoint space map does not name.
	 */
	uint8_t drain_releasable;
	uint8_t copying;
	uint8_t retired;
	uint8_t aborted;
	uint8_t releasing;
	uint8_t admit_weak;
	uint8_t admit_dirty;
} engine_owner_t;

typedef struct engine_hold {
	spa_t *spa;
	uint64_t refs;
	uint_t count;
	engine_owner_t *owner[SPA_DVAS_PER_BP];
	int error;
	/* Diagnostic: 1 inherited, 2 no owner (ENOENT), 3 stale verdict. */
	uint8_t error_site;
	uint64_t started;
	boolean_t gang;
	boolean_t write;
	boolean_t r01_selected;
	/* DVAs on other top-level vdevs: native, never translated. */
	uint_t foreign;
} engine_hold_t;

/*
 * A released owner identity: its journal bindings are folded.  It is
 * needed only while a journal segment up to the recorded index exists.
 */
typedef struct engine_tomb {
	avl_node_t avl;
	uint64_t v;
	uint64_t incarnation;
	uint64_t segment;
	boolean_t expired;
} engine_tomb_t;

typedef struct engine_copy {
	spa_t *spa;
	engine_owner_t *owner;
	engine_version_t *source;
	engine_version_t *stage;
	blkptr_t original;
	uint64_t psize;
} engine_copy_t;

/* Group wall time, partitioned at the real I/O and publication steps. */

typedef struct engine_j_segment {
	struct engine_j_segment *next;
	uint64_t arena;
	uint64_t epoch[2];
	uint64_t seq;
	zio_cksum_t hash;
	uint64_t serial;
	uint64_t phase;
	uint64_t target;
	uint64_t target_epoch[2];
	/* Chain position, catalog txg folding every record, free gate. */
	uint64_t index;
	uint64_t fold_txg;
	uint64_t free_after;
} engine_j_segment_t;

static int engine_journal_bind_copy(engine_copy_t *);
static int engine_journal_roll_wait(spa_t *);
static void engine_journal_bind_copies(engine_copy_t **, uint_t,
    const boolean_t *, int *);
static void engine_journal_test_event(spa_t *, const blkptr_t *, int);
static void engine_journal_fold(spa_t *, uint64_t);
static void engine_admit_owner_update(engine_t *, engine_owner_t *);
static void engine_admit_owner_forget(engine_t *, engine_owner_t *);
static uint_t engine_store_tolerated(engine_t *);
/* Default converter batch the standing reserve keeps free. */
#define	ENGINE_ADMIT_BATCH (8ULL << 20)
/* Reuse deferral batches pending: of the last TXG_DEFER_SIZE txgs and this. */
#define	ENGINE_REUSE_BATCHES (TXG_DEFER_SIZE + 1)
/* A roll that finds no run is retried, not sticky (engine_journal_grow). */
#define	ENGINE_JOURNAL_ROLL_RETRY 1
/* L23 member marks: as many slots as engine_pages_t.member_serial[]. */
#define	ENGINE_MEMBER_BITS 256
#define	ENGINE_MEMBER_WORDS (ENGINE_MEMBER_BITS / 64)

/*
 * Replicated raw metadata: a write or a required read succeeds when at
 * most the certified parity of members are absent or failing.  A store that
 * started with every member present also has a runtime-only base-layout
 * tolerance until its stronger guarantee is certified; that avoids turning
 * one later member loss into a pool sync failure.  It neither publishes nor
 * substitutes for the durable certificate.
 */
typedef struct engine_quorum {
	uint_t failures;
	uint_t absent;
	int error;
} engine_quorum_t;

/*
 * A raw I/O error counted in io_errors drops the control-slot cache
 * (engine_pages_slots_cached()), as do a failed control write, a config
 * generation change and a member resilver.  Not every error is counted.
 * A replacing pair's write or flush result is counted only when no
 * eligible endpoint held the slot, so a failed or skipped endpoint beside
 * one that held only becomes ineligible (a FAULTED new leaf, a stale old
 * leaf); a pair read served by its second endpoint and the R02 observer
 * and seed leaf I/O are not counted either.  The cache stays safe because
 * an ineligible leaf is seeded, or audited and reseeded on any audit
 * failure, and hash-verified before it is eligible again, not because the
 * cache is invalidated (see pr19262_engine_r02_readmit()).
 */

/* One page of a raw step that every member of the vdev writes. */
typedef struct engine_member_page {
	uint64_t arena;
	uint64_t page;
	abd_t *abd;
} engine_member_page_t;

static void engine_members_flush(spa_t *, engine_quorum_t *);

typedef struct engine_copy_cost {
	uint64_t groups;
	uint64_t blocks;
	uint64_t bytes;
	uint64_t setup;
	uint64_t io[3];
	uint64_t check;
	uint64_t flush;
	uint64_t settle;
	uint64_t journal;
	uint64_t publish;
	uint64_t total;
} engine_copy_cost_t;

typedef struct engine_admit_cost {
	uint64_t rebuilds;
	uint64_t rebuilt;
	uint64_t walked;
	uint64_t updates;
	uint64_t checked;
	uint64_t pages_checked;
} engine_admit_cost_t;

/*
 * L18: the last converter request admission refused, and whether it fit
 * without the clients' outstanding charges (then clients starved it).
 */
typedef struct engine_conv_refusal {
	uint64_t reason;	/* 1 arena runs, 2 space */
	uint64_t alone;
	uint64_t free;
	uint64_t reserved;
	uint64_t need;
	uint64_t growth;
	uint64_t arenas;
	uint64_t slots;
	uint64_t client_pages;
	uint64_t alone_growth;
	uint64_t alone_arenas;
	uint64_t alone_slots;
	uint64_t deferred;
	/* The request, and whether it fit alone after the pass's fence. */
	uint64_t p_bytes;
	uint64_t m_pages;
	uint64_t j_frames;
	uint64_t alone_after;
} engine_conv_refusal_t;

typedef struct engine_journal_cost {
	uint64_t calls[3];
	uint64_t pages[3];
	uint64_t write[3];
	uint64_t flush[3];
	uint64_t seal;
	uint64_t rolls;
	uint64_t roll;
	uint64_t lock;
} engine_journal_cost_t;

typedef struct engine_slot_cost {
	uint64_t reads;
	uint64_t writes;
	uint64_t skipped;
	uint64_t cached;
	uint64_t checked;
} engine_slot_cost_t;

typedef struct engine_store_cost {
	uint64_t stores;
	uint64_t pages;
	uint64_t runs;
	hrtime_t tree;
	hrtime_t reserve;
	hrtime_t write;
	hrtime_t flush;
} engine_store_cost_t;

struct pr19262_engine {
	kmutex_t lock;
	kmutex_t journal_lock;
	kmutex_t store_lock;
	/* L20 group commit: queued hold binds and their leader, under lock. */
	list_t journal_reqs;
	boolean_t journal_leader;
	kcondvar_t journal_group_cv;
	/* Reader before SCL_STATE; R02 attach/detach takes writer. */
	krwlock_t r02_topology_lock;
	/*
	 * R02 exact-new-leaf pair (D70 candidate).  The guids and slot change
	 * only under the topology writer plus SCL_ALL writer, or during the
	 * single-threaded writable import recovery; raw I/O reads them under
	 * an SCL reader.  The state moves ADMITTED -> FAULTED atomically from
	 * the I/O path.
	 */
	uint64_t r02_pair_state;
	uint64_t r02_pair_slot;
	uint64_t r02_pair_old_guid;
	uint64_t r02_pair_new_guid;
	uint64_t r02_pair_faults;
	uint64_t r02_pair_seeds;
	uint64_t r02_pair_audits;
	uint64_t r02_pair_copied;
	/* Test cut D4: a detach collapsed the pair, not yet synced. */
	uint64_t r02_collapsed;
	/* The superseded old leaf missed a raw write while paired. */
	uint64_t r02_old_stale;
	/* Pair-slot raw publications held by an eligible endpoint. */
	uint64_t r02_pair_writes;
	uint64_t r02_pair_old_held;
	/* Exact pair reads: synchronous, and asynchronous (control slots). */
	uint64_t r02_pair_reads;
	uint64_t r02_pair_async_reads;
	/* Attach seed: the old leaf passed its own exact audit. */
	uint64_t r02_attach_old_current;
	/* v3.2 readmit: the leaf states of the last failed attempt. */
	uint64_t r02_readmit_fail_sig;
	hrtime_t r02_readmit_fail_time;
	/* Retried (busy, unpublished) skips since the last reset. */
	uint64_t r02_readmit_retries;
	/*
	 * L23: direct members (bit = slot) that failed a raw write or flush
	 * since their last clean closure pass, or were absent at a writable
	 * import; the member readmit's retried skips.  RAM only.
	 */
	uint64_t member_missed[ENGINE_MEMBER_WORDS];
	uint64_t member_readmit_retries;
	/* The member readmit's last failed pass: signature and time. */
	uint64_t member_readmit_fail_sig;
	hrtime_t member_readmit_fail_time;
	kcondvar_t changed;
	spa_t *spa;
	vdev_t *vd;
	/* Space maps loaded before publication may not be evicted yet. */
	vdev_t *loading_vd;
	uint64_t vd_guid;
	/* Another normal-class top-level vdev: client data can fall back. */
	boolean_t foreign_normal;
	uint64_t alloc_guarded;
	avl_tree_t owners;
	avl_tree_t tombstones;
	/*
	 * What the next catalog store has to look at: the owners whose rows
	 * may differ from the stored tree, and the (V, incarnation) of owners
	 * gone since; store_all when anything may differ (engage, import).
	 */
	list_t dirty;
	uint64_t dirty_next;
	uint64_t *gone;
	uint_t gone_count;
	uint_t gone_capacity;
	boolean_t store_all;
	zfs_range_tree_t *free;
	zfs_range_tree_t *legacy;
	zfs_range_tree_t *unusable;
	/*
	 * Reuse deferral (see engine_reuse_add): space that left a catalog
	 * structure after the durable catalog that still lists it.  Frees
	 * since the last store snapshot, and per store txg those that
	 * snapshot recorded but whose release is not due yet, like the
	 * metaslab's ms_defer.  None of it is in free.
	 */
	zfs_range_tree_t *reuse_open;
	zfs_range_tree_t *reuse_batch[ENGINE_REUSE_BATCHES];
	uint64_t reuse_batch_txg[ENGINE_REUSE_BATCHES];
	uint64_t reuse_deferred;
	uint64_t reuse_released;
	uint64_t total;
	uint64_t next_incarnation;
	uint64_t pairs;
	uint64_t alloc_fail_after;
	uint64_t retired;
	uint64_t aborts;
	uint64_t translated;
	uint64_t metadata;
	uint64_t legacy_reads;
	boolean_t block_new;
	uint64_t sequence;
	boolean_t fail_writes;
	uint64_t retired_limit;
	uint64_t copied;
	uint64_t copy_aborts;
	uint64_t deferred;
	/* Writes queued before their vdev children, without SCL_ZIO. */
	uint64_t deferred_logical;
	/* Vdev write children deferred by dispatch: expected to stay 0. */
	uint64_t deferred_vdev;
	/* Repair writes to a copying owner, bypassed by dispatch. */
	uint64_t repair_skipped;
	uint_t repair_skip_prints;
	uint_t deferred_vdev_prints;
	engine_pending_t *parked;
	boolean_t park_next;
#ifdef _KERNEL
	engine_r01_t r01;
	engine_r02_t r02;
#endif
	boolean_t persistent;
	uint64_t arena;
	uint64_t arena_bytes;
	uint8_t *bitmap;
	uint64_t store_codec;
	zfs_range_tree_t *store_extents;
	uint64_t revision;
	uint64_t stored_revision;
	uint64_t journal_arena;
	uint64_t journal_bytes;
	uint64_t journal_epoch[2];
	uint64_t journal_root;
	uint64_t journal_root_epoch[2];
	zfs_range_tree_t *journal_extents;
	engine_j_segment_t *journal_segments;
	engine_j_segment_t *journal_tail;
	engine_j_segment_t *journal_current;
	engine_j_segment_t *journal_pending;
	/* Segments cut from the chain head, freed once no root reaches them. */
	engine_j_segment_t *journal_retired;
	uint64_t journal_segment_count;
	uint64_t journal_freed_segments;
	uint64_t journal_freed_bytes;
	uint64_t journal_seq;
	uint64_t journal_serial;
	zio_cksum_t journal_hash;
	boolean_t journal_formatted;
	boolean_t journal_ready;
	boolean_t journal_repair;
	int journal_error;
	/*
	 * A roll that found no run (ENOSPC) moved nothing, so it does not set
	 * journal_error: the roll stays pending, and binds retry it once a
	 * txg later than journal_roll_txg has synced (one walk per txg).
	 * Under journal_lock and engine->lock to change, either to read.
	 */
	boolean_t journal_roll_pending;
	uint64_t journal_roll_txg;
	uint64_t journal_roll_enospc;
	uint64_t journal_roll_waits;
	uint64_t journal_roll_recovered;
	uberblock_t checkpoint;
	/*
	 * Checkpoint-exempt floor.  A fence may move the floor past the
	 * checkpoint root C while everything C reaches stays guarded.  After
	 * a discard removed C in txg ckpt_until - 1, the roots before it
	 * still name C, so its guards hold until a floor of at least
	 * ckpt_until (ckpt_ub keeps C meanwhile).  Under engine.lock.
	 */
	uberblock_t ckpt_ub;
	uint64_t ckpt_until;
	/*
	 * Checkpoint discard pacing (L11).  The discard retires owners in
	 * syncing context, which admission never sees: per txg it may dirty
	 * catalog rows worth at most ckpt_discard_budget pages (set at the
	 * txg's first call).  An entry past it is cut, and the rest of its
	 * range goes back to the checkpoint space map; a step that could
	 * retire nothing records what its first piece needs, and the discard
	 * thread waits in open context until that fits.  Under engine.lock.
	 */
	uint64_t ckpt_discard_txg;
	uint64_t ckpt_discard_budget;
	uint64_t ckpt_discard_need;
	uint64_t ckpt_discard_rows;
	uint64_t ckpt_discard_cuts;
	uint64_t ckpt_discard_stalls;
	uint64_t ckpt_discard_waits;
	uint64_t ckpt_discard_fences;
	uint64_t ckpt_discard_prints;
	boolean_t ckpt_discard_warned;
	/*
	 * drain_releasable is RAM only (L11): after a load, the first owner
	 * release of a draining discard re-derives it once from the
	 * remaining checkpoint space map (L14).  Under engine.lock.
	 */
	boolean_t ckpt_drain_flags_loaded;
	uint64_t ckpt_drain_flagged;
	/*
	 * The root this session imported, plus TXG_CONCURRENT_STATES for
	 * open-context allocations in flight (0 after an activation).  Journal
	 * records written before it are not in the RAM flags journal_touched
	 * and journal_seq, but C's chain may replay them: while C is guarded,
	 * nothing allocated or born at or before it is reclaimed.
	 */
	uint64_t ckpt_session_txg;
	/*
	 * The physical catalog pages C's paged root reaches, which page GC
	 * keeps, and the C they were collected for.  Built by the fence
	 * under store_lock, never in syncing context; RAM only.
	 */
	zfs_range_tree_t *ckpt_pins;
	uberblock_t ckpt_pins_ub;
	uint64_t ckpt_pinned_pages;
	uint64_t ckpt_builds;
	uint64_t ckpt_build_errors;
	uint64_t ckpt_unpins;
	/*
	 * L19 (A18 R4): the catalog reserve under a pool checkpoint, R_C
	 * pages (ckres_pages; 0: none), of which ckres_disc is the
	 * discard's own (ckres_disc_left of it still undrawn).  Accounting
	 * only: no page or run is set aside.  Armed when C is recorded,
	 * released when C's pins drop or the engine unloads; RAM only.
	 * ckres_churn_x16: 16 x the EWMA of the catalog pages each store
	 * under C used net (ckres_used_last: the pages in use at the last
	 * one), the churn term for sizing the next reserve in this session.
	 * ckres_wait_gen/_deficit: a client's fence wait behind the
	 * reserve (as admit_arena_wait_gen); ckres_conv_refused: the last
	 * converter admission was the reserve's refusal.  Under engine.lock.
	 */
	uint64_t ckres_pages;
	uint64_t ckres_disc;
	uint64_t ckres_disc_left;
	int64_t ckres_churn_x16;
	uint64_t ckres_used_last;
	boolean_t ckres_used_valid;
	boolean_t ckres_warned;
	boolean_t ckres_conv_refused;
	uint64_t ckres_wait_gen;
	uint64_t ckres_wait_deficit;
	uint64_t ckres_min_available;
	uint64_t ckres_arms;
	uint64_t ckres_releases;
	uint64_t ckres_refusals;
	uint64_t ckres_rejections;
	uint64_t ckres_disc_draws;
	/* L22: client restarts that fit with no client charge outstanding. */
	uint64_t ckres_alone_waits;
	/*
	 * The owner release's draw on R_churn in one txg (ckres_rel_left of
	 * it left in txg ckres_rel_txg), and the pages it drew under this
	 * reserve (with ckres_disc_draws: reset when it is armed).
	 */
	uint64_t ckres_rel_txg;
	uint64_t ckres_rel_left;
	uint64_t ckres_rel_pages;
	/*
	 * Bulk retirement pacing (L13).  The async destroy (bptree, free
	 * bpobj) and a fence's owner release change catalog rows in syncing
	 * context, which admission never sees.  They draw on the discard's
	 * per-txg budget above, one pot per txg for all three: bulk_pot_fresh
	 * is its value at the txg's first draw, bulk_rows the rows the async
	 * destroy charged.  A destroy that does not fit pauses to the next txg
	 * (bulk_waits asks for a fence when it made no progress); a
	 * release that does not fit stops, and release_carry makes the next
	 * fence release the rest.  Under engine.lock.
	 */
	uint64_t bulk_pot_fresh;
	/*
	 * L22: under C's catalog reserve, the pages of this txg's pot the
	 * async destroy leaves (the clients' share, engine_ckres_share).
	 */
	uint64_t bulk_pot_keep;
	uint64_t bulk_rows;
	boolean_t bulk_pathed;
	boolean_t bulk_destroy_charged;
	/* A destroy stall: its txg, and whether it asked for a fence. */
	uint64_t bulk_stall_txg;
	boolean_t bulk_destroy_asked;
	/*
	 * Free arena runs at the last store (the walk stops past
	 * ENGINE_BULK_RUNS_MAX, so the count may exceed it), kept current
	 * across L15b's fills and returns; the L13 budget adds the held runs.
	 */
	uint64_t bulk_runs_cached;
	/*
	 * The long-free budget, computed once per synced txg (and the
	 * admitted pages it left out then); a checkpoint wait's fence ask.
	 */
	uint64_t free_budget_synced;
	uint64_t free_budget_pages;
	uint64_t free_budget_admitted;
	boolean_t free_ckpt_asked;
	boolean_t release_carry;
	uint64_t bulk_waits;
	uint64_t bulk_waits_mark;
	uint64_t bulk_destroy_rows;
	uint64_t bulk_destroy_pauses;
	uint64_t bulk_release_cuts;
	uint64_t bulk_release_rounds;
	uint64_t bulk_prints;
	/* Last uberblock written and flushed, and its catalog txg. */
	uint64_t durable_ub_txg;
	uint64_t durable_catalog_txg;
	/* Descriptor of the last stored and of the last durable catalog. */
	uint64_t stored_desc[16];
	uint64_t durable_desc[16];
	uint64_t certified;
	uint64_t store_tolerance;
	/* The owner-release sync task frees native V through this thread. */
	void *release_thread;
	/* Vector admission state and the standing conversion reserve. */
	engine_admit_class_t admit[PR19262_ADMIT_CLASSES];
	uint64_t admit_parity;
	uint64_t admit_batch;
	uint64_t admit_reserve;
	uint64_t admit_reserve_growth;
	uint64_t admit_reserve_legacy;
	uint64_t admit_reserve_weak;
	uint64_t admit_refreshed;
	uint64_t admit_cached_parity;
	/* Pending reserve changes form a suffix of the catalog dirty list. */
	engine_owner_t *admit_first;
	uint64_t admit_growth_peak;
	uint64_t admit_space_rejections;
	uint64_t admit_restarts;
	/* The fence the admission schedules past the retired limit. */
	kcondvar_t fence_cv;
	boolean_t fence_pending;
	boolean_t fence_quiesced;
#ifndef _KERNEL
	pr19262_export_fence_test_t *export_fence_test;
	/* The fence task's thread while it runs a scheduled fence. */
	kthread_t *fence_task_thread;
	/* One ztest allocation may prefer this already-free physical range. */
	uint64_t test_prefer_p;
	uint64_t test_prefer_end;
	boolean_t test_prefer_active;
	/* Growth-defer test: a legacy mark and one steered arena pick. */
	zfs_range_tree_t *test_legacy_mark;
	uint64_t test_arena_p;
	boolean_t test_arena_active;
	boolean_t test_arena_force;
	uint64_t test_arena_result;
	void (*test_start_fn)(spa_t *, void *);
	void *test_start_arg;
	zfs_range_tree_t *test_holes;
	/* Journal roll test: holes carved out of free, a held run + 1. */
	zfs_range_tree_t *test_jholes;
	uint64_t test_jheld;
#endif
	/* The last scheduled fence reclaimed and released nothing. */
	boolean_t fence_stalled;
	int fence_error;
	uint64_t fence_requests;
	uint64_t fence_auto;
	/*
	 * L15: fence_stalled needs the empty fence's catalog to list at least
	 * retired_limit of the retired bytes; one that listed less was a
	 * timing wait (fence_timing: the last scheduled fence was one;
	 * fence_timing_mark: fence_returns then).  fence_returns counts the
	 * direct fences that returned retired bytes.
	 */
	boolean_t fence_timing;
	uint64_t fence_timing_mark;
	uint64_t fence_timing_waits;
	uint64_t fence_returns;
	/*
	 * L21: retired bytes a checkpoint keeps from every fence, and the V
	 * of the retired owners among them, as last counted (valid), with the
	 * checkpoint state and the synced txg of that count.  The retired
	 * limit and the growth trigger judge only retired - retired_pinned;
	 * passes counts client admissions past the limit by pinned bytes only.
	 */
	boolean_t retired_pinned_valid;
	boolean_t retired_pinned_draining;
	uint64_t retired_pinned;
	uint64_t retired_pinned_v;
	uint64_t retired_pinned_ckpt;
	uint64_t retired_pinned_after;
	uint64_t retired_pinned_txg;
	uint64_t retired_pinned_counts;
	uint64_t retired_pinned_passes;
	uint64_t retired_pinned_prints;
	/*
	 * Store/retired growth trigger, evaluated in syncing context after a
	 * catalog store: catalog pages in use and retired bytes when the last
	 * scheduled fence completed, and how often growth dispatched one.
	 */
	uint64_t fence_growth_pages_mark;
	uint64_t fence_growth_retired_mark;
	uint64_t fence_growth_triggers;
	boolean_t fence_growth_seeded;
	/* The pending scheduled fence was dispatched by the growth check. */
	boolean_t fence_growth_running;
	uint64_t fence_growth_dispatch_failures;
	uint64_t admit_arena_rejections;
	/*
	 * One arena-sized physical run held back for the syncing store
	 * (RAM-only; it is in store_extents, not in free).  Admission does
	 * not count it, so a catalog growth in syncing context still finds a
	 * run beyond the ones admission counted (those are held apart in
	 * store_reserved).  Headroom is low when the standby is gone and no
	 * free run is left to refill it; admission then refuses page work.
	 */
	uint64_t store_standby;
	boolean_t store_standby_valid;
	boolean_t store_headroom_low;
	boolean_t store_headroom_warned;
	uint64_t store_standby_fills;
	uint64_t store_standby_uses;
	uint64_t store_standby_releases;
	/*
	 * Arena runs admission counted for the catalog growth of admitted
	 * work, set aside until a store grows into them or the charges that
	 * needed them return (RAM-only; in store_extents, not in free, as
	 * the standby).  No allocator, copy stage, journal roll or standby
	 * fill sees them; every catalog growth takes them first.
	 */
	zfs_range_tree_t *store_reserved;
	uint64_t store_reserve_fills;
	uint64_t store_reserve_uses;
	uint64_t store_reserve_returns;
	uint64_t store_reserve_peak;
	/*
	 * A net free that fits only with held runs waits behind the stores
	 * that hold them, bounded from the synced txg its first wait marks
	 * (a mark nobody renewed for a while starts over); past that it takes
	 * back what it needs of them.  A converter copy that fits only in
	 * runs held for others waits as busy (copy_held_waits).
	 */
	uint64_t netfree_wait_mark;
	uint64_t netfree_wait_last;
	uint64_t admit_held_waits;
	uint64_t copy_held_waits;
	uint64_t admit_held_takes;
	uint64_t store_grow_enospc;
	uint64_t admit_headroom_rejections;
	uint64_t checkpoint_headroom_refusals;
	/*
	 * Long-free pacing (dmu_free_long_range): arena runs in the free
	 * tree as of the last store (bounded count), and the catalog rows
	 * charged to each open txg while no run is left.  A chunk past a
	 * txg's row budget is deferred to the next txg; deferrals since the
	 * last fence make the growth check fence.
	 */
	uint64_t free_runs_cached;
	boolean_t free_runs_known;
	uint64_t free_pace_txg[TXG_SIZE];
	uint64_t free_pace_rows[TXG_SIZE];
	uint64_t free_paced_rows;
	uint64_t free_deferrals;
	uint64_t free_deferrals_mark;
	uint64_t free_aged;
	/*
	 * Completed scheduled fences, and the arena-gate state when a client
	 * was last told to wait: a later fence that raised neither free pages
	 * nor arena runs turns the next wait into ENOSPC.
	 */
	uint64_t fence_completions;
	/* Scheduled fences that returned ESHUTDOWN on a suspended pool. */
	uint64_t fence_suspended;
	uint64_t admit_arena_wait_gen;
	uint64_t admit_arena_wait_deficit;
	/* Synced txg at the first restart behind the reuse deferral. */
	uint64_t admit_defer_txg;
	uint64_t admit_fence_waits;
	/*
	 * L18: a converter pass that clients starved runs next with priority
	 * (new client charges restart until it ends); refusal counters.
	 */
	boolean_t admit_conv_priority;
	/* L18: a conversion runs: its legacy worst case is hidden on top. */
	boolean_t admit_converting;
	uint64_t admit_conv_priority_waits;
	uint64_t admit_conv_refusals;
	uint64_t admit_conv_alone;
	engine_conv_refusal_t admit_conv_last;
	uint64_t stale_verdicts;
	uint64_t retire_prints;
	uint64_t abort_prints;
	uint64_t inherit_prints;
	uint64_t hold_report_prints;
	uint64_t birth_drift_prints;
	uint64_t gate_prints;
	engine_copy_cost_t copy_cost;
	engine_admit_cost_t admit_cost;
	engine_journal_cost_t journal_cost;
	engine_slot_cost_t slot_cost;
	engine_store_cost_t store_cost;
	uint64_t io_errors;
	uint_t dispatch_unsafe_prints;
	uint64_t store_crash_txg;
	uint_t journal_fault_stage;
	uint64_t journal_fault_seq;
	uint_t journal_roll_fault;
	uint_t journal_group_fault;
	uint_t journal_roll_phase;
	boolean_t m_read_injected;
	boolean_t m_active_error_injected;
	uint64_t pages_evicted;
	uint64_t store_checked;
	uint64_t store_check_failures;
	void (*store_test_hook)(spa_t *, int);
	void (*journal_observer)(spa_t *, const blkptr_t *, int);
	struct engine_pages *pages;
#ifdef _KERNEL
	struct engine_kernel *kernel;
#endif
};

typedef struct engine_walk_arg {
	engine_t *engine;
	void *arg;
} engine_walk_arg_t;

static void
engine_quorum_add(engine_t *engine, engine_quorum_t *q, int error)
{
	if (error == 0)
		return;
	atomic_inc_64(&engine->io_errors);
	q->failures++;
	q->absent += error == ENXIO;
	if (q->error == 0 || error != ENXIO)
		q->error = error;
}

static int
engine_quorum_result(engine_t *engine, const engine_quorum_t *q)
{
	return (q->failures > engine_store_tolerated(engine) ? q->error : 0);
}

static void
engine_owner_unlist(engine_t *engine, engine_owner_t *e)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (list_link_active(&e->dirty_node)) {
		if (engine->admit_first == e)
			engine->admit_first = list_next(&engine->dirty, e);
		list_remove(&engine->dirty, e);
	}
	e->admit_dirty = B_FALSE;
}

/*
 * The owner's catalog rows may no longer match the stored tree.  Every
 * change to a field a row carries, to the version list, or to what decides
 * whether the owner is listed marks the owner; debug ztest checks that no
 * row of an unmarked owner changes.
 */
static void
engine_owner_dirty(engine_t *engine, engine_owner_t *e)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	e->dirty_seq = ++engine->dirty_next;
	/* Keep reserve changes behind the already processed prefix. */
	engine_owner_unlist(engine, e);
	list_insert_tail(&engine->dirty, e);
	e->admit_dirty = B_TRUE;
	if (engine->admit_first == NULL)
		engine->admit_first = e;
}

/*
 * Every owner comes from here: a Linux list node is unlinked when it holds
 * the list poison, not when it is zero, so the node needs its init.
 */
static engine_owner_t *
engine_owner_alloc(int kmflag)
{
	engine_owner_t *e = kmem_zalloc(sizeof (*e), kmflag);
	if (e != NULL)
		list_link_init(&e->dirty_node);
	return (e);
}

/* The owner leaves the engine: the next store drops its rows. */
static void
engine_owner_gone(engine_t *engine, engine_owner_t *e)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_admit_owner_forget(engine, e);
	engine_owner_unlist(engine, e);
	if (engine->gone_count == engine->gone_capacity) {
		uint_t cap = MAX(64, engine->gone_capacity * 2);
		uint64_t *gone = pr19262_zalloc(cap * 2 * sizeof (uint64_t),
		    KM_SLEEP);
		if (engine->gone != NULL) {
			memcpy(gone, engine->gone,
			    engine->gone_count * 2 * sizeof (uint64_t));
			kmem_free(engine->gone,
			    engine->gone_capacity * 2 * sizeof (uint64_t));
		}
		engine->gone = gone;
		engine->gone_capacity = cap;
	}
	engine->gone[2 * engine->gone_count] = e->v;
	engine->gone[2 * engine->gone_count + 1] = e->incarnation;
	engine->gone_count++;
}

static void
engine_version_geometry(engine_version_t *b, uint64_t width, uint64_t parity)
{
	VERIFY3U(width, <=, UINT16_MAX);
	VERIFY3U(parity, <=, UINT16_MAX);
	b->width = width;
	b->parity = parity;
}

static void
engine_version_layout(engine_version_t *b, vdev_t *vd, uint64_t birth)
{
	uint64_t width, parity;

	vdev_raidz_layout_for_birth(vd, birth, &width, &parity);
	engine_version_geometry(b, width, parity);
}

static engine_version_t *
engine_version_new(engine_t *engine, engine_owner_t *e, uint64_t p,
    uint64_t size,
    uint64_t birth, boolean_t ready)
{
	engine_version_t *b = kmem_zalloc(sizeof (*b), KM_SLEEP);
	b->owner = e;
	b->p = p;
	b->size = size;
	b->birth = birth;
	b->generation = ++engine->sequence;
	engine_version_layout(b, engine->vd, birth);
	b->ready = ready;
	b->next = e->versions;
	e->versions = b;
	engine->revision++;
	return (b);
}

static void
engine_resume(engine_pending_t *p)
{
	while (p != NULL) {
		engine_pending_t *next = p->next;
		if (p->start)
			zio_nowait(p->zio);
		else
			pr19262_zio_resume(p->zio);
		kmem_free(p, sizeof (*p));
		p = next;
	}
}

static void
engine_defer(zio_t *zio, engine_pending_t **list)
{
	engine_t *engine = zio->io_spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_pending_t *p = kmem_alloc(sizeof (*p), KM_SLEEP);
	p->zio = zio;
	p->start = B_FALSE;
	p->next = *list;
	*list = p;
	/* Resume at this same dispatch stage, without blocking its executor. */
	zio->io_stage >>= 1;
}

#ifdef _KERNEL
/* A parked scan ZIO has not entered its pipeline and must start normally. */
static engine_pending_t *
engine_pending_unstarted_alloc(zio_t *zio)
{
	ASSERT3U(zio->io_stage, ==, ZIO_STAGE_OPEN);
	engine_pending_t *p = kmem_alloc(sizeof (*p), KM_SLEEP);
	p->zio = zio;
	p->start = B_TRUE;
	p->next = NULL;
	return (p);
}

static void
engine_park_unstarted(zio_t *zio, engine_pending_t *p,
    engine_pending_t **list)
{
	engine_t *engine = zio->io_spa->spa_reloc;

	ASSERT(MUTEX_HELD(&engine->lock));
	ASSERT3U(zio->io_stage, ==, ZIO_STAGE_OPEN);
	VERIFY3P(p, !=, NULL);
	p->next = *list;
	*list = p;
}

static const char *const engine_r01_names[] = {
	"idle", "armed", "prebinding", "parked", "released", "drained",
	"cancelled"
};

static const char *const engine_r01_boundary_names[] = {
	"selected", "copying", "published", "final-free"
};

static const char *
engine_r01_boundary_name(uint_t boundary)
{
	return (boundary < PR19262_R01_BOUNDARIES ?
	    engine_r01_boundary_names[boundary] : "none");
}

static const char *
engine_r01_test_name(uint_t mode)
{
	switch (mode) {
	case PR19262_R01_TEST_NONE:
		return ("none");
	case PR19262_R01_TEST_EPOCH_STALE:
		return ("epoch-stale");
	case PR19262_R01_TEST_CONVERT_EIO:
		return ("convert-eio");
	default:
		return ("invalid");
	}
}

static void
engine_r01_reset_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t generation = engine->r01.generation + 1;
	uint64_t scan_cancel_gen = engine->r01.scan_cancel_gen;
	boolean_t copy_prepared = engine->r01.copy_prepared;
	uint64_t copy_birth = engine->r01.copy_birth;
	uint64_t copy_parity = engine->r01.copy_parity;
	memset(&engine->r01, 0, sizeof (engine->r01));
	engine->r01.generation = generation;
	engine->r01.scan_cancel_gen = scan_cancel_gen;
	engine->r01.copy_prepared = copy_prepared;
	engine->r01.copy_birth = copy_birth;
	engine->r01.copy_parity = copy_parity;
	engine->r01.state = ENGINE_R01_ARMED;
}

static uint_t
engine_r01_boundary_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_r01_t *r = &engine->r01;
	if (r->boundary_ready && r->boundary_reached == PR19262_R01_FINAL_FREE)
		return (PR19262_R01_FINAL_FREE);
	if (r->owner != NULL && r->version != NULL) {
		if (r->owner->copying)
			return (PR19262_R01_COPYING);
		if (r->owner->cur != r->version && r->version->retired)
			return (PR19262_R01_PUBLISHED);
	}
	return (PR19262_R01_SELECTED);
}

/* Owner addresses may be reconstructed; V/incarnation bind the identity. */
static boolean_t
engine_r01_owner_matches_locked(const engine_r01_t *r,
    const engine_owner_t *owner)
{
	ASSERT(owner != NULL);
	return (r->owner == owner || (r->owner != NULL && owner->v == r->v &&
	    owner->incarnation == r->incarnation));
}

/* A caller completes queue accounting before making the ZIO releasable. */
static void
engine_r01_prebind_wait_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	while (engine->r01.state == ENGINE_R01_PREBINDING &&
	    engine->r01.prebind_handoff)
		cv_wait(&engine->changed, &engine->lock);
}

static void
engine_r01_boundary_wait(engine_t *engine, engine_owner_t *owner,
    uint_t boundary)
{
	ASSERT3U(boundary, <, PR19262_R01_BOUNDARIES);
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	engine_r01_prebind_wait_locked(engine);
	if (r->copy_requested) {
		r->copy_boundary_calls++;
		r->copy_owner_match = engine_r01_owner_matches_locked(r, owner);
		r->copy_boundary_v = owner->v;
		r->copy_boundary_incarnation = owner->incarnation;
		r->copy_boundary_state = r->state;
	}
	if (engine_r01_owner_matches_locked(r, owner) &&
	    r->boundary == boundary &&
	    (r->state == ENGINE_R01_PARKED ||
	    r->state == ENGINE_R01_RELEASED)) {
		r->boundary_reached = boundary;
		r->boundary_ready = B_TRUE;
		r->boundary_waiting = B_TRUE;
		cv_broadcast(&engine->changed);
		while (!r->boundary_advance && r->state != ENGINE_R01_CANCELLED)
			cv_wait(&engine->changed, &engine->lock);
		r->boundary_waiting = B_FALSE;
		cv_broadcast(&engine->changed);
	}
	mutex_exit(&engine->lock);
}

/* A test copy can release the final owner hold outside engine_rele(). */
static void
engine_r01_try_drain_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_r01_t *r = &engine->r01;
	if (r->owner == NULL)
		return;
	r->holds_terminal = r->owner->holds;
	if (!r->copy_running && !r->epoch_waiting && !r->observer_lease &&
	    !r->final_fence_running && r->parent_done &&
	    r->owner->holds == 0 &&
	    engine->parked == NULL && r->prebound == NULL &&
	    r->selected == NULL && !r->bound_lease &&
	    (r->state == ENGINE_R01_RELEASED ||
	    r->state == ENGINE_R01_CANCELLED)) {
		r->drained = B_TRUE;
		if (r->state == ENGINE_R01_RELEASED)
			r->state = ENGINE_R01_DRAINED;
		r->owner = NULL;
		r->version = NULL;
		cv_broadcast(&engine->changed);
	}
}

static void
engine_r01_observer_lease_rele_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_r01_t *r = &engine->r01;

	if (!r->observer_lease)
		return;
	VERIFY3P(r->version, !=, NULL);
	VERIFY3U(r->version->leases, >, 0);
	r->version->leases--;
	r->observer_lease = B_FALSE;
}

typedef struct engine_r01_copy_job {
	spa_t *spa;
	blkptr_t bp;
	int dva;
	uint64_t birth;
	uint64_t parity;
} engine_r01_copy_job_t;

static void
engine_r01_copy_finish(spa_t *spa, uint64_t birth, uint64_t parity,
    int error)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	r->copy_running = B_FALSE;
	r->copy_done = B_TRUE;
	r->copy_error = error;
	r->copy_birth = birth;
	r->copy_parity = parity;
	engine_r01_try_drain_locked(engine);
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
}

/*
 * Under an armed final-free boundary every fence but the final one retains
 * P.  When final_free_wait() did not already claim the final fence for this
 * copy worker (no boundary wait happened), claim it here, so the worker's
 * own fence below is the one final fence; final_free_finish() releases it.
 */
static void
engine_r01_copy_claim_final_fence(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	/* A public epoch's final fence is the converter's second fence. */
	if (r->boundary == PR19262_R01_FINAL_FREE && !r->epoch_waiting &&
	    r->state != ENGINE_R01_CANCELLED &&
	    !r->final_fence_running && !r->final_fence_done &&
	    r->final_fence_thread == NULL) {
		r->final_fence_running = B_TRUE;
		r->final_fence_thread = curthread;
	}
	mutex_exit(&engine->lock);
}

static void
engine_r01_copy_worker(void *arg)
{
	engine_r01_copy_job_t *job = arg;
	uint64_t floor = 0, reclaimed = 0, released = 0;
	void *copy = NULL;
	int error = pr19262_engine_copy_begin(job->spa, &job->bp, job->dva,
	    job->birth, &copy);
	if (error == 0)
		error = pr19262_engine_copy_run(copy, B_FALSE);
	if (error == 0)
		error = pr19262_engine_r01_final_free_wait(job->spa);
	if (error == 0)
		engine_r01_copy_claim_final_fence(job->spa);
	if (error == 0)
		error = pr19262_engine_fence(job->spa, &floor, &reclaimed,
		    &released, NULL);
	pr19262_engine_r01_final_free_finish(job->spa, error,
	    error == 0 ? reclaimed : 0);
	engine_r01_copy_finish(job->spa, job->birth, job->parity, error);
	kmem_free(job, sizeof (*job));
	thread_exit();
}

static boolean_t
engine_r01_match_locked(engine_t *engine, zio_t *zio)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_r01_t *r = &engine->r01;
	if ((r->state != ENGINE_R01_PARKED &&
	    r->state != ENGINE_R01_RELEASED) || r->owner == NULL ||
	    r->p == 0)
		return (B_FALSE);
	/*
	 * Every repair of the released natural read completed before its
	 * root; a later scan (a restarted resilver) repairing the same block
	 * is not R-01's.
	 */
	if (r->state == ENGINE_R01_RELEASED && r->scan_done)
		return (B_FALSE);
	/*
	 * A RAIDZ repair leaf inherits its owner hold, but its child ZIO can
	 * lack the optional backing stamp.  The held owner is the binding in
	 * that case; a nonzero stamp must still name the selected backing.
	 */
	if (zio->io_pr19262_backing_birth != r->p &&
	    (zio->io_pr19262_backing_birth != 0 ||
	    !(zio->io_flags & ZIO_FLAG_IO_REPAIR)))
		return (B_FALSE);
	engine_hold_t *hold = zio->io_pr19262_owner_hold;
	if (hold == NULL || hold->spa != engine->spa)
		return (B_FALSE);
	for (uint_t i = 0; i < hold->count; i++)
		if (hold->owner[i] == r->owner)
			return (B_TRUE);
	return (B_FALSE);
}

/*
 * The private R-01 arm operation owns a real, assigned DMU transaction T.
 * Ordinary zio_read() seeds io_txg from B, so the arm-time T is installed
 * only after the candidate has been bound to its selected backing P.  This
 * keeps the normal logical ZIO and all of its natural RAIDZ descendants on
 * one operation identity without manufacturing a leaf repair ZIO.
 */
static boolean_t
engine_r01_selectable(const engine_t *engine, const blkptr_t *bp,
    const engine_version_t *b)
{
	uint64_t birth = BP_GET_PHYSICAL_BIRTH(bp);
	uint64_t txg = engine->r01.t;

	return (BP_GET_LEVEL(bp) == 0 &&
	    !DMU_OT_IS_METADATA(BP_GET_TYPE(bp)) && birth != 0 &&
	    b->birth != 0 && txg != 0 && birth != b->birth &&
	    birth != txg && b->birth != txg);
}

static engine_owner_t *engine_find(engine_t *, uint64_t);

#ifdef _KERNEL
/* Definitions arrive through the later textual kernel-driver include. */
static int engine_kernel_r01_arm_hold(spa_t *);
static void engine_kernel_r01_arm_rele(spa_t *);
static void engine_kernel_export_begin(spa_t *);
static void engine_kernel_export_abort(spa_t *);
static void engine_kernel_zil_begin(spa_t *);
static void engine_kernel_zil_finish(spa_t *);
#endif

/*
 * The routed natural read can have logical/vdev ancestors between it and
 * the detached root.
 */
static boolean_t
engine_r01_routed_locked(const engine_t *engine, zio_t *zio)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (!engine->r01.scan_routed || engine->r01.scan_root == NULL)
		return (B_FALSE);
	for (;;) {
		zio_link_t *zl = NULL;
		zio_t *parent = zio_walk_parents(zio, &zl);
		if (parent == NULL)
			return (B_FALSE);
		if (parent == engine->r01.scan_root)
			return (B_TRUE);
		zio = parent;
	}
}

/*
 * Exactly one control path changes scan_root to NULL, after its zio_wait().
 * The root waiter never holds engine->lock: completion can release engine
 * holds and invoke the scan callback.
 */
static int
engine_r01_wait_scan_root(engine_t *engine, zio_t *root)
{
	int error = zio_wait(root);

	mutex_enter(&engine->lock);
	if (engine->r01.scan_root == root) {
		engine->r01.scan_root = NULL;
		engine->r01.scan_waiting = B_FALSE;
		engine->r01.scan_done = B_TRUE;
		engine->r01.scan_root_waits++;
		engine->r01.scan_root_error = error;
		cv_broadcast(&engine->changed);
	}
	mutex_exit(&engine->lock);
	return (error);
}

/* Cancel/export/unload must also drain a pre-existing optional test worker. */
static int
engine_r01_wait_terminal(engine_t *engine, int error)
{
	mutex_enter(&engine->lock);
	while (engine->r01.copy_running || engine->r01.epoch_waiting ||
	    engine->r01.boundary_waiting ||
	    engine->r01.final_fence_running || !engine->r01.drained)
		cv_wait(&engine->changed, &engine->lock);
	mutex_exit(&engine->lock);
	return (error);
}

static void
engine_r01_bound_lease_rele_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_r01_t *r = &engine->r01;

	if (!r->bound_lease)
		return;
	VERIFY3P(r->version, !=, NULL);
	VERIFY3U(r->version->leases, >, 0);
	r->version->leases--;
	r->bound_lease = B_FALSE;
}

/*
 * Pick a root before zio_read() links the natural I/O to the scan root.  The
 * predicates deliberately mirror dispatch, so the one reserved root can
 * never absorb an unrelated scan I/O.
 */
zio_t *
pr19262_engine_r01_scan_parent(spa_t *spa, const blkptr_t *bp,
    zio_flag_t flags, void *owner_hold)
{
	engine_hold_t *hold = owner_hold;

	if (spa == NULL || bp == NULL || hold == NULL ||
	    !(flags & ZIO_FLAG_RESILVER) || hold->spa != spa ||
	    hold->error != 0 || hold->write || hold->gang)
		return (NULL);
	engine_t *engine = spa->spa_reloc;
	if (engine == NULL)
		return (NULL);
	zio_t *root = NULL;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	if (engine->spa == spa && r->state == ENGINE_R01_ARMED &&
	    engine->park_next && !r->scan_routed && r->scan_root != NULL) {
		for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
			const dva_t *dva = &bp->blk_dva[d];
			if (DVA_GET_VDEV(dva) != engine->vd->vdev_id)
				continue;
			engine_owner_t *e = engine_find(engine,
			    DVA_GET_OFFSET(dva));
			if (e != NULL && e->cur != NULL && e->cur->ready &&
			    engine_r01_selectable(engine, bp, e->cur)) {
				boolean_t held = B_FALSE;
				for (uint_t i = 0; i < hold->count; i++)
					if (hold->owner[i] == e)
						held = B_TRUE;
				if (!held)
					break;
				/*
				 * Pin exact P through zio_read() binding.
				 * e->cur may be replaced after linearization.
				 * The natural read consumes this same lease.
				 */
				r->owner = e;
				r->version = e->cur;
				r->v = e->v;
				r->incarnation = e->incarnation;
				r->version->leases++;
				r->bound_lease = B_TRUE;
				r->scan_routed = B_TRUE;
				r->scan_submitting = B_TRUE;
				r->scan_done = B_FALSE;
				root = r->scan_root;
			}
			break;
		}
	}
	mutex_exit(&engine->lock);
	return (root);
}

/*
 * scan_parent() hands root to scan_exec_io before zio_read() is linked.  A
 * concurrent cancel must wait for that handoff, or it could free root while
 * scan_exec_io still has the raw parent pointer.
 */
void
pr19262_engine_r01_scan_submit_done(spa_t *spa, zio_t *root)
{
	if (spa == NULL || root == NULL || spa->spa_reloc == NULL)
		return;
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->r01.scan_root == root && engine->r01.scan_submitting) {
		engine->r01.scan_submitting = B_FALSE;
		cv_broadcast(&engine->changed);
	}
	mutex_exit(&engine->lock);
}

boolean_t
pr19262_engine_r01_scan_pending(spa_t *spa)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return (B_FALSE);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	boolean_t pending = engine->r01.scan_routed && !engine->r01.scan_done;
	mutex_exit(&engine->lock);
	return (pending);
}

static void
engine_r01_prebind_locked(engine_t *engine, zio_t *zio, engine_owner_t *e,
    engine_version_t *b, uint64_t v)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_r01_t *r = &engine->r01;
	VERIFY3U(r->state, ==, ENGINE_R01_ARMED);
	VERIFY(r->scan_routed);
	VERIFY3P(r->scan_root, !=, NULL);
	VERIFY3P(r->owner, ==, e);
	VERIFY3P(r->version, ==, b);
	VERIFY(r->bound_lease);
	r->state = ENGINE_R01_PREBINDING;
	r->prebound = zio;
	r->prebind = B_TRUE;
	r->prebind_no_scl = B_TRUE;
	r->owner = e;
	r->version = b;
	r->copy_bp = zio->io_bp_orig;
	r->copy_dva = -1;
	for (int d = 0; d < BP_GET_NDVAS(&r->copy_bp); d++) {
		if (DVA_GET_VDEV(&r->copy_bp.blk_dva[d]) ==
		    engine->vd->vdev_id &&
		    DVA_GET_OFFSET(&r->copy_bp.blk_dva[d]) == e->v) {
			r->copy_dva = d;
			break;
		}
	}
	VERIFY3S(r->copy_dva, >=, 0);
	r->v = v;
	r->incarnation = e->incarnation;
	r->b = BP_GET_PHYSICAL_BIRTH(&zio->io_bp_orig);
	r->p = b->birth;
	VERIFY3U(r->t, !=, 0);
	r->p_offset = b->p;
	r->p_size = b->size;
	r->width = b->width;
	r->parity = b->parity;
	r->lease_parked = b->leases;
	r->holds_parked = e->holds;
	r->writers_parked = b->writers;
	r->tuple_distinct = r->b != 0 && r->p != 0 && r->t != 0 &&
	    r->b != r->p && r->b != r->t && r->p != r->t;
}

/* The post-release physical child consumes the P lease prebound above. */
static void
engine_r01_selected_locked(engine_t *engine, zio_t *zio, engine_owner_t *e,
    engine_version_t *b, uint64_t v)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_r01_t *r = &engine->r01;
	VERIFY3U(r->state, ==, ENGINE_R01_RELEASED);
	VERIFY3P(r->prebound, ==, zio->io_logical);
	VERIFY(r->scan_routed);
	VERIFY3P(r->scan_root, !=, NULL);
	VERIFY3P(r->owner, ==, e);
	VERIFY3P(r->version, ==, b);
	VERIFY3U(r->v, ==, v);
	VERIFY3U(r->incarnation, ==, e->incarnation);
	VERIFY(r->bound_lease);
	VERIFY3P(zio->io_pr19262_owner_hold, !=, NULL);

	r->bound_lease = B_FALSE;
	r->prebound = NULL;
	r->selected = zio;
	((engine_hold_t *)zio->io_pr19262_owner_hold)->r01_selected = B_TRUE;
	zio->io_txg = r->t;
}

/*
 * Stop the detached natural resilver after its inherited hold is installed.
 * This happens before zio_nowait() can enter SCL_ZIO.  The selected P lease
 * stays held across public conversion; release starts this logical ZIO.
 */
int
pr19262_engine_r01_prebind(zio_t *zio)
{
	if (zio == NULL || zio->io_spa == NULL ||
	    zio->io_spa->spa_reloc == NULL)
		return (0);

	spa_t *spa = zio->io_spa;
	engine_t *engine = spa->spa_reloc;
	engine_pending_t *pending = engine_pending_unstarted_alloc(zio);
	int error = 0;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	r->prebind_attempts++;
	if (zio->io_vd != NULL || zio->io_logical != zio ||
	    zio->io_type != ZIO_TYPE_READ || zio->io_bp == NULL ||
	    !(zio->io_flags & ZIO_FLAG_RESILVER) ||
	    (zio->io_flags & (ZIO_FLAG_GANG_CHILD | ZIO_FLAG_CONFIG_WRITER))) {
		r->prebind_bad_shape++;
		goto out;
	}
	if (spa_config_held(spa, SCL_ZIO, RW_READER) != 0 ||
	    spa_config_held(spa, SCL_ZIO, RW_WRITER) != 0) {
		r->prebind_scl++;
		goto out;
	}
	engine_hold_t *hold = zio->io_pr19262_owner_hold;
	boolean_t held = B_FALSE;
	if (hold != NULL && hold->spa == spa && hold->error == 0 &&
	    !hold->gang) {
		for (uint_t i = 0; i < hold->count; i++)
			if (hold->owner[i] == r->owner)
				held = B_TRUE;
	}
	if (!held)
		r->prebind_unheld++;
	if (engine->spa == spa && r->state == ENGINE_R01_ARMED &&
	    engine->park_next && r->scan_routed && r->scan_root != NULL &&
	    r->bound_lease && r->prebound == NULL && r->selected == NULL &&
	    r->owner != NULL && r->version != NULL && held &&
	    engine_r01_routed_locked(engine, zio) &&
	    engine_r01_selectable(engine, zio->io_bp, r->version)) {
		engine_r01_prebind_locked(engine, zio, r->owner, r->version,
		    r->v);
		engine->park_next = B_FALSE;
		r->prebind_pending = pending;
		r->prebind_handoff = B_TRUE;
		pending = NULL;
		error = EINPROGRESS;
	} else if (held) {
		r->prebind_nomatch++;
	}
out:
	mutex_exit(&engine->lock);
	if (pending != NULL)
		kmem_free(pending, sizeof (*pending));
	return (error);
}

/*
 * Publish only after dsl_scan has returned the exact queued credit and marked
 * the selected ZIO's callback to skip its later duplicate decrement.
 */
void
pr19262_engine_r01_prebind_publish(zio_t *zio)
{
	VERIFY3P(zio, !=, NULL);
	VERIFY3P(zio->io_spa, !=, NULL);
	VERIFY3P(zio->io_spa->spa_reloc, !=, NULL);
	engine_t *engine = zio->io_spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	VERIFY3U(r->state, ==, ENGINE_R01_PREBINDING);
	VERIFY(r->prebind_handoff);
	VERIFY3P(r->prebound, ==, zio);
	VERIFY3P(r->prebind_pending, !=, NULL);
	VERIFY(zio->io_pr19262_r01_scan_credit_released);
	engine_pending_t *pending = r->prebind_pending;
	r->prebind_pending = NULL;
	r->prebind_handoff = B_FALSE;
	engine_park_unstarted(zio, pending, &engine->parked);
	r->state = ENGINE_R01_PARKED;
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
}

/*
 * Allocate an operation identity without changing data, a DTL, or engine
 * state.  NOWAIT keeps a module-parameter write from sleeping; callers can
 * retry an ERESTART/EAGAIN result at a fresh idle boundary.
 */
static int
engine_r01_operation_txg(spa_t *spa, uint64_t *txgp)
{
	dmu_tx_t *tx = dmu_tx_create_dd(spa_get_dsl(spa)->dp_mos_dir);
	int error = dmu_tx_assign(tx, DMU_TX_NOWAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		return (error);
	}
	*txgp = dmu_tx_get_txg(tx);
	dmu_tx_commit(tx);
	return (0);
}

int
pr19262_engine_r01_arm(spa_t *spa, uint_t boundary)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return (ENOTSUP);
	if (boundary >= PR19262_R01_BOUNDARIES)
		return (EINVAL);
#ifdef _KERNEL
	int gate_error = engine_kernel_r01_arm_hold(spa);
	if (gate_error != 0)
		return (gate_error);
#endif
	/* It remains unopened until the selected natural scan child exists. */
	zio_t *scan_root = zio_root(spa, NULL, NULL, ZIO_FLAG_CANFAIL);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->spa != spa || !engine->persistent || engine->vd == NULL)
		goto unsupported;
	if (engine->r01.scan_cancel_inflight)
		goto busy;
	if ((boundary == PR19262_R01_FINAL_FREE &&
	    (!engine->r01.copy_prepared || engine->r01.copy_parity != 3)) ||
	    engine->park_next || engine->parked != NULL ||
	    engine->r01.prebound != NULL || engine->r01.selected != NULL ||
	    (engine->r01.state != ENGINE_R01_IDLE &&
	    engine->r01.state != ENGINE_R01_DRAINED &&
	    engine->r01.state != ENGINE_R01_CANCELLED))
		goto busy;
	mutex_exit(&engine->lock);
	uint64_t txg;
	int error = engine_r01_operation_txg(spa, &txg);
	if (error != 0) {
		(void) zio_wait(scan_root);
#ifdef _KERNEL
		engine_kernel_r01_arm_rele(spa);
#endif
		return (error);
	}
	mutex_enter(&engine->lock);
	if (engine->spa != spa || !engine->persistent || engine->vd == NULL)
		goto unsupported;
	if (engine->r01.scan_cancel_inflight)
		goto busy;
	if ((boundary == PR19262_R01_FINAL_FREE &&
	    (!engine->r01.copy_prepared || engine->r01.copy_parity != 3)) ||
	    engine->park_next || engine->parked != NULL ||
	    engine->r01.prebound != NULL || engine->r01.selected != NULL ||
	    (engine->r01.state != ENGINE_R01_IDLE &&
	    engine->r01.state != ENGINE_R01_DRAINED &&
	    engine->r01.state != ENGINE_R01_CANCELLED))
		goto busy;
	engine_r01_reset_locked(engine);
	engine->r01.boundary = boundary;
	engine->r01.t = txg;
	engine->r01.scan_root = scan_root;
	engine->park_next = B_TRUE;
	mutex_exit(&engine->lock);
#ifdef _KERNEL
	engine_kernel_r01_arm_rele(spa);
#endif
	return (0);
busy:
	mutex_exit(&engine->lock);
	(void) zio_wait(scan_root);
#ifdef _KERNEL
	engine_kernel_r01_arm_rele(spa);
#endif
	return (EBUSY);
unsupported:
	mutex_exit(&engine->lock);
	(void) zio_wait(scan_root);
#ifdef _KERNEL
	engine_kernel_r01_arm_rele(spa);
#endif
	return (ENOTSUP);
}

int
pr19262_engine_r01_release(spa_t *spa)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return (ENOTSUP);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r01_prebind_wait_locked(engine);
	/* A public final-free worker must first publish Q and fence it. */
	if (engine->r01.epoch_waiting &&
	    (!engine->r01.boundary_waiting ||
	    !engine->r01.boundary_ready ||
	    engine->r01.boundary_reached != PR19262_R01_FINAL_FREE)) {
		mutex_exit(&engine->lock);
		return (EBUSY);
	}
	if (engine->r01.state != ENGINE_R01_PARKED || engine->parked == NULL ||
	    engine->r01.prebound == NULL || !engine->r01.scan_routed ||
	    engine->r01.scan_root == NULL) {
		mutex_exit(&engine->lock);
		return (EINVAL);
	}
	engine_pending_t *pending = engine->parked;
	zio_t *root = engine->r01.scan_root;
	engine->parked = NULL;
	engine->r01.state = ENGINE_R01_RELEASED;
	VERIFY(!engine->r01.scan_waiting);
	engine->r01.scan_waiting = B_TRUE;
	mutex_exit(&engine->lock);
	engine_resume(pending);
	return (engine_r01_wait_scan_root(engine, root));
}

/*
 * A final-free R-01 run may only reuse the pre-staged epoch.  Retain P while
 * Q is published and the first ordinary fence proves it, but never wait for
 * the parked root here: that would put natural repair before final-free.
 */
int
pr19262_engine_r01_epoch_wait(spa_t *spa, uint64_t parity)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return (0);
	engine_t *engine = spa->spa_reloc;

	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	engine_r01_prebind_wait_locked(engine);
	if (r->boundary == PR19262_R01_FINAL_FREE &&
	    r->state != ENGINE_R01_IDLE && r->state != ENGINE_R01_DRAINED &&
	    r->state != ENGINE_R01_CANCELLED && r->state != ENGINE_R01_PARKED) {
		mutex_exit(&engine->lock);
		return (EBUSY);
	}
	if (r->state != ENGINE_R01_PARKED ||
	    r->boundary != PR19262_R01_FINAL_FREE) {
		mutex_exit(&engine->lock);
		return (0);
	}
	if (r->owner == NULL || r->version == NULL || !r->copy_prepared ||
	    r->copy_parity != parity ||
	    r->copy_birth == 0 || r->epoch_waiting || r->observer_lease ||
	    r->copy_requested || r->copy_running || r->final_fence_running) {
		/* A copy run's final fence and a public epoch never overlap. */
		mutex_exit(&engine->lock);
		return (EBUSY);
	}
	VERIFY3U(r->version->leases, >, 0);
	r->version->leases++;
	r->observer_lease = B_TRUE;
	r->epoch_waiting = B_TRUE;
	cv_broadcast(&engine->changed);
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
	return (0);
}

/* The idempotent epoch result must be exactly the pre-staged identity. */
int
pr19262_engine_r01_epoch_check(spa_t *spa, uint64_t parity, uint64_t birth)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return (0);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	int error = 0;
	if (r->boundary == PR19262_R01_FINAL_FREE && r->epoch_waiting) {
		if (r->state == ENGINE_R01_CANCELLED)
			error = ECANCELED;
		else if (!r->observer_lease || !r->copy_prepared ||
		    r->copy_parity != parity || r->copy_birth != birth)
			error = ESTALE;
		else if (r->test_armed &&
		    r->test_mode == PR19262_R01_TEST_EPOCH_STALE) {
			r->test_birth = birth;
			r->test_ready = B_TRUE;
			cv_broadcast(&engine->changed);
			while (!r->test_fire &&
			    r->state != ENGINE_R01_CANCELLED)
				cv_wait(&engine->changed, &engine->lock);
			if (r->state == ENGINE_R01_CANCELLED) {
				r->test_error = ECANCELED;
				error = ECANCELED;
			} else {
				r->test_used = B_TRUE;
				r->test_error = ESTALE;
				error = ESTALE;
			}
		}
	}
	mutex_exit(&engine->lock);
	return (error);
}

/*
 * Hold an already admitted final-free worker immediately before its first
 * allocation or traversal.  The hook is ephemeral and shares the ordinary
 * R-01 cancellation CV, so export/unload never waits for a userspace fire.
 */
int
pr19262_engine_r01_convert_test_wait(spa_t *spa, uint64_t birth)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return (0);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	int error = 0;
	if (r->boundary == PR19262_R01_FINAL_FREE && r->epoch_waiting &&
	    r->test_armed && r->test_mode == PR19262_R01_TEST_CONVERT_EIO) {
		if (r->state == ENGINE_R01_CANCELLED) {
			r->test_error = ECANCELED;
			error = ECANCELED;
		} else {
			VERIFY(r->observer_lease);
			r->test_birth = birth;
			r->test_ready = B_TRUE;
			cv_broadcast(&engine->changed);
			while (!r->test_fire &&
			    r->state != ENGINE_R01_CANCELLED)
				cv_wait(&engine->changed, &engine->lock);
			if (r->state == ENGINE_R01_CANCELLED) {
				r->test_error = ECANCELED;
				error = ECANCELED;
			} else {
				r->test_used = B_TRUE;
				r->test_error = EIO;
				error = EIO;
			}
		}
	}
	mutex_exit(&engine->lock);
	return (error);
}

int
pr19262_engine_r01_test_arm(spa_t *spa, uint_t mode)
{
	if (spa == NULL || spa->spa_reloc == NULL ||
	    (mode != PR19262_R01_TEST_EPOCH_STALE &&
	    mode != PR19262_R01_TEST_CONVERT_EIO))
		return (EINVAL);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	if (r->state != ENGINE_R01_PARKED ||
	    r->boundary != PR19262_R01_FINAL_FREE || r->prebound == NULL ||
	    r->scan_root == NULL || engine->parked == NULL || !r->bound_lease ||
	    !r->copy_prepared || r->copy_parity != 3 || r->epoch_waiting ||
	    r->observer_lease || r->test_mode != PR19262_R01_TEST_NONE) {
		mutex_exit(&engine->lock);
		return (EBUSY);
	}
	r->test_mode = mode;
	r->test_armed = B_TRUE;
	r->test_ready = B_FALSE;
	r->test_fire = B_FALSE;
	r->test_used = B_FALSE;
	r->test_birth = 0;
	r->test_error = 0;
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
	return (0);
}

int
pr19262_engine_r01_test_fire(spa_t *spa)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return (ENOTSUP);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	if (!r->test_armed || r->test_mode == PR19262_R01_TEST_NONE ||
	    !r->test_ready || r->test_fire ||
	    r->state == ENGINE_R01_CANCELLED || !r->epoch_waiting ||
	    !r->observer_lease) {
		mutex_exit(&engine->lock);
		return (EBUSY);
	}
	r->test_fire = B_TRUE;
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
	return (0);
}

/*
 * The public worker cannot use generic cancel: that path waits terminal,
 * including epoch_waiting, and this worker clears epoch_waiting only on its
 * own exit.  Take ownership of any parked final-free root, clear this
 * worker's observer state, then wait only for the root it resumed.  This
 * also cleans a pre-admission EBUSY path, where epoch_waiting was never set.
 * External cancel/export and unload retain their generic terminal-wait
 * semantics.
 */
void
pr19262_engine_r01_epoch_abort(spa_t *spa)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return;
	engine_t *engine = spa->spa_reloc;
	engine_pending_t *pending = NULL;
	zio_t *root = NULL;
	boolean_t wait_root = B_FALSE;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	if (r->boundary != PR19262_R01_FINAL_FREE) {
		mutex_exit(&engine->lock);
		return;
	}
	/* The release caller already owns a RELEASED root waiter. */
	if (r->state == ENGINE_R01_PARKED) {
		VERIFY3P(engine->parked, !=, NULL);
		VERIFY3P(r->prebound, !=, NULL);
		VERIFY3P(r->scan_root, !=, NULL);
		pending = engine->parked;
		root = r->scan_root;
		engine->parked = NULL;
		engine_r01_bound_lease_rele_locked(engine);
		r->prebound = NULL;
		r->parent_done = B_TRUE;
		VERIFY(!r->scan_waiting);
		r->scan_waiting = B_TRUE;
		wait_root = B_TRUE;
	}
	if (!wait_root && !r->epoch_waiting) {
		mutex_exit(&engine->lock);
		return;
	}
	/* Release owns the root waiter, but not the stale bound lease. */
	if (!wait_root && r->state == ENGINE_R01_RELEASED &&
	    r->prebound != NULL) {
		engine_r01_bound_lease_rele_locked(engine);
		r->prebound = NULL;
		r->parent_done = B_TRUE;
	}
	r->state = ENGINE_R01_CANCELLED;
	r->boundary_advance = B_TRUE;
	engine_r01_observer_lease_rele_locked(engine);
	r->epoch_waiting = B_FALSE;
	if (!wait_root)
		engine_r01_try_drain_locked(engine);
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
	if (!wait_root)
		return;
	engine_resume(pending);
	(void) engine_r01_wait_scan_root(engine, root);
	mutex_enter(&engine->lock);
	engine_r01_try_drain_locked(engine);
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
	/* epoch_waiting was cleared before this wait. */
	/* Therefore the public worker cannot wait on itself. */
	(void) engine_r01_wait_terminal(engine, 0);
}

void
pr19262_engine_r01_epoch_finish(spa_t *spa)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return;
	engine_t *engine = spa->spa_reloc;

	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	if (r->epoch_waiting) {
		engine_r01_observer_lease_rele_locked(engine);
		r->epoch_waiting = B_FALSE;
		engine_r01_try_drain_locked(engine);
		cv_broadcast(&engine->changed);
	}
	mutex_exit(&engine->lock);
}

int
pr19262_engine_r01_advance(spa_t *spa)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return (ENOTSUP);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (!engine->r01.boundary_waiting) {
		mutex_exit(&engine->lock);
		return (EINVAL);
	}
	/*
	 * A public final-free worker advanced past its boundary would wait
	 * for scan_done while release refuses the still-PARKED root (release
	 * requires boundary_waiting): release first, then advance.
	 */
	if (engine->r01.boundary_reached == PR19262_R01_FINAL_FREE &&
	    engine->r01.epoch_waiting &&
	    engine->r01.state == ENGINE_R01_PARKED) {
		mutex_exit(&engine->lock);
		return (EBUSY);
	}
	engine->r01.boundary_advance = B_TRUE;
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
	return (0);
}

/*
 * This test control copies only the owner pinned by the real selected read.
 * It does not create a repair ZIO or alter a DTL; release still lets the
 * native RAIDZ path decide and create the repair child.
 */
int
pr19262_engine_r01_prepare(spa_t *spa, uint64_t parity)
{
	if (spa == NULL || spa->spa_reloc == NULL || parity < 1 ||
	    parity > VDEV_RAIDZ_MAXPARITY)
		return (EINVAL);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->r01.state != ENGINE_R01_IDLE ||
	    engine->r01.copy_prepared) {
		mutex_exit(&engine->lock);
		return (EBUSY);
	}
	mutex_exit(&engine->lock);

	uint64_t birth;
	int error = pr19262_engine_epoch(spa, parity, &birth);
	if (error != 0)
		return (error);

	mutex_enter(&engine->lock);
	if (engine->r01.state != ENGINE_R01_IDLE ||
	    engine->r01.copy_prepared) {
		mutex_exit(&engine->lock);
		return (EBUSY);
	}
	/* Preparing an epoch precedes selection. */
	/* It creates neither a repair nor a DTL. */
	engine->r01.copy_prepared = B_TRUE;
	engine->r01.copy_birth = birth;
	engine->r01.copy_parity = parity;
	mutex_exit(&engine->lock);
	return (0);
}

int
pr19262_engine_r01_copy(spa_t *spa)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return (ENOTSUP);
	engine_t *engine = spa->spa_reloc;
	engine_r01_copy_job_t *job = kmem_zalloc(sizeof (*job), KM_SLEEP);
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	if (r->state != ENGINE_R01_PARKED || r->owner == NULL ||
	    r->version == NULL || r->copy_dva < 0 || !r->copy_prepared ||
	    r->copy_running || r->copy_requested) {
		mutex_exit(&engine->lock);
		kmem_free(job, sizeof (*job));
		return (EINVAL);
	}
	job->spa = spa;
	job->bp = r->copy_bp;
	job->dva = r->copy_dva;
	job->birth = r->copy_birth;
	job->parity = r->copy_parity;
	r->copy_requested = B_TRUE;
	r->copy_running = B_TRUE;
	r->copy_done = B_FALSE;
	r->copy_error = 0;
	mutex_exit(&engine->lock);
	(void) thread_create(NULL, 0, engine_r01_copy_worker, job, 0, &p0,
	    TS_RUN, minclsyspri);
	return (0);
}

/* Entered with engine->lock held; every path drops it. */
static int
engine_r01_cancel_enter(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_r01_prebind_wait_locked(engine);
	if (engine->r01.state == ENGINE_R01_ARMED) {
		while (engine->r01.scan_submitting)
			cv_wait(&engine->changed, &engine->lock);
	}
	if (engine->r01.state == ENGINE_R01_ARMED) {
		zio_t *root = engine->r01.scan_root;
		engine->park_next = B_FALSE;
		engine_r01_bound_lease_rele_locked(engine);
		engine->r01.state = ENGINE_R01_CANCELLED;
		engine->r01.drained = B_TRUE;
		engine->r01.boundary_advance = B_TRUE;
		if (root != NULL) {
			VERIFY(!engine->r01.scan_waiting);
			engine->r01.scan_waiting = B_TRUE;
		}
		cv_broadcast(&engine->changed);
		mutex_exit(&engine->lock);
		return (engine_r01_wait_terminal(engine, root == NULL ? 0 :
		    engine_r01_wait_scan_root(engine, root)));
	}
	if (engine->r01.state == ENGINE_R01_RELEASED ||
	    engine->r01.state == ENGINE_R01_DRAINED ||
	    engine->r01.state == ENGINE_R01_CANCELLED) {
		engine_r01_observer_lease_rele_locked(engine);
		if (engine->r01.prebound != NULL) {
			engine_r01_bound_lease_rele_locked(engine);
			engine->r01.prebound = NULL;
			engine->r01.parent_done = B_TRUE;
		}
		engine->r01.state = ENGINE_R01_CANCELLED;
		engine->r01.boundary_advance = B_TRUE;
		engine_r01_try_drain_locked(engine);
		cv_broadcast(&engine->changed);
		if (engine->r01.scan_root == NULL) {
			while (engine->r01.copy_running ||
			    engine->r01.boundary_waiting ||
			    engine->r01.final_fence_running)
				cv_wait(&engine->changed, &engine->lock);
			mutex_exit(&engine->lock);
			return (0);
		}
		if (engine->r01.scan_waiting) {
			while (engine->r01.scan_root != NULL ||
			    engine->r01.copy_running ||
			    engine->r01.boundary_waiting ||
			    engine->r01.final_fence_running)
				cv_wait(&engine->changed, &engine->lock);
			int error = engine->r01.scan_root_error;
			mutex_exit(&engine->lock);
			return (error);
		}
		zio_t *root = engine->r01.scan_root;
		engine->r01.scan_waiting = B_TRUE;
		mutex_exit(&engine->lock);
		int error = engine_r01_wait_scan_root(engine, root);
		mutex_enter(&engine->lock);
		while (engine->r01.copy_running ||
		    engine->r01.boundary_waiting ||
		    engine->r01.final_fence_running)
			cv_wait(&engine->changed, &engine->lock);
		mutex_exit(&engine->lock);
		return (error);
	}
	if (engine->r01.state != ENGINE_R01_PARKED || engine->parked == NULL ||
	    engine->r01.prebound == NULL || engine->r01.scan_root == NULL) {
		mutex_exit(&engine->lock);
		return (EINVAL);
	}
	engine_pending_t *pending = engine->parked;
	zio_t *root = engine->r01.scan_root;
	engine->parked = NULL;
	engine_r01_bound_lease_rele_locked(engine);
	engine_r01_observer_lease_rele_locked(engine);
	engine->r01.prebound = NULL;
	engine->r01.parent_done = B_TRUE;
	engine->r01.state = ENGINE_R01_CANCELLED;
	engine->r01.boundary_advance = B_TRUE;
	VERIFY(!engine->r01.scan_waiting);
	engine->r01.scan_waiting = B_TRUE;
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
	engine_resume(pending);
	return (engine_r01_wait_terminal(engine,
	    engine_r01_wait_scan_root(engine, root)));
}

int
pr19262_engine_r01_cancel(spa_t *spa)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return (ENOTSUP);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	return (engine_r01_cancel_enter(engine));
}

/*
 * Would a scan reset in syncing context wait here for work that itself needs
 * a txg?  A released or cancelled root still in flight, or a parked R-01
 * whose public worker or test copy has not reached its boundary wait.
 */
static boolean_t
engine_r01_scan_reset_waits_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_r01_t *r = &engine->r01;
	switch (r->state) {
	case ENGINE_R01_RELEASED:
	case ENGINE_R01_DRAINED:
	case ENGINE_R01_CANCELLED:
		return (r->scan_root != NULL);
	case ENGINE_R01_PARKED:
		return (!r->boundary_waiting &&
		    (r->epoch_waiting || r->copy_running));
	default:
		return (B_FALSE);
	}
}

/*
 * A scan reset (dsl_scan_sync() restart, dsl_scan_cancel_sync()) runs in
 * syncing context.  Before release, the parked natural root still needs the
 * scan state, so R-01 is cancelled as before.  From release on, R-01 needs
 * nothing from the scan once its root has completed: only its final fence
 * remains, which a reset must neither cancel (P would then never be
 * reclaimed by the final fence) nor wait for (that fence waits for a txg
 * this sync thread has to finish).  The decision and the cancel are taken
 * under one hold of engine->lock, so a concurrent release cannot turn a
 * parked cancel into a released one.
 *
 * can_defer (restart only): return B_FALSE, so that the caller postpones
 * the reset to a later txg, instead of waiting here for work that itself
 * needs a txg: a released or cancelled root still in flight, or a parked
 * R-01 whose public worker or test copy has not reached its boundary wait
 * (cancel would wait for it to leave, and it is waiting for a txg).
 * Without can_defer the reset proceeds as before in those cases.
 */
static boolean_t
engine_r01_scan_reset(spa_t *spa, boolean_t can_defer)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return (B_TRUE);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r01_prebind_wait_locked(engine);
	engine_r01_t *r = &engine->r01;
	boolean_t defer = engine_r01_scan_reset_waits_locked(engine);
	switch (r->state) {
	case ENGINE_R01_IDLE:
		mutex_exit(&engine->lock);
		return (B_TRUE);
	case ENGINE_R01_RELEASED:
	case ENGINE_R01_DRAINED:
	case ENGINE_R01_CANCELLED:
		if (r->scan_root == NULL) {
			r->restart_kept++;
			mutex_exit(&engine->lock);
			return (B_TRUE);
		}
		break;
	default:
		break;
	}
	if (defer && can_defer) {
		r->restart_deferred++;
		mutex_exit(&engine->lock);
		return (B_FALSE);
	}
	/* Avoid reset while a detached callback uses scan state. */
	(void) engine_r01_cancel_enter(engine);
	return (B_TRUE);
}

boolean_t
pr19262_engine_r01_scan_restart(spa_t *spa)
{
	return (engine_r01_scan_reset(spa, B_TRUE));
}

void
pr19262_engine_r01_scan_reset_cancel(spa_t *spa)
{
	(void) engine_r01_scan_reset(spa, B_FALSE);
}

/*
 * dsl_scan_cancel_check(): a scan cancel cannot be postponed like a restart,
 * so refuse it (EBUSY) while its reset would wait in syncing context for
 * work that needs a txg.  Not reachable through spa_scan_stop() today: R-01
 * parks only resilver I/O, a parked or in-flight root keeps that resilver
 * running (dsl_scan_sync()), and a running resilver is refused before
 * dsl_scan_cancel().  This narrows, but does not close, the window: the
 * state can still change between this check and dsl_scan_cancel_sync().
 */
boolean_t
pr19262_engine_r01_scan_reset_busy(spa_t *spa)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return (B_FALSE);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	boolean_t busy = engine_r01_scan_reset_waits_locked(engine);
	if (busy)
		engine->r01.stop_busy++;
	mutex_exit(&engine->lock);
	return (busy);
}

/*
 * Reserve the private scan-cancel operation without holding engine->lock
 * while its DSL sync task waits for a TXG.  It is deliberately unavailable
 * while any R01 ZIO, test copy, or ordinary parking state exists.
 */
int
pr19262_engine_r01_scan_cancel_begin(spa_t *spa)
{
	if (spa == NULL || spa->spa_reloc == NULL || !spa_writeable(spa))
		return (ENOTSUP);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	if (engine->spa != spa || !engine->persistent ||
	    engine->vd == NULL) {
		mutex_exit(&engine->lock);
		return (ENOTSUP);
	}
	if (r->scan_cancel_inflight || r->scan_root != NULL ||
	    r->prebound != NULL || r->selected != NULL ||
	    r->owner != NULL || r->version != NULL || engine->parked != NULL ||
	    engine->park_next || r->copy_requested || r->copy_running ||
	    r->boundary_waiting || (r->state != ENGINE_R01_IDLE &&
	    r->state != ENGINE_R01_DRAINED &&
	    r->state != ENGINE_R01_CANCELLED)) {
		mutex_exit(&engine->lock);
		return (EBUSY);
	}
	r->scan_cancel_inflight = B_TRUE;
	r->scan_cancel_gen++;
	r->scan_cancel_error = EINPROGRESS;
	mutex_exit(&engine->lock);
	return (0);
}

void
pr19262_engine_r01_scan_cancel_finish(spa_t *spa, int error)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return;
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->spa == spa && engine->r01.scan_cancel_inflight) {
		engine->r01.scan_cancel_inflight = B_FALSE;
		engine->r01.scan_cancel_error = error;
		cv_broadcast(&engine->changed);
	}
	mutex_exit(&engine->lock);
}

int
pr19262_engine_r01_final_free_wait(spa_t *spa)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return (0);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	/* P may already have been unlinked by the one final second fence. */
	if (r->boundary == PR19262_R01_FINAL_FREE && r->epoch_waiting &&
	    r->final_fence_done) {
		mutex_exit(&engine->lock);
		return (0);
	}
	boolean_t wait = r->owner != NULL && r->version != NULL &&
	    r->owner->cur != r->version && r->version->retired &&
	    r->version->leases != 0;
	engine_owner_t *owner = wait ? r->owner : NULL;
	mutex_exit(&engine->lock);
	if (owner != NULL)
		engine_r01_boundary_wait(engine, owner, PR19262_R01_FINAL_FREE);

	mutex_enter(&engine->lock);
	r = &engine->r01;
	if (r->boundary != PR19262_R01_FINAL_FREE || !r->boundary_ready) {
		mutex_exit(&engine->lock);
		return (0);
	}
	if (r->state == ENGINE_R01_CANCELLED) {
		mutex_exit(&engine->lock);
		return (ECANCELED);
	}
	/* Public conversion keeps observer lease through its first fence. */
	if (!r->epoch_waiting && r->copy_requested) {
		r->final_fence_running = B_TRUE;
		/* The copy worker calling us issues the final fence next. */
		r->final_fence_thread = curthread;
		engine_r01_observer_lease_rele_locked(engine);
		while (!r->scan_done && r->state != ENGINE_R01_CANCELLED)
			cv_wait(&engine->changed, &engine->lock);
		if (r->state == ENGINE_R01_CANCELLED) {
			r->final_fence_running = B_FALSE;
			r->final_fence_thread = NULL;
			engine_r01_try_drain_locked(engine);
			cv_broadcast(&engine->changed);
			mutex_exit(&engine->lock);
			return (ECANCELED);
		}
	}
	mutex_exit(&engine->lock);
	return (0);
}

/*
 * After the first ordinary fence retained P under the observer lease, the
 * final-free boundary is released by r01 release/advance.  The release path
 * owns zio_wait(root); this helper observes scan_done, drops only the
 * observer lease, then executes exactly one normal second fence.
 */
int
pr19262_engine_r01_final_free_second_fence(spa_t *spa, uint64_t *floor,
    uint64_t *reclaimed, uint64_t *released, uint64_t *sample,
    boolean_t *did_fence)
{
	if (did_fence != NULL)
		*did_fence = B_FALSE;
	if (spa == NULL || spa->spa_reloc == NULL)
		return (0);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	if (r->boundary != PR19262_R01_FINAL_FREE || !r->boundary_ready ||
	    !r->epoch_waiting || r->final_fence_done) {
		mutex_exit(&engine->lock);
		return (0);
	}
	if (r->state == ENGINE_R01_CANCELLED) {
		mutex_exit(&engine->lock);
		return (ECANCELED);
	}
	/*
	 * A copy worker that claimed the final fence while the epoch was not
	 * yet waiting owns it: wait for that fence instead of running another.
	 */
	while (r->final_fence_running && r->state != ENGINE_R01_CANCELLED)
		cv_wait(&engine->changed, &engine->lock);
	if (r->state != ENGINE_R01_CANCELLED && r->final_fence_done) {
		mutex_exit(&engine->lock);
		return (0);
	}
	if (r->state == ENGINE_R01_CANCELLED) {
		mutex_exit(&engine->lock);
		return (ECANCELED);
	}
	r->final_fence_running = B_TRUE;
	while (!r->scan_done && r->state != ENGINE_R01_CANCELLED)
		cv_wait(&engine->changed, &engine->lock);
	if (r->state == ENGINE_R01_CANCELLED) {
		r->final_fence_running = B_FALSE;
		engine_r01_try_drain_locked(engine);
		cv_broadcast(&engine->changed);
		mutex_exit(&engine->lock);
		return (ECANCELED);
	}
	if (r->scan_root_error != 0) {
		int error = r->scan_root_error;
		r->final_fence_running = B_FALSE;
		r->final_fence_done = B_TRUE;
		r->final_fence_error = error;
		engine_r01_observer_lease_rele_locked(engine);
		engine_r01_try_drain_locked(engine);
		cv_broadcast(&engine->changed);
		mutex_exit(&engine->lock);
		return (error);
	}
	VERIFY(r->observer_lease);
	engine_r01_observer_lease_rele_locked(engine);
	/* P stays excluded from every other fence until this one runs. */
	VERIFY3P(r->final_fence_thread, ==, NULL);
	r->final_fence_thread = curthread;
	if (did_fence != NULL)
		*did_fence = B_TRUE;
	mutex_exit(&engine->lock);

	int error = pr19262_engine_fence(spa, floor, reclaimed, released,
	    sample);

	mutex_enter(&engine->lock);
	r = &engine->r01;
	r->final_fence_running = B_FALSE;
	r->final_fence_thread = NULL;
	if (error != EBUSY) {
		r->final_fence_done = B_TRUE;
		r->final_fence_error = error;
		r->final_reclaimed = error == 0 ? *reclaimed : 0;
	}
	engine_r01_try_drain_locked(engine);
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
	return (error);
}

/*
 * The final-free gate releases and waits for the detached natural root
 * before the first fence.  That fence is the only operation that may unlink
 * P; this observer records its exact result without issuing another fence.
 */
void
pr19262_engine_r01_final_free_finish(spa_t *spa, int error,
    uint64_t reclaimed)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return;
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	/* Only the thread that claimed the final fence records its result. */
	if (!r->final_fence_running || r->final_fence_thread != curthread) {
		mutex_exit(&engine->lock);
		return;
	}
	r->final_fence_running = B_FALSE;
	r->final_fence_thread = NULL;
	if (error != EBUSY) {
		r->final_fence_done = B_TRUE;
		r->final_fence_error = error;
		r->final_reclaimed = reclaimed;
	}
	engine_r01_try_drain_locked(engine);
	cv_broadcast(&engine->changed);
	mutex_exit(&engine->lock);
}

boolean_t
pr19262_engine_r01_terminal(spa_t *spa)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return (B_FALSE);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	boolean_t terminal = engine->r01.state == ENGINE_R01_DRAINED ||
	    (engine->r01.state == ENGINE_R01_CANCELLED && engine->r01.drained);

	terminal = terminal && engine->r01.scan_root == NULL &&
	    !engine->r01.scan_waiting && engine->r01.prebound == NULL &&
	    engine->r01.selected == NULL && !engine->r01.bound_lease;
	mutex_exit(&engine->lock);
	return (terminal);
}

boolean_t
pr19262_engine_r01_watch(zio_t *zio, uint64_t *birth, uint64_t *txg)
{
	if (zio == NULL || zio->io_spa == NULL ||
	    zio->io_spa->spa_reloc == NULL)
		return (B_FALSE);
	engine_t *engine = zio->io_spa->spa_reloc;
	mutex_enter(&engine->lock);
	boolean_t match = engine_r01_match_locked(engine, zio);
	if (match) {
		*birth = engine->r01.b;
		*txg = engine->r01.t;
	}
	mutex_exit(&engine->lock);
	return (match);
}

void
pr19262_engine_r01_repair_created(zio_t *zio, vdev_t *leaf,
    uint64_t offset, uint64_t size, uint64_t column, zio_flag_t flags)
{
	if (zio == NULL || leaf == NULL || zio->io_spa == NULL ||
	    zio->io_spa->spa_reloc == NULL)
		return;
	engine_t *engine = zio->io_spa->spa_reloc;
	uint64_t p = 0, b = 0, t = 0, generation = 0;
	engine_owner_t *owner = NULL;
	mutex_enter(&engine->lock);
	if (engine_r01_match_locked(engine, zio)) {
		p = engine->r01.p;
		b = engine->r01.b;
		t = engine->r01.t;
		generation = engine->r01.generation;
		owner = engine->r01.owner;
	}
	mutex_exit(&engine->lock);
	if (p == 0)
		return;
	/*
	 * Sample the same leaf and P that RAIDZ has just selected for a
	 * natural repair.  Do this outside the engine lock: DTL inspection may
	 * take vdev locks, while the observer lock is private to the engine.
	 */
	boolean_t dtl_p = vdev_dtl_contains(leaf, DTL_PARTIAL, p, 1);
	boolean_t dtl_b = b == p ? dtl_p :
	    vdev_dtl_contains(leaf, DTL_PARTIAL, b, 1);
	boolean_t dtl_t = t == p ? dtl_p :
	    vdev_dtl_contains(leaf, DTL_PARTIAL, t, 1);
	boolean_t dtl_target = dtl_p || dtl_b || dtl_t;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	if (r->generation == generation && r->owner == owner &&
	    (r->state == ENGINE_R01_PARKED ||
	    r->state == ENGINE_R01_RELEASED)) {
		r->repair_created++;
		r->repair_boundary = engine_r01_boundary_locked(engine);
		r->repair_boundary_valid = B_TRUE;
		/*
		 * Record the first DTL leaf only.
		 */
		if (dtl_target && !r->dtl_target_seen) {
			r->dtl_target_seen = B_TRUE;
			r->dtl_target_guid = leaf->vdev_guid;
			r->dtl_target_id = leaf->vdev_id;
			r->dtl_target_offset = offset;
			r->dtl_target_size = size;
			r->dtl_target_column = column;
			r->dtl_target_self_heal =
			    (flags & ZIO_FLAG_SELF_HEAL) != 0;
			r->dtl_target_scrub =
			    (zio->io_flags & ZIO_FLAG_SCRUB) != 0;
			r->dtl_target_resilver =
			    (zio->io_flags & ZIO_FLAG_RESILVER) != 0;
			r->dtl_target_p = dtl_p;
			r->dtl_target_b = dtl_b;
			r->dtl_target_t = dtl_t;
		}
		if (r->repair_created == 1) {
			r->leaf_guid = leaf->vdev_guid;
			r->leaf_id = leaf->vdev_id;
			r->leaf_offset = offset;
			r->leaf_size = size;
			r->column = column;
			r->self_heal = (flags & ZIO_FLAG_SELF_HEAL) != 0;
			r->scrub = (zio->io_flags & ZIO_FLAG_SCRUB) != 0;
			r->resilver = (zio->io_flags & ZIO_FLAG_RESILVER) != 0;
			r->dtl_p = dtl_p;
			r->dtl_b = dtl_b;
			r->dtl_t = dtl_t;
		} else if (r->repair_created == 2) {
			r->second_leaf_guid = leaf->vdev_guid;
			r->second_leaf_id = leaf->vdev_id;
			r->second_leaf_offset = offset;
			r->second_leaf_size = size;
			r->second_column = column;
			r->second_self_heal =
			    (flags & ZIO_FLAG_SELF_HEAL) != 0;
			r->second_scrub = (zio->io_flags & ZIO_FLAG_SCRUB) != 0;
			r->second_resilver =
			    (zio->io_flags & ZIO_FLAG_RESILVER) != 0;
			r->second_dtl_p = dtl_p;
			r->second_dtl_b = dtl_b;
			r->second_dtl_t = dtl_t;
		}
	}
	mutex_exit(&engine->lock);
}

void
pr19262_engine_r01_repair_decision(zio_t *zio, vdev_t *leaf,
    boolean_t dtl_p, boolean_t dtl_b, boolean_t dtl_t, boolean_t bypassed)
{
	if (zio == NULL || leaf == NULL || zio->io_spa == NULL ||
	    zio->io_spa->spa_reloc == NULL)
		return;
	engine_t *engine = zio->io_spa->spa_reloc;
	uint64_t leaf_offset = zio->io_offset;

	/*
	 * RAIDZ creates the repair child with rc_offset in allocatable-vdev
	 * coordinates.  zio_vdev_child_io() adds the leading label size before
	 * this leaf callback runs, so compare the observer identity in the same
	 * coordinate domain as pr19262_engine_r01_repair_created().
	 */
	if (leaf->vdev_ops->vdev_op_leaf) {
		if (leaf_offset < VDEV_LABEL_START_SIZE)
			return;
		leaf_offset -= VDEV_LABEL_START_SIZE;
	}

	mutex_enter(&engine->lock);
	if (engine_r01_match_locked(engine, zio)) {
		engine_r01_t *r = &engine->r01;
		if (r->repair_created != 0 &&
		    r->leaf_guid == leaf->vdev_guid &&
		    r->leaf_id == leaf->vdev_id &&
		    r->leaf_offset == leaf_offset &&
		    r->leaf_size == zio->io_size) {
			r->dtl_p = dtl_p;
			r->dtl_b = dtl_b;
			r->dtl_t = dtl_t;
			if (bypassed)
				r->repair_bypassed++;
			else
				r->repair_forwarded++;
		} else if (r->repair_created >= 2 &&
		    r->second_leaf_guid == leaf->vdev_guid &&
		    r->second_leaf_id == leaf->vdev_id &&
		    r->second_leaf_offset == leaf_offset &&
		    r->second_leaf_size == zio->io_size) {
			r->second_dtl_p = dtl_p;
			r->second_dtl_b = dtl_b;
			r->second_dtl_t = dtl_t;
			if (bypassed)
				r->second_repair_bypassed++;
			else
				r->second_repair_forwarded++;
		}
		if (r->dtl_target_seen &&
		    r->dtl_target_guid == leaf->vdev_guid &&
		    r->dtl_target_id == leaf->vdev_id &&
		    r->dtl_target_offset == leaf_offset &&
		    r->dtl_target_size == zio->io_size) {
			if (bypassed)
				r->dtl_target_bypassed++;
			else
				r->dtl_target_forwarded++;
		}
	}
	mutex_exit(&engine->lock);
}

int
pr19262_engine_r01_status(spa_t *spa, char *buf, size_t size)
{
	if (spa == NULL || spa->spa_reloc == NULL || size == 0)
		return (-ENOTSUP);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r01_t *r = &engine->r01;
	int n = snprintf(buf, size, "state=%s pool=%s generation=%llu "
	    "scan_cancel_inflight=%u scan_cancel_gen=%llu scan_cancel_error=%d "
	    "async_routed=%u async_root=%u async_done=%u async_waits=%llu "
	    "async_error=%d prebind=%u prebind_no_scl=%u "
	    "prebind_attempts=%llu prebind_bad_shape=%llu prebind_scl=%llu "
	    "prebind_unheld=%llu prebind_nomatch=%llu prebound=%u "
	    "selected=%u "
	    "final_fence_done=%u final_fence_error=%d final_reclaimed=%llu "
	    "final_P_reclaims=%llu final_P_bytes=%llu final_P_birth=%llu "
	    "final_P_offset=%llu final_P_size=%llu final_P_V=%llu "
	    "final_P_incarnation=%llu "
	    "boundary=%s boundary_reached=%s repair_boundary=%s "
	    "boundary_waiting=%u boundary_advance=%u epoch_waiting=%u "
	    "test_mode=%s test_armed=%u test_ready=%u test_fire=%u "
	    "test_used=%u test_birth=%llu test_error=%d "
	    "observer_lease=%u copy_requested=%u "
	    "copy_prepared=%u "
	    "copy_running=%u copy_done=%u copy_error=%d copy_birth=%llu "
	    "copy_parity=%llu copy_calls=%llu copy_owner_match=%u "
	    "copy_owner_V=%llu copy_owner_incarnation=%llu "
	    "copy_owner_state=%s B=%llu "
	    "P=%llu T=%llu V=%llu incarnation=%llu p_offset=%llu p_size=%llu "
	    "width=%llu parity=%llu leaf_guid=%llu leaf_id=%llu "
	    "leaf_offset=%llu leaf_size=%llu column=%llu created=%llu "
	    "second_leaf_guid=%llu second_leaf_id=%llu second_leaf_offset=%llu "
	    "second_leaf_size=%llu second_column=%llu forwarded=%llu "
	    "bypassed=%llu second_forwarded=%llu second_bypassed=%llu "
	    "dtl_target_seen=%u dtl_target_guid=%llu dtl_target_id=%llu "
	    "dtl_target_offset=%llu dtl_target_size=%llu "
	    "dtl_target_column=%llu "
	    "dtl_target_forwarded=%llu dtl_target_bypassed=%llu "
	    "dtl_target_P=%u dtl_target_B=%u dtl_target_T=%u "
	    "dtl_target_selfheal=%u dtl_target_scrub=%u "
	    "dtl_target_resilver=%u "
	    "dtl_P=%u dtl_B=%u dtl_T=%u second_dtl_P=%u second_dtl_B=%u "
	    "second_dtl_T=%u selfheal=%u scrub=%u resilver=%u "
	    "second_selfheal=%u second_scrub=%u second_resilver=%u "
	    "tuple_distinct=%u "
	    "seen_top=%llu seen_no_owner=%llu seen_mapped=%llu "
	    "seen_level0=%llu seen_data=%llu seen_B=%llu seen_P=%llu "
	    "lease_parked=%llu holds_parked=%llu writers_parked=%llu "
	    "lease_terminal=%llu holds_terminal=%llu writers_terminal=%llu "
	    "parent_error=%d parent_done=%u drained=%u "
	    "restart_kept=%llu restart_deferred=%llu stop_busy=%llu\n",
	    engine_r01_names[r->state], spa_name(spa),
	    (u_longlong_t)r->generation, r->scan_cancel_inflight,
	    (u_longlong_t)r->scan_cancel_gen, r->scan_cancel_error,
	    r->scan_routed, r->scan_root != NULL, r->scan_done,
	    (u_longlong_t)r->scan_root_waits, r->scan_root_error,
	    r->prebind, r->prebind_no_scl,
	    (u_longlong_t)r->prebind_attempts,
	    (u_longlong_t)r->prebind_bad_shape,
	    (u_longlong_t)r->prebind_scl,
	    (u_longlong_t)r->prebind_unheld,
	    (u_longlong_t)r->prebind_nomatch, r->prebound != NULL,
	    r->selected != NULL,
	    r->final_fence_done, r->final_fence_error,
	    (u_longlong_t)r->final_reclaimed,
	    (u_longlong_t)r->final_p_reclaims,
	    (u_longlong_t)r->final_p_bytes,
	    (u_longlong_t)r->final_p_birth,
	    (u_longlong_t)r->final_p_offset,
	    (u_longlong_t)r->final_p_size,
	    (u_longlong_t)r->final_p_v,
	    (u_longlong_t)r->final_p_incarnation,
	    engine_r01_boundary_name(r->boundary),
	    r->boundary_ready ? engine_r01_boundary_name(r->boundary_reached) :
	    "none", r->repair_boundary_valid ?
	    engine_r01_boundary_name(r->repair_boundary) : "none",
	    r->boundary_waiting, r->boundary_advance, r->epoch_waiting,
	    engine_r01_test_name(r->test_mode), r->test_armed, r->test_ready,
	    r->test_fire, r->test_used, (u_longlong_t)r->test_birth,
	    r->test_error,
	    r->observer_lease, r->copy_requested,
	    r->copy_prepared,
	    r->copy_running, r->copy_done, r->copy_error,
	    (u_longlong_t)r->copy_birth, (u_longlong_t)r->copy_parity,
	    (u_longlong_t)r->copy_boundary_calls, r->copy_owner_match,
	    (u_longlong_t)r->copy_boundary_v,
	    (u_longlong_t)r->copy_boundary_incarnation,
	    engine_r01_names[r->copy_boundary_state],
	    (u_longlong_t)r->b,
	    (u_longlong_t)r->p, (u_longlong_t)r->t,
	    (u_longlong_t)r->v, (u_longlong_t)r->incarnation,
	    (u_longlong_t)r->p_offset, (u_longlong_t)r->p_size,
	    (u_longlong_t)r->width, (u_longlong_t)r->parity,
	    (u_longlong_t)r->leaf_guid, (u_longlong_t)r->leaf_id,
	    (u_longlong_t)r->leaf_offset, (u_longlong_t)r->leaf_size,
	    (u_longlong_t)r->column, (u_longlong_t)r->repair_created,
	    (u_longlong_t)r->second_leaf_guid,
	    (u_longlong_t)r->second_leaf_id,
	    (u_longlong_t)r->second_leaf_offset,
	    (u_longlong_t)r->second_leaf_size,
	    (u_longlong_t)r->second_column,
	    (u_longlong_t)r->repair_forwarded,
	    (u_longlong_t)r->repair_bypassed,
	    (u_longlong_t)r->second_repair_forwarded,
	    (u_longlong_t)r->second_repair_bypassed,
	    r->dtl_target_seen, (u_longlong_t)r->dtl_target_guid,
	    (u_longlong_t)r->dtl_target_id,
	    (u_longlong_t)r->dtl_target_offset,
	    (u_longlong_t)r->dtl_target_size,
	    (u_longlong_t)r->dtl_target_column,
	    (u_longlong_t)r->dtl_target_forwarded,
	    (u_longlong_t)r->dtl_target_bypassed,
	    r->dtl_target_p, r->dtl_target_b, r->dtl_target_t,
	    r->dtl_target_self_heal, r->dtl_target_scrub,
	    r->dtl_target_resilver,
	    r->dtl_p, r->dtl_b, r->dtl_t,
	    r->second_dtl_p, r->second_dtl_b, r->second_dtl_t,
	    r->self_heal, r->scrub, r->resilver,
	    r->second_self_heal, r->second_scrub, r->second_resilver,
	    r->tuple_distinct, (u_longlong_t)r->seen_top,
	    (u_longlong_t)r->seen_no_owner, (u_longlong_t)r->seen_mapped,
	    (u_longlong_t)r->seen_level0, (u_longlong_t)r->seen_data,
	    (u_longlong_t)r->seen_b, (u_longlong_t)r->seen_p,
	    (u_longlong_t)r->lease_parked,
	    (u_longlong_t)r->holds_parked, (u_longlong_t)r->writers_parked,
	    (u_longlong_t)r->lease_terminal,
	    (u_longlong_t)r->holds_terminal,
	    (u_longlong_t)r->writers_terminal, r->parent_error, r->parent_done,
	    r->drained, (u_longlong_t)r->restart_kept,
	    (u_longlong_t)r->restart_deferred, (u_longlong_t)r->stop_busy);
	mutex_exit(&engine->lock);
	return (n < 0 || (size_t)n >= size ? -ENOSPC : n);
}

static void
engine_r02_reset_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t generation = engine->r02.generation + 1;
	memset(&engine->r02, 0, sizeof (engine->r02));
	engine->r02.generation = generation;
	engine->r02.armed = B_TRUE;
}

int
pr19262_engine_r02_arm(spa_t *spa)
{
	if (spa == NULL || spa->spa_reloc == NULL)
		return (ENOTSUP);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->spa != spa || !engine->persistent || engine->vd == NULL) {
		mutex_exit(&engine->lock);
		return (ENOTSUP);
	}
	engine_r02_reset_locked(engine);
	mutex_exit(&engine->lock);
	return (0);
}

void
pr19262_engine_r02_backing(spa_t *spa, const blkptr_t *bp, int d,
    const dva_t *dva, uint64_t birth, boolean_t mapped)
{
	if (spa == NULL || spa->spa_reloc == NULL || bp == NULL ||
	    dva == NULL || d < 0 || d >= BP_GET_NDVAS(bp))
		return;
	const dva_t *original = &bp->blk_dva[d];
	uint64_t original_birth = BP_GET_PHYSICAL_BIRTH(bp);
	if (!mapped || original_birth == 0 ||
	    birth == 0 || (DVA_GET_VDEV(original) == DVA_GET_VDEV(dva) &&
	    DVA_GET_OFFSET(original) == DVA_GET_OFFSET(dva) &&
	    DVA_GET_ASIZE(original) == DVA_GET_ASIZE(dva) &&
	    original_birth == birth))
		return;
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r02_t *r = &engine->r02;
	if (r->armed) {
		if (BP_GET_LEVEL(bp) != 0) {
			r->indirect_seen++;
		} else if (!DMU_OT_IS_METADATA(BP_GET_TYPE(bp))) {
			r->backing_calls++;
			if (!r->mapped) {
				r->mapped = B_TRUE;
				r->b = original_birth;
				r->p = birth;
				r->original_vdev = DVA_GET_VDEV(original);
				r->original_offset = DVA_GET_OFFSET(original);
				r->original_size = DVA_GET_ASIZE(original);
				r->backing_vdev = DVA_GET_VDEV(dva);
				r->backing_offset = DVA_GET_OFFSET(dva);
				r->backing_size = DVA_GET_ASIZE(dva);
			}
		}
	}
	mutex_exit(&engine->lock);
}

static boolean_t
engine_r02_match_locked(const engine_r02_t *r, const blkptr_t *bp)
{
	ASSERT(r->mapped);
	if (BP_GET_PHYSICAL_BIRTH(bp) != r->b)
		return (B_FALSE);
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		if (DVA_GET_VDEV(dva) == r->original_vdev &&
		    DVA_GET_OFFSET(dva) == r->original_offset &&
		    DVA_GET_ASIZE(dva) == r->original_size)
			return (B_TRUE);
	}
	return (B_FALSE);
}

/* Record a selected BP at the real scan callback, not merely traversal. */
void
pr19262_engine_r02_worker(spa_t *spa, const blkptr_t *bp,
    boolean_t snapshot)
{
	if (spa == NULL || spa->spa_reloc == NULL || bp == NULL || !snapshot)
		return;
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r02_t *r = &engine->r02;
	if (r->armed && r->mapped && engine_r02_match_locked(r, bp))
		r->snapshot_worker++;
	mutex_exit(&engine->lock);
}

void
pr19262_engine_r02_decision(spa_t *spa, const blkptr_t *bp, uint64_t func,
    uint64_t min, uint64_t max, boolean_t needs_io, boolean_t queued)
{
	if (spa == NULL || spa->spa_reloc == NULL || bp == NULL)
		return;
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r02_t *r = &engine->r02;
	if (r->armed && r->mapped && engine_r02_match_locked(r, bp)) {
		r->decisions++;
		r->min = min;
		r->max = max;
		r->p_in_window = r->p > min && r->p < max;
		r->resilver |= func == POOL_SCAN_RESILVER;
		r->needs_io |= needs_io;
		r->queued += queued;
	}
	mutex_exit(&engine->lock);
}

int
pr19262_engine_r02_status(spa_t *spa, char *buf, size_t size)
{
	if (spa == NULL || spa->spa_reloc == NULL || size == 0)
		return (-ENOTSUP);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_r02_t *r = &engine->r02;
	int n = snprintf(buf, size, "state=%s pool=%s generation=%llu "
	    "backing_calls=%llu indirect_seen=%llu snapshot_worker=%llu "
	    "decisions=%llu queued=%llu "
	    "B=%llu P=%llu "
	    "original_vdev=%llu original_offset=%llu original_size=%llu "
	    "backing_vdev=%llu backing_offset=%llu backing_size=%llu "
	    "min=%llu max=%llu mapped=%u p_in_window=%u resilver=%u "
	    "needs_io=%u\n", r->armed ? "armed" : "idle", spa_name(spa),
	    (u_longlong_t)r->generation, (u_longlong_t)r->backing_calls,
	    (u_longlong_t)r->indirect_seen,
	    (u_longlong_t)r->snapshot_worker,
	    (u_longlong_t)r->decisions, (u_longlong_t)r->queued,
	    (u_longlong_t)r->b, (u_longlong_t)r->p,
	    (u_longlong_t)r->original_vdev,
	    (u_longlong_t)r->original_offset,
	    (u_longlong_t)r->original_size,
	    (u_longlong_t)r->backing_vdev,
	    (u_longlong_t)r->backing_offset,
	    (u_longlong_t)r->backing_size, (u_longlong_t)r->min,
	    (u_longlong_t)r->max, r->mapped, r->p_in_window, r->resilver,
	    r->needs_io);
	mutex_exit(&engine->lock);
	return (n < 0 || (size_t)n >= size ? -ENOSPC : n);
}
#endif	/* _KERNEL */

void
pr19262_engine_park_next(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	VERIFY(!engine->park_next && engine->parked == NULL);
	engine->park_next = B_TRUE;
	mutex_exit(&engine->lock);
}

uint64_t
pr19262_engine_waiting(spa_t *spa, boolean_t writers)
{
	engine_t *engine = spa->spa_reloc;
	uint64_t count = 0;
	mutex_enter(&engine->lock);
	if (!writers) {
		for (engine_pending_t *p = engine->parked; p != NULL;
		    p = p->next)
			count++;
	} else {
		for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
		    e = AVL_NEXT(&engine->owners, e))
			for (engine_pending_t *p = e->pending; p != NULL;
			    p = p->next)
				count++;
	}
	mutex_exit(&engine->lock);
	return (count);
}

void
pr19262_engine_unpark(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_pending_t *p = engine->parked;
	engine->parked = NULL;
	mutex_exit(&engine->lock);
	engine_resume(p);
}

void
pr19262_engine_fail_writes(spa_t *spa, boolean_t fail)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine->fail_writes = fail;
	mutex_exit(&engine->lock);
}

void
pr19262_engine_retired_limit(spa_t *spa, uint64_t limit)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine->retired_limit = limit;
	mutex_exit(&engine->lock);
}

void
pr19262_engine_block_new(spa_t *spa, boolean_t block)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine->block_new = block;
	mutex_exit(&engine->lock);
}

void
pr19262_engine_alloc_fail_after(spa_t *spa, uint64_t attempts)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine->alloc_fail_after = attempts;
	mutex_exit(&engine->lock);
}

uint64_t
pr19262_engine_limit(spa_t *spa, boolean_t netfree)
{
	engine_t *engine = spa->spa_reloc;
	uint64_t limit = UINT64_MAX;
	mutex_enter(&engine->lock);
	if (engine->spa == spa && spa != NULL) {
		uint64_t protected = (netfree ? 8ULL : 16ULL) << 20;
		uint64_t free = zfs_range_tree_space(engine->free);
		limit = free > protected ? free - protected : 0;
		if (!netfree && engine->retired >= engine->retired_limit)
			limit = 0;
	}
	mutex_exit(&engine->lock);
	return (limit);
}

static int
engine_compare(const void *arg1, const void *arg2)
{
	const engine_owner_t *a = arg1, *b = arg2;
	return (a->v < b->v ? -1 : a->v > b->v ? 1 : 0);
}

static engine_owner_t *
engine_find(engine_t *engine, uint64_t v)
{
	engine_owner_t key = { .v = v };
	return (avl_find(&engine->owners, &key, NULL));
}

static int
engine_tomb_compare(const void *arg1, const void *arg2)
{
	const engine_tomb_t *a = arg1, *b = arg2;
	if (a->v != b->v)
		return (a->v < b->v ? -1 : 1);
	return (a->incarnation < b->incarnation ? -1 :
	    a->incarnation > b->incarnation ? 1 : 0);
}

static boolean_t
engine_tombstoned(engine_t *engine, uint64_t v, uint64_t incarnation)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_tomb_t key = { .v = v, .incarnation = incarnation };
	return (avl_find(&engine->tombstones, &key, NULL) != NULL);
}

static void
engine_tombstones_clear(engine_t *engine)
{
	void *cookie = NULL;
	engine_tomb_t *t;
	while ((t = avl_destroy_nodes(&engine->tombstones, &cookie)) != NULL)
		kmem_free(t, sizeof (*t));
}

/* Record a released journaled identity; later segments cannot bind it. */
static void
engine_tombstone_add(engine_t *engine, uint64_t v, uint64_t incarnation,
    uint64_t segment)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	engine_tomb_t *t = kmem_zalloc(sizeof (*t), KM_SLEEP);
	t->v = v;
	t->incarnation = incarnation;
	t->segment = segment;
	avl_index_t where;
	engine_tomb_t *found = avl_find(&engine->tombstones, t, &where);
	if (found != NULL) {
		found->segment = MAX(found->segment, segment);
		found->expired = B_FALSE;
		kmem_free(t, sizeof (*t));
		return;
	}
	avl_insert(&engine->tombstones, t, where);
}

static engine_owner_t *
engine_overlap(engine_t *engine, uint64_t v, uint64_t size)
{
	engine_owner_t key = { .v = v };
	avl_index_t where;
	engine_owner_t *e = avl_find(&engine->owners, &key, &where);
	if (e != NULL)
		return (e);
	e = avl_nearest(&engine->owners, where, AVL_BEFORE);
	if (e != NULL && v - e->v < e->size)
		return (e);
	e = avl_nearest(&engine->owners, where, AVL_AFTER);
	return (e != NULL && e->v - v < size ? e : NULL);
}

#ifdef _KERNEL
static void engine_kernel_init(void);
static void engine_kernel_fini(void);
static void engine_kernel_cache(spa_t *);
#endif

/*
 * Engine tasks that block -- a journal bind waits for journal_lock and its
 * member writes, a fence waits for TXGs -- never run on system_taskq.  A
 * zinject-delayed zio completes from system_taskq (zio_delay_interrupt()),
 * and an expired delayed task does not spawn a thread, so with every
 * system_taskq thread blocked behind such an I/O the I/O never completes.
 * A deferred zio resumes on its own zio issue taskq (engine_resume()).
 */
static taskq_t *engine_journal_tq;
static taskq_t *engine_fence_tq;

void
pr19262_engine_init(void)
{
	engine_journal_tq = taskq_create("z_pr19262_jrnl", MAX(boot_ncpus, 64),
	    maxclsyspri, boot_ncpus, INT_MAX,
	    TASKQ_PREPOPULATE | TASKQ_DYNAMIC);
	engine_fence_tq = taskq_create("z_pr19262_fence", MAX(boot_ncpus, 64),
	    maxclsyspri, 1, INT_MAX, TASKQ_DYNAMIC);
#ifdef _KERNEL
	engine_kernel_init();
#endif
}

void
pr19262_engine_fini(void)
{
#ifdef _KERNEL
	engine_kernel_fini();
#endif
	taskq_destroy(engine_fence_tq);
	taskq_destroy(engine_journal_tq);
	engine_fence_tq = NULL;
	engine_journal_tq = NULL;
}

boolean_t
pr19262_engine_active(spa_t *spa)
{
	if (spa == NULL)
		return (B_FALSE);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	boolean_t active = engine->spa == spa;
	mutex_exit(&engine->lock);
	return (active);
}

static uint64_t engine_admit_protected(engine_t *, uint_t);

/* The translated vdev: the first RAIDZ top-level vdev, or the one named. */
static vdev_t *
engine_target_vdev(spa_t *spa, uint64_t guid)
{
	vdev_t *root = spa->spa_root_vdev;
	for (uint64_t c = 0; c < root->vdev_children; c++) {
		vdev_t *vd = root->vdev_child[c];
		if (vd->vdev_ops != &vdev_raidz_ops || vd->vdev_islog)
			continue;
		if (guid == 0 || vd->vdev_guid == guid)
			return (vd);
	}
	return (NULL);
}

/* Another normal-class top-level vdev can take client data instead. */
static boolean_t
engine_foreign_normal(spa_t *spa, vdev_t *target)
{
	vdev_t *root = spa->spa_root_vdev;
	for (uint64_t c = 0; c < root->vdev_children; c++) {
		vdev_t *vd = root->vdev_child[c];
		if (vd != target && !vd->vdev_islog && vd->vdev_mg != NULL &&
		    vd->vdev_mg->mg_class == spa_normal_class(spa))
			return (B_TRUE);
	}
	return (B_FALSE);
}

vdev_t *
pr19262_engine_vdev(spa_t *spa)
{
	if (spa == NULL)
		return (NULL);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	vdev_t *vd = engine->spa == spa && spa != NULL ? engine->vd : NULL;
	mutex_exit(&engine->lock);
	return (vd);
}

/*
 * B3G (A33): the translated vdev keeps the size the engine started on.
 * The store root records it (checked against vdev_asize at every import)
 * and every engine domain covers exactly [0, total).  Growing the vdev,
 * by autoexpand after larger members or by "zpool online -e" after a LUN
 * growth, would put new metaslabs outside every domain and make the next
 * import refuse the store, writable and read-only (reason 20).  vdev_open
 * keeps vdev_asize and vdev_online refuses an explicit expansion; extra
 * member space stays unused (EXPANDSZ).  Called under SCL_STATE.  (libzfs
 * still relabels a whole disk before the refused "online -e", as stock
 * "zpool online" does with autoexpand=on; the label is harmless.)
 */
boolean_t
pr19262_engine_size_fixed(vdev_t *vd)
{
	spa_t *spa = vd != NULL ? vd->vdev_spa : NULL;
	if (spa == NULL || spa->spa_reloc == NULL || vd->vdev_top != vd)
		return (B_FALSE);
	engine_t *engine = spa->spa_reloc;
	/* Also while engine_start reads the metaslabs it will cover. */
	mutex_enter(&engine->lock);
	boolean_t fixed = (engine->spa == spa && engine->vd == vd) ||
	    engine->loading_vd == vd;
	mutex_exit(&engine->lock);
	return (fixed);
}

void
pr19262_engine_size_refused(vdev_t *vd, uint64_t asize, const char *why)
{
	spa_t *spa = vd->vdev_spa;
	pr19262_printf("PR19262_GROW_REFUSED vdev=%llu asize=%llu "
	    "wanted=%llu why=%s\n", (u_longlong_t)vd->vdev_id,
	    (u_longlong_t)vd->vdev_asize, (u_longlong_t)asize, why);
	cmn_err(CE_WARN, "pool '%s': %s refused: top-level vdev %llu is "
	    "translated by the relocation engine and its size stays %llu "
	    "bytes (growing it would make the pool unimportable)",
	    spa_name(spa), why, (u_longlong_t)vd->vdev_id,
	    (u_longlong_t)vd->vdev_asize);
}

/* True when no DVA of the block lives on the translated vdev. */
boolean_t
pr19262_engine_bp_foreign(spa_t *spa, const blkptr_t *bp)
{
	if (spa == NULL || bp == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return (B_FALSE);
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	boolean_t foreign = engine->spa == spa && spa != NULL;
	for (int d = 0; foreign && d < BP_GET_NDVAS(bp); d++)
		foreign = DVA_GET_VDEV(&bp->blk_dva[d]) != engine->vd->vdev_id;
	mutex_exit(&engine->lock);
	return (foreign);
}

static boolean_t
engine_metaslab_pinned(metaslab_t *ms)
{
	if (ms->ms_group == NULL)
		return (B_FALSE);
	vdev_t *vd = ms->ms_group->mg_vd;
	engine_t *engine = vd->vdev_spa->spa_reloc;
	if (engine == NULL)
		return (B_FALSE);
	mutex_enter(&engine->lock);
	boolean_t pinned = engine->loading_vd == vd;
	mutex_exit(&engine->lock);
	return (pinned);
}

static int engine_store_enable_locked(spa_t *);
static void engine_store_enable_done(spa_t *);
static void engine_store_clear(engine_t *);
static boolean_t engine_store_direct_topology(vdev_t *);
static boolean_t engine_store_r02_topology(vdev_t *, boolean_t);
static vdev_t *engine_store_r02_member_leaf(vdev_t *);

/* The initial raw-store format has no certificate for missing members. */
static boolean_t
engine_store_members_ready(vdev_t *vd)
{
	/*
	 * The raw catalog/journal codec addresses one concrete leaf per
	 * logical RAIDZ member.  A replacing parent is not one concrete
	 * member: accepting it here would later issue a raw physical I/O or a
	 * flush to the parent and mistake mirror-parent success for a durable
	 * write to the new leaf.  Replacement remains refused by D64; this
	 * is a second, import/start-time defence until a full reseed protocol
	 * can make the replacing topology authoritative.
	 */
	if (!engine_store_direct_topology(vd) &&
	    !engine_store_r02_topology(vd, B_TRUE))
		return (B_FALSE);
	for (uint_t c = 0; c < vd->vdev_children; c++) {
		/* For the admitted R02 pair, the exact new leaf decides. */
		vdev_t *child = engine_store_r02_member_leaf(vd->vdev_child[c]);
		if (child == NULL || !vdev_readable(child) ||
		    !vdev_writeable(child))
			return (B_FALSE);
	}
	return (B_TRUE);
}

static int
engine_start(spa_t *spa, boolean_t persistent)
{
	engine_t *engine = spa->spa_reloc;
	VERIFY(spa_writeable(spa));
	VERIFY(spa->spa_trust_config);
	/* Serialize topology changes through preparation and publication. */
	mutex_enter(&spa->spa_vdev_top_lock);
	txg_wait_synced(spa->spa_dsl_pool, 0);
#ifndef _KERNEL
	/* Growth-defer test: frees that sync after the turnover. */
	if (engine->test_start_fn != NULL) {
		void (*fn)(spa_t *, void *) = engine->test_start_fn;
		engine->test_start_fn = NULL;
		fn(spa, engine->test_start_arg);
	}
#endif

	/* DMU space-map reads must precede every config writer lock. */
	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
	vdev_t *vd = engine_target_vdev(spa, 0);
	VERIFY3P(vd, !=, NULL);
	if (persistent && !engine_store_members_ready(vd)) {
		spa_config_exit(spa, SCL_CONFIG, FTAG);
		mutex_exit(&spa->spa_vdev_top_lock);
		return (SET_ERROR(ENXIO));
	}
	mutex_enter(&engine->lock);
	VERIFY3P(engine->spa, ==, NULL);
	VERIFY3P(engine->loading_vd, ==, NULL);
	engine->loading_vd = vd;
	mutex_exit(&engine->lock);
	for (uint64_t i = 0; i < vd->vdev_ms_count; i++) {
		metaslab_t *ms = vd->vdev_ms[i];
		mutex_enter(&ms->ms_lock);
		VERIFY0(metaslab_load(ms));
		mutex_exit(&ms->ms_lock);
	}
	spa_config_exit(spa, SCL_CONFIG, FTAG);
	/*
	 * CONFIG drains sync, including deferred-free turnover.  ALLOC stops
	 * new native reservations.  Leave ZIO and FREE open while loading
	 * space maps; a native rollback can still finish during the scan.
	 */
	spa_config_enter(spa, SCL_CONFIG | SCL_ALLOC, FTAG, RW_WRITER);
	VERIFY3P(engine->spa, ==, NULL);
	VERIFY3P(engine_target_vdev(spa, 0), ==, vd);
	zfs_range_tree_t *free = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	zfs_range_tree_t *legacy = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	zfs_range_tree_t *pending = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	zfs_range_tree_add(legacy, 0, vd->vdev_asize);
	for (uint64_t i = 0; i < vd->vdev_ms_count; i++) {
		metaslab_t *ms = vd->vdev_ms[i];
		mutex_enter(&ms->ms_lock);
		VERIFY(ms->ms_loaded);
		zfs_range_tree_walk(ms->ms_allocatable, zfs_range_tree_add,
		    free);
		zfs_range_tree_walk(ms->ms_allocatable, zfs_range_tree_remove,
		    legacy);
		for (uint_t t = 0; t < TXG_SIZE; t++)
			zfs_range_tree_walk(ms->ms_allocating[t],
			    zfs_range_tree_add, pending);
		mutex_exit(&ms->ms_lock);
	}
	/*
	 * Drain native vdev I/O before publishing translated ownership.  The
	 * bp-level hold is acquired under SCL_ZIO, so waiting I/O will bind
	 * after publication.  FREE then stops the remaining immediate undo.
	 * No pool I/O may be issued under these lower writer locks.
	 */
	spa_config_enter(spa, SCL_ZIO | SCL_FREE, FTAG, RW_WRITER);
	for (uint64_t i = 0; i < vd->vdev_ms_count; i++) {
		metaslab_t *ms = vd->vdev_ms[i];
		mutex_enter(&ms->ms_lock);
		for (uint_t t = 0; t < TXG_SIZE; t++)
			zfs_range_tree_walk(ms->ms_allocating[t],
			    zfs_range_tree_remove, pending);
		mutex_exit(&ms->ms_lock);
	}
	/* With allocation and sync stopped, only undo can shrink pending. */
	zfs_range_tree_walk(pending, zfs_range_tree_add, free);
	zfs_range_tree_vacate(pending, zfs_range_tree_remove, legacy);
	zfs_range_tree_destroy(pending);
	/* Rows past the last metaslab belong to neither domain. */
	zfs_range_tree_t *unusable = zfs_range_tree_create(NULL,
	    ZFS_RANGE_SEG64, NULL, 0, 0);
	VERIFY3U(vd->vdev_ms_shift, !=, 0);
	uint64_t covered = P2ALIGN_TYPED(vd->vdev_asize,
	    1ULL << vd->vdev_ms_shift, uint64_t);
	if (covered < vd->vdev_asize) {
		zfs_range_tree_clear(free, covered, vd->vdev_asize - covered);
		zfs_range_tree_clear(legacy, covered, vd->vdev_asize - covered);
		zfs_range_tree_add(unusable, covered, vd->vdev_asize - covered);
	}
	mutex_enter(&engine->lock);
	engine->free = free;
	engine->legacy = legacy;
	engine->unusable = unusable;
	engine->vd = vd;
	engine->vd_guid = vd->vdev_guid;
	engine->foreign_normal = engine_foreign_normal(spa, vd);
	engine->total = vd->vdev_asize;
	engine->retired_limit = 64ULL << 20;
	engine->admit_batch = ENGINE_ADMIT_BATCH;
	engine->admit_refreshed = UINT64_MAX;
	engine->admit_cached_parity = UINT64_MAX;
	/* The strongest layout present is the default conversion target. */
	vdev_raidz_layout_for_birth(vd, spa_last_synced_txg(spa) + 1, NULL,
	    &engine->admit_parity);
	engine->store_tolerance = engine->admit_parity;
	mutex_exit(&engine->lock);
	/* Publish authority only after both raw arenas have been reserved. */
	int error = persistent ? engine_store_enable_locked(spa) : 0;
	mutex_enter(&engine->lock);
	if (error == 0)
		engine->spa = spa;
	else
		engine_store_clear(engine);
	engine->loading_vd = NULL;
	mutex_exit(&engine->lock);
	spa_config_exit(spa, SCL_CONFIG | SCL_ALLOC | SCL_ZIO | SCL_FREE,
	    FTAG);
	mutex_exit(&spa->spa_vdev_top_lock);
	if (persistent && error == 0)
		engine_store_enable_done(spa);
	return (error);
}

void
pr19262_engine_start(spa_t *spa)
{
	VERIFY0(engine_start(spa, B_FALSE));
}

int
pr19262_engine_start_persistent(spa_t *spa)
{
	return (engine_start(spa, B_TRUE));
}

static void engine_pages_standby_fill_locked(engine_t *);
static uint64_t engine_pages_runs_locked(engine_t *);
static void engine_pages_reserved_check_locked(engine_t *);
static uint64_t engine_pages_reserved_runs_locked(engine_t *);
static boolean_t engine_pages_reserved_take_back_locked(engine_t *, uint64_t);
static uint64_t engine_pages_reserved_run_bytes(engine_t *);
static uint64_t engine_admit_runs_for_others(engine_t *);

/*
 * Reuse deferral, the engine's ms_defer.  A range that leaves a catalog
 * structure (legacy, an owner's version, a cut journal segment) is still
 * listed by the durable catalog.  An import of that catalog rebuilds the
 * free tree without the range, so anything written there before a catalog
 * recording the free is durable collides with it: a new arena refuses the
 * writable import (reconcile ECKSUM, decode REFUSE), a journaled backing
 * fails its replay, and the floor root's own data may be overwritten.  So
 * the range waits in reuse_open until the next store snapshot records its
 * absence, then in the batch of that store's txg C, and only joins free
 * once the uberblock of txg C + TXG_DEFER_SIZE is durable.  Native ZFS
 * holds a freed block in ms_defer for the same number of txgs so that an
 * import -F rewind of up to TXG_DEFER_SIZE txgs finds nothing reused; this
 * does the same for the catalogs, whose older roots list the range.  Space
 * that never left free in a stored catalog (an aborted allocation, a
 * returned copy stage, a rolled-back pick, the standby run) returns to
 * free directly.  Without a catalog (RAM-only engine) there is nothing to
 * protect.  RAM-only, and empty after a load: the loaded catalog is
 * durable, and every later free (pr19262_engine_legacy_recover included)
 * passes through here.
 */
static void
engine_reuse_add(engine_t *engine, uint64_t start, uint64_t size)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	zfs_range_tree_verify_not_present(engine->free, start, size);
	if (!engine->persistent) {
		zfs_range_tree_add(engine->free, start, size);
		return;
	}
	if (engine->reuse_open == NULL) {
		engine->reuse_open = zfs_range_tree_create(NULL,
		    ZFS_RANGE_SEG64, NULL, 0, 0);
		for (uint_t i = 0; i < ENGINE_REUSE_BATCHES; i++)
			engine->reuse_batch[i] = zfs_range_tree_create(NULL,
			    ZFS_RANGE_SEG64, NULL, 0, 0);
	}
	zfs_range_tree_verify_not_present(engine->reuse_open, start, size);
	for (uint_t i = 0; i < ENGINE_REUSE_BATCHES; i++)
		zfs_range_tree_verify_not_present(engine->reuse_batch[i],
		    start, size);
	zfs_range_tree_add(engine->reuse_open, start, size);
	engine->reuse_deferred += size;
}

/* Bytes held back from free: open, and in every stored batch. */
static uint64_t
engine_reuse_bytes(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->reuse_open == NULL)
		return (0);
	uint64_t bytes = zfs_range_tree_space(engine->reuse_open);
	for (uint_t i = 0; i < ENGINE_REUSE_BATCHES; i++)
		bytes += zfs_range_tree_space(engine->reuse_batch[i]);
	return (bytes);
}

/*
 * The store snapshot of txg, taken in this same engine->lock hold, records
 * the absence of every range deferred so far.  A retried store of the
 * same txg merges.  Batches pending are those staged in the last
 * TXG_DEFER_SIZE txgs and this one; if none is free, merging into the newest
 * only postpones the release.
 */
static void
engine_reuse_stage_locked(engine_t *engine, uint64_t txg)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->reuse_open == NULL ||
	    zfs_range_tree_is_empty(engine->reuse_open))
		return;
	uint_t slot = ENGINE_REUSE_BATCHES, newest = 0;
	for (uint_t i = 0; i < ENGINE_REUSE_BATCHES; i++) {
		if (!zfs_range_tree_is_empty(engine->reuse_batch[i]) &&
		    engine->reuse_batch_txg[i] == txg)
			slot = i;
		if (engine->reuse_batch_txg[i] >
		    engine->reuse_batch_txg[newest])
			newest = i;
	}
	for (uint_t i = 0; slot == ENGINE_REUSE_BATCHES &&
	    i < ENGINE_REUSE_BATCHES; i++)
		if (zfs_range_tree_is_empty(engine->reuse_batch[i]))
			slot = i;
	if (slot == ENGINE_REUSE_BATCHES)
		slot = newest;
	zfs_range_tree_vacate(engine->reuse_open, zfs_range_tree_add,
	    engine->reuse_batch[slot]);
	engine->reuse_batch_txg[slot] = MAX(engine->reuse_batch_txg[slot], txg);
}

/*
 * The uberblock of txg ub, naming the catalog of txg catalog, is durable:
 * a batch stored by a catalog it names, TXG_DEFER_SIZE txgs ago or more,
 * may be reused.  Syncing context, RAM only.  Once the post-store headroom
 * check runs (never during a load), the standby and the run count see the
 * returned runs now, not one store later.
 */
static void
engine_reuse_release_locked(engine_t *engine, uint64_t ub, uint64_t catalog)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->reuse_open == NULL)
		return;
	boolean_t released = B_FALSE;
	for (uint_t i = 0; i < ENGINE_REUSE_BATCHES; i++) {
		zfs_range_tree_t *rt = engine->reuse_batch[i];
		if (zfs_range_tree_is_empty(rt) ||
		    engine->reuse_batch_txg[i] > catalog ||
		    engine->reuse_batch_txg[i] + TXG_DEFER_SIZE > ub)
			continue;
		engine->reuse_released += zfs_range_tree_space(rt);
		zfs_range_tree_vacate(rt, zfs_range_tree_add, engine->free);
		released = B_TRUE;
	}
	if (released && engine->free_runs_known) {
		engine_pages_standby_fill_locked(engine);
		engine->free_runs_cached = engine_pages_runs_locked(engine);
	}
}

/*
 * Whether the store of txg moves the deferral: it stages reuse_open, or its
 * uberblock, whose root names the catalog of this same txg, releases a
 * batch by the rule of engine_reuse_release_locked(engine, txg, txg).  Any
 * other otherwise clean txg would write an unchanged catalog under a new
 * root and release nothing, so it is not forced; nothing is released earlier.
 */
static boolean_t
engine_reuse_due_locked(engine_t *engine, uint64_t txg)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->reuse_open == NULL)
		return (B_FALSE);
	if (!zfs_range_tree_is_empty(engine->reuse_open))
		return (B_TRUE);
	for (uint_t i = 0; i < ENGINE_REUSE_BATCHES; i++)
		if (!zfs_range_tree_is_empty(engine->reuse_batch[i]) &&
		    engine->reuse_batch_txg[i] <= txg &&
		    engine->reuse_batch_txg[i] + TXG_DEFER_SIZE <= txg)
			return (B_TRUE);
	return (B_FALSE);
}

/*
 * A native free can predate activation and return to circulation later.
 * Only its still-unowned identity backing belongs to legacy; released
 * engine owners have already returned their P through the durable fence.
 * The catalog lists the range as legacy until the next durable store.
 */
static void
engine_legacy_free(void *arg, uint64_t start, uint64_t size)
{
	engine_t *engine = arg;
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t found, length;
	while (zfs_range_tree_find_in(engine->legacy, start, size,
	    &found, &length)) {
		VERIFY3P(engine_overlap(engine, found, length), ==, NULL);
		zfs_range_tree_remove(engine->legacy, found, length);
		engine_reuse_add(engine, found, length);
		engine->revision++;
	}
}

/* Caller holds the native metaslab lock, before making V reusable. */
static void
engine_native_free(vdev_t *vd, zfs_range_tree_t *native)
{
	engine_t *engine = vd->vdev_spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->spa == vd->vdev_spa && engine->vd == vd)
		zfs_range_tree_walk(native, engine_legacy_free, engine);
	mutex_exit(&engine->lock);
}

/*
 * Deferred trees are not restored on import.  Reconcile their catalog
 * legacy quarantine only after native ZIL claims, which may still need
 * identity extents that the selected space maps describe as free.
 */
int
pr19262_engine_legacy_recover(spa_t *spa)
{
	if (!pr19262_engine_persistent(spa))
		return (0);
	engine_t *engine = spa->spa_reloc;
	ASSERT(!spa->spa_sync_on);
	spa_config_enter(spa, SCL_ALLOC, FTAG, RW_READER);
	vdev_t *vd = pr19262_engine_vdev(spa);
	int error = 0;
	for (uint64_t i = 0; error == 0 && i < vd->vdev_ms_count; i++) {
		metaslab_t *ms = vd->vdev_ms[i];
		mutex_enter(&ms->ms_lock);
		error = metaslab_load(ms);
		if (error == 0)
			engine_native_free(vd, ms->ms_allocatable);
		mutex_exit(&ms->ms_lock);
	}
	/*
	 * A catalog may also have materialized a native reservation before
	 * its allocating txg committed.  If no ZIL claim needs that V, keep
	 * its P retired through the ordinary floor fence.  Claim V first so
	 * that the eventual owner release performs exactly one native free.
	 */
	mutex_enter(&engine->lock);
	for (engine_owner_t *e = avl_first(&engine->owners);
	    error == 0 && e != NULL; e = AVL_NEXT(&engine->owners, e)) {
		uint64_t v = e->v;
		uint64_t size = e->size;
		e->holds++;
		mutex_exit(&engine->lock);
		int native = metaslab_claim_impl(vd, v, size, 0);
		mutex_enter(&engine->lock);
		if (native == 0 && (e->allocation_txg != 0 ||
		    e->checkpoint_only || e->journal_pending))
			error = ENGINE_EINVAL(5);
		mutex_exit(&engine->lock);
		if (native == 0 && error == 0)
			error = metaslab_claim_impl(vd, v, size,
			    spa_first_txg(spa));
		else if (native != 0 && native != ENOENT)
			error = native;
		mutex_enter(&engine->lock);
		e->holds--;
		/*
		 * J-only owners were already reserved before ZIL traversal.
		 * A real ZIL claim clears journal_reserved; anything left is
		 * unreachable and must pass through the ordinary floor fence.
		 */
		if ((native == 0 || e->journal_reserved) && error == 0) {
			if (!e->retired) {
				e->retired = B_TRUE;
				e->retired_at = ++engine->sequence;
				engine->retired += e->cur->size;
			}
			e->catalog_txg = e->claimed_txg = spa_first_txg(spa);
			engine_owner_dirty(engine, e);
			engine->revision++;
		}
		e->journal_reserved = B_FALSE;
	}
	mutex_exit(&engine->lock);
	spa_config_exit(spa, SCL_ALLOC, FTAG);
	return (error);
}

typedef struct engine_pick {
	uint64_t start;
	uint64_t end;
	uint64_t size;
	uint64_t found;
} engine_pick_t;

static void
engine_pick_p(void *arg, uint64_t start, uint64_t size)
{
	engine_pick_t *p = arg;
	uint64_t low = MAX(start, p->start);
	uint64_t high = MIN(start + size, p->end);
	if (high >= low && high - low >= p->size)
		p->found = high - p->size;
}

typedef struct engine_alloc_pick {
	uint64_t low;
	uint64_t high;
	uint64_t min;
	uint64_t max;
	uint64_t mult;
	uint64_t align;
	uint64_t found;
	uint64_t size;
	uint64_t native_max;
} engine_alloc_pick_t;

static uint64_t
engine_alloc_size(engine_alloc_pick_t *pick, uint64_t low, uint64_t high)
{
	if (high <= low)
		return (0);
	return (rounddown(MIN(high - low, pick->max), pick->mult));
}

static void
engine_alloc_p(void *arg, uint64_t start, uint64_t size)
{
	engine_alloc_pick_t *pick = arg;
	uint64_t low = MAX(start, pick->low);
	uint64_t high = MIN(start + size, pick->high);
	uint64_t span = engine_alloc_size(pick, low, high);
	if (span >= pick->min && span > pick->size) {
		pick->size = span;
		pick->found = high - span;
	}
}

static boolean_t
engine_alloc_gap(engine_alloc_pick_t *pick, uint64_t low, uint64_t high)
{
	low = P2ROUNDUP(low, pick->align);
	uint64_t span = engine_alloc_size(pick, low, high);
	if (span < pick->min)
		return (B_FALSE);
	pick->found = low;
	pick->size = span;
	return (B_TRUE);
}

static void
engine_alloc_v(void *arg, uint64_t start, uint64_t size)
{
	engine_t *engine = ((engine_walk_arg_t *)arg)->engine;
	arg = ((engine_walk_arg_t *)arg)->arg;
	engine_alloc_pick_t *pick = arg;
	if (pick->found != UINT64_MAX)
		return;
	uint64_t low = P2ROUNDUP(start, pick->align);
	uint64_t high = start + size;
	pick->native_max = MAX(pick->native_max,
	    engine_alloc_size(pick, low, high));
	engine_owner_t key = { .v = low };
	avl_index_t where;
	engine_owner_t *e = avl_find(&engine->owners, &key, &where);
	if (e == NULL) {
		e = avl_nearest(&engine->owners, where, AVL_BEFORE);
		if (e == NULL || low - e->v >= e->size)
			e = avl_nearest(&engine->owners, where, AVL_AFTER);
	}
	while (e != NULL && e->v < high) {
		if (e->v > low && engine_alloc_gap(pick, low, e->v))
			return;
		low = MAX(low, e->v + e->size);
		if (low >= high)
			return;
		e = AVL_NEXT(&engine->owners, e);
	}
	(void) engine_alloc_gap(pick, low, high);
}

/*
 * The owner AVL is also the V exclusion index, including aborted owners
 * awaiting their last hold or physical lease. The candidate and its P pair
 * are committed under the same engine lock, before native V is removed.
 */
pr19262_engine_alloc_result_t
pr19262_engine_alloc(spa_t *spa, vdev_t *vd, zfs_range_tree_t *native,
    uint64_t ms_start, uint64_t size, uint64_t max_size, uint64_t txg,
    uint64_t *offset, uint64_t *actual_size)
{
	if (spa == NULL)
		return (PR19262_ALLOC_INACTIVE);
	engine_t *engine = spa->spa_reloc;
	pr19262_engine_alloc_result_t result = PR19262_ALLOC_INACTIVE;
	engine_owner_t *e = NULL;
	engine_version_t *b = NULL;
	/* Budget lookup takes the transaction lock: before the engine lock. */
	boolean_t client = pr19262_tx_client_txg(spa, txg);
	mutex_enter(&engine->lock);
	if (engine->spa != spa || spa == NULL || vd != engine->vd)
		goto out;
	/*
	 * With another normal-class vdev to fall back to, client data never
	 * eats into the protected reserve of the translated vdev: the
	 * allocator moves on to the native vdev (class fallback).
	 */
	if (engine->foreign_normal && client) {
		uint64_t free = zfs_range_tree_space(engine->free);
		uint64_t protected =
		    engine_admit_protected(engine, PR19262_ADMIT_CLIENT);
		if (free < protected || free - protected < size) {
			engine->alloc_guarded++;
			result = PR19262_ALLOC_P_BLOCKED;
			goto out;
		}
	}
	engine_alloc_pick_t p = {
		.low = ms_start,
		.high = ms_start + (1ULL << vd->vdev_ms_shift),
		.min = size,
		.max = max_size,
		.mult = vdev_alloc_factor(vd, txg),
		.align = 1ULL << vd->vdev_ashift,
		.found = UINT64_MAX
	};
#ifndef _KERNEL
	/*
	 * Gate C needs a real ordinary DMU owner in a returned P extent.  The
	 * hook is armed only after ztest has proved the whole range free.  It
	 * merely selects that valid P range; native V selection, accounting and
	 * the owner commit below remain the regular allocator path.
	 */
	if (engine->test_prefer_active) {
		if (engine->test_prefer_p < p.low ||
		    engine->test_prefer_p + size > p.high ||
		    engine->test_prefer_p + size > engine->test_prefer_end ||
		    !zfs_range_tree_contains(engine->free,
		    engine->test_prefer_p,
		    size)) {
			result = PR19262_ALLOC_P_BLOCKED;
			goto out;
		}
		p.found = engine->test_prefer_p;
		p.size = size;
	} else
#endif
	zfs_range_tree_walk(engine->free, engine_alloc_p, &p);
	if (p.found == UINT64_MAX) {
		result = PR19262_ALLOC_P_BLOCKED;
		goto out;
	}
	engine_alloc_pick_t v = p;
	v.max = p.size;
	v.size = 0;
	v.found = UINT64_MAX;
	engine_walk_arg_t walk = { engine, &v };
	zfs_range_tree_walk(native, engine_alloc_v, &walk);
	if (v.found == UINT64_MAX) {
		result = v.native_max < size ? PR19262_ALLOC_NATIVE_FULL :
		    PR19262_ALLOC_V_BLOCKED;
		goto out;
	}
	if (engine->alloc_fail_after != 0 && --engine->alloc_fail_after == 0) {
		pr19262_printf("PR19262_ALLOC_NOMEM v=%llu size=%llu p=%llu "
		    "eligible=1\n", (u_longlong_t)v.found,
		    (u_longlong_t)v.size,
		    (u_longlong_t)(p.found + p.size - v.size));
		result = PR19262_ALLOC_NOMEM;
		goto out;
	}
	/* Reserve ticket memory without sleeping or issuing I/O. */
	e = engine_owner_alloc(KM_NOSLEEP);
	b = kmem_zalloc(sizeof (*b), KM_NOSLEEP);
	if (e == NULL || b == NULL) {
		result = PR19262_ALLOC_NOMEM;
		goto out;
	}
	VERIFY3P(engine_overlap(engine, v.found, v.size), ==, NULL);
	VERIFY(!zfs_range_tree_contains(engine->legacy, v.found, v.size));
	e->v = v.found;
	e->size = v.size;
	e->allocation_txg = e->catalog_txg = txg;
	e->incarnation = ++engine->next_incarnation;
	e->created = ++engine->sequence;
	b->owner = e;
	b->p = p.found + p.size - v.size;
	b->size = v.size;
	b->birth = txg;
	b->generation = ++engine->sequence;
	engine_version_layout(b, vd, txg);
	e->cur = e->versions = b;
	zfs_range_tree_remove(engine->free, b->p, b->size);
	avl_add(&engine->owners, e);
	engine_owner_dirty(engine, e);
	engine->pairs++;
	engine->revision++;
#ifndef _KERNEL
	engine->test_prefer_active = B_FALSE;
#endif
	*offset = v.found;
	*actual_size = v.size;
	e = NULL;
	b = NULL;
	result = PR19262_ALLOC_OK;
out:
	mutex_exit(&engine->lock);
	if (e != NULL)
		kmem_free(e, sizeof (*e));
	if (b != NULL)
		kmem_free(b, sizeof (*b));
	return (result);
}

int
pr19262_engine_pair(spa_t *spa, vdev_t *vd, uint64_t v, uint64_t size,
    uint64_t txg)
{
	if (spa == NULL)
		return (0);
	engine_t *engine = spa->spa_reloc;
	int error = 0;
	mutex_enter(&engine->lock);
	if (engine->spa != spa || spa == NULL)
		goto out;
	VERIFY3P(vd, ==, engine->vd);
	VERIFY(!zfs_range_tree_contains(engine->legacy, v, size));
	engine_owner_t *collision = engine_overlap(engine, v, size);
	if (collision != NULL) {
		pr19262_printf("PR19262_V_PAIR_COLLISION v=%llu size=%llu "
		    "txg=%llu owner=%llu owner_size=%llu pending=%u "
		    "retired=%u aborted=%u\n", (u_longlong_t)v,
		    (u_longlong_t)size, (u_longlong_t)txg,
		    (u_longlong_t)collision->v,
		    (u_longlong_t)collision->size, collision->journal_pending,
		    collision->retired, collision->aborted);
		error = ENOSPC;
		goto out;
	}
	/* Preserve native ditto separation: P stays in the chosen V MS. */
	uint64_t ms_start = P2ALIGN_TYPED(v, 1ULL << vd->vdev_ms_shift,
	    uint64_t);
	engine_pick_t pick = { ms_start, ms_start +
		(1ULL << vd->vdev_ms_shift), size, UINT64_MAX };
	zfs_range_tree_walk(engine->free, engine_pick_p, &pick);
	if (pick.found == UINT64_MAX) {
		error = ENOSPC;
		goto out;
	}
	engine_owner_t *e = engine_owner_alloc(KM_SLEEP);
	e->v = v;
	e->size = size;
	e->allocation_txg = txg;
	e->catalog_txg = txg;
	e->cur = engine_version_new(engine, e, pick.found, size, txg, B_FALSE);
	e->incarnation = ++engine->next_incarnation;
	e->created = ++engine->sequence;
	zfs_range_tree_remove(engine->free, e->cur->p, size);
	avl_add(&engine->owners, e);
	engine_owner_dirty(engine, e);
	engine->pairs++;
out:
	mutex_exit(&engine->lock);
	return (error);
}

static void
engine_abort_done(engine_t *engine, engine_owner_t *e)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	VERIFY(e->aborted);
	if (e->holds != 0)
		return;
	VERIFY(!e->copying && e->pending == NULL);
	for (engine_version_t *b = e->versions; b != NULL; b = b->next)
		if (b->leases != 0)
			return;
	while (e->versions != NULL) {
		engine_version_t *b = e->versions;
		e->versions = b->next;
		zfs_range_tree_verify_not_present(engine->free, b->p, b->size);
		zfs_range_tree_add(engine->free, b->p, b->size);
		kmem_free(b, sizeof (*b));
	}
	engine_owner_gone(engine, e);
	avl_remove(&engine->owners, e);
	kmem_free(e, sizeof (*e));
}

/* Diagnostic: a journal-touched allocation retained as retired at abort. */
static void
engine_abort_report(spa_t *spa, engine_owner_t *e, uint64_t v,
    uint64_t size, uint64_t txg)
{
	pr19262_printf("PR19262_ABORT_RETAINED v=%llu size=%llu txg=%llu "
	    "holds=%llu copying=%u syncing_txg=%llu last_synced=%llu\n",
	    (u_longlong_t)v, (u_longlong_t)size, (u_longlong_t)txg,
	    (u_longlong_t)e->holds, e->copying,
	    (u_longlong_t)spa_syncing_txg(spa),
	    (u_longlong_t)spa_last_synced_txg(spa));
	pr19262_flush();
	pr19262_backtrace();
}

boolean_t
pr19262_engine_abort(spa_t *spa, vdev_t *vd, uint64_t v, uint64_t size,
    uint64_t txg)
{
	engine_t *engine = spa->spa_reloc;
	boolean_t retained = B_FALSE;
	boolean_t classified = B_FALSE;
	mutex_enter(&engine->lock);
	if (engine->spa == spa && spa != NULL && vd == engine->vd) {
		engine_owner_t *e = engine_find(engine, v);
		if (e == NULL) {
			/* The caller proved the native reservation exists. */
			VERIFY(zfs_range_tree_contains(engine->legacy,
			    v, size));
			engine_legacy_free(engine, v, size);
		} else {
			VERIFY3U(e->size, ==, size);
			VERIFY3U(e->allocation_txg != 0 ? e->allocation_txg :
			    e->cur->birth, ==, txg);
			VERIFY(!e->aborted);
			classified = B_TRUE;
			/* Admission and abort choose exactly one outcome. */
			if (engine->persistent && e->journal_touched) {
				if (!e->retired) {
					e->retired = B_TRUE;
					e->retired_at = ++engine->sequence;
					engine->retired += e->cur->size;
					engine->revision++;
					engine_owner_dirty(engine, e);
				}
				retained = B_TRUE;
				if (engine->abort_prints++ < 4)
					engine_abort_report(spa, e, v, size,
					    txg);
				goto out;
			}
			VERIFY(!e->retired);
			e->aborted = B_TRUE;
			engine_owner_dirty(engine, e);
			engine->revision++;
			engine->aborts++;
			engine_abort_done(engine, e);
		}
	}
out:
	mutex_exit(&engine->lock);
	if (classified) {
		/* The observer records only; caller still holds ms_lock. */
		blkptr_t bp = { 0 };
		DVA_SET_VDEV(&bp.blk_dva[0], vd->vdev_id);
		DVA_SET_OFFSET(&bp.blk_dva[0], v);
		DVA_SET_ASIZE(&bp.blk_dva[0], size);
		BP_SET_BIRTH(&bp, txg, txg);
		engine_journal_test_event(spa, &bp, retained ? 4 : 3);
	}
	return (retained);
}

boolean_t
pr19262_engine_retire(vdev_t *vd, uint64_t v, uint64_t size,
    boolean_t checkpoint)
{
	engine_t *engine = vd->vdev_spa->spa_reloc;
	boolean_t handled = B_FALSE;
	mutex_enter(&engine->lock);
	if (engine->spa != vd->vdev_spa || engine->spa == NULL ||
	    vd != engine->vd)
		goto out;
	/* A fenced owner release performs the native free itself. */
	if (engine->release_thread == curthread)
		goto out;
	engine_owner_t *e = engine_find(engine, v);
	if (e == NULL) {
		VERIFY(zfs_range_tree_contains(engine->legacy, v, size));
		zfs_range_tree_remove(engine->legacy, v, size);
		e = engine_owner_alloc(KM_SLEEP);
		e->v = v;
		e->size = size;
		e->cur = engine_version_new(engine, e, v, size, 0, B_TRUE);
		e->incarnation = ++engine->next_incarnation;
		avl_add(&engine->owners, e);
		engine_owner_dirty(engine, e);
	}
	VERIFY3U(e->size, ==, size);
	VERIFY(!e->retired && !e->aborted);
	engine_owner_dirty(engine, e);
	if (checkpoint) {
		e->checkpoint_only = B_TRUE;
		engine->revision++;
		/* Keep native checkpoint space-map accounting. */
		goto out;
	}
	e->retired = B_TRUE;
	/* A live V: no checkpoint space map entry names it. */
	e->drain_releasable = B_TRUE;
	engine->revision++;
	e->retired_at = ++engine->sequence;
	engine->retired += e->cur->size;
	if (engine->retire_prints++ < 24) {
		spa_t *spa = vd->vdev_spa;
		pr19262_printf("PR19262_RETIRE v=%llu size=%llu birth=%llu "
		    "allocation_txg=%llu sync_context=%u syncing_txg=%llu "
		    "last_synced=%llu retired_txg=%llu\n", (u_longlong_t)v,
		    (u_longlong_t)size, (u_longlong_t)e->cur->birth,
		    (u_longlong_t)e->allocation_txg,
		    dsl_pool_sync_context(spa->spa_dsl_pool),
		    (u_longlong_t)spa_syncing_txg(spa),
		    (u_longlong_t)spa_last_synced_txg(spa),
		    (u_longlong_t)e->retired_txg);
	}
	/* No fake floor: neither authoritative V nor P becomes free. */
	handled = B_TRUE;
out:
	mutex_exit(&engine->lock);
	return (handled);
}

/* L19: the catalog reserve under a checkpoint (convert.c, pages.c). */
static void engine_ckres_arm_locked(spa_t *, engine_t *, const char *);
static void engine_ckres_release_locked(engine_t *, const char *);

void
pr19262_engine_checkpoint(spa_t *spa, const uberblock_t *checkpoint)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	/* Cache native roots before activation as well as translated roots. */
	if (engine->spa == NULL || engine->spa == spa) {
		uberblock_t absent = { 0 };
		const uberblock_t *next = checkpoint != NULL ?
		    checkpoint : &absent;
		if (memcmp(&engine->checkpoint, next,
		    sizeof (engine->checkpoint)) != 0) {
			/*
			 * A discard removes C from the MOS and the catalog of
			 * this syncing txg; the roots before it still name C.
			 */
			if (checkpoint == NULL && engine->spa == spa &&
			    engine->persistent &&
			    engine->checkpoint.ub_txg != 0) {
				engine->ckpt_ub = engine->checkpoint;
				engine->ckpt_until = MAX(spa_syncing_txg(spa),
				    spa_last_synced_txg(spa)) + 1;
			} else if (checkpoint != NULL) {
				engine->ckpt_until = 0;
			}
			engine->checkpoint = *next;
			engine->revision++;
			/*
			 * L21: a count of checkpoint-pinned retired bytes is
			 * of one checkpoint's hold or drain; the next one
			 * counts again (two drains share a state key).
			 */
			engine->retired_pinned_valid = B_FALSE;
			/*
			 * L19: C arms its catalog reserve (sized as admitted);
			 * a discard keeps it until C's pins drop with the guard
			 * (engine_ckpt_unpin_locked); a C gone without a guard
			 * releases it now.
			 */
			if (checkpoint != NULL)
				engine_ckres_arm_locked(spa, engine, "record");
			else if (engine->ckpt_until == 0)
				engine_ckres_release_locked(engine, "gone");
		}
	}
	mutex_exit(&engine->lock);
}

static engine_owner_t *
engine_checkpoint_owner(engine_t *engine, uint64_t v)
{
	engine_owner_t key = { .v = v };
	avl_index_t where;
	engine_owner_t *e = avl_find(&engine->owners, &key, &where);
	if (e == NULL)
		e = avl_nearest(&engine->owners, where, AVL_BEFORE);
	if (e == NULL || v < e->v || v - e->v >= e->size)
		return (NULL);
	return (e);
}

int
pr19262_engine_checkpoint_validate(spa_t *spa, const uberblock_t *checkpoint)
{
	engine_t *engine = spa->spa_reloc;
	uberblock_t absent = { 0 };
	const uberblock_t *expected = checkpoint != NULL ? checkpoint : &absent;
	mutex_enter(&engine->lock);
	int error = 0;
	if (engine->spa == spa && engine->persistent &&
	    memcmp(&engine->checkpoint, expected,
	    sizeof (engine->checkpoint)) != 0)
		error = ENGINE_EINVAL(4);
	mutex_exit(&engine->lock);
	return (error);
}

void *
pr19262_engine_hold(spa_t *spa, const blkptr_t *bp, void *inherit,
    boolean_t read, boolean_t gang_child)
{
	if (spa == NULL)
		return (NULL);
	engine_t *engine = spa->spa_reloc;
	if (bp != NULL && (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp)))
		return (NULL);
	mutex_enter(&engine->lock);
	if (engine->spa != spa || spa == NULL) {
		mutex_exit(&engine->lock);
		return (NULL);
	}
	engine_hold_t *prior = inherit;
	if (prior != NULL)
		VERIFY3P(prior->spa, ==, spa);
	if (engine->block_new && prior == NULL) {
		mutex_exit(&engine->lock);
		return (NULL);
	}
	if (bp == NULL && prior == NULL) {
		mutex_exit(&engine->lock);
		return (NULL);
	}
	engine_hold_t *h = kmem_zalloc(sizeof (*h), KM_SLEEP);
	h->spa = spa;
	h->refs = 1;
	h->write = !read;
	if (prior != NULL) {
		h->error = prior->error;
		h->error_site = prior->error != 0 ? 1 : 0;
	}
	if (prior != NULL && prior->error == ESTALE &&
	    engine->inherit_prints++ < 4) {
		pr19262_printf("PR19262_HOLD_INHERIT_STALE bp=%d write=%u "
		    "prior_count=%u prior_write=%u prior_gang=%u "
		    "prior_started=%llu\n", bp != NULL, !read, prior->count,
		    prior->write, prior->gang, (u_longlong_t)prior->started);
		pr19262_flush();
		pr19262_backtrace();
	}
	h->started = prior != NULL ? prior->started : ++engine->sequence;
	h->gang = (prior != NULL && prior->gang) ||
	    (bp != NULL && BP_IS_GANG(bp));
	if (bp == NULL) {
		*h = *prior;
		/*
		 * A vdev child inherits its owners, but is not the selected
		 * logical R-01 request.  Only that parent publishes completion.
		 * An inheriting child can be the last holder before terminal.
		 */
		h->r01_selected = B_FALSE;
		for (uint_t i = 0; i < h->count; i++) {
			h->owner[i]->holds++;
			h->owner[i]->queued_writers += h->write;
		}
		mutex_exit(&engine->lock);
		return (h);
	}
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		if (DVA_GET_VDEV(dva) != engine->vd->vdev_id) {
			/* Another top-level vdev: native, never translated. */
			h->foreign++;
			continue;
		}
		engine_owner_t *e = engine_find(engine, DVA_GET_OFFSET(dva));
		boolean_t inherited = B_FALSE;
		if (prior != NULL && prior->error == 0)
			for (uint_t i = 0; i < prior->count; i++)
				inherited |= prior->owner[i] == e && e != NULL;
		if (e == NULL) {
			uint64_t size = DVA_GET_GANG(dva) ?
			    vdev_gang_header_asize(engine->vd,
			    BP_GET_PHYSICAL_BIRTH(bp)) : DVA_GET_ASIZE(dva);
			if (!zfs_range_tree_contains(engine->legacy,
			    DVA_GET_OFFSET(dva), size)) {
				h->error = ENOENT;
				h->error_site = 2;
				continue;
			}
			e = engine_owner_alloc(KM_SLEEP);
			e->v = DVA_GET_OFFSET(dva);
			e->size = size;
			e->cur = engine_version_new(engine, e, e->v, size,
			    BP_GET_PHYSICAL_BIRTH(bp), B_TRUE);
			e->incarnation = ++engine->next_incarnation;
			zfs_range_tree_remove(engine->legacy, e->v, size);
			avl_add(&engine->owners, e);
			engine_owner_dirty(engine, e);
		}
		boolean_t descendant = prior != NULL && prior->error == 0 &&
		    prior->gang && gang_child && read;
		/* A released and reused V must not serve an older BP. */
		boolean_t reused = e->allocation_txg != 0 && !inherited &&
		    !BP_GET_DEDUP(bp) &&
		    BP_GET_PHYSICAL_BIRTH(bp) != e->allocation_txg;
		boolean_t gone = (e->retired && !inherited &&
		    !(descendant && h->started < e->retired_at)) ||
		    (e->aborted && !inherited) ||
		    (descendant && e->created > h->started);
		if (reused || gone) {
			h->error = ESTALE;
			h->error_site = 3;
			if (engine->stale_verdicts++ < 16)
				pr19262_printf("PR19262_HOLD_STALE v=%llu "
				    "reused=%u gone=%u retired=%u aborted=%u "
				    "birth=%llu allocation_txg=%llu write=%u "
				    "level=%u type=%u retired_txg=%llu "
				    "last_synced=%llu\n",
				    (u_longlong_t)e->v, reused, gone,
				    e->retired, e->aborted,
				    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(bp),
				    (u_longlong_t)e->allocation_txg, h->write,
				    (uint_t)BP_GET_LEVEL(bp),
				    (uint_t)BP_GET_TYPE(bp),
				    (u_longlong_t)e->retired_txg,
				    (u_longlong_t)spa_last_synced_txg(spa));
			if (engine->stale_verdicts <= 3) {
				pr19262_flush();
				pr19262_backtrace();
			}
		}
		e->holds++;
		e->queued_writers += h->write;
		h->owner[h->count++] = e;
	}
	if (h->count == 0 && h->foreign != 0 && h->error == 0) {
		/* Every DVA lives on a native vdev: no engine identity. */
		mutex_exit(&engine->lock);
		kmem_free(h, sizeof (*h));
		return (NULL);
	}
	mutex_exit(&engine->lock);
	return (h);
}

void
pr19262_engine_rele(void *arg)
{
	if (arg == NULL)
		return;
	engine_t *engine = ((engine_hold_t *)arg)->spa->spa_reloc;
	engine_hold_t *h = arg;
	mutex_enter(&engine->lock);
#ifdef _KERNEL
	engine_r01_t *r = &engine->r01;
	engine_owner_t *r_owner = NULL;
	boolean_t r_seen = B_FALSE;
	if ((r->state == ENGINE_R01_PARKED ||
	    r->state == ENGINE_R01_RELEASED ||
	    r->state == ENGINE_R01_CANCELLED) && r->owner != NULL)
		r_owner = r->owner;
	if (h->r01_selected) {
		VERIFY3P(r->selected, !=, NULL);
		VERIFY3P(r_owner, !=, NULL);
	}
#endif
	for (uint_t i = 0; i < h->count; i++) {
		engine_owner_t *e = h->owner[i];
		VERIFY3U(e->holds, >, 0);
		e->holds--;
#ifdef _KERNEL
		if (e == r_owner)
			r_seen = B_TRUE;
#endif
		VERIFY3U(e->queued_writers, >=, h->write);
		e->queued_writers -= h->write;
		if (e->aborted)
			engine_abort_done(engine, e);
	}
#ifdef _KERNEL
	if (r_seen) {
		if (h->r01_selected)
			r->selected = NULL;
		/*
		 * RAIDZ children inherit an owner hold but do not inherit
		 * r01_selected.  The parent alone records completion; any final
		 * inheriting hold makes the observer terminal.
		 */
		engine_r01_try_drain_locked(engine);
	}
#endif
	mutex_exit(&engine->lock);
	kmem_free(h, sizeof (*h));
}

void
pr19262_engine_io_rele(zio_t *zio)
{
	engine_version_t *b = zio->io_pr19262_owner_version;
	if (b == NULL)
		return;
	engine_t *engine = zio->io_spa->spa_reloc;
	mutex_enter(&engine->lock);
	VERIFY3U(b->leases, >, 0);
	b->leases--;
	if (zio->io_type == ZIO_TYPE_WRITE) {
		VERIFY3U(b->writers, >, 0);
		b->writers--;
	}
#ifdef _KERNEL
	if (engine->r01.selected == zio) {
		engine_r01_t *r = &engine->r01;
		r->parent_error = zio->io_error;
		r->lease_terminal = b->leases;
		r->writers_terminal = b->writers;
		r->parent_done = B_TRUE;
	}
#endif
	if (zio->io_error == 0 && !b->ready) {
		b->ready = B_TRUE;
		engine_owner_dirty(engine, b->owner);
	}
	engine_owner_t *e = b->owner;
	if (e->aborted)
		engine_abort_done(engine, e);
	zio->io_pr19262_owner_version = NULL;
	mutex_exit(&engine->lock);
}

/* Diagnostic: the hold behind a stale dispatch verdict. */
static void
engine_hold_report(const engine_hold_t *h)
{
	pr19262_printf("PR19262_HOLD_REPORT error=%d site=%u count=%u "
	    "write=%u gang=%u started=%llu foreign=%u\n", h->error,
	    h->error_site, h->count, h->write, h->gang,
	    (u_longlong_t)h->started, h->foreign);
	for (uint_t i = 0; i < h->count; i++) {
		const engine_owner_t *e = h->owner[i];
		pr19262_printf("PR19262_HOLD_OWNER i=%u v=%llu size=%llu "
		    "incarnation=%llu allocation_txg=%llu retired=%u "
		    "aborted=%u copying=%u holds=%llu retired_txg=%llu\n", i,
		    (u_longlong_t)e->v, (u_longlong_t)e->size,
		    (u_longlong_t)e->incarnation,
		    (u_longlong_t)e->allocation_txg, e->retired, e->aborted,
		    e->copying, (u_longlong_t)e->holds,
		    (u_longlong_t)e->retired_txg);
	}
	pr19262_flush();
}

/* Diagnostic: report a zio's hold from the pipeline, ungated. */
void
pr19262_engine_hold_report(spa_t *spa, void *hold)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (hold == NULL)
		pr19262_printf("PR19262_HOLD_REPORT null hold\n");
	else
		engine_hold_report(hold);
	pr19262_printf("PR19262_HOLD_REPORT_COUNTER value=%llu\n",
	    (u_longlong_t)engine->hold_report_prints);
	mutex_exit(&engine->lock);
}

/* A refused physical write names itself: nothing else reports it. */

boolean_t
pr19262_engine_scan_backing(spa_t *spa, const blkptr_t *bp, int d,
    dva_t *dva, uint64_t *birth)
{
	if (spa == NULL || bp == NULL || dva == NULL || birth == NULL ||
	    d < 0 || d >= BP_GET_NDVAS(bp))
		return (B_FALSE);
	engine_t *engine = spa->spa_reloc;
	const dva_t *original = &bp->blk_dva[d];

	*dva = *original;
	*birth = BP_GET_PHYSICAL_BIRTH(bp);
	if (engine == NULL)
		return (B_FALSE);
	mutex_enter(&engine->lock);
	if (engine->spa != spa || engine->vd == NULL ||
	    DVA_GET_VDEV(original) != engine->vd->vdev_id) {
		mutex_exit(&engine->lock);
		return (B_FALSE);
	}
	engine_owner_t *owner = engine_find(engine, DVA_GET_OFFSET(original));
	if (owner == NULL || owner->cur == NULL || !owner->cur->ready) {
		mutex_exit(&engine->lock);
		return (B_FALSE);
	}
	DVA_SET_OFFSET(dva, owner->cur->p);
	DVA_SET_ASIZE(dva, owner->cur->size);
	*birth = owner->cur->birth;
	mutex_exit(&engine->lock);
	return (B_TRUE);
}

/*
 * A logical write whose hold names a copying owner waits for that copy here,
 * from zio_vdev_io_start(), before it issues any vdev child.  Its caller then
 * drops its SCL_ZIO reader, so the write holds none while it waits.  Deferred
 * as a vdev child instead, it would keep its parent's reader, which covers
 * every child until zio_vdev_io_assess(); the copy takes SCL_ZIO as reader
 * again before it publishes, and a wanted SCL_ZIO writer
 * (spa_vdev_state_enter() and the like) would wait for the write while the
 * copy waits for the writer.
 *
 * Entered with the zio's SCL_ZIO reader held and its hold just bound, so
 * copy_begin() refuses these owners from now on (queued_writers), and no
 * child of a write that passes here finds its owner copying.  EINPROGRESS:
 * queued on the owner with its stage rewound; engine_copy_publish() resumes
 * it on the issue taskq, and zio_vdev_io_start() retakes SCL_ZIO and binds a
 * fresh hold.  From then on the zio may run, finish and be freed on another
 * thread: the caller must not touch it.
 */
int
pr19262_engine_defer_logical(zio_t *zio)
{
	engine_hold_t *h = zio->io_pr19262_owner_hold;
	ASSERT0P(zio->io_vd);
	/* A CONFIG_WRITER zio holds no SCL_ZIO for the caller to drop. */
	if (zio->io_type != ZIO_TYPE_WRITE || zio->io_bp == NULL ||
	    h == NULL || (zio->io_flags & ZIO_FLAG_CONFIG_WRITER) ||
	    zio->io_pr19262_owner_copy ||
	    zio->io_pr19262_owner_version != NULL)
		return (0);
	ASSERT3U(zio->io_stage, ==, ZIO_STAGE_VDEV_IO_START);
	spa_t *spa = zio->io_spa;
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	/* An error hold is refused by dispatch, as before any copy. */
	if (engine->spa != spa || h->spa != spa || h->error != 0) {
		mutex_exit(&engine->lock);
		return (0);
	}
	for (uint_t i = 0; i < h->count; i++) {
		engine_owner_t *e = h->owner[i];
		if (!e->copying)
			continue;
		engine_defer(zio, &e->pending);
		engine->deferred++;
		engine->deferred_logical++;
		mutex_exit(&engine->lock);
		return (EINPROGRESS);
	}
	mutex_exit(&engine->lock);
	return (0);
}

static void
engine_repair_skip_dtl(vdev_t *vd, uint64_t txg, uint64_t commit,
    boolean_t all)
{
	if (!vd->vdev_ops->vdev_op_leaf) {
		for (uint64_t c = 0; c < vd->vdev_children; c++)
			engine_repair_skip_dtl(vd->vdev_child[c], txg, commit,
			    all);
		return;
	}
	if (!all && vdev_dtl_empty(vd, DTL_PARTIAL))
		return;
	vdev_dtl_dirty(vd, DTL_SCRUB, txg, 1);
	if (vdev_dtl_contains(vd, DTL_MISSING, txg, 1))
		return;
	vdev_t *rvd = vd->vdev_spa->spa_root_vdev;
	for (vdev_t *pvd = vd; pvd != rvd; pvd = pvd->vdev_parent)
		vdev_dtl_dirty(pvd, DTL_PARTIAL, txg, 1);
	vdev_dirty(vd->vdev_top, VDD_DTL, vd, commit);
	vdev_dtl_dirty(vd, DTL_MISSING, txg, 1);
}

/*
 * A scan repair that dispatch bypassed (EALREADY) must not count as done.
 * As vdev_stat_update() does for a scan repair that failed at a leaf, keep
 * its txgs in DTL_SCRUB on the leaves below the target that it would have
 * written, so that scan done keeps them missing there instead of excising
 * them, and the leaves still need a resilver of the block.  Those leaves are
 * every leaf for a SELF_HEAL repair, else the leaves whose DTL_PARTIAL is not
 * empty (zio_vdev_io_start() bypasses a repair leaf by its DTL).  The txgs are
 * the operation txg and the births of the owner's current and newest (the
 * copy's stage) versions, which the leaf DTL check uses after translation.
 * A self-heal repair outside a scan leaves no trail when it fails either.
 * Called from zio_vdev_io_start(), under the parent read's SCL_ZIO.
 */
void
pr19262_engine_repair_skipped(zio_t *zio)
{
	spa_t *spa = zio->io_spa;
	ASSERT(zio->io_flags & ZIO_FLAG_IO_REPAIR);
	if (!(zio->io_flags & ZIO_FLAG_SCAN_THREAD) || zio->io_txg == 0 ||
	    zio->io_vd == NULL || !spa_writeable(spa))
		return;
	uint64_t txgs[3] = { zio->io_txg, 0, 0 };
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_owner_t *e = engine->spa == spa ?
	    engine_find(engine, zio->io_offset) : NULL;
	if (e != NULL && e->cur != NULL)
		txgs[1] = e->cur->birth;
	if (e != NULL && e->versions != NULL)
		txgs[2] = e->versions->birth;
	mutex_exit(&engine->lock);
	boolean_t all = (zio->io_flags & ZIO_FLAG_SELF_HEAL) != 0;
	/* The open txg: valid from any context, never already synced. */
	uint64_t commit = spa_open_txg(spa);
	for (int i = 0; i < 3; i++) {
		if (txgs[i] == 0 || (i > 0 && txgs[i] == txgs[0]) ||
		    (i > 1 && txgs[i] == txgs[1]))
			continue;
		engine_repair_skip_dtl(zio->io_vd, txgs[i], commit, all);
	}
}

int
pr19262_engine_dispatch(zio_t *zio)
{
	if (zio->io_spa == NULL)
		return (0);
	engine_t *engine = zio->io_spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->spa != zio->io_spa || engine->spa == NULL) {
		mutex_exit(&engine->lock);
		return (0);
	}
	if (zio->io_type == ZIO_TYPE_TRIM) {
		mutex_exit(&engine->lock);
		return (ENOTSUP);
	}
	if (zio->io_type != ZIO_TYPE_READ &&
	    zio->io_type != ZIO_TYPE_WRITE) {
		mutex_exit(&engine->lock);
		return (0);
	}
	if (zio->io_logical != NULL &&
	    zio->io_logical->io_pr19262_physical) {
		mutex_exit(&engine->lock);
		/* Isolated-fixture P constructors lack engine authority. */
		return (ENOTSUP);
	}
	if (zio->io_flags & ZIO_FLAG_PHYSICAL) {
		uint64_t low = VDEV_LABEL_START_SIZE;
		vdev_t *slot = zio->io_vd->vdev_parent;
		/* R02: an exact leaf of a strict replacing member slot. */
		if (slot != NULL && slot->vdev_ops == &vdev_replacing_ops &&
		    slot->vdev_parent == engine->vd &&
		    zio->io_vd->vdev_ops->vdev_op_leaf)
			slot = slot->vdev_parent;
		boolean_t metadata = engine->persistent &&
		    zio->io_pr19262_metadata_authority == engine &&
		    slot == engine->vd &&
		    zio->io_offset >= low &&
		    zio->io_offset - low <=
		    engine->total / engine->vd->vdev_children &&
		    zio->io_size <= engine->total / engine->vd->vdev_children -
		    (zio->io_offset - low) &&
		    ((engine->store_extents != NULL &&
		    zfs_range_tree_contains(engine->store_extents,
		    (zio->io_offset - low) * engine->vd->vdev_children,
		    zio->io_size * engine->vd->vdev_children)) ||
		    (engine->journal_extents != NULL &&
		    zfs_range_tree_contains(engine->journal_extents,
		    (zio->io_offset - low) * engine->vd->vdev_children,
		    zio->io_size * engine->vd->vdev_children)));
		/* Labels/probes are outside the data area. */
		boolean_t unsafe = zio->io_type == ZIO_TYPE_WRITE &&
		    zio->io_vd->vdev_ops->vdev_op_leaf &&
		    zio->io_vd->vdev_top == engine->vd &&
		    zio->io_offset >= VDEV_LABEL_START_SIZE &&
		    zio->io_offset < zio->io_vd->vdev_psize -
		    VDEV_LABEL_END_SIZE &&
		    zio->io_pr19262_owner_hold == NULL && !metadata;
		if (unsafe && engine->dispatch_unsafe_prints++ < 8)
			pr19262_printf("PR19262_DISPATCH_UNSAFE child=%llu "
			    "offset=%llu size=%llu flags=%llx\n",
			    (u_longlong_t)zio->io_vd->vdev_id,
			    (u_longlong_t)zio->io_offset,
			    (u_longlong_t)zio->io_size,
			    (u_longlong_t)zio->io_flags);
		mutex_exit(&engine->lock);
		return (unsafe ? ENOTSUP : 0);
	}
	if (zio->io_vd != zio->io_vd->vdev_top || zio->io_vd != engine->vd) {
		/* Leaf I/O below the target, or a native top-level vdev. */
		mutex_exit(&engine->lock);
		return (0);
	}
#ifdef _KERNEL
	if (engine->r01.state == ENGINE_R01_ARMED &&
	    zio->io_type == ZIO_TYPE_READ)
		engine->r01.seen_top++;
#endif
	engine_hold_t *h = zio->io_pr19262_owner_hold;
	if ((!zio->io_pr19262_owner_copy &&
	    (h == NULL || h->error != 0)) || zio->io_bp == NULL) {
		int error = h != NULL && h->error != 0 ? h->error : ENOENT;
		if (error == ESTALE && h != NULL &&
		    engine->hold_report_prints++ < 4)
			engine_hold_report(h);
		mutex_exit(&engine->lock);
		return (error);
	}
	uint64_t v = zio->io_pr19262_owner_mapped ?
	    zio->io_pr19262_owner_v : zio->io_offset;
	engine_owner_t *e = NULL;
	engine_version_t *b = zio->io_pr19262_owner_version;
	if (zio->io_pr19262_owner_copy) {
		VERIFY3P(b, !=, NULL);
		e = b->owner;
		VERIFY(e->copying);
		VERIFY3U(e->v, ==, v);
	} else {
		for (uint_t i = 0; i < h->count; i++)
			if (h->owner[i]->v == v)
				e = h->owner[i];
	}
	if (e == NULL) {
#ifdef _KERNEL
		if (engine->r01.state == ENGINE_R01_ARMED &&
		    zio->io_type == ZIO_TYPE_READ)
			engine->r01.seen_no_owner++;
#endif
		if (engine_find(engine, v) != NULL ||
		    !zfs_range_tree_contains(engine->legacy, v,
		    vdev_psize_to_asize_txg(engine->vd, zio->io_size,
		    BP_GET_PHYSICAL_BIRTH(zio->io_bp)))) {
			mutex_exit(&engine->lock);
			return (ENOENT);
		}
		engine->legacy_reads++;
		mutex_exit(&engine->lock);
		return (0);
	}
	if (b == NULL && e->copying && zio->io_type == ZIO_TYPE_WRITE) {
		/*
		 * The child of a logical write cannot get here: the write
		 * waited in pr19262_engine_defer_logical() if its owner was
		 * copying, and its hold keeps a copy from beginning since.
		 * What does is a repair write that a mirror-ops read issues
		 * for another DVA (self-heal, scrub, resilver).  Its parent
		 * read holds SCL_ZIO until every child is done, so it must not
		 * wait for the copy, which needs SCL_ZIO to publish.  Bypass
		 * it (zio_vdev_io_start()): a self-heal repair is best effort,
		 * and a scan repair leaves its txg in the leaves' DTLs as a
		 * failed one does (pr19262_engine_repair_skipped()), so that
		 * the resilver is not taken as complete.
		 */
		if (zio->io_flags & ZIO_FLAG_IO_REPAIR) {
			engine->repair_skipped++;
			if (engine->repair_skip_prints++ < 8)
				pr19262_printf("PR19262_DISPATCH_REPAIR_SKIP "
				    "v=%llu size=%llu flags=%llx\n",
				    (u_longlong_t)e->v,
				    (u_longlong_t)zio->io_size,
				    (u_longlong_t)zio->io_flags);
			mutex_exit(&engine->lock);
			return (EALREADY);
		}
		/* Unreachable by the argument above; kept as a backstop. */
		engine->deferred_vdev++;
		if (engine->deferred_vdev_prints++ < 8)
			pr19262_printf("PR19262_DEFER_VDEV v=%llu size=%llu "
			    "flags=%llx\n", (u_longlong_t)e->v,
			    (u_longlong_t)zio->io_size,
			    (u_longlong_t)zio->io_flags);
		engine_defer(zio, &e->pending);
		engine->deferred++;
		mutex_exit(&engine->lock);
		return (EINPROGRESS);
	}
	boolean_t selected = !zio->io_pr19262_owner_mapped;
	boolean_t r01_bound = B_FALSE;
#ifdef _KERNEL
	if (selected && engine->r01.state == ENGINE_R01_RELEASED &&
	    zio->io_type == ZIO_TYPE_READ && !zio->io_pr19262_owner_copy &&
	    engine->r01.prebound == zio->io_logical &&
	    engine_r01_routed_locked(engine, zio)) {
		engine_r01_t *r = &engine->r01;
		VERIFY3P(r->owner, ==, e);
		VERIFY3U(r->v, ==, e->v);
		VERIFY3U(r->incarnation, ==, e->incarnation);
		VERIFY3P(b, ==, NULL);
		VERIFY3P(r->version, !=, NULL);
		VERIFY(r->bound_lease);
		b = r->version;
		r01_bound = B_TRUE;
		r->seen_top++;
	}
#endif
	if (selected) {
		if (b == NULL)
			b = e->cur;
		if (!r01_bound)
			b->leases++;
		else
			VERIFY3U(b->leases, >, 0);
		if (zio->io_type == ZIO_TYPE_WRITE)
			b->writers++;
		zio->io_pr19262_owner_version = b;
	}
	VERIFY3P(b->owner, ==, e);
	uint64_t sectors = howmany(zio->io_size,
	    1ULL << engine->vd->vdev_ashift);
	uint64_t required = roundup(sectors + b->parity *
	    howmany(sectors, b->width - b->parity), b->parity + 1) <<
	    engine->vd->vdev_ashift;
	VERIFY3U(required, <=, b->size);
	/* Keep the original V-BP for checksum/crypto/gang verification. */
	blkptr_t *physical = &zio->io_pr19262_owner_bp;
	*physical = *zio->io_bp;
	boolean_t matched = B_FALSE;
	for (int d = 0; d < BP_GET_NDVAS(physical); d++) {
		dva_t *dva = &physical->blk_dva[d];
		if (DVA_GET_VDEV(dva) == engine->vd->vdev_id &&
		    DVA_GET_OFFSET(dva) == v) {
			DVA_SET_OFFSET(dva, b->p);
			DVA_SET_ASIZE(dva, b->size);
			matched = B_TRUE;
		}
	}
	VERIFY(matched);
	if (engine->fail_writes && zio->io_type == ZIO_TYPE_WRITE &&
	    BP_GET_LEVEL(zio->io_bp) == 0 &&
	    !DMU_OT_IS_METADATA(BP_GET_TYPE(zio->io_bp))) {
		mutex_exit(&engine->lock);
		return (EIO);
	}
	/*
	 * A backing birth names the concrete write for DTL and recovery, not
	 * merely the parity-epoch used when its fixed geometry was selected.
	 * The geometry already lives in b, so an unwritten binding records the
	 * actual write TXG at the first successful dispatch.
	 */
	if (zio->io_type == ZIO_TYPE_WRITE &&
	    (!b->ready || zio->io_pr19262_owner_copy)) {
		uint64_t birth = zio->io_txg;
		/*
		 * A backing birth names the concrete write for DTL and
		 * recovery.
		 * Copy begin chose the stage geometry from its requested epoch;
		 * that geometry is stored separately in b.  Its transfer write
		 * still needs its own concrete operation TXG as P's birth.
		 */
		if (birth == 0)
			birth = BP_GET_PHYSICAL_BIRTH(zio->io_bp);
		if (birth == 0) {
			mutex_exit(&engine->lock);
			return (EIO);
		}
		/*
		 * A journaled binding's birth is durable in its J_BIND record,
		 * which replay compares strictly with the catalog: changing
		 * it here without a new record would make the next catalog
		 * disagree with the live journal.  A binding journaled ahead
		 * of its first write therefore already names that write (a
		 * rebound log tail journals its BP birth); keep the durable
		 * birth if one ever does not.
		 */
		if (b->journal_seq != 0 && b->birth != birth) {
			if (engine->birth_drift_prints++ < 8) {
				pr19262_printf("PR19262_J_BIRTH_DRIFT v=%llu "
				    "gen=%llu seq=%llu birth=%llu write=%llu "
				    "txg=%llu copy=%d\n", (u_longlong_t)v,
				    (u_longlong_t)b->generation,
				    (u_longlong_t)b->journal_seq,
				    (u_longlong_t)b->birth,
				    (u_longlong_t)birth,
				    (u_longlong_t)zio->io_txg,
				    zio->io_pr19262_owner_copy);
				pr19262_flush();
			}
			ASSERT3U(b->birth, ==, birth);
		} else {
			b->birth = birth;
		}
	}
	BP_SET_PHYSICAL_BIRTH(physical, b->birth);
	zio->io_pr19262_owner_mapped = B_TRUE;
	zio->io_pr19262_owner_v = v;
	zio->io_offset = b->p;
	zio->io_pr19262_backing_birth = b->birth;
	/*
	 * A resilver must retain the original logical BP birth B as a
	 * repair-selection input, separately from current physical P.
	 * Do not apply this to ordinary reads or self-heal policy yet.
	 */
	if (zio->io_type == ZIO_TYPE_READ &&
	    (zio->io_flags & ZIO_FLAG_RESILVER) != 0)
		zio->io_pr19262_repair_source_birth =
		    BP_GET_PHYSICAL_BIRTH(&zio->io_bp_orig);
	zio->io_pr19262_backing_width = b->width;
	zio->io_pr19262_backing_parity = b->parity;
	/* Operation TXG is never replaced by an encoding/backing birth. */
	engine->translated++;
	engine->legacy_reads += e->v == b->p;
	if (BP_GET_LEVEL(zio->io_bp) != 0 ||
	    DMU_OT_IS_METADATA(BP_GET_TYPE(zio->io_bp)))
		engine->metadata++;
#ifdef _KERNEL
	if (r01_bound) {
		engine->r01.seen_mapped++;
		engine->r01.seen_b = BP_GET_PHYSICAL_BIRTH(&zio->io_bp_orig);
		engine->r01.seen_p = b->birth;
		if (BP_GET_LEVEL(zio->io_bp) == 0)
			engine->r01.seen_level0++;
		if (!DMU_OT_IS_METADATA(BP_GET_TYPE(zio->io_bp)))
			engine->r01.seen_data++;
	}
#endif
#ifdef _KERNEL
	if (r01_bound)
		engine_r01_selected_locked(engine, zio, e, b, v);
#endif
#ifndef _KERNEL
	/*
	 * Test-only, not part of the release tree:
	 * ztest's pr19262_engine_park_next() parks the next ordinary read
	 * here, after its translation took a lease on the version it reads,
	 * until pr19262_engine_unpark() resumes it at this stage.  This is
	 * the ordinary-read park the R-01 b198 prebind step removed; the
	 * kernel consumes park_next only on its R-01 scan paths.
	 */
	if (selected && engine->park_next &&
	    zio->io_type == ZIO_TYPE_READ && !zio->io_pr19262_owner_copy) {
		engine->park_next = B_FALSE;
		engine_defer(zio, &engine->parked);
		mutex_exit(&engine->lock);
		return (EINPROGRESS);
	}
#endif
	mutex_exit(&engine->lock);
	return (0);
}

static int
engine_copy_begin(spa_t *spa, const blkptr_t *bp, int d, uint64_t birth,
    void **handle, boolean_t unwritten, boolean_t wait_held)
{
	*handle = NULL;
	if (spa == NULL)
		return (ENOTSUP);
	engine_t *engine = spa->spa_reloc;
	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp) || d < 0 ||
	    d >= BP_GET_NDVAS(bp))
		return (ENGINE_EINVAL(3));
	const dva_t *dva = &bp->blk_dva[d];
	uint64_t psize = BP_GET_PSIZE(bp);
	/*
	 * The requested epoch selects an unwritten rebind's geometry, but
	 * the stage is journaled before anything writes it: the ZIL later
	 * writes it at TXG 0, so its first write names the BP's physical
	 * birth (pr19262_engine_dispatch).  Bind that birth now, so its
	 * J_BIND record already holds the birth every later catalog lists.
	 */
	uint64_t concrete = unwritten ? BP_GET_PHYSICAL_BIRTH(bp) : birth;
	if (unwritten && concrete == 0)
		return (ENGINE_EINVAL(3));
	/* A pending journal roll: retried once due; until then, busy. */
	int roll = engine_journal_roll_wait(spa);
	if (roll != 0)
		return (roll);
	mutex_enter(&engine->lock);
	if (engine->spa != spa || spa == NULL ||
	    DVA_GET_VDEV(dva) != engine->vd->vdev_id) {
		mutex_exit(&engine->lock);
		return (ENOTSUP);
	}
	if (engine->persistent && engine->journal_error != 0) {
		int error = engine->journal_error;
		mutex_exit(&engine->lock);
		return (error);
	}
	if (DVA_GET_GANG(dva))
		psize = vdev_gang_header_psize(engine->vd);
	engine_owner_t *e = engine_find(engine, DVA_GET_OFFSET(dva));
	if (e == NULL || e->retired || e->aborted) {
		mutex_exit(&engine->lock);
		return (ESTALE);
	}
	/*
	 * A block allocated ahead of its write (a log chain's tail) has
	 * nothing to copy; it may be rebound while nothing writes it, since
	 * writes issued from now on defer behind the copying flag (logical
	 * writes in pr19262_engine_defer_logical()).  A legacy extent
	 * classified by a read is marked ready because its
	 * content is unknown; when the caller has read it and found no
	 * valid content (the unwritten tail a log header points at, born
	 * before the engine), the rebind is allowed as well.
	 */
	boolean_t legacy = e->allocation_txg == 0 && e->cur->p == e->v &&
	    e->cur->journal_seq == 0;
	/* A native allocation crossing activation still permits undo. */
	boolean_t native_pending = e->allocation_txg == 0 &&
	    e->cur->p == e->v && e->cur->birth > spa_last_synced_txg(spa);
	if (e->copying || (e->cur->ready == unwritten &&
	    !(unwritten && legacy)) ||
	    e->cur->writers != 0 || e->queued_writers != 0 ||
	    e->catalog_txg > spa_last_synced_txg(spa) || native_pending) {
		mutex_exit(&engine->lock);
		return (EBUSY);
	}
	uint64_t size = vdev_psize_to_asize_txg(engine->vd, psize, birth);
	uint64_t ms = P2ALIGN_TYPED(e->v, 1ULL << engine->vd->vdev_ms_shift,
	    uint64_t);
	engine_pick_t pick = { ms,
	    ms + (1ULL << engine->vd->vdev_ms_shift), size, UINT64_MAX };
	zfs_range_tree_walk(engine->free, engine_pick_p, &pick);
	if (pick.found == UINT64_MAX) {
		/*
		 * The V's own metaslab is full: a relocation may take backing
		 * anywhere on the vdev rather than fail, so a metaslab packed
		 * with weak data does not need its own headroom.
		 */
		pick.start = 0;
		pick.end = engine->total;
		zfs_range_tree_walk(engine->free, engine_pick_p, &pick);
	}
	/*
	 * Only runs held for txgs not yet stored can take the copy.  The
	 * converter (wait_held) never takes one back: it answers EAGAIN
	 * while runs are held for others' charges (they return once those
	 * txgs are stored; the caller drops the copy's charge and ends the
	 * pass as starved by clients, so L18 runs the next pass with
	 * priority, at most ENGINE_CONVERT_STARVE_ROUNDS in a row), and
	 * ENOSPC, as before, when only its own rows hold them.
	 * Every other caller (an unwritten rebind, R-01) takes one back at
	 * once.
	 */
	if (pick.found == UINT64_MAX &&
	    engine_pages_reserved_runs_locked(engine) != 0 &&
	    size <= engine_pages_reserved_run_bytes(engine)) {
		if (wait_held) {
			if (engine_admit_runs_for_others(engine) != 0) {
				engine->copy_held_waits++;
				mutex_exit(&engine->lock);
				return (EAGAIN);
			}
		} else {
			VERIFY(engine_pages_reserved_take_back_locked(engine,
			    size));
			pick.start = 0;
			pick.end = engine->total;
			pick.found = UINT64_MAX;
			zfs_range_tree_walk(engine->free, engine_pick_p, &pick);
		}
	}
	if (pick.found == UINT64_MAX) {
		mutex_exit(&engine->lock);
		return (ENOSPC);
	}
	engine_copy_t *c = kmem_zalloc(sizeof (*c), KM_SLEEP);
	c->spa = spa;
	c->owner = e;
	c->source = e->cur;
	c->original = *bp;
	c->psize = psize;
	c->stage = engine_version_new(engine, e, pick.found, size, birth,
	    B_FALSE);
	c->stage->birth = concrete;
	zfs_range_tree_remove(engine->free, pick.found, size);
	e->copying = B_TRUE;
	e->holds++;
	c->source->leases++;
	*handle = c;
	mutex_exit(&engine->lock);
#ifdef _KERNEL
	engine_r01_boundary_wait(engine, e, PR19262_R01_COPYING);
#endif
	return (0);
}

boolean_t
pr19262_engine_free_range(spa_t *spa, uint64_t p, uint64_t size)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	VERIFY3P(spa, ==, engine->spa);
	boolean_t free = zfs_range_tree_contains(engine->free, p, size);
	mutex_exit(&engine->lock);
	return (free);
}

#ifndef _KERNEL
/*
 * ztest-only placement steering.  Do not reserve or synthesize a P owner:
 * the normal allocation transaction must still find native V space and
 * atomically pair it with this already-free physical extent.
 */
int
pr19262_engine_test_prefer_p(spa_t *spa, uint64_t p, uint64_t size)
{
	engine_t *engine = spa->spa_reloc;
	int error = 0;
	mutex_enter(&engine->lock);
	if (engine->spa != spa || engine->vd == NULL || size == 0 ||
	    p + size < p || P2PHASE(p, 1ULL << engine->vd->vdev_ashift) != 0 ||
	    p + size > engine->total || !zfs_range_tree_contains(engine->free,
	    p,
	    size)) {
		error = EINVAL;
	} else if (P2ALIGN_TYPED(p, 1ULL << engine->vd->vdev_ms_shift,
	    uint64_t) != P2ALIGN_TYPED(p + size - 1,
	    1ULL << engine->vd->vdev_ms_shift, uint64_t)) {
		error = ERANGE;
	} else {
		VERIFY(!engine->test_prefer_active);
		engine->test_prefer_p = p;
		engine->test_prefer_end = p + size;
		engine->test_prefer_active = B_TRUE;
	}
	mutex_exit(&engine->lock);
	return (error);
}

/*
 * ztest only, floor-flake negative control.  [v, v + size) is a range a
 * remaining checkpoint space map entry names.  Give its first
 * checkpoint-only owner the state a discard step gives the owners of its
 * processed prefix (retired, drain_releasable) without taking the range
 * out of the map: a wrong drain_releasable flag.  Syncing context, like a
 * discard step.  Returns the owner's V size (its V in *out), or 0.
 */
uint64_t
pr19262_engine_test_flag_covered(spa_t *spa, uint64_t v, uint64_t size,
    uint64_t *out)
{
	engine_t *engine = spa->spa_reloc;
	uint64_t flagged = 0;
	mutex_enter(&engine->lock);
	if (engine->spa != spa || engine->vd == NULL) {
		mutex_exit(&engine->lock);
		return (0);
	}
	uint64_t align = 1ULL << engine->vd->vdev_ashift;
	for (uint64_t c = v; c < v + size; ) {
		engine_owner_t *e = engine_checkpoint_owner(engine, c);
		if (e == NULL) {
			c += align;
			continue;
		}
		if (e->checkpoint_only && !e->retired && e->v >= v &&
		    e->v + e->size <= v + size) {
			e->checkpoint_only = B_FALSE;
			e->retired = B_TRUE;
			e->retired_at = ++engine->sequence;
			engine->retired += e->cur->size;
			engine->revision++;
			engine_owner_dirty(engine, e);
			e->drain_releasable = B_TRUE;
			flagged = e->size;
			*out = e->v;
			break;
		}
		c = e->v + e->size;
	}
	mutex_exit(&engine->lock);
	return (flagged);
}
#endif

/* Diagnostic: the owner state behind a block pointer, at a named site. */
void
pr19262_engine_describe(spa_t *spa, const blkptr_t *bp, const char *site,
    uint64_t a, uint64_t b)
{
	engine_t *engine = spa->spa_reloc;
	const dva_t *dva = &bp->blk_dva[0];
	mutex_enter(&engine->lock);
	engine_owner_t *e = engine->spa == spa && engine->vd != NULL &&
	    DVA_GET_VDEV(dva) == engine->vd->vdev_id ?
	    engine_find(engine, DVA_GET_OFFSET(dva)) : NULL;
	uint64_t versions = 0;
	for (engine_version_t *v = e != NULL ? e->versions : NULL; v != NULL;
	    v = v->next)
		versions++;
	pr19262_printf("PR19262_DESCRIBE site=%s a=%llu b=%llu vdev=%llu "
	    "v=%llu asize=%llu pbirth=%llu lbirth=%llu type=%u level=%u "
	    "dedup=%u owner=%d size=%llu incarnation=%llu allocation_txg=%llu "
	    "retired=%u aborted=%u copying=%u holds=%llu versions=%llu "
	    "cur_birth=%llu cur_p=%llu\n", site, (u_longlong_t)a,
	    (u_longlong_t)b, (u_longlong_t)DVA_GET_VDEV(dva),
	    (u_longlong_t)DVA_GET_OFFSET(dva), (u_longlong_t)DVA_GET_ASIZE(dva),
	    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(bp),
	    (u_longlong_t)BP_GET_LOGICAL_BIRTH(bp), (uint_t)BP_GET_TYPE(bp),
	    (uint_t)BP_GET_LEVEL(bp), (uint_t)BP_GET_DEDUP(bp), e != NULL,
	    (u_longlong_t)(e != NULL ? e->size : 0),
	    (u_longlong_t)(e != NULL ? e->incarnation : 0),
	    (u_longlong_t)(e != NULL ? e->allocation_txg : 0),
	    e != NULL ? e->retired : 0, e != NULL ? e->aborted : 0,
	    e != NULL ? e->copying : 0,
	    (u_longlong_t)(e != NULL ? e->holds : 0), (u_longlong_t)versions,
	    (u_longlong_t)(e != NULL && e->cur != NULL ? e->cur->birth : 0),
	    (u_longlong_t)(e != NULL && e->cur != NULL ? e->cur->p : 0));
	mutex_exit(&engine->lock);
	/* The caller may assert next: do not lose this line in a buffer. */
	pr19262_flush();
}

void
pr19262_engine_inspect(spa_t *spa, const blkptr_t *bp, int d,
    pr19262_engine_info_t *info)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	VERIFY3P(spa, ==, engine->spa);
	engine_owner_t *e = engine_find(engine,
	    DVA_GET_OFFSET(&bp->blk_dva[d]));
	VERIFY3P(e, !=, NULL);
	*info = (pr19262_engine_info_t) {
	    .p = e->cur->p, .vsize = e->size,
	    .psize = e->cur->size, .birth = e->cur->birth,
	    .leases = e->cur->leases, .incarnation = e->incarnation,
	    .holds = e->holds, .journal_seq = e->cur->journal_seq,
	    .aborted = e->aborted, .retired = e->retired,
	    .journal_touched = e->journal_touched,
	    .queued_writers = e->queued_writers, .copying = e->copying,
	    .ready = e->cur->ready, .catalog_txg = e->catalog_txg };
	for (engine_version_t *b = e->versions; b != NULL; b = b->next) {
		info->versions++;
		if (b != e->cur)
			info->old_leases += b->leases;
	}
	mutex_exit(&engine->lock);
}

static int
engine_copy_io(engine_copy_t *c, engine_version_t *b, abd_t *abd,
    zio_type_t type)
{
	engine_t *engine = c->spa->spa_reloc;
	spa_t *spa = engine->spa;
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	zio_t *zio = pr19262_engine_physical_io(spa, &c->original,
	    engine->vd, c->owner->v, abd, c->psize, type,
	    spa_last_synced_txg(spa) + 1, b, 0);
	int error = zio_wait(zio);
	spa_config_exit(spa, SCL_ZIO, FTAG);
	return (error);
}

/* A copy freed meanwhile is stale; otherwise its stage takes ready. */
static boolean_t
engine_copy_settle(engine_copy_t *c, int *error, boolean_t ready)
{
	engine_t *engine = c->spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_owner_t *e = c->owner;
	VERIFY(e->copying);
	VERIFY3P(e->cur, ==, c->source);
	VERIFY0(c->stage->leases);
	if (*error == 0 && e->retired)
		*error = ESTALE;
	boolean_t journal_attempted = *error == 0 && engine->persistent;
	if (*error == 0) {
		c->stage->ready = ready;
		engine_owner_dirty(engine, c->owner);
	}
	mutex_exit(&engine->lock);
	return (journal_attempted);
}

/* Publish the bound stage, or keep or return it, and end the copy. */
static int
engine_copy_publish(engine_copy_t *c, int error, boolean_t journal_attempted)
{
	engine_t *engine = c->spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine_owner_t *e = c->owner;
	if (error == 0 && e->retired)
		error = ESTALE;
	if (error == 0) {
		e->cur = c->stage;
		c->source->retired = B_TRUE;
		engine->retired += c->source->size;
		engine->copied++;
		engine->revision++;
		/* The next catalogs list the published version. */
		if (journal_attempted)
			engine_journal_fold(engine->spa, e->catalog_txg);
	} else if (journal_attempted) {
		/* A prepared record can survive a failed publication. */
		c->stage->retired = B_TRUE;
		engine->retired += c->stage->size;
		engine->copy_aborts++;
		engine->revision++;
	} else {
		VERIFY3P(e->versions, ==, c->stage);
		e->versions = c->stage->next;
		zfs_range_tree_add(engine->free, c->stage->p, c->stage->size);
		kmem_free(c->stage, sizeof (*c->stage));
		engine->copy_aborts++;
		engine->revision++;
	}
	engine_owner_dirty(engine, e);
	VERIFY3U(c->source->leases, >, 0);
	c->source->leases--;
	VERIFY3U(e->holds, >, 0);
	e->holds--;
#ifdef _KERNEL
	if (engine->r01.owner == e && engine->r01.version == c->source) {
		engine->r01.lease_terminal = c->source->leases;
		engine->r01.writers_terminal = c->source->writers;
		engine->r01.holds_terminal = e->holds;
	}
#endif
	e->copying = B_FALSE;
	cv_broadcast(&engine->changed);
	engine_pending_t *pending = e->pending;
	e->pending = NULL;
	mutex_exit(&engine->lock);
#ifdef _KERNEL
	engine_r01_boundary_wait(engine, e, PR19262_R01_PUBLISHED);
#endif
	engine_resume(pending);
	kmem_free(c, sizeof (*c));
	return (error);
}

static int
engine_copy_finish(engine_copy_t *c, int error, boolean_t ready)
{
	boolean_t journal_attempted = engine_copy_settle(c, &error, ready);
	if (journal_attempted)
		error = engine_journal_bind_copy(c);
	return (engine_copy_publish(c, error, journal_attempted));
}

int
pr19262_engine_copy_run(void *arg, boolean_t fail_verify)
{
	engine_t *engine = ((engine_copy_t *)arg)->spa->spa_reloc;
	engine_copy_t *c = arg;
	abd_t *source = abd_alloc_for_io(c->psize, B_FALSE);
	abd_t *verify = abd_alloc_for_io(c->psize, B_FALSE);
	int error = engine_copy_io(c, c->source, source, ZIO_TYPE_READ);
	if (error == 0)
		error = engine_copy_io(c, c->stage, source, ZIO_TYPE_WRITE);
	if (error == 0 && fail_verify) {
		/* Corrupt real staging bytes for the negative control. */
		abd_copy(verify, source, c->psize);
		uint64_t corrupt;
		abd_copy_to_buf(&corrupt, verify, sizeof (corrupt));
		corrupt ^= 1;
		abd_copy_from_buf_off(verify, &corrupt, 0, sizeof (corrupt));
		error = engine_copy_io(c, c->stage, verify, ZIO_TYPE_WRITE);
	}
	if (error == 0)
		error = engine_copy_io(c, c->stage, verify, ZIO_TYPE_READ);
	if (error == 0 && abd_cmp(source, verify) != 0)
		error = ECKSUM;
	abd_free(source);
	abd_free(verify);
	/* Flush data before publishing its J binding: every member at once. */
	if (error == 0 && engine->persistent) {
		spa_t *spa = engine->spa;
		engine_quorum_t q = { 0 };
		spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
		engine_members_flush(spa, &q);
		spa_config_exit(spa, SCL_ZIO, FTAG);
		error = engine_quorum_result(engine, &q);
	}
	return (engine_copy_finish(c, error, B_TRUE));
}

/*
 * Copy a group of extents with each step issued for all of them at once:
 * the reads, the writes, the verifying reads, then one flush of every
 * member before one write of all their journal frames and one head.  A
 * copy run alone pays the three flush rounds by itself.  errors[i]
 * receives each copy's result; every handle is consumed.  A source that
 * may never have been written (speculative[i], a log chain's tail) is
 * read without charging its checksum failure to the children.
 */
void
pr19262_engine_copy_group(void **handles, uint_t n,
    const boolean_t *speculative, int *errors)
{
	if (n == 0)
		return;
	engine_t *engine = ((engine_copy_t *)handles[0])->spa->spa_reloc;
	hrtime_t start = gethrtime();
	uint64_t bytes = 0;
	hrtime_t io[3];
	engine_copy_t **cv = (engine_copy_t **)handles;
	spa_t *spa = engine->spa;
	abd_t **source = kmem_alloc(n * sizeof (abd_t *), KM_SLEEP);
	abd_t **verify = kmem_alloc(n * sizeof (abd_t *), KM_SLEEP);
	zio_t **zio = kmem_alloc(n * sizeof (zio_t *), KM_SLEEP);
	boolean_t *attempted = kmem_alloc(n * sizeof (boolean_t), KM_SLEEP);
	for (uint_t i = 0; i < n; i++) {
		VERIFY3P(cv[i]->spa, ==, spa);
		errors[i] = 0;
		bytes += cv[i]->psize;
		source[i] = abd_alloc_for_io(cv[i]->psize, B_FALSE);
		verify[i] = abd_alloc_for_io(cv[i]->psize, B_FALSE);
	}
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	hrtime_t setup = gethrtime();
	/* Read the sources, write the stages, read the stages back. */
	for (uint_t step = 0; step < 3; step++) {
		hrtime_t before = gethrtime();
		for (uint_t i = 0; i < n; i++) {
			engine_copy_t *c = cv[i];
			zio[i] = errors[i] != 0 ? NULL :
			    pr19262_engine_physical_io(spa, &c->original,
			    engine->vd, c->owner->v,
			    step == 2 ? verify[i] : source[i], c->psize,
			    step == 1 ? ZIO_TYPE_WRITE : ZIO_TYPE_READ,
			    spa_last_synced_txg(spa) + 1,
			    step == 0 ? c->source : c->stage,
			    step == 0 && speculative != NULL &&
			    speculative[i] ? ZIO_FLAG_SPECULATIVE : 0);
		}
		for (uint_t i = 0; i < n; i++)
			if (zio[i] != NULL)
				errors[i] = zio_wait(zio[i]);
		io[step] = gethrtime() - before;
	}
	hrtime_t readback = gethrtime();
	boolean_t any = B_FALSE;
	for (uint_t i = 0; i < n; i++) {
		if (errors[i] == 0 && abd_cmp(source[i], verify[i]) != 0)
			errors[i] = ECKSUM;
		any |= errors[i] == 0;
		abd_free(source[i]);
		abd_free(verify[i]);
	}
	hrtime_t checked = gethrtime();
	/* Flush data before publishing its J bindings: every member at once. */
	if (any && engine->persistent) {
		engine_quorum_t q = { 0 };
		engine_members_flush(spa, &q);
		int error = engine_quorum_result(engine, &q);
		for (uint_t i = 0; i < n; i++)
			if (errors[i] == 0)
				errors[i] = error;
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	hrtime_t flushed = gethrtime();
	for (uint_t i = 0; i < n; i++)
		attempted[i] = engine_copy_settle(cv[i], &errors[i], B_TRUE);
	hrtime_t settled = gethrtime();
	engine_journal_bind_copies(cv, n, attempted, errors);
	hrtime_t bound = gethrtime();
	for (uint_t i = 0; i < n; i++)
		errors[i] = engine_copy_publish(cv[i], errors[i],
		    attempted[i]);
	kmem_free(attempted, n * sizeof (boolean_t));
	kmem_free(zio, n * sizeof (zio_t *));
	kmem_free(verify, n * sizeof (abd_t *));
	kmem_free(source, n * sizeof (abd_t *));
	hrtime_t end = gethrtime();
	atomic_inc_64(&engine->copy_cost.groups);
	atomic_add_64(&engine->copy_cost.blocks, n);
	atomic_add_64(&engine->copy_cost.bytes, bytes);
	atomic_add_64(&engine->copy_cost.setup, setup - start);
	for (uint_t step = 0; step < 3; step++)
		atomic_add_64(&engine->copy_cost.io[step], io[step]);
	atomic_add_64(&engine->copy_cost.check, checked - readback);
	atomic_add_64(&engine->copy_cost.flush, flushed - checked);
	atomic_add_64(&engine->copy_cost.settle, settled - flushed);
	atomic_add_64(&engine->copy_cost.journal, bound - settled);
	atomic_add_64(&engine->copy_cost.publish, end - bound);
	atomic_add_64(&engine->copy_cost.total, end - start);
}

int
pr19262_engine_copy_begin(spa_t *spa, const blkptr_t *bp, int d,
    uint64_t birth, void **handle)
{
	return (engine_copy_begin(spa, bp, d, birth, handle, B_FALSE,
	    B_FALSE));
}

/*
 * A log block allocated ahead of its write carries nothing to copy: the
 * chain ends at it until the ZIL writes it.  Bind it to a fresh backing
 * of the requested layout so that write lands in the stronger layout.
 */
int
pr19262_engine_rebind_unwritten(spa_t *spa, const blkptr_t *bp, int d,
    uint64_t birth)
{
	void *handle;
	int error = engine_copy_begin(spa, bp, d, birth, &handle, B_TRUE,
	    B_FALSE);
	if (error != 0)
		return (error);
	return (engine_copy_finish(handle, 0, B_FALSE));
}

void
pr19262_engine_check(spa_t *spa, const char *phase)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	VERIFY3P(spa, ==, engine->spa);
	uint64_t owned = 0, holds = 0, different = 0, leases = 0;
	uint64_t versions = 0, checkpoint_only = 0;
	uint64_t count = avl_numnodes(&engine->owners);
	zfs_range_seg64_t *virtual = pr19262_alloc(count * sizeof (*virtual),
	    KM_SLEEP);
	boolean_t *pending = kmem_alloc(count * sizeof (*pending), KM_SLEEP);
	uint64_t n = 0;
	zfs_range_tree_t *seen = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	for (engine_owner_t *e = avl_first(&engine->owners); e != NULL;
	    e = AVL_NEXT(&engine->owners, e)) {
		for (engine_version_t *b = e->versions; b != NULL;
		    b = b->next) {
			zfs_range_tree_verify_not_present(engine->free,
			    b->p, b->size);
			owned += b->size;
			zfs_range_tree_add(seen, b->p, b->size);
			leases += b->leases;
			versions++;
		}
		VERIFY(!e->aborted);
		pending[n] = e->journal_pending;
		virtual[n++] = (zfs_range_seg64_t) { e->v, e->v + e->size };
		holds += e->holds;
		checkpoint_only += e->checkpoint_only;
		different += e->v != e->cur->p;
	}
	VERIFY3U(owned + zfs_range_tree_space(engine->free) +
	    zfs_range_tree_space(engine->legacy) +
	    zfs_range_tree_space(engine->unusable) +
	    (engine->store_extents == NULL ? 0 :
	    zfs_range_tree_space(engine->store_extents)) +
	    (engine->journal_extents == NULL ? 0 :
	    zfs_range_tree_space(engine->journal_extents)) +
	    engine_reuse_bytes(engine), ==, engine->total);
	engine_pages_reserved_check_locked(engine);
	if (engine->store_extents != NULL)
		zfs_range_tree_walk(engine->store_extents,
		    zfs_range_tree_add, seen);
	if (engine->journal_extents != NULL)
		zfs_range_tree_walk(engine->journal_extents,
		    zfs_range_tree_add, seen);
	if (engine->reuse_open != NULL) {
		zfs_range_tree_walk(engine->reuse_open, zfs_range_tree_add,
		    seen);
		for (uint_t i = 0; i < ENGINE_REUSE_BATCHES; i++)
			zfs_range_tree_walk(engine->reuse_batch[i],
			    zfs_range_tree_add, seen);
	}
	zfs_range_tree_walk(engine->free, zfs_range_tree_add, seen);
	zfs_range_tree_walk(engine->legacy, zfs_range_tree_add, seen);
	zfs_range_tree_walk(engine->unusable, zfs_range_tree_add, seen);
	VERIFY3U(zfs_range_tree_space(seen), ==, engine->total);
	zfs_range_tree_vacate(seen, NULL, NULL);
	zfs_range_tree_destroy(seen);
	pr19262_printf("PR19262_ENGINE %s owners=%llu different=%llu "
	    "owned=%llu free=%llu retired=%llu holds=%llu pairs=%llu "
	    "aborts=%llu io=%llu metadata=%llu legacy=%llu versions=%llu "
	    "leases=%llu copied=%llu copy_aborts=%llu deferred=%llu "
	    "checkpoint=%llu cp_catalog=%llu checkpoint_only=%llu "
	    "ckpt_until=%llu ckpt_pins=%llu ckpt_unpins=%llu\n", phase,
	    (u_longlong_t)avl_numnodes(&engine->owners),
	    (u_longlong_t)different, (u_longlong_t)owned,
	    (u_longlong_t)zfs_range_tree_space(engine->free),
	    (u_longlong_t)engine->retired, (u_longlong_t)holds,
	    (u_longlong_t)engine->pairs, (u_longlong_t)engine->aborts,
	    (u_longlong_t)engine->translated, (u_longlong_t)engine->metadata,
	    (u_longlong_t)engine->legacy_reads, (u_longlong_t)versions,
	    (u_longlong_t)leases, (u_longlong_t)engine->copied,
	    (u_longlong_t)engine->copy_aborts, (u_longlong_t)engine->deferred,
	    (u_longlong_t)engine->checkpoint.ub_txg,
	    (u_longlong_t)engine->checkpoint.ub_pr19262[6],
	    (u_longlong_t)checkpoint_only,
	    (u_longlong_t)engine->ckpt_until,
	    (u_longlong_t)engine->ckpt_pinned_pages,
	    (u_longlong_t)engine->ckpt_unpins);
	mutex_exit(&engine->lock);
	/* Independent native V allocation checks, outside the catalog lock. */
	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	for (uint64_t i = 0; i < n; i++) {
		int claim = metaslab_claim_impl(engine->vd, virtual[i].rs_start,
		    virtual[i].rs_end - virtual[i].rs_start, 0);
		int expected = pending[i] ? 0 : ENOENT;
		if (claim != expected) {
			mutex_enter(&engine->lock);
			engine_owner_t *e = engine_find(engine,
			    virtual[i].rs_start);
			pr19262_printf("PR19262_V_CLAIM v=%llu "
			    "size=%llu allocation=%llu retired=%u result=%d "
			    "ub=%llu\n", (u_longlong_t)e->v,
			    (u_longlong_t)e->size,
			    (u_longlong_t)e->allocation_txg, e->retired, claim,
			    (u_longlong_t)spa_last_synced_txg(spa));
			mutex_exit(&engine->lock);
		}
		VERIFY3S(claim, ==, expected);
	}
	spa_config_exit(spa, SCL_ALL, FTAG);
	kmem_free(virtual, count * sizeof (*virtual));
	kmem_free(pending, count * sizeof (*pending));
}

static int engine_journal_format(spa_t *);
static int engine_journal_recover(spa_t *);
#include "vdev_reloc_store.c"
#include "vdev_reloc_journal.c"
#include "vdev_reloc_convert.c"
#include "vdev_reloc_admit.c"

#ifdef _KERNEL
/* The worker join completes only after its transferred SPA ref is closed. */
static void
engine_r01_kernel_wait_idle(engine_t *engine)
{
	engine_kernel_join_wait(engine->kernel);
}

#endif

void
pr19262_engine_spa_init(spa_t *spa)
{
	engine_t *engine = kmem_zalloc(sizeof (*engine), KM_SLEEP);
	engine->pages = kmem_zalloc(sizeof (*engine->pages), KM_SLEEP);
	engine->pages->root.p = UINT64_MAX;
	mutex_init(&engine->lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&engine->journal_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&engine->store_lock, NULL, MUTEX_DEFAULT, NULL);
	rw_init(&engine->r02_topology_lock, NULL, RW_DEFAULT, NULL);
	cv_init(&engine->changed, NULL, CV_DEFAULT, NULL);
	cv_init(&engine->fence_cv, NULL, CV_DEFAULT, NULL);
	cv_init(&engine->journal_group_cv, NULL, CV_DEFAULT, NULL);
	list_create(&engine->journal_reqs, sizeof (engine_j_req_t),
	    offsetof(engine_j_req_t, node));
	avl_create(&engine->owners, engine_compare, sizeof (engine_owner_t),
	    offsetof(engine_owner_t, avl));
	list_create(&engine->dirty, sizeof (engine_owner_t),
	    offsetof(engine_owner_t, dirty_node));
	avl_create(&engine->tombstones, engine_tomb_compare,
	    sizeof (engine_tomb_t), offsetof(engine_tomb_t, avl));
	spa->spa_reloc = engine;
#ifdef _KERNEL
	engine->kernel = kmem_zalloc(sizeof (*engine->kernel), KM_SLEEP);
	mutex_init(&engine->kernel->lock, NULL, MUTEX_DEFAULT, NULL);
	engine->kernel->join = engine_kernel_join_alloc();
	(void) strlcpy(engine->kernel->state, "idle", ENGINE_KERNEL_STATE);
#endif
}

#ifdef _KERNEL
static void
engine_r01_cancel_selected(spa_t *spa)
{
	boolean_t selector = engine_selector_r01_take(spa);
	(void) pr19262_engine_r01_cancel(spa);
	if (selector)
		engine_selector_r01_put_taken(spa);
}
#endif

void
pr19262_engine_zil_prepare(spa_t *spa)
{
#ifdef _KERNEL
	if (spa == NULL || spa->spa_reloc == NULL)
		return;
	/* Keep arm closed through zil_close(), then cancel an existing root. */
	engine_kernel_zil_begin(spa);
	engine_r01_cancel_selected(spa);
#else
	(void) spa;
#endif
}

void
pr19262_engine_zil_finish(spa_t *spa)
{
#ifdef _KERNEL
	if (spa != NULL && spa->spa_reloc != NULL)
		engine_kernel_zil_finish(spa);
#else
	(void) spa;
#endif
}

void
pr19262_engine_export_prepare(spa_t *spa)
{
#ifdef _KERNEL
	if (spa == NULL || spa->spa_reloc == NULL)
		return;
	engine_kernel_export_begin(spa);
	engine_r01_cancel_selected(spa);
	/*
	 * Export closed public admission, so draining existing workers
	 * terminates.
	 */
	engine_r01_kernel_wait_idle(spa->spa_reloc);
#else
	(void) spa;
#endif
}

void
pr19262_engine_export_abort(spa_t *spa)
{
#ifdef _KERNEL
	if (spa != NULL && spa->spa_reloc != NULL)
		engine_kernel_export_abort(spa);
#else
	(void) spa;
#endif
}

void
pr19262_engine_unload_prepare(spa_t *spa)
{
#ifdef _KERNEL
	if (spa == NULL || spa->spa_reloc == NULL)
		return;
	engine_t *engine = spa->spa_reloc;
	boolean_t selector = engine_selector_r01_take(spa);

	/*
	 * R-01 cancel resumes/waits the root and advances final-free waiting.
	 */
	(void) pr19262_engine_r01_cancel(spa);
	engine_kernel_unload_begin(engine->kernel);
	if (selector)
		engine_selector_r01_put_taken(spa);
#else
	(void) spa;
#endif
}

void
pr19262_engine_spa_fini(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	/* Drain the raw R02 observer before its private topology disappears. */
	rw_enter(&engine->r02_topology_lock, RW_WRITER);
	mutex_enter(&engine->store_lock);
	mutex_enter(&engine->lock);
	VERIFY(!engine->fence_pending);
	VERIFY3P(engine->parked, ==, NULL);
#ifdef _KERNEL
	/* spa_unload() waited the detached root before dsl_pool teardown. */
	VERIFY3P(engine->r01.scan_root, ==, NULL);
	VERIFY(!engine->r01.scan_waiting);
	VERIFY3P(engine->r01.prebound, ==, NULL);
	VERIFY3P(engine->r01.selected, ==, NULL);
	VERIFY(!engine->r01.bound_lease);
	VERIFY(!engine->r01.copy_running);
#endif
	engine_store_clear(engine);
	mutex_exit(&engine->lock);
	mutex_exit(&engine->store_lock);
#ifdef _KERNEL
	VERIFY(!engine->kernel->running);
	engine_kernel_join_rele(engine->kernel->join);
	mutex_destroy(&engine->kernel->lock);
	kmem_free(engine->kernel, sizeof (*engine->kernel));
#endif
	avl_destroy(&engine->owners);
	avl_destroy(&engine->tombstones);
	list_destroy(&engine->dirty);
	if (engine->gone != NULL)
		kmem_free(engine->gone,
		    engine->gone_capacity * 2 * sizeof (uint64_t));
	cv_destroy(&engine->changed);
	cv_destroy(&engine->fence_cv);
	VERIFY(!engine->journal_leader);
	VERIFY(list_is_empty(&engine->journal_reqs));
	list_destroy(&engine->journal_reqs);
	cv_destroy(&engine->journal_group_cv);
	mutex_destroy(&engine->journal_lock);
	mutex_destroy(&engine->store_lock);
	rw_exit(&engine->r02_topology_lock);
	rw_destroy(&engine->r02_topology_lock);
	mutex_destroy(&engine->lock);
#ifndef _KERNEL
	if (engine->test_legacy_mark != NULL) {
		zfs_range_tree_vacate(engine->test_legacy_mark, NULL, NULL);
		zfs_range_tree_destroy(engine->test_legacy_mark);
	}
	if (engine->test_holes != NULL) {
		zfs_range_tree_vacate(engine->test_holes, NULL, NULL);
		zfs_range_tree_destroy(engine->test_holes);
	}
#endif
	kmem_free(engine->pages, sizeof (*engine->pages));
	kmem_free(engine, sizeof (*engine));
	spa->spa_reloc = NULL;
}

#ifndef _KERNEL
/* Growth-defer test: mark a native extent, also before activation. */
int
pr19262_engine_test_mark(spa_t *spa, uint64_t start, uint64_t size)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->test_legacy_mark == NULL)
		engine->test_legacy_mark = zfs_range_tree_create(NULL,
		    ZFS_RANGE_SEG64, NULL, 0, 0);
	zfs_range_tree_clear(engine->test_legacy_mark, start, size);
	zfs_range_tree_add(engine->test_legacy_mark, start, size);
	mutex_exit(&engine->lock);
	return (0);
}

static void
engine_test_clear(void *arg, uint64_t start, uint64_t size)
{
	zfs_range_tree_clear(arg, start, size);
}

static boolean_t
engine_test_overlaps(zfs_range_tree_t *rt, uint64_t p, uint64_t size)
{
	uint64_t found, length;
	return (rt != NULL && zfs_range_tree_find_in(rt, p, size, &found,
	    &length));
}

/*
 * The highest arena-sized, stripe-aligned run of marked space that is not
 * legacy now, and every structure it overlaps.  elsewhere: in none of
 * them (with the reuse deferral: deferred space).
 */
int
pr19262_engine_test_reuse_probe(spa_t *spa, pr19262_reuse_probe_t *out)
{
	engine_t *engine = spa->spa_reloc;
	memset(out, 0, sizeof (*out));
	out->p = UINT64_MAX;
	mutex_enter(&engine->lock);
	if (engine->spa != spa || engine->test_legacy_mark == NULL ||
	    engine->legacy == NULL) {
		mutex_exit(&engine->lock);
		return (EINVAL);
	}
	uint64_t bytes = ENGINE_STORE_BYTES * engine->vd->vdev_children;
	zfs_range_tree_t *left = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	zfs_range_tree_walk(engine->test_legacy_mark, zfs_range_tree_add,
	    left);
	zfs_range_tree_walk(engine->legacy, engine_test_clear, left);
	out->left = zfs_range_tree_space(left);
	uint64_t range[2] = { UINT64_MAX, bytes };
	engine_walk_arg_t walk = { engine, range };
	zfs_range_tree_walk(left, engine_pages_pick, &walk);
	zfs_range_tree_vacate(left, NULL, NULL);
	zfs_range_tree_destroy(left);
	out->bytes = bytes;
	uint64_t p = range[0];
	if (p != UINT64_MAX) {
		out->p = p;
		out->in_free = zfs_range_tree_contains(engine->free, p, bytes);
		out->any_free = engine_test_overlaps(engine->free, p, bytes);
		out->in_legacy = engine_test_overlaps(engine->legacy, p, bytes);
		out->in_store = engine_test_overlaps(engine->store_extents, p,
		    bytes);
		out->in_journal = engine_test_overlaps(engine->journal_extents,
		    p, bytes);
		for (engine_owner_t *e = avl_first(&engine->owners);
		    e != NULL && !out->owned; e = AVL_NEXT(&engine->owners, e))
			for (engine_version_t *b = e->versions; b != NULL;
			    b = b->next)
				if (b->p < p + bytes && p < b->p + b->size)
					out->owned = 1;
		out->elsewhere = !out->any_free && !out->in_legacy &&
		    !out->in_store && !out->in_journal && !out->owned;
	}
	out->durable_ub = engine->durable_ub_txg;
	out->durable_catalog = engine->durable_catalog_txg;
	out->stored_catalog = engine->stored_desc[6];
	out->free_bytes = zfs_range_tree_space(engine->free);
	out->capacity = engine->pages->capacity;
	out->steer = engine->test_arena_result;
	mutex_exit(&engine->lock);
	return (0);
}

/* The next engine_pages_grow takes p if it is free (or, forced, anyway). */
int
pr19262_engine_test_prefer_arena(spa_t *spa, uint64_t p, boolean_t force)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->spa != spa || engine->vd == NULL ||
	    p % (ENGINE_STORE_PAGE * engine->vd->vdev_children) != 0) {
		mutex_exit(&engine->lock);
		return (EINVAL);
	}
	engine->test_arena_p = p;
	engine->test_arena_force = force;
	engine->test_arena_active = B_TRUE;
	mutex_exit(&engine->lock);
	return (0);
}

/*
 * Growth-defer test: native frees that straddle activation.  A free synced
 * after engine_start's own deferral turnover and before its scan is still in
 * ms_defer when the scan runs: legacy in the first catalog, and returned by
 * metaslab_sync_done (engine_native_free) after a store lists it.
 */
void
pr19262_engine_test_start_hook(spa_t *spa, void (*fn)(spa_t *, void *),
    void *arg)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	engine->test_start_fn = fn;
	engine->test_start_arg = arg;
	mutex_exit(&engine->lock);
}

#ifdef ENGINE_REUSE_BATCHES
typedef struct engine_test_take {
	uint64_t bytes;
	uint64_t start;
} engine_test_take_t;

static void
engine_test_take_cb(void *arg, uint64_t start, uint64_t size)
{
	engine_test_take_t *t = arg;
	if (size >= t->bytes)
		t->start = start;
}

int
pr19262_engine_test_defer_free(spa_t *spa, uint64_t bytes, uint64_t *deferred)
{
	engine_t *engine = spa->spa_reloc;
	engine_test_take_t t = { bytes, UINT64_MAX };
	mutex_enter(&engine->lock);
	if (engine->spa != spa || !engine->persistent) {
		mutex_exit(&engine->lock);
		return (EINVAL);
	}
	if (bytes != 0) {
		/* The highest free segment that holds them. */
		zfs_range_tree_walk(engine->free, engine_test_take_cb, &t);
		if (t.start == UINT64_MAX) {
			mutex_exit(&engine->lock);
			return (ENOSPC);
		}
		zfs_range_tree_remove(engine->free, t.start, bytes);
		engine_reuse_add(engine, t.start, bytes);
	}
	*deferred = engine_reuse_bytes(engine);
	mutex_exit(&engine->lock);
	return (0);
}
#else
int
pr19262_engine_test_defer_free(spa_t *spa, uint64_t bytes, uint64_t *deferred)
{
	(void) spa, (void) bytes;
	*deferred = 0;
	return (ENOTSUP);
}
#endif

static void
engine_test_holes_cb(void *arg, uint64_t start, uint64_t size)
{
	zfs_range_tree_t *holes = arg;
	uint64_t at = P2ROUNDUP(start, 8ULL << 20);
	if (at == start)
		at += 8ULL << 20;
	for (; at < start + size; at += 8ULL << 20)
		zfs_range_tree_add(holes, at - 4096, 4096);
}

int
pr19262_engine_test_free_holes(spa_t *spa, boolean_t carve)
{
	engine_t *engine = spa->spa_reloc;
	mutex_enter(&engine->lock);
	if (engine->spa != spa || !engine->persistent ||
	    (carve && engine->test_holes != NULL) ||
	    (!carve && engine->test_holes == NULL)) {
		mutex_exit(&engine->lock);
		return (EINVAL);
	}
	if (carve) {
		engine->test_holes = zfs_range_tree_create(NULL,
		    ZFS_RANGE_SEG64, NULL, 0, 0);
		zfs_range_tree_walk(engine->free, engine_test_holes_cb,
		    engine->test_holes);
		zfs_range_tree_walk(engine->test_holes, engine_test_clear,
		    engine->free);
	} else {
		zfs_range_tree_vacate(engine->test_holes, zfs_range_tree_add,
		    engine->free);
		zfs_range_tree_destroy(engine->test_holes);
		engine->test_holes = NULL;
	}
	mutex_exit(&engine->lock);
	return (0);
}
#endif
