/* SPDX-License-Identifier: CDDL-1.0 */
/* Private raw COW pages, included after the store's physical I/O helpers. */
#define	ENGINE_M_MAGIC 0x3139323632503036ULL
#define	ENGINE_M_ALLOC_MAGIC 0x3139323632413037ULL
#define	ENGINE_M_ALLOC_OLD_MAGIC 0x3139323632413036ULL
#define	ENGINE_M_LEAF 1
#define	ENGINE_M_BRANCH 2
#define	ENGINE_M_BLOB 3
#define	ENGINE_M_DIRECTORY 4
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
} engine_mbatch_t;

typedef struct engine_marena {
	struct engine_marena *next;
	uint64_t p;
	uint64_t first;
	uint64_t pages;
	uint64_t uuid[2];
	uint64_t format_generation;
	engine_mref_t candidate;
	boolean_t formatted;
	boolean_t directory_flushed;
	boolean_t pending;
} engine_marena_t;

typedef struct engine_mcontrol {
	uint64_t serial;
	uint64_t codec;
	engine_mref_t directory;
	uint64_t pending[4];
	/* Durable recovery floor and certified parity travel with it. */
	uint64_t floor;
	uint64_t certified;
} engine_mcontrol_t;

static struct {
	engine_mnode_t **cache;
	uint8_t *bits;
	uint64_t capacity;
	engine_marena_t *arenas;
	engine_mref_t directory;
	uint64_t pending[4];
	uint64_t floor;
	uint64_t certified;
	uint64_t serial;
	uint64_t control_serial;
	engine_mcontrol_t control;
	boolean_t publishing_active;
	/* RAM bits are a subset of the disk masks until both slots refresh. */
	boolean_t gc_relaxed;
	zfs_range_tree_t *directory_pages;
	engine_mref_t root;
	uint64_t generation;
	engine_mrow_t *rows;
	uint_t count;
} engine_pages = { .root = { .p = UINT64_MAX } };

static int engine_pages_grow(engine_mbatch_t *);
static int engine_pages_reconcile(spa_t *, boolean_t);
static int engine_pages_control_repair(spa_t *, const engine_mcontrol_t *);
static boolean_t engine_store_extent(uint64_t, uint64_t);
static boolean_t engine_m_read_injected;
static boolean_t engine_m_active_error_injected;

static int
engine_pages_add_arena(uint64_t p, uint64_t pages)
{
	uint64_t stripe = ENGINE_STORE_PAGE * engine.vd->vdev_children;
	if (pages < 3 || pages > ENGINE_M_PAYLOAD ||
	    pages > engine.total / stripe ||
	    p % stripe != 0 || p >= engine.total ||
	    pages > (engine.total - p) / stripe ||
	    engine_pages.capacity > engine.total / stripe - pages)
		return (EINVAL);
	for (engine_marena_t *a = engine_pages.arenas; a != NULL;
	    a = a->next)
		if (p < a->p + a->pages * stripe &&
		    a->p < p + pages * stripe)
			return (EINVAL);
	uint64_t old = engine_pages.capacity, count = old + pages;
	if (count > SIZE_MAX / sizeof (engine_mnode_t *))
		return (EOVERFLOW);
	engine_mnode_t **cache = kmem_zalloc(count * sizeof (*cache),
	    KM_SLEEP);
	uint8_t *bits = kmem_zalloc(count, KM_SLEEP);
	bits[old] = bits[old + 1] = 1;
	if (old != 0) {
		memcpy(cache, engine_pages.cache, old * sizeof (*cache));
		memcpy(bits, engine_pages.bits, old);
		kmem_free(engine_pages.cache, old * sizeof (*cache));
		kmem_free(engine_pages.bits, old);
	}
	engine_marena_t *a = kmem_zalloc(sizeof (*a), KM_SLEEP);
	a->p = p; a->first = old; a->pages = pages;
	a->next = engine_pages.arenas;
	engine_pages.arenas = a;
	engine_pages.cache = cache;
	engine_pages.bits = bits;
	engine_pages.capacity = count;
	return (0);
}

static void
engine_pages_init(void)
{
	if (engine_pages.capacity != 0)
		return;
	VERIFY0(engine_pages_add_arena(engine.arena, ENGINE_STORE_PAGES));
	if (engine.bitmap != NULL)
		memcpy(engine_pages.bits, engine.bitmap, ENGINE_STORE_PAGES);
}

static void
engine_mmark(void *arg, uint64_t p, uint64_t size)
{
	zfs_range_tree_t *seen = arg;
	zfs_range_tree_clear(seen, p, size);
	zfs_range_tree_add(seen, p, size);
}

static engine_mheader_t *
engine_mheader(engine_mnode_t *n)
{
	return ((engine_mheader_t *)n->data);
}

static boolean_t
engine_mdirty(engine_mbatch_t *b, engine_mnode_t *n)
{
	return (n != NULL && n->dirty &&
	    engine_mheader(n)->generation == b->generation);
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
	engine_pages_init();
	for (engine_marena_t *a = engine_pages.arenas; a != NULL;
	    a = a->next) {
		if (a->pending)
			continue;
		if (p < a->p || (p - a->p) % stripe != 0)
			continue;
		uint64_t page = (p - a->p) / stripe;
		if (page < a->pages) {
			*index = a->first + page;
			return (B_TRUE);
		}
	}
	return (B_FALSE);
}

static uint64_t
engine_mphysical(uint64_t index)
{
	for (engine_marena_t *a = engine_pages.arenas; a != NULL;
	    a = a->next)
		if (index >= a->first && index - a->first < a->pages)
			return (a->p + (index - a->first) * ENGINE_STORE_PAGE *
			    engine.vd->vdev_children);
	return (UINT64_MAX);
}

