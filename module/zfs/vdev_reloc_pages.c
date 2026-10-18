/* SPDX-License-Identifier: CDDL-1.0 */
/* Private raw COW pages, included after the store's physical I/O helpers. */
#define	ENGINE_M_MAGIC 0x3139323632503036ULL
#define	ENGINE_M_ALLOC_MAGIC 0x3139323632413036ULL
#define	ENGINE_M_LEAF 1
#define	ENGINE_M_BRANCH 2
#define	ENGINE_M_BLOB 3
#define	ENGINE_M_LEVELS 16

typedef struct engine_mref {
	uint64_t p;
	uint64_t generation;
	zio_cksum_t hash;
} engine_mref_t;

typedef struct engine_mheader {
	uint64_t magic;
	uint64_t type;
	uint64_t count;
	uint64_t p;
	uint64_t generation;
	uint64_t pool;
	uint64_t top;
	uint64_t level;
	zio_cksum_t hash;
	uint64_t low[4];
	uint64_t high[4];
	uint64_t reserved[12];
} engine_mheader_t;

typedef struct engine_mrow {
	uint64_t key[4];
	uint64_t data[16];
} engine_mrow_t;

typedef struct engine_mchild {
	uint64_t high[4];
	engine_mref_t ref;
} engine_mchild_t;

#define	ENGINE_M_PAYLOAD (ENGINE_STORE_PAGE - sizeof (engine_mheader_t))
#define	ENGINE_M_ROWS (ENGINE_M_PAYLOAD / sizeof (engine_mrow_t))
#define	ENGINE_M_CHILDREN (ENGINE_M_PAYLOAD / sizeof (engine_mchild_t))

typedef struct engine_mnode {
	uint64_t data[ENGINE_STORE_PAGE / sizeof (uint64_t)];
	boolean_t dirty;
	boolean_t sealed;
} engine_mnode_t;

typedef struct engine_mbatch {
	spa_t *spa;
	uint64_t generation;
	uint_t pages;
	uint_t changes;
	uint8_t allocated[ENGINE_STORE_PAGES];
} engine_mbatch_t;

static struct {
	engine_mnode_t *cache[ENGINE_STORE_PAGES];
	engine_mref_t root;
	uint64_t generation;
	engine_mrow_t *rows;
	uint_t count;
} engine_pages = { .root = { .p = UINT64_MAX } };

static engine_mheader_t *
engine_mheader(engine_mnode_t *n)
{
	return ((engine_mheader_t *)n->data);
}

static void *
engine_mpayload(engine_mnode_t *n)
{
	return ((uchar_t *)n->data + sizeof (engine_mheader_t));
}

static int
engine_mkey_compare(const void *left, const void *right)
{
	const uint64_t *a = left, *b = right;
	for (uint_t i = 0; i < 4; i++)
		if (a[i] != b[i])
			return (a[i] < b[i] ? -1 : 1);
	return (0);
}

static boolean_t
engine_mindex(uint64_t p, uint64_t *index)
{
	uint64_t stripe = ENGINE_STORE_PAGE * engine.vd->vdev_children;
	if (p < engine.arena || (p - engine.arena) % stripe != 0)
		return (B_FALSE);
	*index = (p - engine.arena) / stripe;
	return (*index < ENGINE_STORE_PAGES);
}

static engine_mref_t
engine_mreference(engine_mnode_t *n)
{
	engine_mheader_t *h = engine_mheader(n);
	engine_mref_t r = { h->p, h->generation, h->hash };
	return (r);
}

static void
engine_mhash(engine_mnode_t *n, zio_cksum_t *hash)
{
	engine_mheader_t *h = engine_mheader(n);
	zio_cksum_t saved = h->hash;
	memset(&h->hash, 0, sizeof (h->hash));
	abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
	zio_cksum_t computed;
	abd_checksum_sha256(abd, ENGINE_STORE_PAGE, NULL, &computed);
	abd_free(abd);
	h->hash = saved;
	*hash = computed;
}

static int
engine_mread(spa_t *spa, const engine_mref_t *ref, engine_mnode_t **result)
{
	uint64_t index;
	if (!engine_mindex(ref->p, &index))
		return (EINVAL);
	engine_mnode_t *n = kmem_zalloc(sizeof (*n), KM_SLEEP);
	abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
	int error = ECKSUM;
	for (uint_t c = 0; c < engine.vd->vdev_children; c++) {
		if (engine_store_io(engine.vd->vdev_child[c], index, 1,
		    abd, B_FALSE) != 0)
			continue;
		engine_mheader_t *h = engine_mheader(n);
		boolean_t swap = h->magic == BSWAP_64(ENGINE_M_MAGIC);
		if (h->magic != ENGINE_M_MAGIC && !swap)
			continue;
		zio_cksum_t hash;
		engine_mhash(n, &hash);
		if (memcmp(&hash, &ref->hash, sizeof (hash)) != 0)
			continue;
		if (swap)
			byteswap_uint64_array(h, sizeof (*h));
		if (h->p != ref->p || h->generation == 0 ||
		    (ref->generation != 0 &&
		    ref->generation != h->generation) ||
		    h->pool != spa_guid(spa) ||
		    h->top != engine.vd->vdev_guid ||
		    memcmp(&h->hash, &ref->hash, sizeof (hash)) != 0 ||
		    h->level > ENGINE_M_LEVELS ||
		    (h->type != ENGINE_M_LEAF && h->type != ENGINE_M_BRANCH &&
		    h->type != ENGINE_M_BLOB))
			continue;
		uint64_t stride = h->type == ENGINE_M_LEAF ?
		    sizeof (engine_mrow_t) : sizeof (engine_mchild_t);
		if ((h->type == ENGINE_M_BLOB && (h->level != 0 ||
		    h->count == 0 || h->count > ENGINE_M_PAYLOAD)) ||
		    (h->type != ENGINE_M_BLOB && (h->count == 0 ||
		    h->count > ENGINE_M_PAYLOAD / stride ||
		    (h->type == ENGINE_M_LEAF && h->level != 0) ||
		    (h->type == ENGINE_M_BRANCH && h->level == 0))))
			continue;
		if (swap && h->type != ENGINE_M_BLOB)
			byteswap_uint64_array(engine_mpayload(n),
			    h->count * stride);
		error = 0;
		break;
	}
	abd_free(abd);
	if (error != 0) {
		kmem_free(n, sizeof (*n));
		return (error);
	}
	n->sealed = B_TRUE;
	*result = n;
	return (0);
}

