/* SPDX-License-Identifier: CDDL-1.0 */
#ifndef _VDEV_RELOC_ENGINE_H
#define	_VDEV_RELOC_ENGINE_H
#ifndef _KERNEL
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
void pr19262_engine_journal_ub_committed(spa_t *);
void pr19262_engine_journal_fault(uint_t);
int pr19262_engine_journal_roll(spa_t *);
void pr19262_engine_journal_roll_fault(uint_t);
int pr19262_engine_journal_wait_error(void);
void pr19262_engine_init(void);
void pr19262_engine_fini(void);
void pr19262_engine_start(spa_t *);
boolean_t pr19262_engine_active(spa_t *);
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
} pr19262_engine_info_t;
void pr19262_engine_inspect(spa_t *, const blkptr_t *, int,
    pr19262_engine_info_t *);
boolean_t pr19262_engine_free_range(spa_t *, uint64_t, uint64_t);
void pr19262_engine_check(spa_t *, const char *);
zio_t *pr19262_engine_read_bound(zio_t *, spa_t *, const blkptr_t *,
    abd_t *, uint64_t, zio_done_func_t *, void *, zio_priority_t,
    zio_flag_t, const zbookmark_phys_t *, void **);
#endif
#endif
