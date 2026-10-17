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

#endif
#endif