static int
engine_mget(engine_mbatch_t *b, const engine_mref_t *ref,
    engine_mnode_t **result)
{
	uint64_t index;
	if (!engine_mindex(ref->p, &index))
		return (EINVAL);
	engine_mnode_t *n = engine_pages.cache[index];
	if (n == NULL) {
		int error = engine_mread(b->spa, ref, &n);
		if (error != 0)
			return (error);
		engine_pages.cache[index] = n;
	}
	engine_mheader_t *h = engine_mheader(n);
	if (h->generation != ref->generation ||
	    (!(n->dirty && h->generation == b->generation) &&
	    memcmp(&h->hash, &ref->hash, sizeof (h->hash)) != 0))
		return (ECKSUM);
	*result = n;
	return (0);
}

static int
engine_mnew(engine_mbatch_t *b, uint64_t type, uint64_t level,
    engine_mnode_t **result)
{
	uint64_t index;
	mutex_enter(&engine.lock);
	for (index = 0; index < ENGINE_STORE_PAGES; index++)
		if (engine.bitmap[index] == 0)
			break;
	if (index == ENGINE_STORE_PAGES) {
		mutex_exit(&engine.lock);
		return (ENOSPC);
	}
	engine.bitmap[index] = b->allocated[index] = 1;
	mutex_exit(&engine.lock);
	engine_mnode_t *n = kmem_zalloc(sizeof (*n), KM_SLEEP);
	engine_mheader_t *h = engine_mheader(n);
	h->magic = ENGINE_M_MAGIC;
	h->type = type; h->level = level;
	h->p = engine.arena + index * ENGINE_STORE_PAGE *
	    engine.vd->vdev_children;
	h->generation = b->generation;
	h->pool = spa_guid(b->spa); h->top = engine.vd->vdev_guid;
	n->dirty = B_TRUE;
	VERIFY3P(engine_pages.cache[index], ==, NULL);
	engine_pages.cache[index] = n;
	b->pages++;
	*result = n;
	return (0);
}

static int
engine_mcow(engine_mbatch_t *b, engine_mnode_t *old, engine_mnode_t **out)
{
	engine_mheader_t *h = engine_mheader(old);
	if (old->dirty && h->generation == b->generation) {
		*out = old;
		return (0);
	}
	int error = engine_mnew(b, h->type, h->level, out);
	if (error == 0) {
		engine_mheader_t allocated = *engine_mheader(*out);
		memcpy((*out)->data, old->data, ENGINE_STORE_PAGE);
		*engine_mheader(*out) = allocated;
		engine_mheader(*out)->count = h->count;
		memcpy(engine_mheader(*out)->low, h->low, sizeof (h->low));
		memcpy(engine_mheader(*out)->high, h->high, sizeof (h->high));
	}
	return (error);
}

typedef struct engine_mchange {
	engine_mref_t left;
	engine_mref_t right;
	boolean_t changed;
} engine_mchange_t;

static void
engine_mleaf_set(engine_mnode_t *n, const engine_mrow_t *rows, uint_t count)
{
	engine_mheader_t *h = engine_mheader(n);
	memset(engine_mpayload(n), 0, ENGINE_M_PAYLOAD);
	memcpy(engine_mpayload(n), rows, count * sizeof (*rows));
	h->count = count;
	memcpy(h->low, rows[0].key, sizeof (h->low));
	memcpy(h->high, rows[count - 1].key, sizeof (h->high));
}

static int
engine_mbranch_set(engine_mbatch_t *b, engine_mnode_t *n,
    const engine_mchild_t *children, uint_t count)
{
	engine_mnode_t *first;
	int error = engine_mget(b, &children[0].ref, &first);
	if (error != 0)
		return (error);
	engine_mheader_t *h = engine_mheader(n);
	memset(engine_mpayload(n), 0, ENGINE_M_PAYLOAD);
	memcpy(engine_mpayload(n), children, count * sizeof (*children));
	h->count = count;
	memcpy(h->low, engine_mheader(first)->low, sizeof (h->low));
	memcpy(h->high, children[count - 1].high, sizeof (h->high));
	return (0);
}