static int
engine_mio(vdev_t *vd, uint64_t p, abd_t *abd, boolean_t write)
{
	uint64_t index;
	if (!engine_mindex(p, &index))
		return (EINVAL);
	return (engine_store_io_at(vd, p, 0, 1, abd, write));
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
engine_mread_ref(spa_t *spa, const engine_mref_t *ref,
    engine_mnode_t **result, boolean_t bootstrap)
{
	uint64_t index;
	uint64_t stripe = ENGINE_STORE_PAGE * engine.vd->vdev_children;
	if (ref->p % stripe != 0 || ref->p >= engine.total ||
	    stripe > engine.total - ref->p ||
	    (!bootstrap && !engine_mindex(ref->p, &index)))
		return (EINVAL);
	engine_mnode_t *n = kmem_zalloc(sizeof (*n), KM_SLEEP);
	abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
	int error = ECKSUM;
	for (uint_t c = 0; c < engine.vd->vdev_children; c++) {
		if (engine_store_io_at(engine.vd->vdev_child[c], ref->p,
		    0, 1, abd, B_FALSE) != 0)
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
		    h->type != ENGINE_M_BLOB && h->type != ENGINE_M_DIRECTORY))
			continue;
		uint64_t stride = h->type == ENGINE_M_LEAF ?
		    sizeof (engine_mrow_t) : sizeof (engine_mchild_t);
		if ((h->type == ENGINE_M_DIRECTORY && (h->level != 0 ||
		    h->count != 10 * sizeof (uint64_t))) ||
		    (h->type == ENGINE_M_BLOB && (h->level != 0 ||
		    h->count == 0 || h->count > ENGINE_M_PAYLOAD)) ||
		    ((h->type == ENGINE_M_LEAF || h->type == ENGINE_M_BRANCH) &&
		    (h->count == 0 ||
		    h->count > ENGINE_M_PAYLOAD / stride ||
		    (h->type == ENGINE_M_LEAF && h->level != 0) ||
		    (h->type == ENGINE_M_BRANCH && h->level == 0))))
			continue;
		if (swap && h->type == ENGINE_M_DIRECTORY)
			byteswap_uint64_array(engine_mpayload(n), h->count);
		else if (swap && h->type != ENGINE_M_BLOB)
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
engine_mread(spa_t *spa, const engine_mref_t *ref, engine_mnode_t **result)
{
	return (engine_mread_ref(spa, ref, result, B_FALSE));
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
	engine_pages_init();
	if (engine_pages.pending[1] != 0) {
		int error = engine_pages_reconcile(b->spa, B_TRUE);
		if (error != 0)
			return (error);
	}
	mutex_enter(&engine.lock);
	for (index = 0; index < engine_pages.capacity; index++)
		if (engine_pages.bits[index] == 0)
			break;
	if (index == engine_pages.capacity) {
		mutex_exit(&engine.lock);
		int error = engine_pages_grow(b);
		if (error != 0)
			return (error);
		return (engine_mnew(b, type, level, result));
	}
	engine_pages.bits[index] = 1;
	if (index < ENGINE_STORE_PAGES)
		engine.bitmap[index] = 1;
	mutex_exit(&engine.lock);
	engine_mnode_t *n = kmem_zalloc(sizeof (*n), KM_SLEEP);
	engine_mheader_t *h = engine_mheader(n);
	h->magic = ENGINE_M_MAGIC;
	h->type = type; h->level = level;
	h->p = engine_mphysical(index);
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
engine_pages_tree_clear(void)
{
	for (uint64_t i = 0; i < engine_pages.capacity; i++) {
		if (engine_pages.cache[i] != NULL)
			kmem_free(engine_pages.cache[i],
			    sizeof (engine_mnode_t));
		engine_pages.cache[i] = NULL;
	}
	if (engine_pages.rows != NULL)
		kmem_free(engine_pages.rows,
		    MAX(1, engine_pages.count) * sizeof (engine_mrow_t));
	engine_pages.rows = NULL;
	engine_pages.count = 0;
	memset(&engine_pages.root, 0, sizeof (engine_pages.root));
	engine_pages.root.p = UINT64_MAX;
}

static void
engine_pages_clear(void)
{
	engine_pages_tree_clear();
	if (engine_pages.capacity != 0) {
		kmem_free(engine_pages.cache,
		    engine_pages.capacity * sizeof (*engine_pages.cache));
		kmem_free(engine_pages.bits, engine_pages.capacity);
	}
	while (engine_pages.arenas != NULL) {
		engine_marena_t *a = engine_pages.arenas;
		engine_pages.arenas = a->next;
		kmem_free(a, sizeof (*a));
	}
	if (engine_pages.directory_pages != NULL) {
		zfs_range_tree_vacate(engine_pages.directory_pages, NULL, NULL);
		zfs_range_tree_destroy(engine_pages.directory_pages);
	}
	memset(&engine_pages, 0, sizeof (engine_pages));
	engine_pages.root.p = UINT64_MAX;
}

static uint64_t
engine_pages_row_limit(void)
{
	return (MIN(UINT_MAX, engine_pages.capacity * ENGINE_M_ROWS));
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
		    np / 8 > engine_pages_row_limit() - total)
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

static boolean_t
engine_pages_active_error(engine_marena_t *a, uint_t child, uint_t slot,
    const char *phase, uint64_t serial)
{
	const char *fault = getenv("PR19262_M_ACTIVE_ERROR");
	const char *member = getenv("PR19262_M_ACTIVE_ERROR_CHILD");
	const char *selected = getenv("PR19262_M_ACTIVE_ERROR_SLOT");
	if (engine_m_active_error_injected || !engine_pages.publishing_active ||
	    a->p != engine.arena || fault == NULL ||
	    strcmp(fault, phase) != 0 ||
	    child != (member == NULL ? 0 : strtoull(member, NULL, 10)) ||
	    (selected != NULL && slot != strtoull(selected, NULL, 10)))
		return (B_FALSE);
	engine_m_active_error_injected = B_TRUE;
	(void) printf("PR19262_M_ACTIVE_ERROR phase=%s child=%u slot=%u "
	    "serial=%llu\n", phase, child, slot, (u_longlong_t)serial);
	return (B_TRUE);
}

static int
engine_pages_reserve_arena(spa_t *spa, engine_marena_t *a,
    uint64_t generation, uint64_t serial, const engine_mcontrol_t *control)
{
	engine_mnode_t *n = kmem_zalloc(sizeof (*n), KM_SLEEP);
	engine_mheader_t *h = engine_mheader(n);
	uint_t slot = serial % 2;
	h->magic = ENGINE_M_ALLOC_MAGIC;
	h->type = ENGINE_M_BLOB;
	h->count = a->pages;
	h->p = a->p + slot * ENGINE_STORE_PAGE *
	    engine.vd->vdev_children;
	h->generation = generation;
	h->pool = spa_guid(spa); h->top = engine.vd->vdev_guid;
	h->low[0] = a->p; h->low[1] = a->pages * ENGINE_STORE_PAGE;
	h->low[2] = engine.vd->vdev_children;
	h->high[0] = a->pages;
	h->high[1] = serial;
	h->high[2] = control->floor;
	h->high[3] = control->certified;
	memcpy(h->reserved, &control->directory, sizeof (control->directory));
	memcpy(h->reserved + 6, control->pending, sizeof (control->pending));
	memcpy(h->reserved + 10, a->uuid, sizeof (a->uuid));
	mutex_enter(&engine.lock);
	memcpy(engine_mpayload(n), engine_pages.bits + a->first, a->pages);
	mutex_exit(&engine.lock);
	engine_mhash(n, &h->hash);
	abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
	int error = 0;
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	for (uint_t c = 0; c < engine.vd->vdev_children; c++) {
		int e = EIO;
		if (!engine_pages_active_error(a, c, slot, "write", serial))
			e = engine_store_io_at(engine.vd->vdev_child[c], a->p,
			    slot, 1, abd, B_TRUE);
		if (e == 0) {
			if (engine_pages_active_error(a, c, slot,
			    "flush", serial))
				e = EIO;
			else
				e = pr19262_engine_metadata_flush(
				    engine.vd->vdev_child[c]);
		}
		if (e != 0)
			error = e;
		else if (c == 0)
			engine_pages_fault(1, generation, c, slot);
		if (e == 0 && c == 0 && a->p == engine.arena &&
		    control->pending[1] != 0)
			engine_pages_fault(10, generation, c, slot);
		if (e == 0 && c == 0 && a->p == engine.arena &&
		    engine_pages.publishing_active)
			engine_pages_fault(15, generation, c, slot);
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	abd_free(abd);
	kmem_free(n, sizeof (*n));
	return (error);
}

static int
engine_pages_reserve_control(spa_t *spa, uint64_t generation,
    const engine_mcontrol_t *control)
{
	int error = engine_pages_control_repair(spa, control);
	if (error != 0)
		return (error);
	if (engine_pages.serial == UINT64_MAX)
		return (EOVERFLOW);
	uint64_t serial = ++engine_pages.serial;
	error = 0;
	for (engine_marena_t *a = engine_pages.arenas; a != NULL;
	    a = a->next) {
		if (a->pending && !a->formatted)
			continue;
		if (engine_pages.publishing_active && a->p == engine.arena)
			engine_pages_fault(16, generation, 0, serial % 2);
		int e = engine_pages_reserve_arena(spa, a, generation, serial,
		    control);
		if (e != 0)
			error = e;
	}
	if (error == 0) {
		engine_pages.control = *control;
		engine_pages.control.serial = serial;
		engine_pages.control.codec = ENGINE_M_ALLOC_MAGIC;
		engine_pages.control_serial = serial;
		engine_pages.floor = control->floor;
		engine_pages.certified = control->certified;
	}
	return (error);
}

static engine_mcontrol_t
engine_pages_control_current(const engine_mref_t *directory)
{
	engine_mcontrol_t control = { .directory = *directory,
	    .floor = engine_pages.floor,
	    .certified = engine_pages.certified };
	memcpy(control.pending, engine_pages.pending, sizeof (control.pending));
	return (control);
}

static int
engine_pages_reserve(spa_t *spa, uint64_t generation)
{
	engine_mcontrol_t control =
	    engine_pages_control_current(&engine_pages.directory);
	return (engine_pages_reserve_control(spa, generation, &control));
}

/*
 * Publish a new durable floor and certificate in both control slots of
 * every member.  The caller holds store_lock and SCL_STATE.
 */
static int
engine_pages_publish_control(spa_t *spa, uint64_t floor, uint64_t certified)
{
	if (engine_pages.pending[1] != 0)
		return (EBUSY);
	engine_mcontrol_t control =
	    engine_pages_control_current(&engine_pages.directory);
	control.floor = floor;
	control.certified = certified;
	int error = engine_pages_reserve_control(spa, engine_pages.generation,
	    &control);
	if (error == 0)
		error = engine_pages_reserve_control(spa,
		    engine_pages.generation, &control);
	if (error == 0)
		engine_pages.gc_relaxed = B_FALSE;
	return (error);
}

static boolean_t
engine_mcontrol_same(const engine_mcontrol_t *a, const engine_mcontrol_t *b)
{
	return (memcmp(a, b, sizeof (*a)) == 0);
}

static int
engine_pages_reservations_arena(spa_t *spa, engine_marena_t *a,
    uint8_t *bitmap, uint64_t *generation, uint64_t *serial_high,
    engine_mcontrol_t *control, boolean_t strict)
{
	engine_mnode_t *n = kmem_zalloc(sizeof (*n), KM_SLEEP);
	abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
	boolean_t found = B_FALSE;
	boolean_t conflict = B_FALSE;
	int error = 0;
	for (uint_t slot = 0; slot < 2; slot++) {
		for (uint_t c = 0; c < engine.vd->vdev_children; c++) {
			uint64_t p = a->p + slot * ENGINE_STORE_PAGE *
			    engine.vd->vdev_children;
			const char *hidden =
			    getenv("PR19262_M_A_FIRST_READ_EIO");
			if (!engine_m_read_injected && a->p == engine.arena &&
			    hidden != NULL && c == 0 &&
			    slot == strtoull(hidden, NULL, 10)) {
				engine_m_read_injected = B_TRUE;
				(void) printf("PR19262_M_A_READ_EIO child=0 "
				    "slot=%u\n", slot);
				if (strict)
					error = EIO;
				continue;
			}
			int e = engine_store_io_at(engine.vd->vdev_child[c],
			    p, 0, 1, abd, B_FALSE);
			if (e != 0) {
				/* Read-only import tolerates absent members. */
				if (strict && (e != ENXIO ||
				    spa_writeable(spa)))
					error = EIO;
				continue;
			}
			engine_mheader_t *h = engine_mheader(n);
			boolean_t swap = h->magic ==
			    BSWAP_64(ENGINE_M_ALLOC_MAGIC) || h->magic ==
			    BSWAP_64(ENGINE_M_ALLOC_OLD_MAGIC);
			if (h->magic != ENGINE_M_ALLOC_MAGIC &&
			    h->magic != ENGINE_M_ALLOC_OLD_MAGIC && !swap)
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
			    h->count != a->pages ||
			    h->low[0] != a->p ||
			    h->low[1] != a->pages * ENGINE_STORE_PAGE ||
			    h->low[2] != engine.vd->vdev_children ||
			    h->high[0] != a->pages || h->p != p ||
			    (h->magic == ENGINE_M_ALLOC_MAGIC &&
			    h->high[1] == 0) ||
			    !bits[0] || !bits[1])
				continue;
			if ((h->reserved[10] != 0 || h->reserved[11] != 0) &&
			    memcmp(h->reserved + 10, a->uuid,
			    sizeof (a->uuid)) != 0) {
				error = ECKSUM;
				continue;
			}
			boolean_t valid = B_TRUE;
			for (uint_t i = 0; i < a->pages; i++)
				valid &= bits[i] <= 1;
			if (!valid)
				continue;
			for (uint_t i = 0; i < a->pages; i++)
				bitmap[i] |= bits[i];
			uint64_t serial = h->high[1] == 0 ?
			    h->generation : h->high[1];
			if (control != NULL && serial >= control->serial) {
				engine_mcontrol_t selected = { .serial = serial,
				    .codec = h->magic, .floor = h->high[2],
				    .certified = h->high[3] };
				memcpy(&selected.directory, h->reserved,
				    sizeof (selected.directory));
				memcpy(selected.pending, h->reserved + 6,
				    sizeof (selected.pending));
				if (serial > control->serial) {
					*control = selected;
					conflict = B_FALSE;
				} else if (!engine_mcontrol_same(control,
				    &selected)) {
					conflict = B_TRUE;
				}
			}
			*serial_high = MAX(*serial_high, serial);
			*generation = MAX(*generation, h->generation);
			found = B_TRUE;
		}
	}
	abd_free(abd);
	kmem_free(n, sizeof (*n));
	if (error != 0)
		return (error);
	return (conflict ? ECKSUM : (found ? 0 : ENOENT));
}

static int
engine_pages_reservations(spa_t *spa, uint8_t *bitmap,
    uint64_t *generation, uint64_t *serial_high)
{
	for (engine_marena_t *a = engine_pages.arenas; a != NULL;
	    a = a->next) {
		if (a->pending) {
			memset(bitmap + a->first, 1, a->pages);
			continue;
		}
		engine_mcontrol_t control = { 0 };
		int error = engine_pages_reservations_arena(spa, a,
		    bitmap + a->first, generation, serial_high,
		    a->p == engine.arena ? &control : NULL, B_TRUE);
		if (error != 0)
			return (error);
		if (a->p == engine.arena &&
		    !engine_mcontrol_same(&control, &engine_pages.control))
			return (EAGAIN);
	}
	return (0);
}

static int
engine_pages_control_check(spa_t *spa, boolean_t strict)
{
	engine_marena_t *a = engine_pages.arenas;
	while (a != NULL && a->p != engine.arena)
		a = a->next;
	if (a == NULL)
		return (EINVAL);
	uint8_t *bits = kmem_zalloc(a->pages, KM_SLEEP);
	uint64_t generation = 0, serial = 0;
	engine_mcontrol_t control = { 0 };
	int error = engine_pages_reservations_arena(spa, a, bits,
	    &generation, &serial, &control, strict);
	kmem_free(bits, a->pages);
	if (error == 0 &&
	    !engine_mcontrol_same(&control, &engine_pages.control))
		error = EAGAIN;
	if (error == 0)
		engine_pages.serial = MAX(engine_pages.serial, serial);
	return (error);
}

static boolean_t
engine_mcontrol_payload_same(const engine_mcontrol_t *a,
    const engine_mcontrol_t *b)
{
	return (memcmp(&a->directory, &b->directory,
	    sizeof (a->directory)) == 0 &&
	    memcmp(a->pending, b->pending, sizeof (a->pending)) == 0 &&
	    a->floor == b->floor && a->certified == b->certified);
}

static int
engine_pages_control_repair(spa_t *spa, const engine_mcontrol_t *proposed)
{
	engine_mcontrol_t control = { 0 };
	uint64_t generation = 0, serial = engine_pages.serial;
	for (engine_marena_t *a = engine_pages.arenas; a != NULL;
	    a = a->next) {
		if (a->pending && !a->formatted)
			continue;
		uint8_t *bits = kmem_zalloc(a->pages, KM_SLEEP);
		engine_mcontrol_t *selected = a->p == engine.arena ?
		    &control : NULL;
		int error = engine_pages_reservations_arena(spa, a, bits,
		    &generation, &serial, selected,
		    B_TRUE);
		mutex_enter(&engine.lock);
		for (uint64_t i = 0; error == 0 && i < a->pages; i++)
			if (bits[i] && !engine_pages.bits[a->first + i] &&
			    !engine_pages.gc_relaxed)
				error = ECKSUM;
		mutex_exit(&engine.lock);
		kmem_free(bits, a->pages);
		/* A fresh arena has no reservation until its first write. */
		if (error == ENOENT && (a->pending ||
		    engine_pages.control.serial == 0))
			error = 0;
		if (error != 0)
			return (error);
	}
	if (control.serial != 0 &&
	    !engine_mcontrol_payload_same(&control, &engine_pages.control) &&
	    !engine_mcontrol_payload_same(&control, proposed))
		return (ECKSUM);
	if (control.serial < engine_pages.control.serial)
		return (EIO);
	engine_pages.serial = serial;
	return (0);
}

static boolean_t
engine_mref_empty(const engine_mref_t *ref)
{
	engine_mref_t empty = { 0 };
	return (memcmp(ref, &empty, sizeof (empty)) == 0);
}

typedef struct engine_mmarker {
	spa_t *spa;
	uint8_t *marked;
} engine_mmarker_t;

/* Mark every page reachable from a blob chain or a page-tree reference. */
static int
engine_pages_mark_ref(engine_mmarker_t *m, const engine_mref_t *ref)
{
	uint64_t index;
	if (ref->p == UINT64_MAX || engine_mref_empty(ref))
		return (0);
	if (!engine_mindex(ref->p, &index))
		return (EINVAL);
	if (m->marked[index])
		return (0);
	m->marked[index] = 1;
	engine_mnode_t *n = engine_pages.cache[index];
	boolean_t owned = B_FALSE;
	if (n == NULL || n->dirty || !n->sealed ||
	    memcmp(&engine_mheader(n)->hash, &ref->hash,
	    sizeof (ref->hash)) != 0) {
		int error = engine_mread(m->spa, ref, &n);
		if (error != 0)
			return (error);
		owned = B_TRUE;
	}
	engine_mheader_t *h = engine_mheader(n);
	int error = 0;
	if (h->type == ENGINE_M_BRANCH) {
		engine_mchild_t *children = engine_mpayload(n);
		for (uint_t i = 0; error == 0 && i < h->count; i++)
			error = engine_pages_mark_ref(m, &children[i].ref);
	} else if (h->type == ENGINE_M_BLOB) {
		engine_mref_t next;
		memcpy(&next, h->reserved, sizeof (next));
		error = engine_pages_mark_ref(m, &next);
	}
	if (owned)
		kmem_free(n, sizeof (*n));
	return (error);
}

static void
engine_pages_mark_range(void *arg, uint64_t p, uint64_t size)
{
	engine_mmarker_t *m = arg;
	uint64_t stripe = ENGINE_STORE_PAGE * engine.vd->vdev_children;
	for (uint64_t at = p; at < p + size; at += stripe) {
		uint64_t index;
		if (engine_mindex(at, &index))
			m->marked[index] = 1;
	}
}

/*
 * Free every page that neither the last durable catalog nor the current
 * RAM tree, the control slots or a directory node reaches.  Only valid
 * once every root below the latest one is refused by the floor, so the
 * caller publishes the shrunken masks together with that floor in both
 * control slots.  The caller holds store_lock.
 */
static int
engine_pages_gc(spa_t *spa, uint64_t *freed)
{
	ASSERT(MUTEX_HELD(&engine.store_lock));
	*freed = 0;
	if (engine_pages.capacity == 0 || engine_pages.pending[1] != 0)
		return (0);
	mutex_enter(&engine.lock);
	boolean_t durable = engine.stored_desc[0] == ENGINE_STORE_MAGIC &&
	    memcmp(engine.stored_desc, engine.durable_desc,
	    sizeof (engine.stored_desc)) == 0;
	mutex_exit(&engine.lock);
	if (!durable)
		return (0);
	engine_mmarker_t m = { .spa = spa,
	    .marked = kmem_zalloc(engine_pages.capacity, KM_SLEEP) };
	for (engine_marena_t *a = engine_pages.arenas; a != NULL; a = a->next)
		m.marked[a->first] = m.marked[a->first + 1] = 1;
	if (engine_pages.directory_pages != NULL)
		zfs_range_tree_walk(engine_pages.directory_pages,
		    engine_pages_mark_range, &m);
	engine_mref_t blob = { .p = engine.stored_desc[4] };
	memcpy(&blob.hash, engine.stored_desc + 8, sizeof (blob.hash));
	int error = engine_pages_mark_ref(&m, &blob);
	if (error == 0)
		error = engine_pages_mark_ref(&m, &engine_pages.root);
	if (error == 0) {
		mutex_enter(&engine.lock);
		for (uint64_t i = 0; i < engine_pages.capacity; i++) {
			if (!engine_pages.bits[i] || m.marked[i])
				continue;
			engine_pages.bits[i] = 0;
			if (i < ENGINE_STORE_PAGES)
				engine.bitmap[i] = 0;
			if (engine_pages.cache[i] != NULL) {
				VERIFY(!engine_pages.cache[i]->dirty);
				kmem_free(engine_pages.cache[i],
				    sizeof (engine_mnode_t));
				engine_pages.cache[i] = NULL;
			}
			(*freed)++;
		}
		if (*freed != 0) {
			engine_pages.gc_relaxed = B_TRUE;
			engine.revision++;
		}
		mutex_exit(&engine.lock);
	}
	kmem_free(m.marked, engine_pages.capacity);
	return (error);
}

static int
engine_pages_manifest(spa_t *spa)
{
	engine_pages_init();
	if (engine_pages.directory_pages != NULL)
		return (0);
	engine_marena_t *initial = engine_pages.arenas;
	uint8_t *bits = kmem_zalloc(initial->pages, KM_SLEEP);
	uint64_t generation = 0;
	uint64_t serial = 0;
	engine_mcontrol_t control = { 0 };
	int error = engine_pages_reservations_arena(spa, initial, bits,
	    &generation, &serial, &control, B_FALSE);
	kmem_free(bits, initial->pages);
	if (error != 0)
		return (error);
	engine_pages.control = control;
	engine_pages.control_serial = control.serial;
	engine_pages.serial = serial;
	engine_pages.directory = control.directory;
	engine_pages.floor = control.floor;
	engine_pages.certified = control.certified;
	memcpy(engine_pages.pending, control.pending,
	    sizeof (engine_pages.pending));
	engine_mref_t ref = engine_pages.directory;
	engine_pages.directory_pages = zfs_range_tree_create(NULL,
	    ZFS_RANGE_SEG64, NULL, 0, 0);
	uint64_t stripe = ENGINE_STORE_PAGE * engine.vd->vdev_children;
	while (!engine_mref_empty(&ref)) {
		if (ref.generation == 0 || ref.generation > generation ||
		    zfs_range_tree_contains(engine_pages.directory_pages,
		    ref.p, stripe))
			return (EINVAL);
		engine_mnode_t *n;
		error = engine_mread_ref(spa, &ref, &n, B_TRUE);
		if (error != 0)
			return (error);
		engine_mheader_t *h = engine_mheader(n);
		uint64_t *data = engine_mpayload(n);
		if (h->type != ENGINE_M_DIRECTORY ||
		    data[0] >= ref.p || ref.p - data[0] != 2 * stripe ||
		    (data[2] == 0 && data[3] == 0)) {
			error = EINVAL;
		} else {
			error = engine_pages_add_arena(data[0], data[1]);
			if (error == 0) {
				engine_marena_t *a = engine_pages.arenas;
				memcpy(a->uuid, data + 2, sizeof (a->uuid));
				a->formatted = B_TRUE;
				a->directory_flushed = B_TRUE;
				a->format_generation = h->generation;
				a->candidate = ref;
			}
		}
		if (error == 0) {
			zfs_range_tree_add(engine_pages.directory_pages,
			    ref.p, stripe);
			uint64_t previous = ref.generation;
			memcpy(&ref, data + 4, sizeof (ref));
			if (!engine_mref_empty(&ref) &&
			    ref.generation > previous)
				error = EINVAL;
		}
		kmem_free(n, sizeof (*n));
		if (error != 0)
			return (error);
	}
	if (engine_pages.pending[1] != 0) {
		if (engine_pages.pending[2] == 0 &&
		    engine_pages.pending[3] == 0)
			return (EINVAL);
		error = engine_pages_add_arena(engine_pages.pending[0],
		    engine_pages.pending[1]);
		if (error != 0)
			return (error);
		engine_marena_t *a = engine_pages.arenas;
		memcpy(a->uuid, engine_pages.pending + 2, sizeof (a->uuid));
		a->pending = B_TRUE;
		memset(engine_pages.bits + a->first, 1, a->pages);
	} else {
		uint64_t empty[4] = { 0 };
		if (memcmp(engine_pages.pending, empty, sizeof (empty)) != 0)
			return (EINVAL);
	}
	return (0);
}

static void
engine_pages_pick(void *arg, uint64_t p, uint64_t size)
{
	uint64_t *range = arg;
	uint64_t stripe = ENGINE_STORE_PAGE * engine.vd->vdev_children;
	uint64_t start = roundup(p, stripe);
	if (start >= p && start - p <= size &&
	    range[1] <= size - (start - p))
		range[0] = start;
}

static int
engine_pages_directory_write(spa_t *spa, engine_marena_t *a,
    uint64_t generation, engine_mref_t *ref)
{
	uint64_t index = a->first + 2;
	engine_mnode_t *n = engine_pages.cache[index];
	engine_mref_t candidate = a->candidate;
	if (n == NULL) {
		n = kmem_zalloc(sizeof (*n), KM_SLEEP);
		engine_mheader_t *h = engine_mheader(n);
		h->magic = ENGINE_M_MAGIC; h->type = ENGINE_M_DIRECTORY;
		h->count = 10 * sizeof (uint64_t);
		h->p = a->p + 2 * ENGINE_STORE_PAGE * engine.vd->vdev_children;
		h->generation = generation;
		h->pool = spa_guid(spa); h->top = engine.vd->vdev_guid;
		memcpy(engine_mpayload(n), engine_pages.pending,
		    sizeof (engine_pages.pending));
		memcpy((uint64_t *)engine_mpayload(n) + 4,
		    &engine_pages.directory, sizeof (engine_pages.directory));
		candidate.p = h->p;
		candidate.generation = generation;
		boolean_t swap = getenv("PR19262_M_DIRECTORY_BYTESWAP") != NULL;
		if (swap) {
			byteswap_uint64_array(engine_mpayload(n), h->count);
			byteswap_uint64_array(h, sizeof (*h));
		}
		engine_mhash(n, &candidate.hash);
		h->hash = candidate.hash;
		if (swap)
			byteswap_uint64_array(&h->hash, sizeof (h->hash));
		if (a->candidate.generation != 0 &&
		    memcmp(&candidate, &a->candidate,
		    sizeof (candidate)) != 0) {
			kmem_free(n, sizeof (*n));
			return (ECKSUM);
		}
		n->sealed = B_TRUE;
		a->candidate = candidate;
		engine_pages.cache[index] = n;
	}
	engine_pages.bits[index] = 1;
	uint64_t stripe = ENGINE_STORE_PAGE * engine.vd->vdev_children;
	if (engine_pages.directory_pages == NULL)
		engine_pages.directory_pages = zfs_range_tree_create(NULL,
		    ZFS_RANGE_SEG64, NULL, 0, 0);
	/* A selectable partial ACTIVE must never have an unpinned DIR. */
	engine_mmark(engine_pages.directory_pages, candidate.p, stripe);
	/* PREPARE is already durable; now reserve before the first format. */
	int error = engine_pages_reserve(spa, generation);
	if (error == 0)
		error = engine_pages_reserve(spa, generation);
	if (error != 0)
		return (error);
	engine_pages_fault(12, generation, 0, candidate.p);
	abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	for (uint_t c = 0; c < engine.vd->vdev_children; c++) {
		int e = engine_store_io_at(engine.vd->vdev_child[c],
		    candidate.p, 0, 1, abd, B_TRUE);
		if (e == 0)
			e = pr19262_engine_metadata_flush(
			    engine.vd->vdev_child[c]);
		if (e != 0)
			error = e;
		else if (c == 0)
			engine_pages_fault(13, generation, c, candidate.p);
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	abd_free(abd);
	if (error == 0)
		*ref = candidate;
	return (error);
}

static int
engine_pages_finish(spa_t *spa, engine_marena_t *a, uint64_t generation)
{
	if (memcmp(a->uuid, engine_pages.pending + 2, sizeof (a->uuid)) != 0)
		return (ECKSUM);
	if (a->format_generation == 0)
		a->format_generation = generation;
	generation = a->format_generation;
	int error = 0;
	if (!a->directory_flushed) {
		error = engine_pages_reserve(spa, generation);
		if (error == 0)
			error = engine_pages_reserve(spa, generation);
		if (error != 0)
			return (error);
		engine_pages_fault(11, generation, 0, a->p);
		if (!a->formatted) {
			memset(engine_pages.bits + a->first, 0, a->pages);
			engine_pages.bits[a->first] = 1;
			engine_pages.bits[a->first + 1] = 1;
			a->formatted = B_TRUE;
		}
		error = engine_pages_directory_write(spa, a, generation,
		    &a->candidate);
		if (error != 0)
			return (error);
		a->directory_flushed = B_TRUE;
	}
	engine_mcontrol_t active = { .directory = a->candidate,
	    .floor = engine_pages.floor,
	    .certified = engine_pages.certified };
	engine_pages.publishing_active = B_TRUE;
	error = engine_pages_reserve_control(spa, generation, &active);
	engine_pages.publishing_active = B_FALSE;
	if (error != 0)
		return (error);
	/* Every ACTIVE carrier is durable before the RAM handoff. */
	engine_pages.directory = a->candidate;
	memset(engine_pages.pending, 0, sizeof (engine_pages.pending));
	a->pending = B_FALSE;
	engine_pages_fault(14, generation, 0, a->p);
	(void) printf("PR19262_M_GROW p=%llu pages=%llu capacity=%llu "
	    "generation=%llu serial=%llu\n", (u_longlong_t)a->p,
	    (u_longlong_t)a->pages, (u_longlong_t)engine_pages.capacity,
	    (u_longlong_t)generation, (u_longlong_t)engine_pages.serial);
	return (0);
}

static int
engine_pages_grow(engine_mbatch_t *b)
{
	if (engine_pages.pending[1] != 0)
		return (engine_pages_reconcile(b->spa, B_TRUE));
	uint64_t bytes = ENGINE_STORE_BYTES * engine.vd->vdev_children;
	uint64_t range[2] = { UINT64_MAX, bytes };
	mutex_enter(&engine.lock);
	zfs_range_tree_walk(engine.free, engine_pages_pick, range);
	if (range[0] == UINT64_MAX) {
		mutex_exit(&engine.lock);
		return (ENOSPC);
	}
	zfs_range_tree_remove(engine.free, range[0], bytes);
	zfs_range_tree_add(engine.store_extents, range[0], bytes);
	mutex_exit(&engine.lock);
	int error = engine_pages_add_arena(range[0], ENGINE_STORE_PAGES);
	if (error != 0) {
		mutex_enter(&engine.lock);
		zfs_range_tree_remove(engine.store_extents, range[0], bytes);
		zfs_range_tree_add(engine.free, range[0], bytes);
		mutex_exit(&engine.lock);
		return (error);
	}
	engine_marena_t *a = engine_pages.arenas;
	a->pending = B_TRUE;
	memset(engine_pages.bits + a->first, 1, a->pages);
	engine_pages.pending[0] = range[0];
	engine_pages.pending[1] = a->pages;
	VERIFY0(random_get_pseudo_bytes((void *)(engine_pages.pending + 2),
	    2 * sizeof (uint64_t)));
	if (engine_pages.pending[2] == 0 && engine_pages.pending[3] == 0)
		engine_pages.pending[2] = 1;
	memcpy(a->uuid, engine_pages.pending + 2, sizeof (a->uuid));
	return (engine_pages_finish(b->spa, a, b->generation));
}

static void
engine_pages_remove_pending(engine_marena_t *a)
{
	VERIFY3P(a, ==, engine_pages.arenas);
	VERIFY3U(a->first + a->pages, ==, engine_pages.capacity);
	uint64_t count = a->first;
	engine_mnode_t **cache = kmem_alloc(count * sizeof (*cache), KM_SLEEP);
	uint8_t *bits = kmem_alloc(count, KM_SLEEP);
	memcpy(cache, engine_pages.cache, count * sizeof (*cache));
	memcpy(bits, engine_pages.bits, count);
	for (uint64_t i = count; i < engine_pages.capacity; i++)
		if (engine_pages.cache[i] != NULL)
			kmem_free(engine_pages.cache[i],
			    sizeof (engine_mnode_t));
	kmem_free(engine_pages.cache,
	    engine_pages.capacity * sizeof (*engine_pages.cache));
	kmem_free(engine_pages.bits, engine_pages.capacity);
	engine_pages.cache = cache;
	engine_pages.bits = bits;
	engine_pages.capacity = count;
	engine_pages.arenas = a->next;
	kmem_free(a, sizeof (*a));
}

/* A PREPARE may have formatted bytes; never infer that from P membership. */
static int
engine_pages_pending_inspect(spa_t *spa, engine_marena_t *a)
{
	if (a->formatted)
		return (0);
	engine_mnode_t *n = kmem_zalloc(sizeof (*n), KM_SLEEP);
	engine_mnode_t *wire = kmem_zalloc(sizeof (*wire), KM_SLEEP);
	engine_mnode_t *directory = NULL;
	abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
	uint8_t *bits = kmem_zalloc(a->pages, KM_SLEEP);
	bits[0] = bits[1] = 1;
	uint64_t stripe = ENGINE_STORE_PAGE * engine.vd->vdev_children;
	boolean_t formatted = B_FALSE;
	uint64_t generation = 0, serial = engine_pages.serial;
	engine_mref_t candidate = { 0 };
	engine_mref_t forward = { 0 };
	engine_mcontrol_t highest = { 0 };
	boolean_t conflict = B_FALSE;
	int error = 0;
	/* Cold reconcile runs from spa_load with no config lock held. */
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	for (uint_t page = 0; error == 0 && page < 3; page++) {
		for (uint_t c = 0; error == 0 &&
		    c < engine.vd->vdev_children; c++) {
			uint64_t p = a->p + page * stripe;
			error = engine_store_io_at(engine.vd->vdev_child[c], p,
			    0, 1, abd, B_FALSE);
			if (error != 0)
				break;
			engine_mheader_t *h = engine_mheader(n);
			boolean_t swap = h->magic == BSWAP_64(ENGINE_M_MAGIC) ||
			    h->magic == BSWAP_64(ENGINE_M_ALLOC_MAGIC);
			if (!swap && h->magic != ENGINE_M_MAGIC &&
			    h->magic != ENGINE_M_ALLOC_MAGIC)
				continue;
			memcpy(wire->data, n->data, ENGINE_STORE_PAGE);
			zio_cksum_t hash;
			engine_mhash(n, &hash);
			if (swap)
				byteswap_uint64_array(h, sizeof (*h));
			if (memcmp(&h->hash, &hash, sizeof (hash)) != 0 ||
			    h->pool != spa_guid(spa) ||
			    h->top != engine.vd->vdev_guid ||
			    h->p != p || h->generation == 0 || h->level != 0 ||
			    (generation != 0 && generation != h->generation)) {
				error = ECKSUM;
				break;
			}
			generation = h->generation;
			if (page < 2) {
				uint8_t *mask = engine_mpayload(n);
				engine_mref_t published;
				uint64_t pending[4];
				uint64_t empty[4] = { 0 };
				memcpy(&published, h->reserved,
				    sizeof (published));
				memcpy(pending, h->reserved + 6,
				    sizeof (pending));
				boolean_t prepare = memcmp(&published,
				    &engine_pages.directory,
				    sizeof (published)) == 0 &&
				    memcmp(pending, engine_pages.pending,
				    sizeof (pending)) == 0;
				boolean_t active = memcmp(pending, empty,
				    sizeof (pending)) == 0 && published.p ==
				    a->p + 2 * stripe && published.generation ==
				    h->generation;
				if (h->magic != ENGINE_M_ALLOC_MAGIC ||
				    h->type != ENGINE_M_BLOB ||
				    h->count != a->pages || h->high[1] == 0 ||
				    h->low[0] != a->p ||
				    h->high[0] != a->pages ||
				    h->low[1] != a->pages * ENGINE_STORE_PAGE ||
				    h->low[2] != engine.vd->vdev_children ||
				    (!prepare && !active) ||
				    ((h->reserved[10] != 0 ||
				    h->reserved[11] != 0) &&
				    memcmp(h->reserved + 10, a->uuid,
				    sizeof (a->uuid)) != 0) ||
				    !mask[0] || !mask[1]) {
					error = ECKSUM;
					break;
				}
				if (active) {
					if (forward.generation != 0 &&
					    memcmp(&forward, &published,
					    sizeof (forward)) != 0) {
						error = ECKSUM;
						break;
					}
					forward = published;
				}
				engine_mcontrol_t control = {
				    .serial = h->high[1], .codec = h->magic,
				    .directory = published,
				    .floor = h->high[2],
				    .certified = h->high[3] };
				memcpy(control.pending, pending,
				    sizeof (pending));
				if (control.serial > highest.serial) {
					highest = control;
					conflict = B_FALSE;
				} else if (control.serial == highest.serial &&
				    !engine_mcontrol_same(&control, &highest)) {
					conflict = B_TRUE;
				}
				for (uint_t i = 0; i < a->pages; i++) {
					if (mask[i] > 1 ||
					    (i > 2 && mask[i] != 0))
						error = ECKSUM;
					bits[i] |= mask[i];
				}
				serial = MAX(serial, h->high[1]);
			} else {
				uint64_t *data = engine_mpayload(n);
				if (swap)
					byteswap_uint64_array(data,
					    10 * sizeof (*data));
				if (h->magic != ENGINE_M_MAGIC ||
				    h->type != ENGINE_M_DIRECTORY ||
				    h->count != 80 ||
				    memcmp(data, engine_pages.pending,
				    sizeof (engine_pages.pending)) != 0 ||
				    memcmp(data + 4, &engine_pages.directory,
				    sizeof (engine_pages.directory)) != 0) {
					error = ECKSUM;
					break;
				}
				engine_mref_t ref = engine_mreference(n);
				if (candidate.generation != 0 &&
				    memcmp(&candidate, &ref,
				    sizeof (ref)) != 0) {
					error = ECKSUM;
					break;
				}
				candidate = ref;
				if (directory == NULL) {
					directory = wire;
					wire = kmem_zalloc(sizeof (*wire),
					    KM_SLEEP);
					directory->sealed = B_TRUE;
				}
				bits[2] = 1;
			}
			formatted = B_TRUE;
		}
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	/* Arena-local ACTIVE must refer to this operation's verified DIR. */
	if (conflict)
		error = ECKSUM;
	if (error == 0 && forward.generation != 0 &&
	    (candidate.generation == 0 ||
	    memcmp(&forward, &candidate, sizeof (forward)) != 0))
		error = ECKSUM;
	if (error == 0 && formatted) {
		a->formatted = B_TRUE;
		a->format_generation = generation;
		a->candidate = candidate;
		if (directory != NULL) {
			/* Preserve the durable DIR's wire encoding. */
			engine_pages.cache[a->first + 2] = directory;
			directory = NULL;
		}
		memcpy(engine_pages.bits + a->first, bits, a->pages);
		engine_pages.serial = serial;
	}
	kmem_free(bits, a->pages);
	if (directory != NULL)
		kmem_free(directory, sizeof (*directory));
	kmem_free(wire, sizeof (*wire));
	abd_free(abd);
	kmem_free(n, sizeof (*n));
	return (error);
}

static int
engine_pages_reconcile(spa_t *spa, boolean_t write)
{
	if (engine_pages.pending[1] == 0)
		return (0);
	engine_marena_t *a = engine_pages.arenas;
	if (!a->pending || a->p != engine_pages.pending[0])
		return (EINVAL);
	if (write) {
		int error = engine_pages_pending_inspect(spa, a);
		if (error != 0)
			return (error);
	}
	uint64_t bytes = a->pages * ENGINE_STORE_PAGE *
	    engine.vd->vdev_children;
	mutex_enter(&engine.lock);
	boolean_t claimed = zfs_range_tree_contains(engine.store_extents,
	    a->p, bytes);
	boolean_t free = engine_store_extent(a->p, bytes);
	if (free) {
		zfs_range_tree_remove(engine.free, a->p, bytes);
		zfs_range_tree_add(engine.store_extents, a->p, bytes);
	}
	mutex_exit(&engine.lock);
	if (!write)
		return (0);
	if (free || claimed)
		return (engine_pages_finish(spa, a, engine_pages.generation));
	if (a->formatted)
		return (ECKSUM);
	/* A lone, unformatted PREPARE cannot override a recovered owner. */
	engine_mcontrol_t cancel = { .directory = engine_pages.directory,
	    .floor = engine_pages.floor,
	    .certified = engine_pages.certified };
	int error = engine_pages_reserve_control(spa, engine_pages.generation,
	    &cancel);
	if (error == 0)
		error = engine_pages_reserve_control(spa,
		    engine_pages.generation, &cancel);
	if (error != 0)
		return (error);
	memset(engine_pages.pending, 0, sizeof (engine_pages.pending));
	engine_pages_remove_pending(a);
	return (0);
}

typedef struct engine_munion {
	uint64_t *ranges;
	uint64_t at;
} engine_munion_t;

static void
engine_pages_union_record(void *arg, uint64_t p, uint64_t size)
{
	engine_munion_t *u = arg;
	u->ranges[u->at++] = BE_64(p);
	u->ranges[u->at++] = BE_64(size);
}

static void
engine_pages_probe_hash(void *data, uint64_t size, uint64_t result[4])
{
	abd_t *abd = abd_get_from_buf(data, size);
	zio_cksum_t hash;
	abd_checksum_sha256(abd, size, NULL, &hash);
	abd_free(abd);
	memcpy(result, &hash, sizeof (hash));
}

/* Private userspace witness: execute the real operation, including errors. */
static int
engine_pages_growth_probe(spa_t *spa, boolean_t advance,
    pr19262_mgrowth_probe_t *out)
{
	ASSERT(MUTEX_HELD(&engine.store_lock));
	if (engine_pages.capacity == 0) {
		memset(out, 0, sizeof (*out));
		return (ENOTSUP);
	}
	int error = 0;
	if (advance) {
		if (engine_pages.generation == UINT64_MAX)
			error = EOVERFLOW;
		else if (engine_pages.pending[1] != 0)
			error = engine_pages_reconcile(spa, B_TRUE);
		else {
			engine_mbatch_t b = { .spa = spa,
			    .generation = engine_pages.generation + 1 };
			error = engine_pages_grow(&b);
		}
	}
	memset(out, 0, sizeof (*out));
	uint64_t stripe = ENGINE_STORE_PAGE * engine.vd->vdev_children;
	engine_marena_t *operation = NULL;
	for (engine_marena_t *a = engine_pages.arenas; a != NULL;
	    a = a->next) {
		if ((a->pending && a->p == engine_pages.pending[0]) ||
		    (!a->pending && a->p + 2 * stripe ==
		    engine_pages.directory.p)) {
			operation = a;
			break;
		}
	}
	if (operation != NULL) {
		engine_marena_t *a = operation;
		out->p = a->p;
		out->pages = a->pages;
		memcpy(out->uuid, a->uuid, sizeof (out->uuid));
		if (a->pending)
			out->phase = 1 | (a->formatted ? 2 : 0) |
			    (a->directory_flushed ? 4 : 0);
		out->format_generation = a->format_generation;
		memcpy(out->candidate, &a->candidate, sizeof (out->candidate));
	}
	memcpy(out->directory, &engine_pages.directory,
	    sizeof (out->directory));
	out->serial = engine_pages.serial;
	out->control_serial = engine_pages.control_serial;
	out->capacity = engine_pages.capacity;
	out->floor = engine_pages.floor;
	out->certified = engine_pages.certified;
	uint64_t count = engine_pages.directory_pages == NULL ? 0 :
	    zfs_range_tree_numsegs(engine_pages.directory_pages);
	engine_munion_t u = { .ranges = kmem_zalloc(MAX(1, count) * 16,
	    KM_SLEEP) };
	if (engine_pages.directory_pages != NULL) {
		out->directory_bytes =
		    zfs_range_tree_space(engine_pages.directory_pages);
		zfs_range_tree_walk(engine_pages.directory_pages,
		    engine_pages_union_record, &u);
	}
	engine_pages_probe_hash(u.ranges, MAX(1, u.at) * 8,
	    out->directory_hash);
	kmem_free(u.ranges, MAX(1, count) * 16);
	uint8_t *bits = kmem_alloc(engine_pages.capacity, KM_SLEEP);
	mutex_enter(&engine.lock);
	memcpy(bits, engine_pages.bits, engine_pages.capacity);
	mutex_exit(&engine.lock);
	engine_pages_probe_hash(bits, engine_pages.capacity, out->bitmap_hash);
	/*
	 * Cache offsets follow discovery order, so also hash the same
	 * allocation bytes in physical arena order: ascending P, each
	 * arena prefixed by its BE64 base and page count.
	 */
	uint64_t arenas = 0, pages = 0;
	for (engine_marena_t *a = engine_pages.arenas; a != NULL;
	    a = a->next) {
		arenas++;
		pages += a->pages;
	}
	uint64_t size = arenas * 16 + pages, at = 0;
	uint8_t *canonical = kmem_alloc(size, KM_SLEEP);
	engine_marena_t *previous = NULL;
	for (uint64_t i = 0; i < arenas; i++) {
		engine_marena_t *next = NULL;
		for (engine_marena_t *a = engine_pages.arenas; a != NULL;
		    a = a->next) {
			if ((previous == NULL || a->p > previous->p) &&
			    (next == NULL || a->p < next->p))
				next = a;
		}
		uint64_t header[2] = { BE_64(next->p), BE_64(next->pages) };
		memcpy(canonical + at, header, sizeof (header));
		at += sizeof (header);
		memcpy(canonical + at, bits + next->first, next->pages);
		for (uint64_t j = 0; j < next->pages; j++)
			out->bitmap_used += bits[next->first + j] != 0;
		at += next->pages;
		previous = next;
	}
	engine_pages_probe_hash(canonical, at, out->bitmap_canonical);
	out->arenas = arenas;
	kmem_free(canonical, size);
	kmem_free(bits, engine_pages.capacity);
	return (error);
}

static int
engine_pages_store(spa_t *spa, const uberblock_t *ub, nvlist_t *nv,
    uint64_t desc[16])
{
	ASSERT(MUTEX_HELD(&engine.store_lock));
	engine_pages_init();
	if (engine_pages.pending[1] != 0) {
		int error = engine_pages_reconcile(spa, B_TRUE);
		if (error != 0)
			return (error);
	}
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
		if (nb == 0 || nb > engine_pages.capacity)
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
		/* Fence both legacy control slots before codec7 page writes. */
		if (error == 0)
			error = engine_pages_reserve(spa, b->generation);
		if (error == 0)
			engine_pages_fault(2, b->generation, 0, UINT64_MAX);
	}
	if (error == 0) {
		spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
		for (uint_t c = 0; c < engine.vd->vdev_children; c++) {
			for (uint64_t i = 0; i < engine_pages.capacity; i++) {
				engine_mnode_t *n = engine_pages.cache[i];
				if (!engine_mdirty(b, n))
					continue;
				VERIFY(n->sealed);
				abd_t *abd = abd_get_from_buf(n->data,
				    ENGINE_STORE_PAGE);
				int e = engine_mio(
				    engine.vd->vdev_child[c],
				    engine_mheader(n)->p, abd, B_TRUE);
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
		for (uint64_t i = 0; i < engine_pages.capacity; i++) {
			engine_mnode_t *n = engine_pages.cache[i];
			if (engine_mdirty(b, n))
				n->dirty = B_FALSE;
		}
		uint64_t d[16] = { ENGINE_STORE_MAGIC,
		    engine.vd->vdev_guid, engine.arena, engine.arena_bytes,
		    root.p, 1, ub->ub_txg, size, 0, 0, 0, 0,
		    engine.vd->vdev_ashift, engine.vd->vdev_children,
		    engine.total, 7 };
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
	zfs_range_tree_t *seen;
	engine_mrow_t *rows;
	uint_t count;
	uint_t capacity;
	uint64_t last[4];
	boolean_t have_last;
	uint64_t *tombs;
	uint_t tomb_count;
	uint_t tomb_capacity;
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
	if (row->data[6] == 3) {
		/* Keep released owner identities for journal folding. */
		if (row->key[3] != 0)
			return (0);
		if (r->tomb_count == r->tomb_capacity) {
			uint_t cap = r->tomb_capacity == 0 ? 64 :
			    r->tomb_capacity * 2;
			uint64_t *tombs = kmem_alloc(cap * 2 *
			    sizeof (uint64_t), KM_SLEEP);
			if (r->tombs != NULL) {
				memcpy(tombs, r->tombs, r->tomb_count * 2 *
				    sizeof (uint64_t));
				kmem_free(r->tombs, r->tomb_capacity * 2 *
				    sizeof (uint64_t));
			}
			r->tombs = tombs;
			r->tomb_capacity = cap;
		}
		r->tombs[2 * r->tomb_count] = row->key[1];
		r->tombs[2 * r->tomb_count + 1] = row->key[2];
		r->tomb_count++;
		return (0);
	}
	if (r->count == r->capacity) {
		uint64_t wanted = r->capacity == 0 ? 64 :
		    (uint64_t)r->capacity * 2;
		uint_t cap = MIN(wanted, engine_pages_row_limit());
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
	uint64_t stripe = ENGINE_STORE_PAGE * engine.vd->vdev_children;
	if (ref->generation == 0 || ref->generation > r->generation ||
	    !engine_mindex(ref->p, &index) ||
	    zfs_range_tree_contains(r->seen, ref->p, stripe))
		return (EINVAL);
	engine_mnode_t *n;
	int error = engine_mread(r->spa, ref, &n);
	if (error != 0)
		return (error);
	engine_mheader_t *h = engine_mheader(n);
	zfs_range_tree_add(r->seen, ref->p, stripe);
	if ((level != UINT64_MAX && level != h->level) ||
	    (h->type != ENGINE_M_LEAF && h->type != ENGINE_M_BRANCH)) {
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
	if (error == 0) {
		fnvlist_add_nvlist_array(nv, "owners",
		    (const nvlist_t * const *)owners, nr);
		if (r->tomb_count != 0)
			fnvlist_add_uint64_array(nv, "tombstones", r->tombs,
			    r->tomb_count * 2);
	}
	for (uint_t i = 0; i < nr; i++)
		fnvlist_free(owners[i]);
	kmem_free(owners, MAX(1, r->count) * sizeof (*owners));
	return (error);
}

static int
engine_pages_read_once(spa_t *spa, const uberblock_t *ub, nvlist_t **out,
	zfs_range_tree_t *seen, boolean_t allocator)
{
	if (allocator) {
		int error = engine_pages_manifest(spa);
		if (error != 0)
			return (error);
		/* Pages of roots below the floor may already be reused. */
		if (ub->ub_txg < engine_pages.floor) {
			(void) printf("PR19262_FLOOR_REFUSED ub=%llu "
			    "floor=%llu\n", (u_longlong_t)ub->ub_txg,
			    (u_longlong_t)engine_pages.floor);
			return (SET_ERROR(ESTALE));
		}
	}
	const uint64_t *d = ub->ub_pr19262;
	engine_mreader_t *r = kmem_zalloc(sizeof (*r), KM_SLEEP);
	r->spa = spa;
	r->seen = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64, NULL, 0, 0);
	uint64_t stripe = ENGINE_STORE_PAGE * engine.vd->vdev_children;
	engine_mref_t ref = { .p = d[4] };
	memcpy(&ref.hash, d + 8, sizeof (ref.hash));
	char *packed = kmem_alloc(d[7], KM_SLEEP);
	uint64_t offset = 0;
	int error = 0;
	while (error == 0 && ref.p != UINT64_MAX) {
		uint64_t index;
		if (!engine_mindex(ref.p, &index) ||
		    zfs_range_tree_contains(r->seen, ref.p, stripe)) {
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
			zfs_range_tree_add(r->seen, ref.p, stripe);
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
		if (bitmap[i] > 1 ||
		    (zfs_range_tree_contains(r->seen,
		    engine.arena + i * stripe, stripe) && !bitmap[i]))
			error = EINVAL;
	if (error == 0 && (!bitmap[0] || !bitmap[1]))
		error = EINVAL;
	if (error == 0 && allocator) {
		uint8_t *reserved = kmem_zalloc(engine_pages.capacity,
		    KM_SLEEP);
		uint64_t latest = 0;
		uint64_t serial = 0;
		error = engine_pages_reservations(spa, reserved, &latest,
		    &serial);
		if (error == 0 && latest < r->generation)
			error = ECKSUM;
		for (uint64_t i = 0; error == 0 &&
		    i < engine_pages.capacity; i++)
			if (!reserved[i] && zfs_range_tree_contains(r->seen,
			    engine_mphysical(i), stripe))
				error = ECKSUM;
		if (error == 0) {
			engine_pages.serial = MAX(engine_pages.serial, serial);
			/* Control masks are the allocator; roots only read. */
			memcpy(bitmap, reserved, ENGINE_STORE_PAGES);
			memcpy(engine_pages.bits, reserved,
			    engine_pages.capacity);
			fnvlist_add_uint64(nv, "page-reservation-generation",
			    latest);
		}
		kmem_free(reserved, engine_pages.capacity);
	}
	if (error == 0)
		error = engine_mowners(r, nv);
	if (error == 0 && allocator)
		error = engine_pages_control_check(spa, B_FALSE);
	if (error == 0) {
		for (engine_marena_t *a = engine_pages.arenas; a != NULL;
		    a = a->next)
			zfs_range_tree_add(r->seen, a->p, 2 * stripe);
		if (engine_pages.directory_pages != NULL)
			zfs_range_tree_walk(engine_pages.directory_pages,
			    engine_mmark, r->seen);
		if (seen != NULL)
			zfs_range_tree_walk(r->seen, engine_mmark, seen);
		*out = nv;
	} else {
		nvlist_free(nv);
	}
	if (r->rows != NULL)
		kmem_free(r->rows, r->capacity * sizeof (engine_mrow_t));
	if (r->tombs != NULL)
		kmem_free(r->tombs, r->tomb_capacity * 2 * sizeof (uint64_t));
	zfs_range_tree_vacate(r->seen, NULL, NULL);
	zfs_range_tree_destroy(r->seen);
	kmem_free(r, sizeof (*r));
	return (error);
}

static int
engine_pages_read(spa_t *spa, const uberblock_t *ub, nvlist_t **out,
    zfs_range_tree_t *seen, boolean_t allocator)
{
	for (uint_t attempt = 0; attempt < 3; attempt++) {
		int error = engine_pages_read_once(spa, ub, out, seen,
		    allocator);
		if (error != EAGAIN || !allocator || engine.bitmap != NULL)
			return (error);
		/* Nothing has been admitted to the physical allocator yet. */
		engine_pages_clear();
		(void) printf("PR19262_M_CONTROL_RETRY attempt=%u\n",
		    attempt + 1);
	}
	return (EAGAIN);
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
	engine_pages_tree_clear();
	memcpy(engine_pages.bits, engine.bitmap, ENGINE_STORE_PAGES);
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
