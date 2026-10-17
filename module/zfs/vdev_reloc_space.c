/* SPDX-License-Identifier: CDDL-1.0 */
/*
 * Private userspace allocator feasibility, included by metaslab.c.
 * V uses loaded metaslab allocation policy/claim; P has a separate free tree
 * seeded from the same native maps. Metadata is readonly and never dirtied.
 * This does not implement persistent dual allocators or normal TXG hooks.
 */
#include <sys/vdev_reloc_probe.h>

typedef struct probe_stage_node {
	list_node_t link;
	pr19262_stage_t token;
	boolean_t published;
} probe_stage_node_t;

typedef struct probe_space {
	spa_t *spa;
	vdev_t *vd;
	kmutex_t lock;
	zfs_range_tree_t *physical_free;
	uint64_t reserved;
	uint64_t pairs;
	list_t stages;
	uint64_t next_owner;
} probe_space_t;

void *
pr19262_space_create(spa_t *spa)
{
	VERIFY(!spa_writeable(spa));
	probe_space_t *s = kmem_zalloc(sizeof (*s), KM_SLEEP);
	s->spa = spa;
	mutex_init(&s->lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&s->stages, sizeof (probe_stage_node_t),
	    offsetof(probe_stage_node_t, link));
	s->physical_free = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	s->vd = vdev_lookup_top(spa, 0);
	VERIFY3P(s->vd, !=, NULL);
	for (uint64_t i = 0; i < s->vd->vdev_ms_count; i++) {
		metaslab_t *m = s->vd->vdev_ms[i];
		mutex_enter(&m->ms_lock);
		VERIFY0(metaslab_load(m));
		zfs_range_tree_walk(m->ms_allocatable, zfs_range_tree_add,
		    s->physical_free);
		mutex_exit(&m->ms_lock);
	}
	spa_config_exit(spa, SCL_ALL, FTAG);
	return (s);
}

void
pr19262_space_destroy(void *arg)
{
	probe_space_t *s = arg;
	VERIFY0(s->reserved);
	VERIFY0(s->pairs);
	VERIFY(list_is_empty(&s->stages));
	list_destroy(&s->stages);
	zfs_range_tree_vacate(s->physical_free, NULL, NULL);
	zfs_range_tree_destroy(s->physical_free);
	mutex_destroy(&s->lock);
	kmem_free(s, sizeof (*s));
}

typedef struct probe_pick {
	uint64_t size;
	uint64_t alignment;
	uint64_t ms_shift;
	uint64_t found;
} probe_pick_t;

static void
probe_stage_pick(void *arg, uint64_t start, uint64_t size)
{
	probe_pick_t *p = arg;
	if (p->found != UINT64_MAX)
		return;
	uint64_t end = start + size;
	uint64_t offset = P2ROUNDUP(start, p->alignment);
	while (offset < end) {
		uint64_t boundary = P2ALIGN_TYPED(offset,
		    1ULL << p->ms_shift, uint64_t) +
		    (1ULL << p->ms_shift);
		uint64_t limit = MIN(end, boundary);
		if (p->size <= limit - offset) {
			p->found = offset;
			return;
		}
		offset = boundary;
	}
}

int
pr19262_space_stage_alloc(void *arg, uint64_t asize, pr19262_stage_t *token)
{
	probe_space_t *s = arg;
	int error = ENOSPC;
	mutex_enter(&s->lock);
	if (s->reserved < asize)
		goto out;
	probe_pick_t p = { asize, 1ULL << s->vd->vdev_ashift,
		s->vd->vdev_ms_shift, UINT64_MAX };
	zfs_range_tree_walk(s->physical_free, probe_stage_pick, &p);
	if (p.found != UINT64_MAX) {
		probe_stage_node_t *n = kmem_zalloc(sizeof (*n), KM_SLEEP);
		n->token = (pr19262_stage_t) { p.found, asize,
			++s->next_owner };
		list_insert_tail(&s->stages, n);
		zfs_range_tree_remove(s->physical_free, p.found, asize);
		s->reserved -= asize;
		*token = n->token;
		error = 0;
	}
out:
	mutex_exit(&s->lock);
	return (error);
}

static probe_stage_node_t *
probe_stage_find(probe_space_t *s, const pr19262_stage_t *token)
{
	ASSERT(MUTEX_HELD(&s->lock));
	for (probe_stage_node_t *n = list_head(&s->stages); n != NULL;
	    n = list_next(&s->stages, n)) {
		if (n->token.owner == token->owner &&
		    n->token.offset == token->offset &&
		    n->token.asize == token->asize)
			return (n);
	}
	return (NULL);
}

int
pr19262_space_stage_publish(void *arg, const pr19262_stage_t *token)
{
	probe_space_t *s = arg;
	int error = EINVAL;
	mutex_enter(&s->lock);
	probe_stage_node_t *n = probe_stage_find(s, token);
	if (n != NULL) {
		error = n->published ? EALREADY : 0;
		n->published = B_TRUE;
	}
	mutex_exit(&s->lock);
	return (error);
}

int
pr19262_space_stage_free(void *arg, const pr19262_stage_t *token,
    boolean_t floor)
{
	probe_space_t *s = arg;
	int error = EINVAL;
	mutex_enter(&s->lock);
	probe_stage_node_t *n = probe_stage_find(s, token);
	if (n != NULL) {
		if (n->published && !floor) {
			error = EAGAIN;
		} else {
			zfs_range_tree_verify_not_present(s->physical_free,
			    token->offset, token->asize);
			zfs_range_tree_add(s->physical_free,
			    token->offset, token->asize);
			list_remove(&s->stages, n);
			kmem_free(n, sizeof (*n));
			error = 0;
		}
	}
	mutex_exit(&s->lock);
	return (error);
}