static int
engine_minsert(engine_mbatch_t *b, const engine_mref_t *root,
    const engine_mrow_t *row, engine_mchange_t *change)
{
	engine_mnode_t *old;
	int error = engine_mget(b, root, &old);
	if (error != 0)
		return (error);
	engine_mheader_t *h = engine_mheader(old);
	change->left = *root;
	change->right.p = UINT64_MAX;
	change->changed = B_FALSE;
	if (h->type == ENGINE_M_LEAF) {
		engine_mrow_t *rows = engine_mpayload(old);
		uint_t at = 0, count = h->count;
		while (at < count && engine_mkey_compare(rows[at].key,
		    row->key) < 0)
			at++;
		boolean_t replace = at < count &&
		    engine_mkey_compare(rows[at].key, row->key) == 0;
		if (replace && memcmp(&rows[at], row, sizeof (*row)) == 0)
			return (0);
		engine_mrow_t *copy = kmem_alloc((count + 1) * sizeof (*copy),
		    KM_SLEEP);
		memcpy(copy, rows, at * sizeof (*copy));
		copy[at] = *row;
		memcpy(copy + at + 1, rows + at + replace,
		    (count - at - replace) * sizeof (*copy));
		uint_t total = count + !replace;
		engine_mnode_t *left, *right = NULL;
		error = engine_mcow(b, old, &left);
		if (error == 0 && total > ENGINE_M_ROWS)
			error = engine_mnew(b, ENGINE_M_LEAF, 0, &right);
		if (error == 0) {
			uint_t half = right != NULL ? total / 2 : total;
			engine_mleaf_set(left, copy, half);
			change->left = engine_mreference(left);
			if (right != NULL) {
				engine_mleaf_set(right, copy + half,
				    total - half);
				change->right = engine_mreference(right);
			}
			change->changed = B_TRUE;
		}
		kmem_free(copy, (count + 1) * sizeof (*copy));
		return (error);
	}
	if (h->type != ENGINE_M_BRANCH || h->level == 0)
		return (EINVAL);
	engine_mchild_t *children = engine_mpayload(old);
	uint_t at = 0, count = h->count;
	while (at + 1 < count && engine_mkey_compare(row->key,
	    children[at].high) > 0)
		at++;
	engine_mchange_t child;
	error = engine_minsert(b, &children[at].ref, row, &child);
	if (error != 0 || !child.changed)
		return (error);
	engine_mchild_t *copy = kmem_alloc((count + 1) * sizeof (*copy),
	    KM_SLEEP);
	memcpy(copy, children, count * sizeof (*copy));
	engine_mnode_t *child_node;
	error = engine_mget(b, &child.left, &child_node);
	if (error == 0) {
		copy[at].ref = child.left;
		memcpy(copy[at].high, engine_mheader(child_node)->high,
		    sizeof (copy[at].high));
	}
	uint_t total = count;
	if (error == 0 && child.right.p != UINT64_MAX) {
		memmove(copy + at + 2, copy + at + 1,
		    (count - at - 1) * sizeof (*copy));
		error = engine_mget(b, &child.right, &child_node);
		if (error == 0) {
			copy[at + 1].ref = child.right;
			memcpy(copy[at + 1].high,
			    engine_mheader(child_node)->high,
			    sizeof (copy[at + 1].high));
			total++;
		}
	}
	engine_mnode_t *left, *right = NULL;
	if (error == 0)
		error = engine_mcow(b, old, &left);
	if (error == 0 && total > ENGINE_M_CHILDREN)
		error = engine_mnew(b, ENGINE_M_BRANCH, h->level, &right);
	if (error == 0) {
		uint_t half = right != NULL ? total / 2 : total;
		error = engine_mbranch_set(b, left, copy, half);
		if (error == 0 && right != NULL)
			error = engine_mbranch_set(b, right, copy + half,
			    total - half);
		if (error == 0) {
			change->left = engine_mreference(left);
			if (right != NULL)
				change->right = engine_mreference(right);
			change->changed = B_TRUE;
		}
	}
	kmem_free(copy, (count + 1) * sizeof (*copy));
	return (error);
}

static int
engine_mput(engine_mbatch_t *b, engine_mref_t *root,
    const engine_mrow_t *row)
{
	if (root->p == UINT64_MAX) {
		engine_mnode_t *leaf;
		int error = engine_mnew(b, ENGINE_M_LEAF, 0, &leaf);
		if (error == 0) {
			engine_mleaf_set(leaf, row, 1);
			*root = engine_mreference(leaf);
			b->changes++;
		}
		return (error);
	}
	engine_mchange_t change;
	int error = engine_minsert(b, root, row, &change);
	if (error != 0 || !change.changed)
		return (error);
	if (change.right.p == UINT64_MAX) {
		*root = change.left;
		b->changes++;
		return (0);
	}
	engine_mnode_t *left, *right, *branch;
	error = engine_mget(b, &change.left, &left);
	if (error == 0)
		error = engine_mget(b, &change.right, &right);
	if (error != 0)
		return (error);
	uint64_t level = engine_mheader(left)->level + 1;
	if (level > ENGINE_M_LEVELS)
		return (EOVERFLOW);
	error = engine_mnew(b, ENGINE_M_BRANCH, level, &branch);
	if (error != 0)
		return (error);
	engine_mchild_t children[2] = { { .ref = change.left },
		{ .ref = change.right } };
	memcpy(children[0].high, engine_mheader(left)->high,
	    sizeof (children[0].high));
	memcpy(children[1].high, engine_mheader(right)->high,
	    sizeof (children[1].high));
	error = engine_mbranch_set(b, branch, children, 2);
	if (error == 0) {
		*root = engine_mreference(branch);
		b->changes++;
	}
	return (error);
}

