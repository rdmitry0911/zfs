/* SPDX-License-Identifier: CDDL-1.0 */
/* Private feasibility API. No persistent representation is defined here. */
#ifndef _VDEV_RELOC_PROBE_H
#define	_VDEV_RELOC_PROBE_H
#ifndef _KERNEL

typedef struct pr19262_backing_desc {
	uint64_t offset;
	uint64_t birth;
	uint64_t asize;
} pr19262_backing_desc_t;

void pr19262_reloc_init(void);
void pr19262_reloc_fini(void);
void pr19262_tx_probe(spa_t *);
void pr19262_tx_allocated(spa_t *, metaslab_class_t *, vdev_t *,
    uint64_t, uint64_t, uint64_t);
void pr19262_tx_unallocated(spa_t *, vdev_t *, uint64_t, uint64_t,
    uint64_t);
boolean_t pr19262_tx_fail_dva(spa_t *, int);
void pr19262_tx_fail_second(boolean_t);
void pr19262_tx_check(spa_t *);
void *pr19262_reloc_hold(spa_t *, const blkptr_t *);
void pr19262_reloc_rele(void *);
int pr19262_reloc_resolve(zio_t *, pr19262_backing_desc_t *);
int pr19262_reloc_test(spa_t *, const char *);
zio_t *pr19262_reloc_read_bound(zio_t *, spa_t *, const blkptr_t *, abd_t *,
    uint64_t, zio_done_func_t *, void *, zio_priority_t, zio_flag_t,
    const zbookmark_phys_t *, void **);
void *pr19262_scan_probe_create(spa_t *, const blkptr_t *);
void pr19262_scan_probe_insert(void *, const blkptr_t *);
void pr19262_scan_probe_cancel(void *, const blkptr_t *);
void pr19262_scan_probe_destroy(void *);
int pr19262_scan_probe_issue(void *);
boolean_t pr19262_reloc_known(spa_t *, const blkptr_t *);
boolean_t pr19262_reloc_fresh_write(zio_t *);
typedef struct pr19262_space_pair {
	uint64_t virtual_offset;
	uint64_t virtual_size;
	uint64_t physical_offset;
	uint64_t physical_size;
} pr19262_space_pair_t;
void *pr19262_space_create(spa_t *);
void pr19262_space_destroy(void *);
uint64_t pr19262_space_available(void *, boolean_t);
int pr19262_space_reserve(void *, uint64_t);
void pr19262_space_unreserve(void *, uint64_t);
int pr19262_space_allocate(void *, uint64_t, uint64_t, uint64_t,
    pr19262_space_pair_t *);
void pr19262_space_release(void *, const pr19262_space_pair_t *);
int pr19262_space_reclaim(void *, uint64_t, uint64_t, boolean_t);
typedef struct pr19262_stage {
	uint64_t offset;
	uint64_t asize;
	uint64_t owner;
} pr19262_stage_t;
int pr19262_space_stage_alloc(void *, uint64_t, pr19262_stage_t *);
int pr19262_space_stage_publish(void *, const pr19262_stage_t *);
int pr19262_space_stage_free(void *, const pr19262_stage_t *, boolean_t);
int pr19262_space_allocate_at(void *, uint64_t, uint64_t, uint64_t,
    uint64_t, pr19262_space_pair_t *);
zio_t *pr19262_read_physical(spa_t *, const blkptr_t *, abd_t *, uint64_t);
zio_t *pr19262_write_physical(spa_t *, uint64_t, blkptr_t *, abd_t *,
    uint64_t);

#endif
#endif