int
pr19262_space_allocate_at(void *arg, uint64_t asize, uint64_t txg,
    uint64_t virtual_hint, uint64_t physical_hint, pr19262_space_pair_t *pair)
{
	probe_space_t *s = arg;
	int error = ENOSPC;
	VERIFY(!spa_writeable(s->spa));
	spa_config_enter(s->spa, SCL_ALL, FTAG, RW_READER);
	mutex_enter(&s->lock);
	if (s->reserved >= asize && zfs_range_tree_contains(s->physical_free,
	    physical_hint, asize)) {
		error = metaslab_claim_impl(s->vd, virtual_hint, asize, txg);
		if (error == 0) {
			zfs_range_tree_remove(s->physical_free,
			    physical_hint, asize);
			s->reserved -= asize;
			s->pairs++;
			*pair = (pr19262_space_pair_t) {
				virtual_hint, asize, physical_hint, asize };
		}
	}
	mutex_exit(&s->lock);
	spa_config_exit(s->spa, SCL_ALL, FTAG);
	return (error);
}

uint64_t
pr19262_space_available(void *arg, boolean_t physical)
{
	probe_space_t *s = arg;
	uint64_t bytes = 0;
	mutex_enter(&s->lock);
	if (physical) {
		bytes = zfs_range_tree_space(s->physical_free) - s->reserved;
	} else {
		for (uint64_t i = 0; i < s->vd->vdev_ms_count; i++) {
			metaslab_t *m = s->vd->vdev_ms[i];
			mutex_enter(&m->ms_lock);
			bytes += zfs_range_tree_space(m->ms_allocatable);
			mutex_exit(&m->ms_lock);
		}
	}
	mutex_exit(&s->lock);
	return (bytes);
}

int
pr19262_space_reserve(void *arg, uint64_t bytes)
{
	probe_space_t *s = arg;
	int error = 0;
	mutex_enter(&s->lock);
	if (bytes > zfs_range_tree_space(s->physical_free) - s->reserved)
		error = ENOSPC;
	else
		s->reserved += bytes;
	mutex_exit(&s->lock);
	return (error);
}

void
pr19262_space_unreserve(void *arg, uint64_t bytes)
{
	probe_space_t *s = arg;
	mutex_enter(&s->lock);
	VERIFY3U(s->reserved, >=, bytes);
	s->reserved -= bytes;
	mutex_exit(&s->lock);
}

int
pr19262_space_reclaim(void *arg, uint64_t offset, uint64_t size,
    boolean_t floor)
{
	probe_space_t *s = arg;
	int error = 0;
	mutex_enter(&s->lock);
	if (!floor)
		error = EAGAIN;
	else if (zfs_range_tree_contains(s->physical_free, offset, size))
		error = EEXIST;
	else {
		VERIFY3U(offset, <=, s->vd->vdev_asize);
		VERIFY3U(size, <=, s->vd->vdev_asize - offset);
		zfs_range_tree_verify_not_present(s->physical_free,
		    offset, size);
		zfs_range_tree_add(s->physical_free, offset, size);
	}
	mutex_exit(&s->lock);
	return (error);
}

int
pr19262_space_allocate(void *arg, uint64_t asize, uint64_t txg,
    uint64_t physical_hint, pr19262_space_pair_t *pair)
{
	probe_space_t *s = arg;
	uint64_t found = UINT64_MAX;
	int error = ENOSPC;
	VERIFY(!spa_writeable(s->spa));
	spa_config_enter(s->spa, SCL_ALL, FTAG, RW_READER);
	mutex_enter(&s->lock);
	if (s->reserved < asize || !zfs_range_tree_contains(s->physical_free,
	    physical_hint, asize))
		goto out;
	for (uint64_t i = 0; i < s->vd->vdev_ms_count; i++) {
		metaslab_t *m = s->vd->vdev_ms[i];
		uint64_t actual = asize;
		mutex_enter(&m->ms_lock);
		found = m->ms_group->mg_class->mc_ops->msop_alloc(m,
		    asize, asize, txg, &actual);
		mutex_exit(&m->ms_lock);
		if (found != UINT64_MAX) {
			VERIFY3U(actual, ==, asize);
			error = metaslab_claim_impl(s->vd, found, asize, txg);
			if (error == 0)
				break;
		}
	}
	if (error == 0) {
		zfs_range_tree_remove(s->physical_free, physical_hint, asize);
		s->reserved -= asize;
		s->pairs++;
		*pair = (pr19262_space_pair_t) {
			found, asize, physical_hint, asize };
	}
out:
	mutex_exit(&s->lock);
	spa_config_exit(s->spa, SCL_ALL, FTAG);
	return (error);
}

void
pr19262_space_release(void *arg, const pr19262_space_pair_t *pair)
{
	probe_space_t *s = arg;
	VERIFY(!spa_writeable(s->spa));
	mutex_enter(&s->lock);
	VERIFY3U(s->pairs, >, 0);
	metaslab_t *m = s->vd->vdev_ms[
	    pair->virtual_offset >> s->vd->vdev_ms_shift];
	mutex_enter(&m->ms_lock);
	zfs_range_tree_verify_not_present(m->ms_allocatable,
	    pair->virtual_offset, pair->virtual_size);
	zfs_range_tree_add(m->ms_allocatable,
	    pair->virtual_offset, pair->virtual_size);
	mutex_exit(&m->ms_lock);
	zfs_range_tree_verify_not_present(s->physical_free,
	    pair->physical_offset, pair->physical_size);
	zfs_range_tree_add(s->physical_free,
	    pair->physical_offset, pair->physical_size);
	s->pairs--;
	mutex_exit(&s->lock);
}