static int
engine_mseal(engine_mbatch_t *b, engine_mref_t *ref)
{
	if (ref->p == UINT64_MAX)
		return (0);
	engine_mnode_t *n;
	int error = engine_mget(b, ref, &n);
	if (error != 0)
		return (error);
	engine_mheader_t *h = engine_mheader(n);
	if (n->dirty && h->generation == b->generation && !n->sealed) {
		if (h->type == ENGINE_M_BRANCH) {
			engine_mchild_t *children = engine_mpayload(n);
			for (uint_t i = 0; error == 0 && i < h->count; i++)
				error = engine_mseal(b, &children[i].ref);
		}
		if (error != 0)
			return (error);
		engine_mhash(n, &h->hash);
		n->sealed = B_TRUE;
	}
	*ref = engine_mreference(n);
	return (0);
}

static void
engine_pages_clear(void)
{
	for (uint_t i = 0; i < ENGINE_STORE_PAGES; i++) {
		if (engine_pages.cache[i] != NULL)
			kmem_free(engine_pages.cache[i],
			    sizeof (engine_mnode_t));
		engine_pages.cache[i] = NULL;
	}
	if (engine_pages.rows != NULL)
		kmem_free(engine_pages.rows,
		    MAX(1, engine_pages.count) * sizeof (engine_mrow_t));
	memset(&engine_pages, 0, sizeof (engine_pages));
	engine_pages.root.p = UINT64_MAX;
}

static int
engine_mrows(nvlist_t *nv, engine_mrow_t **out, uint_t *count)
{
	nvlist_t **owners;
	uint_t nr;
	if (nvlist_lookup_nvlist_array(nv, "owners", &owners, &nr) != 0)
		return (EINVAL);
	uint_t total = 0;
	for (uint_t i = 0; i < nr; i++) {
		uint64_t *p;
		uint_t np;
		if (nvlist_lookup_uint64_array(owners[i], "P", &p, &np) != 0 ||
		    np == 0 || np % 8 != 0 ||
		    np / 8 > ENGINE_STORE_PAGES * ENGINE_M_ROWS - total)
			return (EINVAL);
		total += np / 8;
	}
	engine_mrow_t *rows = kmem_zalloc(MAX(1, total) * sizeof (*rows),
	    KM_SLEEP);
	uint_t at = 0;
	int error = 0;
	for (uint_t i = 0; i < nr; i++) {
		uint64_t *v, *p;
		uint_t nv, np;
		if (nvlist_lookup_uint64_array(owners[i], "V", &v, &nv) != 0 ||
		    nvlist_lookup_uint64_array(owners[i], "P", &p, &np) != 0 ||
		    nv != 8) {
			error = EINVAL;
			break;
		}
		for (uint_t j = 0; j < np; j += 8) {
			engine_mrow_t *r = &rows[at++];
			r->key[0] = 1; r->key[1] = v[0]; r->key[2] = v[3];
			r->key[3] = j == 0 ? 0 : p[j + 7];
			memcpy(r->data, v, 8 * sizeof (uint64_t));
			memcpy(r->data + 8, p + j, 8 * sizeof (uint64_t));
		}
	}
	if (error == 0) {
		qsort(rows, total, sizeof (*rows), engine_mkey_compare);
		for (uint_t i = 1; i < total; i++)
			if (engine_mkey_compare(rows[i - 1].key,
			    rows[i].key) >= 0)
				error = EINVAL;
	}
	if (error != 0) {
		kmem_free(rows, MAX(1, total) * sizeof (*rows));
		return (error);
	}
	*out = rows; *count = total;
	return (0);
}

static void
engine_pages_fault(uint_t stage, uint64_t generation, uint_t child,
    uint64_t page)
{
	const char *fault = getenv("PR19262_M_PAGE_FAULT");
	if (fault == NULL || strtoull(fault, NULL, 0) != stage)
		return;
	(void) printf("PR19262_M_PAGE_CUT stage=%u generation=%llu "
	    "child=%u page=%llu\n", stage, (u_longlong_t)generation,
	    child, (u_longlong_t)page);
	(void) fflush(NULL);
	_exit(120 + stage);
}

