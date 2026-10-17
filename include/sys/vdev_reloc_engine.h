/* SPDX-License-Identifier: CDDL-1.0 */
#ifndef _VDEV_RELOC_ENGINE_H
#define	_VDEV_RELOC_ENGINE_H
#ifndef _KERNEL
/* Private normal-path runtime engine. No persistent representation. */
void pr19262_engine_init(void);
void pr19262_engine_fini(void);
void pr19262_engine_start(spa_t *);
boolean_t pr19262_engine_active(spa_t *);
uint64_t pr19262_engine_limit(spa_t *, boolean_t);
void pr19262_engine_block_new(boolean_t);
void pr19262_engine_fail_writes(boolean_t);
void pr19262_engine_retired_limit(uint64_t);
int pr19262_engine_pair(spa_t *, vdev_t *, uint64_t, uint64_t, uint64_t);
void pr19262_engine_abort(spa_t *, vdev_t *, uint64_t, uint64_t);
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
} pr19262_engine_info_t;
void pr19262_engine_inspect(spa_t *, const blkptr_t *, int,
    pr19262_engine_info_t *);
void pr19262_engine_check(spa_t *, const char *);
zio_t *pr19262_engine_read_bound(zio_t *, spa_t *, const blkptr_t *,
    abd_t *, uint64_t, zio_done_func_t *, void *, zio_priority_t,
    zio_flag_t, const zbookmark_phys_t *, void **);
#endif
#endif