static int
engine_pages_reserve(spa_t *spa, uint64_t generation)
{
	engine_mnode_t *n = kmem_zalloc(sizeof (*n), KM_SLEEP);
	engine_mheader_t *h = engine_mheader(n);
	uint_t slot = generation % 2;
	h->magic = ENGINE_M_ALLOC_MAGIC;
	h->type = ENGINE_M_BLOB;
	h->count = ENGINE_STORE_PAGES;
	h->p = engine.arena + slot * ENGINE_STORE_PAGE *
	    engine.vd->vdev_children;
	h->generation = generation;
	h->pool = spa_guid(spa); h->top = engine.vd->vdev_guid;
	h->low[0] = engine.arena; h->low[1] = engine.arena_bytes;
	h->low[2] = engine.vd->vdev_children;
	h->high[0] = ENGINE_STORE_PAGES;
	mutex_enter(&engine.lock);
	memcpy(engine_mpayload(n), engine.bitmap, ENGINE_STORE_PAGES);
	mutex_exit(&engine.lock);
	engine_mhash(n, &h->hash);
	abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
	int error = 0;
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	for (uint_t c = 0; c < engine.vd->vdev_children; c++) {
		int e = engine_store_io(engine.vd->vdev_child[c], slot, 1,
		    abd, B_TRUE);
		if (e == 0)
			e = pr19262_engine_metadata_flush(
			    engine.vd->vdev_child[c]);
		if (e != 0)
			error = e;
		else if (c == 0)
			engine_pages_fault(1, generation, c, slot);
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	abd_free(abd);
	kmem_free(n, sizeof (*n));
	return (error);
}

static int
engine_pages_reservations(spa_t *spa, uint8_t bitmap[ENGINE_STORE_PAGES],
    uint64_t *generation)
{
	engine_mnode_t *n = kmem_zalloc(sizeof (*n), KM_SLEEP);
	abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
	boolean_t found = B_FALSE;
	for (uint_t slot = 0; slot < 2; slot++) {
		for (uint_t c = 0; c < engine.vd->vdev_children; c++) {
			if (engine_store_io(engine.vd->vdev_child[c], slot, 1,
			    abd, B_FALSE) != 0)
				continue;
			engine_mheader_t *h = engine_mheader(n);
			boolean_t swap = h->magic ==
			    BSWAP_64(ENGINE_M_ALLOC_MAGIC);
			if (h->magic != ENGINE_M_ALLOC_MAGIC && !swap)
				continue;
			zio_cksum_t hash;
			engine_mhash(n, &hash);
			if (swap)
				byteswap_uint64_array(h, sizeof (*h));
			uint8_t *bits = engine_mpayload(n);
			if (memcmp(&h->hash, &hash, sizeof (hash)) != 0 ||
			    h->pool != spa_guid(spa) ||
			    h->top != engine.vd->vdev_guid ||
			    h->generation == 0 || h->level != 0 ||
			    h->type != ENGINE_M_BLOB ||
			    h->count != ENGINE_STORE_PAGES ||
			    h->low[0] != engine.arena ||
			    h->low[1] != engine.arena_bytes ||
			    h->low[2] != engine.vd->vdev_children ||
			    h->high[0] != ENGINE_STORE_PAGES ||
			    h->p != engine.arena + slot * ENGINE_STORE_PAGE *
			    engine.vd->vdev_children || !bits[0] || !bits[1])
				continue;
			boolean_t valid = B_TRUE;
			for (uint_t i = 0; i < ENGINE_STORE_PAGES; i++)
				valid &= bits[i] <= 1;
			if (!valid)
				continue;
			for (uint_t i = 0; i < ENGINE_STORE_PAGES; i++)
				bitmap[i] |= bits[i];
			*generation = MAX(*generation, h->generation);
			found = B_TRUE;
		}
	}
	abd_free(abd);
	kmem_free(n, sizeof (*n));
	return (found ? 0 : ECKSUM);
}

static int
engine_pages_store(spa_t *spa, const uberblock_t *ub, nvlist_t *nv,
    uint64_t desc[16])
{
	ASSERT(MUTEX_HELD(&engine.store_lock));
	engine_mrow_t *rows;
	uint_t count;
	int error = engine_mrows(nv, &rows, &count);
	if (error != 0)
		return (error);
	if (engine_pages.generation == UINT64_MAX) {
		kmem_free(rows, MAX(1, count) * sizeof (*rows));
		return (EOVERFLOW);
	}
	engine_mbatch_t *b = kmem_zalloc(sizeof (*b), KM_SLEEP);
	b->spa = spa;
	b->generation = ++engine_pages.generation;
	engine_mref_t tree = engine_pages.root;
	uint_t before = 0, after = 0;
	while (error == 0 && (before < engine_pages.count || after < count)) {
		int order = before == engine_pages.count ? 1 :
		    (after == count ? -1 : engine_mkey_compare(
		    engine_pages.rows[before].key, rows[after].key));
		if (order < 0) {
			engine_mrow_t removed = engine_pages.rows[before++];
			removed.data[6] = 3;
			error = engine_mput(b, &tree, &removed);
		} else {
			if (order == 0)
				before++;
			error = engine_mput(b, &tree, &rows[after++]);
		}
	}
	if (error == 0)
		error = engine_mseal(b, &tree);
	char *packed = NULL;
	size_t size = 0;
	engine_mnode_t **blobs = NULL;
	uint_t nb = 0;
	engine_mref_t root = { .p = UINT64_MAX };
	if (error == 0) {
		fnvlist_remove(nv, "owners");
		fnvlist_add_uint64_array(nv, "page-tree", (uint64_t *)&tree, 6);
		fnvlist_add_uint64(nv, "page-generation", b->generation);
		error = nvlist_pack(nv, &packed, &size, NV_ENCODE_XDR,
		    KM_SLEEP);
	}
	if (error == 0) {
		nb = howmany(size, ENGINE_M_PAYLOAD);
		if (nb == 0 || nb > ENGINE_STORE_PAGES)
			error = ENOSPC;
		else
			blobs = kmem_zalloc(nb * sizeof (*blobs), KM_SLEEP);
	}
	for (uint_t i = 0; error == 0 && i < nb; i++)
		error = engine_mnew(b, ENGINE_M_BLOB, 0, &blobs[i]);
	if (error == 0) {
		kmem_free(packed, size); packed = NULL;
		fnvlist_remove(nv, "bitmap");
		mutex_enter(&engine.lock);
		fnvlist_add_byte_array(nv, "bitmap", engine.bitmap,
		    ENGINE_STORE_PAGES);
		mutex_exit(&engine.lock);
		size = 0;
		error = nvlist_pack(nv, &packed, &size, NV_ENCODE_XDR,
		    KM_SLEEP);
		if (error == 0 && howmany(size, ENGINE_M_PAYLOAD) != nb)
			error = EINVAL;
	}
	if (error == 0) {
		engine_mref_t next = { .p = UINT64_MAX };
		for (uint_t i = nb; i-- != 0; ) {
			engine_mheader_t *h = engine_mheader(blobs[i]);
			h->low[0] = i * ENGINE_M_PAYLOAD;
			h->high[0] = size;
			h->count = MIN(ENGINE_M_PAYLOAD, size - h->low[0]);
			memcpy(h->reserved, &next, sizeof (next));
			memcpy(engine_mpayload(blobs[i]), packed + h->low[0],
			    h->count);
			engine_mhash(blobs[i], &h->hash);
			blobs[i]->sealed = B_TRUE;
			next = engine_mreference(blobs[i]);
		}
		root = next;
		/* Independent of BIND errors; reserve before any page write. */
		error = engine_pages_reserve(spa, b->generation);
		if (error == 0)
			engine_pages_fault(2, b->generation, 0, UINT64_MAX);
	}
	if (error == 0) {
		spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
		for (uint_t c = 0; c < engine.vd->vdev_children; c++) {
			for (uint_t i = 0; i < ENGINE_STORE_PAGES; i++) {
				if (!b->allocated[i])
					continue;
				engine_mnode_t *n = engine_pages.cache[i];
				VERIFY(n->sealed);
				abd_t *abd = abd_get_from_buf(n->data,
				    ENGINE_STORE_PAGE);
				int e = engine_store_io(
				    engine.vd->vdev_child[c],
				    i, 1, abd, B_TRUE);
				abd_free(abd);
				if (e != 0)
					error = e;
				else if (c == 0)
					engine_pages_fault(3, b->generation,
					    c, i);
			}
			int e = pr19262_engine_metadata_flush(
			    engine.vd->vdev_child[c]);
			if (e != 0)
				error = e;
			else if (c == 0)
				engine_pages_fault(4, b->generation, c,
				    UINT64_MAX);
		}
		spa_config_exit(spa, SCL_ZIO, FTAG);
	}
	if (error == 0) {
		if (engine_pages.rows != NULL) {
			uint64_t bytes = MAX(1, engine_pages.count) *
			    sizeof (engine_mrow_t);
			kmem_free(engine_pages.rows, bytes);
		}
		engine_pages.rows = rows; rows = NULL;
		engine_pages.count = count;
		engine_pages.root = tree;
		for (uint_t i = 0; i < ENGINE_STORE_PAGES; i++)
			if (b->allocated[i])
				engine_pages.cache[i]->dirty = B_FALSE;
		uint64_t d[16] = { ENGINE_STORE_MAGIC,
		    engine.vd->vdev_guid, engine.arena, engine.arena_bytes,
		    root.p, 1, ub->ub_txg, size, 0, 0, 0, 0,
		    engine.vd->vdev_ashift, engine.vd->vdev_children,
		    engine.total, 6 };
		memcpy(d + 8, &root.hash, sizeof (root.hash));
		memcpy(desc, d, sizeof (d));
		(void) printf("PR19262_PAGED_SYNC txg=%llu newpages=%u "
		    "changes=%u rows=%u root=%llu generation=%llu\n",
		    (u_longlong_t)ub->ub_txg, b->pages, b->changes, count,
		    (u_longlong_t)root.p, (u_longlong_t)b->generation);
	}
	if (packed != NULL)
		kmem_free(packed, size);
	if (blobs != NULL)
		kmem_free(blobs, nb * sizeof (*blobs));
	if (rows != NULL)
		kmem_free(rows, MAX(1, count) * sizeof (*rows));
	kmem_free(b, sizeof (*b));
	return (error);
}

typedef struct engine_mreader {
	spa_t *spa;
	uint64_t generation;
	uint8_t seen[ENGINE_STORE_PAGES];
	engine_mrow_t *rows;
	uint_t count;
	uint_t capacity;
	uint64_t last[4];
	boolean_t have_last;
} engine_mreader_t;

static int
engine_mcollect(engine_mreader_t *r, const engine_mrow_t *row)
{
	if (row->key[0] != 1 || row->key[1] != row->data[0] ||
	    row->key[2] != row->data[3] || row->data[6] > 3 ||
	    (row->key[3] != 0 && row->key[3] != row->data[15]) ||
	    (r->have_last && engine_mkey_compare(r->last, row->key) >= 0))
		return (EINVAL);
	memcpy(r->last, row->key, sizeof (r->last));
	r->have_last = B_TRUE;
	if (row->data[6] == 3)
		return (0);
	if (r->count == r->capacity) {
		uint_t cap = r->capacity == 0 ? 64 : r->capacity * 2;
		cap = MIN(cap, ENGINE_STORE_PAGES * ENGINE_M_ROWS);
		if (cap == r->capacity)
			return (EOVERFLOW);
		engine_mrow_t *rows = kmem_alloc(cap * sizeof (*rows),
		    KM_SLEEP);
		if (r->rows != NULL) {
			memcpy(rows, r->rows, r->count * sizeof (*rows));
			kmem_free(r->rows, r->capacity * sizeof (*rows));
		}
		r->rows = rows; r->capacity = cap;
	}
	r->rows[r->count++] = *row;
	return (0);
}

static int
engine_mwalk(engine_mreader_t *r, const engine_mref_t *ref, uint64_t level,
    uint64_t low[4], uint64_t high[4])
{
	uint64_t index;
	if (ref->generation == 0 || ref->generation > r->generation ||
	    !engine_mindex(ref->p, &index) || r->seen[index])
		return (EINVAL);
	engine_mnode_t *n;
	int error = engine_mread(r->spa, ref, &n);
	if (error != 0)
		return (error);
	engine_mheader_t *h = engine_mheader(n);
	r->seen[index] = 1;
	if ((level != UINT64_MAX && level != h->level) ||
	    h->type == ENGINE_M_BLOB) {
		error = EINVAL;
	} else if (h->type == ENGINE_M_LEAF) {
		engine_mrow_t *rows = engine_mpayload(n);
		if (engine_mkey_compare(rows[0].key, h->low) != 0 ||
		    engine_mkey_compare(rows[h->count - 1].key, h->high) != 0)
			error = EINVAL;
		for (uint_t i = 0; error == 0 && i < h->count; i++)
			error = engine_mcollect(r, &rows[i]);
	} else {
		engine_mchild_t *children = engine_mpayload(n);
		uint64_t previous[4] = { 0 };
		for (uint_t i = 0; error == 0 && i < h->count; i++) {
			uint64_t clow[4] = { 0 }, chigh[4] = { 0 };
			if (children[i].ref.generation > h->generation)
				error = EINVAL;
			else
				error = engine_mwalk(r, &children[i].ref,
				    h->level - 1, clow, chigh);
			if (error == 0) {
				boolean_t bad = engine_mkey_compare(chigh,
				    children[i].high) != 0;
				bad |= i == 0 && engine_mkey_compare(clow,
				    h->low) != 0;
				bad |= i != 0 && engine_mkey_compare(previous,
				    clow) >= 0;
				if (bad)
					error = EINVAL;
			}
			memcpy(previous, chigh, sizeof (previous));
		}
		if (error == 0 && engine_mkey_compare(previous, h->high) != 0)
			error = EINVAL;
	}
	memcpy(low, h->low, sizeof (h->low));
	memcpy(high, h->high, sizeof (h->high));
	kmem_free(n, sizeof (*n));
	return (error);
}

static int
engine_mowners(engine_mreader_t *r, nvlist_t *nv)
{
	nvlist_t **owners = kmem_alloc(MAX(1, r->count) * sizeof (*owners),
	    KM_SLEEP);
	uint_t nr = 0, at = 0;
	int error = 0;
	while (at < r->count && error == 0) {
		uint_t end = at + 1;
		engine_mrow_t *current = &r->rows[at];
		if (current->key[3] != 0) {
			error = EINVAL;
			break;
		}
		while (end < r->count &&
		    r->rows[end].key[1] == current->key[1] &&
		    r->rows[end].key[2] == current->key[2])
			end++;
		uint64_t *versions = kmem_alloc((end - at) * 8 *
		    sizeof (uint64_t), KM_SLEEP);
		for (uint_t i = at; i < end; i++) {
			if (memcmp(current->data, r->rows[i].data,
			    8 * sizeof (uint64_t)) != 0 ||
			    (i != at && r->rows[i].data[14] != 1))
				error = EINVAL;
			memcpy(versions + (i - at) * 8, r->rows[i].data + 8,
			    8 * sizeof (uint64_t));
		}
		if (error == 0) {
			owners[nr] = fnvlist_alloc();
			fnvlist_add_uint64_array(owners[nr], "V",
			    current->data, 8);
			fnvlist_add_uint64_array(owners[nr], "P", versions,
			    (end - at) * 8);
			nr++;
		}
		kmem_free(versions, (end - at) * 8 * sizeof (uint64_t));
		at = end;
	}
	if (error == 0)
		fnvlist_add_nvlist_array(nv, "owners",
		    (const nvlist_t * const *)owners, nr);
	for (uint_t i = 0; i < nr; i++)
		fnvlist_free(owners[i]);
	kmem_free(owners, MAX(1, r->count) * sizeof (*owners));
	return (error);
}

static int
engine_pages_read(spa_t *spa, const uberblock_t *ub, nvlist_t **out,
	uint8_t seen[ENGINE_STORE_PAGES], boolean_t allocator)
{
	const uint64_t *d = ub->ub_pr19262;
	engine_mreader_t *r = kmem_zalloc(sizeof (*r), KM_SLEEP);
	r->spa = spa;
	engine_mref_t ref = { .p = d[4] };
	memcpy(&ref.hash, d + 8, sizeof (ref.hash));
	char *packed = kmem_alloc(d[7], KM_SLEEP);
	uint64_t offset = 0;
	int error = 0;
	while (error == 0 && ref.p != UINT64_MAX) {
		uint64_t index;
		if (!engine_mindex(ref.p, &index) || r->seen[index]) {
			error = EINVAL;
			break;
		}
		engine_mnode_t *n;
		error = engine_mread(spa, &ref, &n);
		if (error != 0)
			break;
		engine_mheader_t *h = engine_mheader(n);
		if (offset == 0)
			r->generation = h->generation;
		if (h->type != ENGINE_M_BLOB ||
		    h->generation != r->generation ||
		    h->low[0] != offset || h->high[0] != d[7] ||
		    offset > d[7] || h->count > d[7] - offset)
			error = EINVAL;
		else {
			r->seen[index] = 1;
			memcpy(packed + offset, engine_mpayload(n), h->count);
			offset += h->count;
			memcpy(&ref, h->reserved, sizeof (ref));
			if (ref.p != UINT64_MAX && ref.generation == 0)
				error = EINVAL;
		}
		kmem_free(n, sizeof (*n));
	}
	nvlist_t *nv = NULL;
	if (error == 0 && offset != d[7])
		error = EINVAL;
	if (error == 0)
		error = nvlist_unpack(packed, d[7], &nv, KM_SLEEP);
	kmem_free(packed, d[7]);
	uint64_t *tree, generation;
	uint_t nt;
	if (error == 0 && (nvlist_exists(nv, "owners") ||
	    nvlist_lookup_uint64_array(nv, "page-tree", &tree, &nt) != 0 ||
	    nt != 6 || nvlist_lookup_uint64(nv, "page-generation",
	    &generation) != 0 || generation != r->generation))
		error = EINVAL;
	if (error == 0) {
		memcpy(&ref, tree, sizeof (ref));
		if (ref.p != UINT64_MAX) {
			uint64_t low[4], high[4];
			error = engine_mwalk(r, &ref, UINT64_MAX, low, high);
		}
	}
	uchar_t *bitmap;
	uint_t nb;
	if (error == 0 && (nvlist_lookup_byte_array(nv, "bitmap", &bitmap,
	    &nb) != 0 || nb != ENGINE_STORE_PAGES))
		error = EINVAL;
	for (uint_t i = 0; error == 0 && i < ENGINE_STORE_PAGES; i++)
		if (bitmap[i] > 1 || (r->seen[i] && !bitmap[i]))
			error = EINVAL;
	if (error == 0 && (!bitmap[0] || !bitmap[1]))
		error = EINVAL;
	if (error == 0 && allocator) {
		uint8_t reserved[ENGINE_STORE_PAGES] = { 0 };
		uint64_t latest = 0;
		error = engine_pages_reservations(spa, reserved, &latest);
		if (error == 0 && latest < r->generation)
			error = ECKSUM;
		for (uint_t i = 0; error == 0 && i < ENGINE_STORE_PAGES; i++)
			if (bitmap[i] && !reserved[i])
				error = ECKSUM;
		if (error == 0) {
			for (uint_t i = 0; i < ENGINE_STORE_PAGES; i++)
				bitmap[i] |= reserved[i];
			fnvlist_add_uint64(nv, "page-reservation-generation",
			    latest);
		}
	}
	if (error == 0)
		error = engine_mowners(r, nv);
	if (error == 0) {
		r->seen[0] = r->seen[1] = 1;
		memcpy(seen, r->seen, ENGINE_STORE_PAGES);
		*out = nv;
	} else {
		nvlist_free(nv);
	}
	if (r->rows != NULL)
		kmem_free(r->rows, r->capacity * sizeof (engine_mrow_t));
	kmem_free(r, sizeof (*r));
	return (error);
}

static int
engine_pages_install(nvlist_t *nv)
{
	uint64_t *tree, generation;
	uint_t nt, count;
	if (nvlist_lookup_uint64_array(nv, "page-tree", &tree, &nt) != 0 ||
	    nt != 6 || nvlist_lookup_uint64(nv, "page-generation",
	    &generation) != 0)
		return (EINVAL);
	engine_mrow_t *rows;
	int error = engine_mrows(nv, &rows, &count);
	if (error != 0)
		return (error);
	engine_pages_clear();
	memcpy(&engine_pages.root, tree, sizeof (engine_pages.root));
	engine_pages.generation = generation;
	uint64_t reserved;
	if (nvlist_lookup_uint64(nv, "page-reservation-generation",
	    &reserved) == 0)
		engine_pages.generation = MAX(generation, reserved);
	engine_pages.rows = rows; engine_pages.count = count;
	return (0);
}

_Static_assert(sizeof (engine_mheader_t) == 256, "private page header size");
_Static_assert(sizeof (engine_mref_t) == 48, "private page reference size");
_Static_assert(sizeof (engine_mrow_t) == 160, "private page record size");
