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

/*
 * A catalog page in memory.  The page is its own 4 KiB buffer: kept inline,
 * the 4104-byte node took an 8 KiB slab and doubled the memory of every
 * cached page.  Nodes come only from engine_mnode_alloc().
 */
typedef struct engine_mnode {
	uint64_t *data;
	boolean_t dirty;
	boolean_t sealed;
	/* The batch generation that last used the page: eviction order. */
	uint64_t used;
} engine_mnode_t;

static engine_mnode_t *
engine_mnode_alloc(void)
{
	engine_mnode_t *n = kmem_zalloc(sizeof (*n), KM_SLEEP);
	n->data = kmem_zalloc(ENGINE_STORE_PAGE, KM_SLEEP);
	return (n);
}

static void
engine_mnode_free(engine_mnode_t *n)
{
	kmem_free(n->data, ENGINE_STORE_PAGE);
	kmem_free(n, sizeof (*n));
}

typedef struct engine_mbatch {
	spa_t *spa;
	uint64_t generation;
	uint_t pages;
	uint_t changes;
	uint_t deletes;
	uint_t tombstones;
	uint_t joins;
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
	/* Auxiliary arenas have their own consecutive publication serials. */
	uint64_t serial;
	uint64_t slot_serial[2];
	uint8_t *slots;
	uint64_t cache_errors;
	uint64_t cache_config;
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

typedef struct engine_pages {
	engine_mnode_t **cache;
	uint8_t *bits;
	uint64_t capacity;
	/* Only ACTIVE arenas contribute to admission's free-page budget. */
	uint64_t available;
	/*
	 * Every page below it is allocated: a single free lowers it, a bulk
	 * change of the bits resets it to 0.
	 */
	uint64_t free_hint;
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
	/* Rows in the stored tree, tombstones not counted. */
	uint64_t live;
	/*
	 * Legacy version tombstones (key[3] != 0) found at load: never
	 * deleted, they keep leaf slots (counted by the admission bound).
	 */
	uint64_t legacy_dead;
	/* Tree height: one leaf level plus the branch levels above it. */
	uint64_t levels;
	/* Highest control serial each member holds; stale ones resilver. */
	uint64_t member_serial[256];
} engine_pages_t;

static int engine_pages_grow(engine_mbatch_t *);
static int engine_pages_read(spa_t *, const uberblock_t *, nvlist_t **,
    zfs_range_tree_t *, boolean_t);
static int engine_pages_reconcile(spa_t *, boolean_t);
static int engine_pages_control_repair(spa_t *, const engine_mcontrol_t *);
static boolean_t engine_store_extent(engine_t *, uint64_t, uint64_t);

/*
 * Readers holding only engine.lock (admission, the census) walk the arena
 * list and the page bits.  A new arena is prepared in full, bits and
 * pending flag included, and published with the new arrays under
 * engine.lock; the old arrays are freed after.  Callers hold the store
 * lock, never engine.lock.
 */
static int
engine_pages_add_arena(engine_t *engine, uint64_t p, uint64_t pages,
    const uint8_t *init,
    boolean_t pending)
{
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	if (pages < 3 || pages > ENGINE_M_PAYLOAD ||
	    pages > engine->total / stripe ||
	    p % stripe != 0 || p >= engine->total ||
	    pages > (engine->total - p) / stripe ||
	    engine->pages->capacity > engine->total / stripe - pages)
		return (ENGINE_EINVAL(1));
	for (engine_marena_t *a = engine->pages->arenas; a != NULL;
	    a = a->next)
		if (p < a->p + a->pages * stripe &&
		    a->p < p + pages * stripe)
			return (ENGINE_EINVAL(2));
	uint64_t old = engine->pages->capacity, count = old + pages;
	if (count > SIZE_MAX / sizeof (engine_mnode_t *))
		return (EOVERFLOW);
	engine_mnode_t **cache = pr19262_zalloc(count * sizeof (*cache),
	    KM_SLEEP);
	uint8_t *bits = pr19262_zalloc(count, KM_SLEEP);
	if (old != 0) {
		memcpy(cache, engine->pages->cache, old * sizeof (*cache));
		memcpy(bits, engine->pages->bits, old);
	}
	/* A pending arena stays allocated until its growth finishes. */
	if (pending)
		memset(bits + old, 1, pages);
	else if (init != NULL)
		memcpy(bits + old, init, pages);
	else
		bits[old] = bits[old + 1] = 1;
	engine_marena_t *a = kmem_zalloc(sizeof (*a), KM_SLEEP);
	a->p = p; a->first = old; a->pages = pages; a->pending = pending;
	a->slots = kmem_zalloc(2 * pages, KM_SLEEP);
	engine_mnode_t **old_cache = engine->pages->cache;
	uint8_t *old_bits = engine->pages->bits;
	ASSERT(!MUTEX_HELD(&engine->lock));
	mutex_enter(&engine->lock);
	a->next = engine->pages->arenas;
	engine->pages->arenas = a;
	engine->pages->cache = cache;
	engine->pages->bits = bits;
	engine->pages->capacity = count;
	engine->pages->free_hint = 0;
	if (!pending)
		for (uint64_t i = old; i < count; i++)
			engine->pages->available += bits[i] == 0;
	mutex_exit(&engine->lock);
	if (old != 0) {
		kmem_free(old_cache, old * sizeof (*cache));
		kmem_free(old_bits, old);
	}
	return (0);
}

static void
engine_pages_init(engine_t *engine)
{
	if (engine->pages->capacity != 0)
		return;
	VERIFY0(engine_pages_add_arena(engine, engine->arena,
	    ENGINE_STORE_PAGES,
	    engine->bitmap, B_FALSE));
}

	/* Bootstrap restore only; live mutations maintain the count. */
static void
engine_pages_recount_free(engine_t *engine)
{
	uint64_t available = 0;
	for (engine_marena_t *a = engine->pages->arenas; a != NULL; a =
	    a->next) {
		if (a->pending)
			continue;
		for (uint64_t i = 0; i < a->pages; i++)
			available += engine->pages->bits[a->first + i] == 0;
	}
	engine->pages->available = available;
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
engine_mindex(engine_t *engine, uint64_t p, uint64_t *index)
{
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	engine_pages_init(engine);
	for (engine_marena_t *a = engine->pages->arenas; a != NULL;
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
engine_mphysical(engine_t *engine, uint64_t index)
{
	for (engine_marena_t *a = engine->pages->arenas; a != NULL;
	    a = a->next)
		if (index >= a->first && index - a->first < a->pages)
			return (a->p + (index - a->first) * ENGINE_STORE_PAGE *
			    engine->vd->vdev_children);
	return (UINT64_MAX);
}

static int
engine_mio(vdev_t *vd, uint64_t p, abd_t *abd, boolean_t write)
{
	engine_t *engine = vd->vdev_spa->spa_reloc;
	uint64_t index;
	if (!engine_mindex(engine, p, &index))
		return (ENGINE_EINVAL(3));
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

/* Is the copy read into n the page ref names?  A foreign one is swapped. */
static boolean_t
engine_mpage_accept(spa_t *spa, const engine_mref_t *ref, engine_mnode_t *n)
{
	engine_t *engine = spa->spa_reloc;
	engine_mheader_t *h = engine_mheader(n);
	boolean_t swap = h->magic == BSWAP_64(ENGINE_M_MAGIC);
	if (h->magic != ENGINE_M_MAGIC && !swap)
		return (B_FALSE);
	zio_cksum_t hash;
	engine_mhash(n, &hash);
	if (memcmp(&hash, &ref->hash, sizeof (hash)) != 0)
		return (B_FALSE);
	if (swap)
		byteswap_uint64_array(h, sizeof (*h));
	if (h->p != ref->p || h->generation == 0 ||
	    (ref->generation != 0 &&
	    ref->generation != h->generation) ||
	    h->pool != spa_guid(spa) ||
	    h->top != engine->vd->vdev_guid ||
	    memcmp(&h->hash, &ref->hash, sizeof (hash)) != 0 ||
	    h->level > ENGINE_M_LEVELS ||
	    (h->type != ENGINE_M_LEAF && h->type != ENGINE_M_BRANCH &&
	    h->type != ENGINE_M_BLOB && h->type != ENGINE_M_DIRECTORY))
		return (B_FALSE);
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
		return (B_FALSE);
	if (swap && h->type == ENGINE_M_DIRECTORY)
		byteswap_uint64_array(engine_mpayload(n), h->count);
	else if (swap && h->type != ENGINE_M_BLOB)
		byteswap_uint64_array(engine_mpayload(n),
		    h->count * stride);
	return (B_TRUE);
}

static int
engine_mread_ref(spa_t *spa, const engine_mref_t *ref,
    engine_mnode_t **result, boolean_t bootstrap)
{
	engine_t *engine = spa->spa_reloc;
	uint64_t index;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	if (ref->p % stripe != 0 || ref->p >= engine->total ||
	    stripe > engine->total - ref->p ||
	    (!bootstrap && !engine_mindex(engine, ref->p, &index)))
		return (ENGINE_EINVAL(4));
	engine_mnode_t *n = engine_mnode_alloc();
	abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
	int error = ECKSUM;
	for (uint_t c = 0; c < engine->vd->vdev_children; c++) {
		if (engine_store_io_at(engine->vd->vdev_child[c], ref->p,
		    0, 1, abd, B_FALSE) != 0 ||
		    !engine_mpage_accept(spa, ref, n))
			continue;
		error = 0;
		break;
	}
	abd_free(abd);
	if (error != 0) {
		engine_mnode_free(n);
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
	engine_t *engine = b->spa->spa_reloc;
	uint64_t index;
	if (!engine_mindex(engine, ref->p, &index))
		return (ENGINE_EINVAL(5));
	engine_mnode_t *n = engine->pages->cache[index];
	if (n == NULL) {
		int error = engine_mread(b->spa, ref, &n);
		if (error != 0)
			return (error);
		engine->pages->cache[index] = n;
	}
	engine_mheader_t *h = engine_mheader(n);
	if (h->generation != ref->generation ||
	    (!(n->dirty && h->generation == b->generation) &&
	    memcmp(&h->hash, &ref->hash, sizeof (h->hash)) != 0))
		return (ECKSUM);
	n->used = b->generation;
	*result = n;
	return (0);
}

/*
 * The lowest free page, or the capacity: from the hint on, so a store's
 * new pages cost no scan of the whole arena space.
 */
static uint64_t
engine_pages_free_index(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t hint = MIN(engine->pages->free_hint, engine->pages->capacity);
#ifdef ZFS_DEBUG
	for (uint64_t i = 0; i < hint; i++)
		VERIFY(engine->pages->bits[i] != 0);
#endif
	for (uint64_t i = hint; i < engine->pages->capacity; i++)
		if (engine->pages->bits[i] == 0)
			return (i);
	/* A missed hint update costs a scan, never a growth. */
	for (uint64_t i = 0; i < hint; i++)
		if (engine->pages->bits[i] == 0)
			return (i);
	return (engine->pages->capacity);
}

static int
engine_mnew(engine_mbatch_t *b, uint64_t type, uint64_t level,
    engine_mnode_t **result)
{
	engine_t *engine = b->spa->spa_reloc;
	uint64_t index;
	engine_pages_init(engine);
	if (engine->pages->pending[1] != 0) {
		int error = engine_pages_reconcile(b->spa, B_TRUE);
		if (error != 0)
			return (error);
	}
	mutex_enter(&engine->lock);
	index = engine_pages_free_index(engine);
	if (index == engine->pages->capacity) {
		mutex_exit(&engine->lock);
		int error = engine_pages_grow(b);
		if (error != 0)
			return (error);
		return (engine_mnew(b, type, level, result));
	}
	engine->pages->bits[index] = 1;
	VERIFY3U(engine->pages->available, >, 0);
	engine->pages->available--;
	engine->pages->free_hint = index + 1;
	if (index < ENGINE_STORE_PAGES)
		engine->bitmap[index] = 1;
	mutex_exit(&engine->lock);
	engine_mnode_t *n = engine_mnode_alloc();
	engine_mheader_t *h = engine_mheader(n);
	h->magic = ENGINE_M_MAGIC;
	h->type = type; h->level = level;
	h->p = engine_mphysical(engine, index);
	h->generation = b->generation;
	h->pool = spa_guid(b->spa); h->top = engine->vd->vdev_guid;
	n->dirty = B_TRUE;
	n->used = b->generation;
	VERIFY3P(engine->pages->cache[index], ==, NULL);
	engine->pages->cache[index] = n;
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

/* A page whose low key a branch rebuild knows without reading it. */
typedef struct engine_mlow {
	uint64_t p;
	uint64_t low[4];
} engine_mlow_t;

static void
engine_mlow_add(engine_mlow_t *known, uint_t *n, uint64_t p,
    engine_mnode_t *node)
{
	known[*n].p = p;
	memcpy(known[*n].low, engine_mheader(node)->low,
	    sizeof (known[*n].low));
	(*n)++;
}

/*
 * A branch's low key is its first child's, exact (import checks it).  A
 * rebuild knows it for the children it changed (in memory) and for the
 * old first child (the old branch's low); only another first child, after
 * a split or a removal, is read.  Debug ztest reads it always and checks.
 */
static int
engine_mbranch_set(engine_mbatch_t *b, engine_mnode_t *n,
    const engine_mchild_t *children, uint_t count,
    const engine_mlow_t *known, uint_t nknown)
{
	uint64_t low[4];
	const uint64_t *found = NULL;
	for (uint_t i = 0; found == NULL && i < nknown; i++)
		if (known[i].p == children[0].ref.p)
			found = known[i].low;
#if defined(ZFS_DEBUG) && !defined(_KERNEL)
	boolean_t check = B_TRUE;
#else
	boolean_t check = B_FALSE;
#endif
	if (found == NULL || check) {
		engine_mnode_t *first;
		int error = engine_mget(b, &children[0].ref, &first);
		if (error != 0)
			return (error);
		if (found != NULL)
			VERIFY0(memcmp(found, engine_mheader(first)->low,
			    sizeof (low)));
		found = engine_mheader(first)->low;
	}
	memcpy(low, found, sizeof (low));
	engine_mheader_t *h = engine_mheader(n);
	memset(engine_mpayload(n), 0, ENGINE_M_PAYLOAD);
	memcpy(engine_mpayload(n), children, count * sizeof (*children));
	h->count = count;
	memcpy(h->low, low, sizeof (h->low));
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
		engine_mrow_t *copy = pr19262_alloc(
		    (count + 1) * sizeof (*copy),
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
		return (ENGINE_EINVAL(6));
	engine_mchild_t *children = engine_mpayload(old);
	uint_t at = 0, count = h->count;
	while (at + 1 < count && engine_mkey_compare(row->key,
	    children[at].high) > 0)
		at++;
	engine_mchange_t child;
	error = engine_minsert(b, &children[at].ref, row, &child);
	if (error != 0 || !child.changed)
		return (error);
	engine_mchild_t *copy = pr19262_alloc((count + 1) * sizeof (*copy),
	    KM_SLEEP);
	memcpy(copy, children, count * sizeof (*copy));
	engine_mlow_t known[3];
	uint_t nknown = 0;
	engine_mnode_t *child_node;
	error = engine_mget(b, &child.left, &child_node);
	if (error == 0) {
		copy[at].ref = child.left;
		memcpy(copy[at].high, engine_mheader(child_node)->high,
		    sizeof (copy[at].high));
		engine_mlow_add(known, &nknown, child.left.p, child_node);
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
			engine_mlow_add(known, &nknown, child.right.p,
			    child_node);
			total++;
		}
	}
	/* The old first child: the old branch's low. */
	known[nknown].p = children[0].ref.p;
	memcpy(known[nknown].low, h->low, sizeof (known[nknown].low));
	nknown++;
	engine_mnode_t *left, *right = NULL;
	if (error == 0)
		error = engine_mcow(b, old, &left);
	if (error == 0 && total > ENGINE_M_CHILDREN)
		error = engine_mnew(b, ENGINE_M_BRANCH, h->level, &right);
	if (error == 0) {
		uint_t half = right != NULL ? total / 2 : total;
		error = engine_mbranch_set(b, left, copy, half, known,
		    nknown);
		if (error == 0 && right != NULL)
			error = engine_mbranch_set(b, right, copy + half,
			    total - half, known, nknown);
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
	engine_mlow_t known[2];
	uint_t nknown = 0;
	engine_mlow_add(known, &nknown, change.left.p, left);
	engine_mlow_add(known, &nknown, change.right.p, right);
	error = engine_mbranch_set(b, branch, children, 2, known, nknown);
	if (error == 0) {
		*root = engine_mreference(branch);
		b->changes++;
	}
	return (error);
}

/* A page allocated in this batch that nothing references any more. */
static void
engine_mrelease(engine_mbatch_t *b, engine_mnode_t *n)
{
	engine_t *engine = b->spa->spa_reloc;
	engine_mheader_t *h = engine_mheader(n);
	if (!n->dirty || h->generation != b->generation)
		return;
	uint64_t index;
	VERIFY(engine_mindex(engine, h->p, &index));
	VERIFY3P(engine->pages->cache[index], ==, n);
	mutex_enter(&engine->lock);
	engine->pages->bits[index] = 0;
	engine->pages->available++;
	engine->pages->free_hint = MIN(engine->pages->free_hint, index);
	if (index < ENGINE_STORE_PAGES)
		engine->bitmap[index] = 0;
	mutex_exit(&engine->lock);
	engine->pages->cache[index] = NULL;
	engine_mnode_free(n);
	VERIFY3U(b->pages, >, 0);
	b->pages--;
}

/*
 * Remove one row along a COW path.  A leaf or branch left empty drops out
 * of its parent; the root may become empty.  Durable pages abandoned this
 * way are unreachable and the fence's page collection returns them; pages
 * of this batch are released at once.
 */
/*
 * Deletes never merge leaves on their own, so a catalog that churns thins
 * out (a quarter full after a conversion).  A leaf left under half full
 * takes in its neighbour's rows when both fit in three quarters of a page.
 * The leaf is the copy this batch already made, so the join writes no
 * page; the neighbour's page goes (now if this batch made it, otherwise
 * at the next page GC).
 */
static int
engine_mjoin(engine_mbatch_t *b, engine_mchild_t *copy, uint_t *total,
    uint_t at, engine_mnode_t *leaf)
{
	engine_mheader_t *lh = engine_mheader(leaf);
	if (lh->type != ENGINE_M_LEAF || lh->count >= ENGINE_M_ROWS / 2)
		return (0);
	uint_t sib = at + 1 < *total ? at + 1 : at - 1;
	engine_mnode_t *other;
	int error = engine_mget(b, &copy[sib].ref, &other);
	if (error != 0)
		return (error);
	engine_mheader_t *oh = engine_mheader(other);
	if (oh->type != ENGINE_M_LEAF ||
	    lh->count + oh->count > ENGINE_M_ROWS * 3 / 4)
		return (0);
	VERIFY(leaf->dirty && lh->generation == b->generation);
	uint_t n = lh->count + oh->count;
	engine_mnode_t *first = sib < at ? other : leaf;
	uint_t nf = engine_mheader(first)->count;
	engine_mrow_t *rows = kmem_alloc(n * sizeof (*rows), KM_SLEEP);
	memcpy(rows, engine_mpayload(first), nf * sizeof (*rows));
	memcpy(rows + nf, engine_mpayload(sib < at ? leaf : other),
	    (n - nf) * sizeof (*rows));
	engine_mleaf_set(leaf, rows, n);
	kmem_free(rows, n * sizeof (*rows));
	engine_mrelease(b, other);
	uint_t lo = MIN(at, sib), hi = MAX(at, sib);
	copy[lo].ref = engine_mreference(leaf);
	memcpy(copy[lo].high, lh->high, sizeof (copy[lo].high));
	memmove(copy + hi, copy + hi + 1, (*total - hi - 1) * sizeof (*copy));
	(*total)--;
	b->joins++;
	return (0);
}

static int
engine_mremove(engine_mbatch_t *b, const engine_mref_t *root,
    const uint64_t key[4], engine_mchange_t *change)
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
		while (at < count && engine_mkey_compare(rows[at].key, key) < 0)
			at++;
		if (at == count || engine_mkey_compare(rows[at].key, key) != 0)
			return (0);
		if (count == 1) {
			change->left.p = UINT64_MAX;
			change->changed = B_TRUE;
			engine_mrelease(b, old);
			return (0);
		}
		engine_mrow_t *copy = pr19262_alloc(count * sizeof (*copy),
		    KM_SLEEP);
		memcpy(copy, rows, at * sizeof (*copy));
		memcpy(copy + at, rows + at + 1,
		    (count - at - 1) * sizeof (*copy));
		engine_mnode_t *left;
		error = engine_mcow(b, old, &left);
		if (error == 0) {
			engine_mleaf_set(left, copy, count - 1);
			change->left = engine_mreference(left);
			change->changed = B_TRUE;
		}
		kmem_free(copy, count * sizeof (*copy));
		return (error);
	}
	if (h->type != ENGINE_M_BRANCH || h->level == 0)
		return (ENGINE_EINVAL(7));
	engine_mchild_t *children = engine_mpayload(old);
	uint_t at = 0, count = h->count;
	while (at + 1 < count && engine_mkey_compare(key,
	    children[at].high) > 0)
		at++;
	engine_mchange_t child;
	error = engine_mremove(b, &children[at].ref, key, &child);
	if (error != 0 || !child.changed)
		return (error);
	engine_mchild_t *copy = kmem_alloc(count * sizeof (*copy), KM_SLEEP);
	memcpy(copy, children, count * sizeof (*copy));
	engine_mlow_t known[2];
	uint_t nknown = 0;
	engine_mnode_t *child_node = NULL;
	uint_t total = count;
	if (child.left.p == UINT64_MAX) {
		memmove(copy + at, copy + at + 1,
		    (count - at - 1) * sizeof (*copy));
		total--;
	} else {
		error = engine_mget(b, &child.left, &child_node);
		if (error == 0) {
			copy[at].ref = child.left;
			memcpy(copy[at].high, engine_mheader(child_node)->high,
			    sizeof (copy[at].high));
		}
		if (error == 0 && h->level == 1 && total > 1)
			error = engine_mjoin(b, copy, &total, at, child_node);
	}
	/* After any join: the changed child's low, then the old first's. */
	if (error == 0 && child_node != NULL)
		engine_mlow_add(known, &nknown, child.left.p, child_node);
	known[nknown].p = children[0].ref.p;
	memcpy(known[nknown].low, h->low, sizeof (known[nknown].low));
	nknown++;
	if (error == 0 && total == 0) {
		change->left.p = UINT64_MAX;
		change->changed = B_TRUE;
		engine_mrelease(b, old);
	} else if (error == 0) {
		engine_mnode_t *left;
		error = engine_mcow(b, old, &left);
		if (error == 0)
			error = engine_mbranch_set(b, left, copy, total, known,
			    nknown);
		if (error == 0) {
			change->left = engine_mreference(left);
			change->changed = B_TRUE;
		}
	}
	kmem_free(copy, count * sizeof (*copy));
	return (error);
}

static int
engine_mdel(engine_mbatch_t *b, engine_mref_t *root, const uint64_t key[4])
{
	if (root->p == UINT64_MAX)
		return (0);
	engine_mchange_t change;
	int error = engine_mremove(b, root, key, &change);
	if (error != 0 || !change.changed)
		return (error);
	if (change.left.p == UINT64_MAX) {
		memset(root, 0, sizeof (*root));
		root->p = UINT64_MAX;
	} else {
		*root = change.left;
	}
	b->changes++;
	b->deletes++;
	return (0);
}

static int
engine_mseal(engine_mbatch_t *b, engine_mref_t *ref)
{
	if (ref->p == UINT64_MAX)
		return (0);
	engine_mnode_t *n;
	int error;
	/*
	 * Only this batch's pages carry its generation; an older page is
	 * sealed and its reference final, so it needs no lookup (an evicted
	 * leaf under a changed branch would be read back).  Debug ztest
	 * looks and checks.
	 */
	if (ref->generation != b->generation) {
#if defined(ZFS_DEBUG) && !defined(_KERNEL)
		error = engine_mget(b, ref, &n);
		if (error != 0)
			return (error);
		engine_mref_t same = engine_mreference(n);
		VERIFY(n->sealed);
		VERIFY0(memcmp(&same, ref, sizeof (same)));
#endif
		return (0);
	}
	error = engine_mget(b, ref, &n);
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
engine_pages_tree_clear(engine_t *engine)
{
	for (uint64_t i = 0; i < engine->pages->capacity; i++) {
		if (engine->pages->cache[i] != NULL)
			engine_mnode_free(engine->pages->cache[i]);
		engine->pages->cache[i] = NULL;
	}
	if (engine->pages->rows != NULL)
		kmem_free(engine->pages->rows,
		    MAX(1, engine->pages->count) * sizeof (engine_mrow_t));
	engine->pages->rows = NULL;
	engine->pages->count = 0;
	engine->pages->live = 0;
	memset(&engine->pages->root, 0, sizeof (engine->pages->root));
	engine->pages->root.p = UINT64_MAX;
}

static void
engine_pages_clear(engine_t *engine)
{
	engine_pages_tree_clear(engine);
	if (engine->pages->capacity != 0) {
		kmem_free(engine->pages->cache,
		    engine->pages->capacity * sizeof (*engine->pages->cache));
		kmem_free(engine->pages->bits, engine->pages->capacity);
	}
	while (engine->pages->arenas != NULL) {
		engine_marena_t *a = engine->pages->arenas;
		engine->pages->arenas = a->next;
		kmem_free(a->slots, 2 * a->pages);
		kmem_free(a, sizeof (*a));
	}
	if (engine->pages->directory_pages != NULL) {
		zfs_range_tree_vacate(engine->pages->directory_pages, NULL,
		    NULL);
		zfs_range_tree_destroy(engine->pages->directory_pages);
	}
	memset(engine->pages, 0, sizeof (*engine->pages));
	engine->pages->root.p = UINT64_MAX;
}

static uint64_t
engine_pages_row_limit(engine_t *engine)
{
	return (MIN(UINT_MAX, engine->pages->capacity * ENGINE_M_ROWS));
}

/* A catalog row: one owner's identity with one of its versions. */
static void
engine_mrow_fill(engine_mrow_t *r, const uint64_t *v, const uint64_t *p,
    boolean_t head)
{
	r->key[0] = 1; r->key[1] = v[0]; r->key[2] = v[3];
	r->key[3] = head ? 0 : p[7];
	memcpy(r->data, v, 8 * sizeof (uint64_t));
	memcpy(r->data + 8, p, 8 * sizeof (uint64_t));
}

/* Rows in key order; a duplicate key is a malformed catalog. */
static int
engine_mrows_order(engine_mrow_t *rows, uint_t total)
{
	qsort(rows, total, sizeof (*rows), engine_mkey_compare);
	for (uint_t i = 1; i < total; i++)
		if (engine_mkey_compare(rows[i - 1].key, rows[i].key) >= 0)
			return (ENGINE_EINVAL(20));
	return (0);
}

static int
engine_mrows(engine_t *engine, nvlist_t *nv, engine_mrow_t **out, uint_t *count)
{
	nvlist_t **owners;
	uint_t nr;
	if (nvlist_lookup_nvlist_array(nv, "owners", &owners, &nr) != 0)
		return (ENGINE_EINVAL(8));
	uint_t total = 0;
	for (uint_t i = 0; i < nr; i++) {
		uint64_t *p;
		uint_t np;
		if (nvlist_lookup_uint64_array(owners[i], "P", &p, &np) != 0 ||
		    np == 0 || np % 8 != 0 ||
		    np / 8 > engine_pages_row_limit(engine) - total)
			return (ENGINE_EINVAL(9));
		total += np / 8;
	}
	engine_mrow_t *rows = pr19262_zalloc(MAX(1, total) * sizeof (*rows),
	    KM_SLEEP);
	uint_t at = 0;
	int error = 0;
	for (uint_t i = 0; i < nr; i++) {
		uint64_t *v, *p;
		uint_t nv, np;
		if (nvlist_lookup_uint64_array(owners[i], "V", &v, &nv) != 0 ||
		    nvlist_lookup_uint64_array(owners[i], "P", &p, &np) != 0 ||
		    nv != 8) {
			error = ENGINE_EINVAL(19);
			break;
		}
		for (uint_t j = 0; j < np; j += 8)
			engine_mrow_fill(&rows[at++], v, p + j, j == 0);
	}
	if (error == 0)
		error = engine_mrows_order(rows, total);
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
	const char *fault = pr19262_env("PR19262_M_PAGE_FAULT");
	if (fault == NULL || pr19262_strtoull(fault, NULL, 0) != stage)
		return;
	pr19262_printf("PR19262_M_PAGE_CUT stage=%u generation=%llu "
	    "child=%u page=%llu\n", stage, (u_longlong_t)generation,
	    child, (u_longlong_t)page);
	pr19262_flush();
	pr19262_exit(120 + stage);
}

static boolean_t
engine_pages_active_error(engine_t *engine, engine_marena_t *a, uint_t child,
    uint_t slot,
    const char *phase, uint64_t serial)
{
	const char *fault = pr19262_env("PR19262_M_ACTIVE_ERROR");
	const char *member = pr19262_env("PR19262_M_ACTIVE_ERROR_CHILD");
	const char *selected = pr19262_env("PR19262_M_ACTIVE_ERROR_SLOT");
	if (engine->m_active_error_injected ||
	    !engine->pages->publishing_active ||
	    a->p != engine->arena || fault == NULL ||
	    strcmp(fault, phase) != 0 ||
	    child != (member == NULL ? 0 :
	    pr19262_strtoull(member, NULL, 10)) ||
	    (selected != NULL && slot != pr19262_strtoull(selected, NULL, 10)))
		return (B_FALSE);
	engine->m_active_error_injected = B_TRUE;
	pr19262_printf("PR19262_M_ACTIVE_ERROR phase=%s child=%u slot=%u "
	    "serial=%llu\n", phase, child, slot, (u_longlong_t)serial);
	return (B_TRUE);
}

static int
engine_pages_reserve_arena(spa_t *spa, engine_marena_t *a,
    uint64_t generation, uint64_t serial, const engine_mcontrol_t *control)
{
	engine_t *engine = spa->spa_reloc;
	a->serial = serial;
	engine->slot_cost.writes += engine->vd->vdev_children;
	engine_mnode_t *n = engine_mnode_alloc();
	engine_mheader_t *h = engine_mheader(n);
	uint_t slot = serial % 2;
	h->magic = ENGINE_M_ALLOC_MAGIC;
	h->type = ENGINE_M_BLOB;
	h->count = a->pages;
	h->p = a->p + slot * ENGINE_STORE_PAGE *
	    engine->vd->vdev_children;
	h->generation = generation;
	h->pool = spa_guid(spa); h->top = engine->vd->vdev_guid;
	h->low[0] = a->p; h->low[1] = a->pages * ENGINE_STORE_PAGE;
	h->low[2] = engine->vd->vdev_children;
	h->high[0] = a->pages;
	h->high[1] = serial;
	h->high[2] = control->floor;
	h->high[3] = control->certified;
	memcpy(h->reserved, &control->directory, sizeof (control->directory));
	memcpy(h->reserved + 6, control->pending, sizeof (control->pending));
	memcpy(h->reserved + 10, a->uuid, sizeof (a->uuid));
	mutex_enter(&engine->lock);
	memcpy(engine_mpayload(n), engine->pages->bits + a->first, a->pages);
	mutex_exit(&engine->lock);
	engine_mhash(n, &h->hash);
	abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
	engine_quorum_t q = { 0 };
	/*
	 * Without a cut or error hook, every member writes the slot and then
	 * flushes at once: a reader takes the highest serial of any member,
	 * so which members hold the new one after a crash does not matter.
	 * The hooks cut after the first member and keep the member order.
	 */
	boolean_t hooked = pr19262_env("PR19262_M_PAGE_FAULT") != NULL ||
	    pr19262_env("PR19262_M_ACTIVE_ERROR") != NULL;
	if (!hooked) {
		uint_t children = engine->vd->vdev_children;
		int *errors = kmem_alloc(children * sizeof (int), KM_SLEEP);
		engine_member_page_t pv = { .arena = a->p, .page = slot,
		    .abd = abd };
		engine_members_write(spa, &pv, 1, B_TRUE, B_FALSE, errors,
		    NULL);
		for (uint_t c = 0; c < children; c++) {
			engine_quorum_add(engine, &q, errors[c]);
			if (errors[c] == 0 && a->p == engine->arena)
				engine->pages->member_serial[c] = serial;
		}
		kmem_free(errors, children * sizeof (int));
	}
	for (uint_t c = 0; hooked && c < engine->vd->vdev_children; c++) {
		int e = EIO;
		if (!engine_pages_active_error(engine, a, c, slot, "write",
		    serial))
			e = engine_store_io_at(engine->vd->vdev_child[c], a->p,
			    slot, 1, abd, B_TRUE);
		if (e == 0) {
			if (engine_pages_active_error(engine, a, c, slot,
			    "flush", serial))
				e = EIO;
			else
				e = engine_store_flush(
				    engine->vd->vdev_child[c]);
		}
		engine_quorum_add(engine, &q, e);
		if (e == 0 && a->p == engine->arena)
			engine->pages->member_serial[c] = serial;
		if (e == 0 && c == 0)
			engine_pages_fault(1, generation, c, slot);
		if (e == 0 && c == 0 && a->p == engine->arena &&
		    control->pending[1] != 0)
			engine_pages_fault(10, generation, c, slot);
		if (e == 0 && c == 0 && a->p == engine->arena &&
		    engine->pages->publishing_active)
			engine_pages_fault(15, generation, c, slot);
	}
	abd_free(abd);
	if (q.failures == 0) {
		uint64_t errors = atomic_load_64(&engine->io_errors);
		if (a->cache_errors != errors ||
		    a->cache_config != spa->spa_config_generation)
			a->slot_serial[0] = a->slot_serial[1] = 0;
		memcpy(a->slots + slot * a->pages, engine_mpayload(n),
		    a->pages);
		a->slot_serial[slot] = serial;
		a->cache_errors = errors;
		a->cache_config = spa->spa_config_generation;
	} else {
		a->slot_serial[0] = a->slot_serial[1] = 0;
	}
	engine_mnode_free(n);
	return (engine_quorum_result(engine, &q));
}

/* Only our own writes, flushed on every member, establish this cache. */
static boolean_t
engine_pages_slots_cached(spa_t *spa, engine_marena_t *a)
{
	engine_t *engine = spa->spa_reloc;
	if (a->cache_errors != atomic_load_64(&engine->io_errors) ||
	    a->cache_config != spa->spa_config_generation) {
		a->slot_serial[0] = a->slot_serial[1] = 0;
	}
	return (a->slot_serial[0] != 0 && a->slot_serial[1] != 0 &&
	    pr19262_env("PR19262_M_A_FIRST_READ_EIO") == NULL);
}

static int
engine_pages_reserve_control(spa_t *spa, uint64_t generation,
    const engine_mcontrol_t *control)
{
	engine_t *engine = spa->spa_reloc;
	/* Keep topology stable through checks and publication. */
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	int error = engine_pages_control_repair(spa, control);
	if (error != 0)
		goto out;
	if (engine->pages->serial == UINT64_MAX) {
		error = EOVERFLOW;
		goto out;
	}
	uint64_t serial = ++engine->pages->serial;
	error = 0;
	for (engine_marena_t *a = engine->pages->arenas; a != NULL;
	    a = a->next) {
		if (a->pending && !a->formatted)
			continue;
		if (engine->pages->publishing_active && a->p == engine->arena)
			engine_pages_fault(16, generation, 0, serial % 2);
		/* The initial arena is last; always publish its control. */
		if (a->p != engine->arena && !a->pending &&
		    engine->pages->pending[1] == 0 &&
		    !engine->pages->publishing_active &&
		    engine_pages_slots_cached(spa, a) &&
		    memcmp(a->slots, engine->pages->bits + a->first,
		    a->pages) == 0 &&
		    memcmp(a->slots + a->pages, engine->pages->bits + a->first,
		    a->pages) == 0) {
			engine->slot_cost.skipped++;
			continue;
		}
		if (a->serial == UINT64_MAX) {
			error = EOVERFLOW;
			break;
		}
		/* A gap would drop the old mask before the floor is durable. */
		uint64_t local = a->p == engine->arena ? serial : a->serial + 1;
		int e = engine_pages_reserve_arena(spa, a, generation, local,
		    control);
		if (e != 0)
			error = e;
	}
	if (error == 0) {
		engine->pages->control = *control;
		engine->pages->control.serial = serial;
		engine->pages->control.codec = ENGINE_M_ALLOC_MAGIC;
		engine->pages->control_serial = serial;
		engine->pages->floor = control->floor;
		engine->pages->certified = control->certified;
	}
out:
	spa_config_exit(spa, SCL_ZIO, FTAG);
	return (error);
}

static engine_mcontrol_t
engine_pages_control_current(engine_t *engine, const engine_mref_t *directory)
{
	engine_mcontrol_t control = { .directory = *directory,
	    .floor = engine->pages->floor,
	    .certified = engine->pages->certified };
	memcpy(control.pending, engine->pages->pending,
	    sizeof (control.pending));
	return (control);
}

static int
engine_pages_reserve(spa_t *spa, uint64_t generation)
{
	engine_t *engine = spa->spa_reloc;
	engine_mcontrol_t control =
	    engine_pages_control_current(engine, &engine->pages->directory);
	return (engine_pages_reserve_control(spa, generation, &control));
}

/*
 * Publish a new durable floor and certificate in both control slots of
 * every member.  The caller holds store_lock and SCL_STATE.
 */
static int
engine_pages_publish_control(spa_t *spa, uint64_t floor, uint64_t certified)
{
	engine_t *engine = spa->spa_reloc;
	if (engine->pages->pending[1] != 0)
		return (EBUSY);
	engine_mcontrol_t control =
	    engine_pages_control_current(engine, &engine->pages->directory);
	control.floor = floor;
	control.certified = certified;
	int error = engine_pages_reserve_control(spa, engine->pages->generation,
	    &control);
	if (error == 0)
		error = engine_pages_reserve_control(spa,
		    engine->pages->generation, &control);
	if (error == 0)
		engine->pages->gc_relaxed = B_FALSE;
	return (error);
}

static boolean_t
engine_mcontrol_same(const engine_mcontrol_t *a, const engine_mcontrol_t *b)
{
	return (memcmp(a, b, sizeof (*a)) == 0);
}

/*
 * Both control slots of an arena on every member, read at once: slot s of
 * member c lands in n[s * children + c] and its result in errors[] (ENXIO:
 * the member is absent).  A reader that went member by member paid a
 * round trip per page.
 */
static void
engine_pages_slots_read(spa_t *spa, const engine_marena_t *a,
    engine_mnode_t **n, int *errors)
{
	engine_t *engine = spa->spa_reloc;
	uint_t children = engine->vd->vdev_children;
	engine->slot_cost.reads += 2 * children;
	zio_t **root = kmem_zalloc(2 * children * sizeof (zio_t *), KM_SLEEP);
	abd_t **abd = kmem_zalloc(2 * children * sizeof (abd_t *), KM_SLEEP);
	for (uint_t i = 0; i < 2 * children; i++) {
		uint_t slot = i / children, c = i % children;
		zio_t *zio;
		n[i] = engine_mnode_alloc();
		abd[i] = abd_get_from_buf(n[i]->data, ENGINE_STORE_PAGE);
		root[i] = zio_root(spa, NULL, NULL, ZIO_FLAG_CANFAIL);
		errors[i] = engine_store_zio(root[i], engine->vd->vdev_child[c],
		    a->p + slot * ENGINE_STORE_PAGE * children, 0, 1, abd[i],
		    B_FALSE, &zio);
		if (errors[i] == 0)
			zio_nowait(zio);
	}
	for (uint_t i = 0; i < 2 * children; i++) {
		int e = zio_wait(root[i]);
		engine_member_error(engine, &errors[i], e);
		abd_free(abd[i]);
	}
	kmem_free(abd, 2 * children * sizeof (abd_t *));
	kmem_free(root, 2 * children * sizeof (zio_t *));
}

static int
engine_pages_reservations_arena(spa_t *spa, engine_marena_t *a,
    uint8_t *bitmap, uint64_t *generation, uint64_t *serial_high,
    engine_mcontrol_t *control, boolean_t strict)
{
	engine_t *engine = spa->spa_reloc;
	boolean_t found = B_FALSE;
	boolean_t conflict = B_FALSE;
	int error = 0;
	uint_t children = engine->vd->vdev_children;
	/* Every slot of every member first, then each in the same order. */
	engine_mnode_t **slots = kmem_zalloc(2 * children *
	    sizeof (engine_mnode_t *), KM_SLEEP);
	int *read = kmem_zalloc(2 * children * sizeof (int), KM_SLEEP);
	engine_pages_slots_read(spa, a, slots, read);
	/* Masks by (slot, member): only the two latest serials are unioned. */
	uint64_t *serials = pr19262_zalloc(2 * children * sizeof (uint64_t),
	    KM_SLEEP);
	uint8_t *masks = pr19262_zalloc(2 * children * a->pages, KM_SLEEP);
	boolean_t *absent = kmem_zalloc(children * sizeof (boolean_t),
	    KM_SLEEP);
	for (uint_t slot = 0; slot < 2; slot++) {
		for (uint_t c = 0; c < engine->vd->vdev_children; c++) {
			uint64_t p = a->p + slot * ENGINE_STORE_PAGE *
			    engine->vd->vdev_children;
			engine_mnode_t *n = slots[slot * children + c];
			const char *hidden =
			    pr19262_env("PR19262_M_A_FIRST_READ_EIO");
			if (!engine->m_read_injected && a->p == engine->arena &&
			    hidden != NULL && c == 0 &&
			    slot == pr19262_strtoull(hidden, NULL, 10)) {
				engine->m_read_injected = B_TRUE;
				pr19262_printf("PR19262_M_A_READ_EIO child=0 "
				    "slot=%u\n", slot);
				if (strict)
					error = EIO;
				continue;
			}
			int e = read[slot * children + c];
			if (e != 0) {
				/* Absent members count against the quorum. */
				if (e == ENXIO)
					absent[c] = B_TRUE;
				else if (strict)
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
			    h->top != engine->vd->vdev_guid ||
			    h->generation == 0 || h->level != 0 ||
			    h->type != ENGINE_M_BLOB ||
			    h->count != a->pages ||
			    h->low[0] != a->p ||
			    h->low[1] != a->pages * ENGINE_STORE_PAGE ||
			    h->low[2] != engine->vd->vdev_children ||
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
			uint64_t serial = h->high[1] == 0 ?
			    h->generation : h->high[1];
			memcpy(masks + (slot * children + c) * a->pages, bits,
			    a->pages);
			serials[slot * children + c] = MAX(serial, 1);
			if (a->p == engine->arena)
				engine->pages->member_serial[c] =
				    MAX(engine->pages->member_serial[c],
				    serial);
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
	/*
	 * A member that missed publications holds older masks; only the two
	 * latest serials (the current publication and the one before it)
	 * describe pages that may still be in use.
	 */
	uint64_t top = 0;
	for (uint_t i = 0; i < 2 * children; i++)
		top = MAX(top, serials[i]);
	a->serial = MAX(a->serial, top);
	for (uint_t i = 0; i < 2 * children; i++) {
		if (serials[i] == 0 || serials[i] + 1 < top)
			continue;
		for (uint_t j = 0; j < a->pages; j++)
			bitmap[j] |= masks[i * a->pages + j];
	}
	uint_t missing = 0;
	for (uint_t c = 0; c < children; c++)
		missing += absent[c];
	uint_t certified = control != NULL ?
	    MIN(control->certified, VDEV_RAIDZ_MAXPARITY) : 0;
	/* Runtime tolerance is never reconstructed from an old control slot. */
	uint_t tolerated = MAX(certified, engine_store_tolerated(engine));
	/* A writable import needs every member beyond the certified loss. */
	if (strict && spa_writeable(spa) && missing > tolerated)
		error = EIO;
	kmem_free(serials, 2 * children * sizeof (uint64_t));
	kmem_free(masks, 2 * children * a->pages);
	kmem_free(absent, children * sizeof (boolean_t));
	for (uint_t i = 0; i < 2 * children; i++)
		engine_mnode_free(slots[i]);
	kmem_free(read, 2 * children * sizeof (int));
	kmem_free(slots, 2 * children * sizeof (engine_mnode_t *));
	if (error != 0)
		return (error);
	return (conflict ? ECKSUM : (found ? 0 : ENOENT));
}

static int
engine_pages_reservations(spa_t *spa, uint8_t *bitmap,
    uint64_t *generation, uint64_t *serial_high)
{
	engine_t *engine = spa->spa_reloc;
	for (engine_marena_t *a = engine->pages->arenas; a != NULL;
	    a = a->next) {
		if (a->pending) {
			memset(bitmap + a->first, 1, a->pages);
			continue;
		}
		engine_mcontrol_t control = { 0 };
		int error = engine_pages_reservations_arena(spa, a,
		    bitmap + a->first, generation, serial_high,
		    a->p == engine->arena ? &control : NULL, B_TRUE);
		if (error != 0)
			return (error);
		if (a->p == engine->arena &&
		    !engine_mcontrol_same(&control, &engine->pages->control))
			return (EAGAIN);
	}
	return (0);
}

static int
engine_pages_control_check(spa_t *spa, boolean_t strict)
{
	engine_t *engine = spa->spa_reloc;
	engine_marena_t *a = engine->pages->arenas;
	while (a != NULL && a->p != engine->arena)
		a = a->next;
	if (a == NULL)
		return (ENGINE_EINVAL(10));
	uint8_t *bits = pr19262_zalloc(a->pages, KM_SLEEP);
	uint64_t generation = 0, serial = 0;
	engine_mcontrol_t control = { 0 };
	int error = engine_pages_reservations_arena(spa, a, bits,
	    &generation, &serial, &control, strict);
	kmem_free(bits, a->pages);
	if (error == 0 &&
	    !engine_mcontrol_same(&control, &engine->pages->control))
		error = EAGAIN;
	if (error == 0)
		engine->pages->serial = MAX(engine->pages->serial, serial);
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
	engine_t *engine = spa->spa_reloc;
	engine_mcontrol_t control = { 0 };
	uint64_t generation = 0, serial = engine->pages->serial;
	for (engine_marena_t *a = engine->pages->arenas; a != NULL;
	    a = a->next) {
		if (a->pending && !a->formatted)
			continue;
		uint8_t *bits = pr19262_zalloc(a->pages, KM_SLEEP);
		engine_mcontrol_t *selected = a->p == engine->arena ?
		    &control : NULL;
		int error = 0;
		if (engine_pages_slots_cached(spa, a)) {
			for (uint_t s = 0; s < 2; s++) {
				if (a->serial - a->slot_serial[s] > 1)
					continue;
				for (uint64_t i = 0; i < a->pages; i++)
					bits[i] |= a->slots[s * a->pages + i];
			}
			serial = MAX(serial, a->serial);
			if (selected != NULL)
				*selected = engine->pages->control;
			engine->slot_cost.cached++;
#if defined(ZFS_DEBUG) && !defined(_KERNEL)
			/* Check each cache hit against disk in debug ztest. */
			uint8_t *disk = kmem_zalloc(a->pages, KM_SLEEP);
			uint64_t before = a->serial;
			engine_mcontrol_t actual = { 0 };
			error = engine_pages_reservations_arena(spa, a, disk,
			    &generation, &serial, selected != NULL ?
			    &actual : NULL, B_TRUE);
			if (error == 0) {
				VERIFY3U(a->serial, ==, before);
				VERIFY0(memcmp(bits, disk, a->pages));
				if (selected != NULL)
					VERIFY(engine_mcontrol_same(selected,
					    &actual));
				engine->slot_cost.checked++;
			}
			kmem_free(disk, a->pages);
#endif
		} else {
			error = engine_pages_reservations_arena(spa, a, bits,
			    &generation, &serial, selected, B_TRUE);
		}
		mutex_enter(&engine->lock);
		for (uint64_t i = 0; error == 0 && i < a->pages; i++)
			if (bits[i] && !engine->pages->bits[a->first + i] &&
			    !engine->pages->gc_relaxed)
				error = ECKSUM;
		mutex_exit(&engine->lock);
		kmem_free(bits, a->pages);
		/* A fresh arena has no reservation until its first write. */
		if (error == ENOENT && (a->pending ||
		    engine->pages->control.serial == 0))
			error = 0;
		if (error != 0)
			return (error);
	}
	if (control.serial != 0 &&
	    !engine_mcontrol_payload_same(&control, &engine->pages->control) &&
	    !engine_mcontrol_payload_same(&control, proposed))
		return (ECKSUM);
	if (control.serial < engine->pages->control.serial)
		return (EIO);
	engine->pages->serial = serial;
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
	/* Rewrite every reached page on this member (-1: none). */
	int target;
	uint64_t written;
	int write_error;
	/* Per-member audit of every reached page, when non-NULL. */
	uint64_t *audit_ok;
	uint64_t *audit_missing;
} engine_mmarker_t;

/* Rewrite a reached page on the target member and audit every member. */
static void
engine_pages_mark_io(engine_mmarker_t *m, engine_mnode_t *n, uint64_t p,
    const zio_cksum_t *hash)
{
	engine_t *engine = m->spa->spa_reloc;
	uint_t children = engine->vd->vdev_children;
	if (m->target >= 0) {
		abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
		int e = engine_store_io_at(engine->vd->vdev_child[m->target], p,
		    0, 1, abd, B_TRUE);
		abd_free(abd);
		if (e != 0)
			m->write_error = e;
		else
			m->written++;
	}
	if (m->audit_ok == NULL)
		return;
	engine_mnode_t *probe = engine_mnode_alloc();
	abd_t *pabd = abd_get_from_buf(probe->data, ENGINE_STORE_PAGE);
	for (uint_t c = 0; c < children; c++) {
		boolean_t ok = B_FALSE;
		if (engine_store_io_at(engine->vd->vdev_child[c], p, 0, 1, pabd,
		    B_FALSE) == 0) {
			engine_mheader_t *h = engine_mheader(probe);
			if (h->magic == ENGINE_M_MAGIC ||
			    h->magic == BSWAP_64(ENGINE_M_MAGIC)) {
				zio_cksum_t computed;
				engine_mhash(probe, &computed);
				ok = memcmp(&computed, hash,
				    sizeof (computed)) == 0;
			}
		}
		if (ok)
			m->audit_ok[c]++;
		else
			m->audit_missing[c]++;
	}
	abd_free(pabd);
	engine_mnode_free(probe);
}

/*
 * A leaf reached by a plain sweep (no rewrite, no audit): its reference
 * names its page and it has no children, so it is marked unread; an
 * evicted leaf would otherwise be read back by every fence's GC.  Debug
 * ztest reads it and checks that it is a leaf.
 */
static int
engine_pages_mark_leaf(engine_mmarker_t *m, const engine_mref_t *ref)
{
	engine_t *engine = m->spa->spa_reloc;
	uint64_t index;
	if (ref->p == UINT64_MAX || engine_mref_empty(ref))
		return (0);
	if (!engine_mindex(engine, ref->p, &index))
		return (ENGINE_EINVAL(11));
#if defined(ZFS_DEBUG) && !defined(_KERNEL)
	engine_mnode_t *n = engine->pages->cache[index];
	boolean_t owned = B_FALSE;
	if (n == NULL || n->dirty || !n->sealed ||
	    memcmp(&engine_mheader(n)->hash, &ref->hash,
	    sizeof (ref->hash)) != 0) {
		int error = engine_mread(m->spa, ref, &n);
		if (error != 0)
			return (error);
		owned = B_TRUE;
	}
	VERIFY3U(engine_mheader(n)->type, ==, ENGINE_M_LEAF);
	VERIFY0(engine_mheader(n)->level);
	if (owned)
		engine_mnode_free(n);
#endif
	m->marked[index] = 1;
	return (0);
}

/* Mark every page reachable from a blob chain or a page-tree reference. */
static int
engine_pages_mark_ref(engine_mmarker_t *m, const engine_mref_t *ref)
{
	engine_t *engine = m->spa->spa_reloc;
	uint64_t index;
	if (ref->p == UINT64_MAX || engine_mref_empty(ref))
		return (0);
	if (!engine_mindex(engine, ref->p, &index))
		return (ENGINE_EINVAL(11));
	if (m->marked[index])
		return (0);
	m->marked[index] = 1;
	engine_mnode_t *n = engine->pages->cache[index];
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
	engine_pages_mark_io(m, n, ref->p, &ref->hash);
	if (h->type == ENGINE_M_BRANCH) {
		/* A level-1 branch's children are leaves. */
		boolean_t leaves = h->level == 1 && m->target < 0 &&
		    m->audit_ok == NULL;
		engine_mchild_t *children = engine_mpayload(n);
		for (uint_t i = 0; error == 0 && i < h->count; i++)
			error = leaves ?
			    engine_pages_mark_leaf(m, &children[i].ref) :
			    engine_pages_mark_ref(m, &children[i].ref);
	} else if (h->type == ENGINE_M_BLOB) {
		engine_mref_t next;
		memcpy(&next, h->reserved, sizeof (next));
		error = engine_pages_mark_ref(m, &next);
	}
	if (owned)
		engine_mnode_free(n);
	return (error);
}

static void
engine_pages_mark_range(void *arg, uint64_t p, uint64_t size)
{
	engine_t *engine = ((engine_mmarker_t *)arg)->spa->spa_reloc;
	engine_mmarker_t *m = arg;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	for (uint64_t at = p; at < p + size; at += stripe) {
		uint64_t index;
		if (engine_mindex(engine, at, &index))
			m->marked[index] = 1;
	}
}

/*
 * Checkpoint-exempt floor.  The importable roots are those at or above
 * the floor plus the checkpoint root C.  C is guarded while the pool has
 * it and, after a discard, until the floor reaches the first root that no
 * longer names it (engine.ckpt_until).  Returns the guarded C, valid while
 * the caller holds engine.lock, or NULL.
 */
static const uberblock_t *
engine_ckpt_guard_locked(engine_t *engine, uint64_t floor)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->checkpoint.ub_txg != 0)
		return (&engine->checkpoint);
	if (engine->ckpt_until != 0 && floor < engine->ckpt_until)
		return (&engine->ckpt_ub);
	return (NULL);
}

/*
 * The guarded paged C is exempt from the floor in the root graphs.  A
 * catalog names C as a pin uberblock (ub_checkpoint_txg == ub_txg).  The
 * graphs dedup roots by trailer and MOS root, so a label ring root with
 * C's trailer and MOS root (C itself as written, or an earlier txg whose
 * root C repeats unchanged) stands for C: it reaches exactly C's pages.
 */
static boolean_t
engine_ckpt_same_root(const uberblock_t *ub, const uberblock_t *c)
{
	return (c->ub_txg != 0 &&
	    memcmp(ub->ub_pr19262, c->ub_pr19262,
	    sizeof (ub->ub_pr19262)) == 0 &&
	    memcmp(&ub->ub_rootbp, &c->ub_rootbp,
	    sizeof (ub->ub_rootbp)) == 0);
}

static boolean_t
engine_ckpt_exempt(engine_t *engine, const uberblock_t *ub)
{
	if (!pr19262_engine_store_ub(ub))
		return (B_FALSE);
	mutex_enter(&engine->lock);
	boolean_t exempt = engine_ckpt_same_root(ub, &engine->checkpoint) ||
	    (engine->ckpt_until != 0 &&
	    engine_ckpt_same_root(ub, &engine->ckpt_ub));
	mutex_exit(&engine->lock);
	return (exempt);
}

/*
 * L19: release the catalog reserve under a checkpoint (convert.c): the
 * pages C pinned come back with its pins, or the engine unloads.
 */
static void
engine_ckres_release_locked(engine_t *engine, const char *why)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->ckres_pages == 0)
		return;
	pr19262_printf("PR19262_CKRES op=release why=%s pages=%llu disc=%llu "
	    "disc_left=%llu available=%llu capacity=%llu min_available=%llu "
	    "churn_x16=%lld rel_pages=%llu\n", why,
	    (u_longlong_t)engine->ckres_pages,
	    (u_longlong_t)engine->ckres_disc,
	    (u_longlong_t)engine->ckres_disc_left,
	    (u_longlong_t)engine->pages->available,
	    (u_longlong_t)engine->pages->capacity,
	    (u_longlong_t)engine->ckres_min_available,
	    (longlong_t)engine->ckres_churn_x16,
	    (u_longlong_t)engine->ckres_rel_pages);
	engine->ckres_pages = engine->ckres_disc = engine->ckres_disc_left = 0;
	engine->ckres_used_valid = B_FALSE;
	engine->ckres_warned = B_FALSE;
	engine->ckres_wait_gen = engine->ckres_wait_deficit = 0;
	engine->ckres_rel_txg = engine->ckres_rel_left = 0;
	engine->ckres_releases++;
}

/* Drop the pins and the post-discard guard.  Caller holds engine.lock. */
static void
engine_ckpt_unpin_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	/* L19: no C, no guard: the reserve goes with the pins. */
	if (engine->checkpoint.ub_txg == 0)
		engine_ckres_release_locked(engine, "unpin");
	if (engine->ckpt_pins != NULL) {
		zfs_range_tree_vacate(engine->ckpt_pins, NULL, NULL);
		zfs_range_tree_destroy(engine->ckpt_pins);
		engine->ckpt_pins = NULL;
		engine->ckpt_unpins++;
	}
	memset(&engine->ckpt_pins_ub, 0, sizeof (engine->ckpt_pins_ub));
	memset(&engine->ckpt_ub, 0, sizeof (engine->ckpt_ub));
	engine->ckpt_until = 0;
	engine->ckpt_pinned_pages = 0;
}

/*
 * Collect the catalog pages the paged checkpoint root reaches: its blob
 * chain and page tree.  A read without the allocator walks and checks
 * both (it adds the control slots and directory nodes, which GC keeps
 * anyway).  The set is RAM only and built here, by the first fence that
 * needs it after the checkpoint, an import or a rebuild failure: page GC
 * is the only path that frees a durable page.  A codec 5 root keeps its
 * catalog in the bootstrap bitmap, which is not pinned: refuse.  A failed
 * walk refuses the fence (EBUSY) as before rather than collect without
 * pins.  The caller holds store_lock.
 */
static int
engine_ckpt_pins_build(spa_t *spa, const uberblock_t *cp)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->store_lock));
	if (engine->ckpt_pins != NULL &&
	    memcmp(&engine->ckpt_pins_ub, cp, sizeof (*cp)) == 0)
		return (0);
	zfs_range_tree_t *pins = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	nvlist_t *nv = NULL;
	int error = cp->ub_pr19262[15] >= 6 ?
	    engine_pages_read(spa, cp, &nv, pins, B_FALSE) : ENOTSUP;
	nvlist_free(nv);
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	uint64_t pinned = zfs_range_tree_space(pins) / stripe;
	zfs_range_tree_t *old = pins;
	mutex_enter(&engine->lock);
	if (error == 0) {
		old = engine->ckpt_pins;
		engine->ckpt_pins = pins;
		engine->ckpt_pins_ub = *cp;
		engine->ckpt_pinned_pages = pinned;
		engine->ckpt_builds++;
	} else {
		engine->ckpt_build_errors++;
	}
	mutex_exit(&engine->lock);
	if (old != NULL) {
		zfs_range_tree_vacate(old, NULL, NULL);
		zfs_range_tree_destroy(old);
	}
	pr19262_printf("PR19262_CKPT_PINS ub=%llu catalog=%llu pages=%llu "
	    "error=%d\n", (u_longlong_t)cp->ub_txg,
	    (u_longlong_t)cp->ub_pr19262[6],
	    (u_longlong_t)(error == 0 ? pinned : 0), error);
	return (error == 0 ? 0 : SET_ERROR(EBUSY));
}

/*
 * Free every page that neither the last durable catalog nor the current
 * RAM tree, the control slots, a directory node or the checkpoint pins
 * (when non-NULL) reach.  Only valid once every other root below the
 * latest one is refused by the floor, so the caller publishes the
 * shrunken masks together with that floor in both control slots.  The
 * caller holds store_lock.
 */
static int
engine_pages_gc(spa_t *spa, uint64_t *freed, zfs_range_tree_t *pins)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->store_lock));
	*freed = 0;
	if (engine->pages->capacity == 0 || engine->pages->pending[1] != 0)
		return (0);
	mutex_enter(&engine->lock);
	boolean_t durable = engine->stored_desc[0] == ENGINE_STORE_MAGIC &&
	    memcmp(engine->stored_desc, engine->durable_desc,
	    sizeof (engine->stored_desc)) == 0;
	mutex_exit(&engine->lock);
	if (!durable)
		return (0);
	engine_mmarker_t m = { .spa = spa, .target = -1,
	    .marked = pr19262_zalloc(engine->pages->capacity, KM_SLEEP) };
	for (engine_marena_t *a = engine->pages->arenas; a != NULL; a = a->next)
		m.marked[a->first] = m.marked[a->first + 1] = 1;
	if (engine->pages->directory_pages != NULL)
		zfs_range_tree_walk(engine->pages->directory_pages,
		    engine_pages_mark_range, &m);
	if (pins != NULL)
		zfs_range_tree_walk(pins, engine_pages_mark_range, &m);
	engine_mref_t blob = { .p = engine->stored_desc[4] };
	memcpy(&blob.hash, engine->stored_desc + 8, sizeof (blob.hash));
	int error = engine_pages_mark_ref(&m, &blob);
	if (error == 0)
		error = engine_pages_mark_ref(&m, &engine->pages->root);
	if (error == 0) {
		mutex_enter(&engine->lock);
		for (uint64_t i = 0; i < engine->pages->capacity; i++) {
			if (!engine->pages->bits[i] || m.marked[i])
				continue;
			engine->pages->bits[i] = 0;
			engine->pages->available++;
			engine->pages->free_hint = MIN(engine->pages->free_hint,
			    i);
			if (i < ENGINE_STORE_PAGES)
				engine->bitmap[i] = 0;
			if (engine->pages->cache[i] != NULL) {
				/*
				 * Failed stores leave dirty orphans.
				 * No writer can reattach them here.
				 */
				engine_mnode_free(engine->pages->cache[i]);
				engine->pages->cache[i] = NULL;
			}
			(*freed)++;
		}
		if (*freed != 0) {
			engine->pages->gc_relaxed = B_TRUE;
			engine->revision++;
		}
		mutex_exit(&engine->lock);
	}
	kmem_free(m.marked, engine->pages->capacity);
	return (error);
}

/*
 * Visit every live page (the durable root blob chain, the page tree, the
 * directory nodes) and the control slots: rewrite them on one member and
 * audit which members hold a valid copy.  The caller holds store_lock and
 * the stored catalog is the durable one.
 */
static int
engine_pages_members(spa_t *spa, int target, uint64_t *ok,
    uint64_t *missing, uint64_t *written)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->store_lock));
	*written = 0;
	mutex_enter(&engine->lock);
	boolean_t durable = engine->stored_desc[0] == ENGINE_STORE_MAGIC &&
	    memcmp(engine->stored_desc, engine->durable_desc,
	    sizeof (engine->stored_desc)) == 0;
	mutex_exit(&engine->lock);
	if (!durable || engine->pages->capacity == 0)
		return (EBUSY);
	uint_t children = engine->vd->vdev_children;
	uint64_t stripe = ENGINE_STORE_PAGE * children;
	engine_mmarker_t m = { .spa = spa, .target = target,
	    .audit_ok = ok, .audit_missing = missing,
	    .marked = pr19262_zalloc(engine->pages->capacity, KM_SLEEP) };
	engine_mref_t blob = { .p = engine->stored_desc[4] };
	memcpy(&blob.hash, engine->stored_desc + 8, sizeof (blob.hash));
	int error = engine_pages_mark_ref(&m, &blob);
	if (error == 0)
		error = engine_pages_mark_ref(&m, &engine->pages->root);
	for (engine_marena_t *a = engine->pages->arenas; error == 0 && a !=
	    NULL;
	    a = a->next) {
		if (a->pending)
			continue;
		engine_mnode_t *dir = engine->pages->cache[a->first + 2];
		if (dir != NULL && dir->sealed && !dir->dirty &&
		    engine->pages->directory_pages != NULL &&
		    zfs_range_tree_contains(engine->pages->directory_pages,
		    a->p + 2 * stripe, stripe))
			engine_pages_mark_io(&m, dir, a->p + 2 * stripe,
			    &engine_mheader(dir)->hash);
		if (ok == NULL)
			continue;
		/* Each arena's current serial or the one before it. */
		engine_mnode_t *probe = engine_mnode_alloc();
		abd_t *pabd = abd_get_from_buf(probe->data, ENGINE_STORE_PAGE);
		for (uint_t slot = 0; slot < 2; slot++) {
			for (uint_t c = 0; c < children; c++) {
				boolean_t valid = B_FALSE;
				if (engine_store_io_at(
				    engine->vd->vdev_child[c],
				    a->p + slot * stripe, 0, 1, pabd,
				    B_FALSE) == 0) {
					engine_mheader_t *h =
					    engine_mheader(probe);
					zio_cksum_t computed;
					engine_mhash(probe, &computed);
					valid = h->magic ==
					    ENGINE_M_ALLOC_MAGIC &&
					    memcmp(&computed, &h->hash,
					    sizeof (computed)) == 0 &&
					    h->high[1] + 1 >=
					    a->serial;
				}
				if (valid)
					ok[c]++;
				else
					missing[c]++;
			}
		}
		abd_free(pabd);
		engine_mnode_free(probe);
	}
	kmem_free(m.marked, engine->pages->capacity);
	*written = m.written;
	return (error != 0 ? error : m.write_error);
}

static void
engine_pages_audit_print(spa_t *spa, const char *label, const uint64_t *ok,
    const uint64_t *missing)
{
	engine_t *engine = spa->spa_reloc;
	(void) spa;
	for (uint_t c = 0; c < engine->vd->vdev_children; c++)
		pr19262_printf("PR19262_RAW_AUDIT label=%s child=%u present=%u "
		    "ok=%llu missing=%llu serial=%llu current=%llu\n", label,
		    c, vdev_readable(engine->vd->vdev_child[c]),
		    (u_longlong_t)ok[c], (u_longlong_t)missing[c],
		    (u_longlong_t)engine->pages->member_serial[c],
		    (u_longlong_t)engine->pages->control.serial);
	pr19262_flush();
}

/* Rewrite every live page on one member, then refresh its control slots. */
static int
engine_pages_resilver(spa_t *spa, uint_t child, uint64_t *pages)
{
	engine_t *engine = spa->spa_reloc;
	/* An audit can find silent damage without a reported I/O error. */
	for (engine_marena_t *a = engine->pages->arenas; a != NULL; a = a->next)
		a->slot_serial[0] = a->slot_serial[1] = 0;
	int error = engine_pages_members(spa, child, NULL, NULL, pages);
	if (error == 0) {
		spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
		error = engine_store_flush(engine->vd->vdev_child[child]);
		spa_config_exit(spa, SCL_ZIO, FTAG);
	}
	if (error == 0)
		error = engine_pages_publish_control(spa, engine->pages->floor,
		    engine->pages->certified);
	if (error == 0)
		engine->pages->member_serial[child] =
		    engine->pages->control.serial;
	return (error);
}

/*
 * At a writable import, every present member that missed publications or
 * holds no valid copy of a live page is brought up to date: the data
 * below it is repaired by the native resilver, the raw metadata here.
 */
static int
engine_pages_resilver_members(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->store_lock));
	uint_t children = engine->vd->vdev_children;
	uint64_t *ok = kmem_zalloc(children * sizeof (uint64_t), KM_SLEEP);
	uint64_t *missing = kmem_zalloc(children * sizeof (uint64_t), KM_SLEEP);
	uint64_t written;
	int error = engine_pages_members(spa, -1, ok, missing, &written);
	if (error == 0)
		engine_pages_audit_print(spa, "recover", ok, missing);
	/* Decide on the audit; a resilver republishes the serial for all. */
	boolean_t *stale = kmem_zalloc(children * sizeof (boolean_t), KM_SLEEP);
	for (uint_t c = 0; error == 0 && c < children; c++) {
		vdev_t *vd = engine->vd->vdev_child[c];
		stale[c] = vdev_readable(vd) && vdev_writeable(vd) &&
		    (missing[c] != 0 || engine->pages->member_serial[c] <
		    engine->pages->control.serial);
	}
	uint_t resilvered = 0;
	for (uint_t c = 0; error == 0 && c < children; c++) {
		if (!stale[c])
			continue;
		/* L23: this repairs the stored root only; the pass the rest. */
		engine_member_missed(engine, engine->vd->vdev_child[c], ESTALE);
		uint64_t pages = 0;
		error = engine_pages_resilver(spa, c, &pages);
		pr19262_printf("PR19262_RAW_RESILVER child=%u pages=%llu "
		    "missing_before=%llu error=%d\n", c, (u_longlong_t)pages,
		    (u_longlong_t)missing[c], error);
		resilvered++;
	}
	if (error == 0 && resilvered != 0) {
		memset(ok, 0, children * sizeof (uint64_t));
		memset(missing, 0, children * sizeof (uint64_t));
		error = engine_pages_members(spa, -1, ok, missing, &written);
		if (error == 0)
			engine_pages_audit_print(spa, "resilvered", ok,
			    missing);
	}
	kmem_free(stale, children * sizeof (boolean_t));
	kmem_free(ok, children * sizeof (uint64_t));
	kmem_free(missing, children * sizeof (uint64_t));
	return (error);
}

static int
engine_pages_manifest(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	engine_pages_init(engine);
	if (engine->pages->directory_pages != NULL)
		return (0);
	engine_marena_t *initial = engine->pages->arenas;
	uint8_t *bits = pr19262_zalloc(initial->pages, KM_SLEEP);
	uint64_t generation = 0;
	uint64_t serial = 0;
	engine_mcontrol_t control = { 0 };
	int error = engine_pages_reservations_arena(spa, initial, bits,
	    &generation, &serial, &control, B_FALSE);
	kmem_free(bits, initial->pages);
	if (error != 0)
		return (error);
	engine->pages->control = control;
	engine->pages->control_serial = control.serial;
	engine->pages->serial = serial;
	engine->pages->directory = control.directory;
	engine->pages->floor = control.floor;
	engine->pages->certified = control.certified;
	memcpy(engine->pages->pending, control.pending,
	    sizeof (engine->pages->pending));
	engine_mref_t ref = engine->pages->directory;
	engine->pages->directory_pages = zfs_range_tree_create(NULL,
	    ZFS_RANGE_SEG64, NULL, 0, 0);
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	while (!engine_mref_empty(&ref)) {
		if (ref.generation == 0 || ref.generation > generation ||
		    zfs_range_tree_contains(engine->pages->directory_pages,
		    ref.p, stripe))
			return (ENGINE_EINVAL(12));
		engine_mnode_t *n;
		error = engine_mread_ref(spa, &ref, &n, B_TRUE);
		if (error != 0)
			return (error);
		engine_mheader_t *h = engine_mheader(n);
		uint64_t *data = engine_mpayload(n);
		if (h->type != ENGINE_M_DIRECTORY ||
		    data[0] >= ref.p || ref.p - data[0] != 2 * stripe ||
		    (data[2] == 0 && data[3] == 0)) {
			error = ENGINE_EINVAL(21);
		} else {
			error = engine_pages_add_arena(engine, data[0], data[1],
			    NULL, B_FALSE);
			if (error == 0) {
				engine_marena_t *a = engine->pages->arenas;
				memcpy(a->uuid, data + 2, sizeof (a->uuid));
				a->formatted = B_TRUE;
				a->directory_flushed = B_TRUE;
				a->format_generation = h->generation;
				a->candidate = ref;
			}
		}
		if (error == 0) {
			zfs_range_tree_add(engine->pages->directory_pages,
			    ref.p, stripe);
			uint64_t previous = ref.generation;
			memcpy(&ref, data + 4, sizeof (ref));
			if (!engine_mref_empty(&ref) &&
			    ref.generation > previous)
				error = ENGINE_EINVAL(22);
		}
		engine_mnode_free(n);
		if (error != 0)
			return (error);
	}
	if (engine->pages->pending[1] != 0) {
		if (engine->pages->pending[2] == 0 &&
		    engine->pages->pending[3] == 0)
			return (ENGINE_EINVAL(13));
		error = engine_pages_add_arena(engine,
		    engine->pages->pending[0],
		    engine->pages->pending[1], NULL, B_TRUE);
		if (error != 0)
			return (error);
		engine_marena_t *a = engine->pages->arenas;
		memcpy(a->uuid, engine->pages->pending + 2, sizeof (a->uuid));
	} else {
		uint64_t empty[4] = { 0 };
		if (memcmp(engine->pages->pending, empty, sizeof (empty)) != 0)
			return (ENGINE_EINVAL(14));
	}
	return (0);
}

static void
engine_pages_pick(void *arg, uint64_t p, uint64_t size)
{
	engine_t *engine = ((engine_walk_arg_t *)arg)->engine;
	arg = ((engine_walk_arg_t *)arg)->arg;
	uint64_t *range = arg;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	uint64_t start = roundup(p, stripe);
	if (start >= p && start - p <= size &&
	    range[1] <= size - (start - p))
		range[0] = start;
}

static int
engine_pages_directory_write(spa_t *spa, engine_marena_t *a,
    uint64_t generation, engine_mref_t *ref)
{
	engine_t *engine = spa->spa_reloc;
	uint64_t index = a->first + 2;
	engine_mnode_t *n = engine->pages->cache[index];
	engine_mref_t candidate = a->candidate;
	if (n == NULL) {
		n = engine_mnode_alloc();
		engine_mheader_t *h = engine_mheader(n);
		h->magic = ENGINE_M_MAGIC; h->type = ENGINE_M_DIRECTORY;
		h->count = 10 * sizeof (uint64_t);
		h->p = a->p + 2 * ENGINE_STORE_PAGE * engine->vd->vdev_children;
		h->generation = generation;
		h->pool = spa_guid(spa); h->top = engine->vd->vdev_guid;
		memcpy(engine_mpayload(n), engine->pages->pending,
		    sizeof (engine->pages->pending));
		memcpy((uint64_t *)engine_mpayload(n) + 4,
		    &engine->pages->directory,
		    sizeof (engine->pages->directory));
		candidate.p = h->p;
		candidate.generation = generation;
		boolean_t swap =
		    pr19262_env("PR19262_M_DIRECTORY_BYTESWAP") != NULL;
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
			engine_mnode_free(n);
			return (ECKSUM);
		}
		n->sealed = B_TRUE;
		a->candidate = candidate;
		engine->pages->cache[index] = n;
	}
	engine->pages->bits[index] = 1;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	if (engine->pages->directory_pages == NULL)
		engine->pages->directory_pages = zfs_range_tree_create(NULL,
		    ZFS_RANGE_SEG64, NULL, 0, 0);
	/* A selectable partial ACTIVE must never have an unpinned DIR. */
	engine_mmark(engine->pages->directory_pages, candidate.p, stripe);
	/* PREPARE is already durable; now reserve before the first format. */
	int error = engine_pages_reserve(spa, generation);
	if (error == 0)
		error = engine_pages_reserve(spa, generation);
	if (error != 0)
		return (error);
	engine_pages_fault(12, generation, 0, candidate.p);
	abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
	engine_quorum_t q = { 0 };
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	for (uint_t c = 0; c < engine->vd->vdev_children; c++) {
		int e = engine_store_io_at(engine->vd->vdev_child[c],
		    candidate.p, 0, 1, abd, B_TRUE);
		if (e == 0)
			e = engine_store_flush(engine->vd->vdev_child[c]);
		engine_quorum_add(engine, &q, e);
		if (e == 0 && c == 0)
			engine_pages_fault(13, generation, c, candidate.p);
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	abd_free(abd);
	error = engine_quorum_result(engine, &q);
	if (error == 0)
		*ref = candidate;
	return (error);
}

static int
engine_pages_finish(spa_t *spa, engine_marena_t *a, uint64_t generation)
{
	engine_t *engine = spa->spa_reloc;
	if (memcmp(a->uuid, engine->pages->pending + 2, sizeof (a->uuid)) != 0)
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
			memset(engine->pages->bits + a->first, 0, a->pages);
			engine->pages->bits[a->first] = 1;
			engine->pages->bits[a->first + 1] = 1;
			engine->pages->free_hint = 0;
			a->formatted = B_TRUE;
		}
		error = engine_pages_directory_write(spa, a, generation,
		    &a->candidate);
		if (error != 0)
			return (error);
		a->directory_flushed = B_TRUE;
	}
	engine_mcontrol_t active = { .directory = a->candidate,
	    .floor = engine->pages->floor,
	    .certified = engine->pages->certified };
	engine->pages->publishing_active = B_TRUE;
	error = engine_pages_reserve_control(spa, generation, &active);
	engine->pages->publishing_active = B_FALSE;
	if (error != 0)
		return (error);
	/* Every ACTIVE carrier is durable before the RAM handoff. */
	engine->pages->directory = a->candidate;
	memset(engine->pages->pending, 0, sizeof (engine->pages->pending));
	mutex_enter(&engine->lock);
	a->pending = B_FALSE;
	for (uint64_t i = 0; i < a->pages; i++)
		engine->pages->available += engine->pages->bits[a->first + i] ==
		    0;
	mutex_exit(&engine->lock);
	engine_pages_fault(14, generation, 0, a->p);
	pr19262_printf("PR19262_M_GROW p=%llu pages=%llu capacity=%llu "
	    "generation=%llu serial=%llu\n", (u_longlong_t)a->p,
	    (u_longlong_t)a->pages, (u_longlong_t)engine->pages->capacity,
	    (u_longlong_t)generation, (u_longlong_t)engine->pages->serial);
	return (0);
}

/* Count disjoint stripe-aligned arena runs, as engine_pages_pick fits. */
static void
engine_pages_runs_count(void *arg, uint64_t p, uint64_t size)
{
	engine_t *engine = ((engine_walk_arg_t *)arg)->engine;
	uint64_t *runs = ((engine_walk_arg_t *)arg)->arg;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	uint64_t bytes = ENGINE_STORE_BYTES * engine->vd->vdev_children;
	uint64_t start = roundup(p, stripe);
	if (start < p || start - p >= size)
		return;
	*runs += (size - (start - p)) / bytes;
}

/*
 * Arena runs in the free tree, counting stops at limit: the walk ends at
 * the first runs when there are some, and is whole only when there are
 * fewer than limit.
 */
static uint64_t
engine_pages_runs_bounded_locked(engine_t *engine, uint64_t limit)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t runs = 0;
	engine_walk_arg_t walk = { engine, &runs };
	if (engine->free == NULL || engine->vd == NULL)
		return (0);
	zfs_range_tree_t *rt = engine->free;
	zfs_btree_index_t where;
	for (zfs_range_seg_t *rs = zfs_btree_first(&rt->rt_root, &where);
	    rs != NULL && runs < limit;
	    rs = zfs_btree_next(&rt->rt_root, &where, &where)) {
		uint64_t start = zfs_rs_get_start(rs, rt);
		engine_pages_runs_count(&walk, start,
		    zfs_rs_get_end(rs, rt) - start);
	}
	return (runs);
}

static uint64_t
engine_pages_runs_locked(engine_t *engine)
{
	return (engine_pages_runs_bounded_locked(engine, 2));
}

/*
 * Hold one arena-sized run back from the physical free tree for the
 * syncing store.  Called after a successful store, in syncing context: a
 * RAM-only range move under engine->lock, no I/O.  The run is in
 * store_extents, so no allocator or admission sees it; an import rebuilds
 * the free tree from the catalog and the run simply returns to it.
 */
static void
engine_pages_standby_fill_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->store_standby_valid || !engine->persistent ||
	    engine->free == NULL || engine->store_extents == NULL ||
	    engine->pages->capacity == 0)
		return;
	uint64_t bytes = ENGINE_STORE_BYTES * engine->vd->vdev_children;
	uint64_t range[2] = { UINT64_MAX, bytes };
	engine_walk_arg_t walk = { engine, range };
	zfs_range_tree_walk(engine->free, engine_pages_pick, &walk);
	if (range[0] == UINT64_MAX) {
		engine->store_headroom_low = B_TRUE;
		return;
	}
	zfs_range_tree_remove(engine->free, range[0], bytes);
	zfs_range_tree_add(engine->store_extents, range[0], bytes);
	engine->store_standby = range[0];
	engine->store_standby_valid = B_TRUE;
	engine->store_standby_fills++;
	engine->store_headroom_low = B_FALSE;
	engine->store_headroom_warned = B_FALSE;
}

/*
 * Return the standby run to the free tree (a journal roll needs a run and
 * finds none).  The next store refills it if a run is left; otherwise
 * headroom is low.  Caller holds engine->lock.
 */
static boolean_t
engine_pages_standby_release_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (!engine->store_standby_valid)
		return (B_FALSE);
	uint64_t bytes = ENGINE_STORE_BYTES * engine->vd->vdev_children;
	zfs_range_tree_remove(engine->store_extents, engine->store_standby,
	    bytes);
	zfs_range_tree_add(engine->free, engine->store_standby, bytes);
	engine->store_standby_valid = B_FALSE;
	engine->store_standby_releases++;
	return (B_TRUE);
}

/*
 * Reserved arena runs (store_reserved): the runs admission counted for the
 * catalog growth of the work it admitted.  Each is a whole stripe-aligned
 * run moved from free into store_extents, as the standby, so neither data
 * allocation, a copy stage, a journal roll nor the standby fill can take or
 * cut it; engine_pages_grow() takes them first.  RAM-only and never listed
 * by a catalog: a run returns to free directly, and a load rebuilds free
 * from the catalog with every reserved run in it.  Caller holds
 * engine->lock.
 */
static uint64_t
engine_pages_reserved_runs_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->store_reserved == NULL)
		return (0);
	return (zfs_range_tree_space(engine->store_reserved) /
	    (ENGINE_STORE_BYTES * engine->vd->vdev_children));
}

/* Set aside one more run, the one engine_pages_grow() would pick. */
static boolean_t
engine_pages_reserved_fill_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (!engine->persistent || engine->free == NULL ||
	    engine->store_extents == NULL)
		return (B_FALSE);
	uint64_t bytes = ENGINE_STORE_BYTES * engine->vd->vdev_children;
	uint64_t range[2] = { UINT64_MAX, bytes };
	engine_walk_arg_t walk = { engine, range };
	zfs_range_tree_walk(engine->free, engine_pages_pick, &walk);
	if (range[0] == UINT64_MAX)
		return (B_FALSE);
	if (engine->store_reserved == NULL)
		engine->store_reserved = zfs_range_tree_create(NULL,
		    ZFS_RANGE_SEG64, NULL, 0, 0);
	zfs_range_tree_remove(engine->free, range[0], bytes);
	zfs_range_tree_add(engine->store_extents, range[0], bytes);
	zfs_range_tree_add(engine->store_reserved, range[0], bytes);
	engine->store_reserve_fills++;
	/* L13's per-store run count follows: one free run fewer. */
	if (engine->bulk_runs_cached != 0)
		engine->bulk_runs_cached--;
	engine->store_reserve_peak = MAX(engine->store_reserve_peak,
	    engine_pages_reserved_runs_locked(engine));
	return (B_TRUE);
}

/*
 * Take the highest reserved run out of the set; it stays in store_extents.
 * Every segment of the set is a union of whole stripe-aligned runs, so its
 * last run is one too.  UINT64_MAX if none.
 */
static uint64_t
engine_pages_reserved_take_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->store_reserved == NULL ||
	    zfs_range_tree_is_empty(engine->store_reserved))
		return (UINT64_MAX);
	uint64_t bytes = ENGINE_STORE_BYTES * engine->vd->vdev_children;
	uint64_t p = zfs_range_tree_max(engine->store_reserved) - bytes;
	VERIFY0(p % (ENGINE_STORE_PAGE * engine->vd->vdev_children));
	VERIFY(zfs_range_tree_contains(engine->store_reserved, p, bytes));
	zfs_range_tree_remove(engine->store_reserved, p, bytes);
	return (p);
}

/*
 * Return reserved runs past keep to free.  The runs come back whole, so
 * the standby refills from them and the cached run count sees them, as
 * when the reuse deferral returns runs.
 */
static void
engine_pages_reserved_return_locked(engine_t *engine, uint64_t keep)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t bytes = ENGINE_STORE_BYTES * engine->vd->vdev_children;
	boolean_t returned = B_FALSE;
	while (engine_pages_reserved_runs_locked(engine) > keep) {
		uint64_t p = engine_pages_reserved_take_locked(engine);
		zfs_range_tree_remove(engine->store_extents, p, bytes);
		zfs_range_tree_add(engine->free, p, bytes);
		engine->store_reserve_returns++;
		engine->bulk_runs_cached++;
		returned = B_TRUE;
	}
	if (returned && engine->free_runs_known) {
		boolean_t standby = engine->store_standby_valid;
		engine_pages_standby_fill_locked(engine);
		/* A refilled standby took one of the free runs counted. */
		if (!standby && engine->store_standby_valid &&
		    engine->bulk_runs_cached != 0)
			engine->bulk_runs_cached--;
		engine->free_runs_cached = engine_pages_runs_locked(engine);
	}
}

static uint64_t
engine_pages_reserved_run_bytes(engine_t *engine)
{
	return (ENGINE_STORE_BYTES * engine->vd->vdev_children);
}

/*
 * Give the highest held run back to free for a request nothing else in
 * free can take (a net free or a copy, past its wait): the stores that
 * counted it fall back to free runs and the standby, as before L15b.  No
 * standby refill here, but nothing keeps the run for the request either:
 * a later standby refill may take it whole (as it may any free run).  A
 * net-free caller refreshes free_runs_cached after its loop; a copy cuts
 * the run at once, so the cached count stays as it was.  B_FALSE if
 * nothing is held or one run cannot hold size.
 */
static boolean_t
engine_pages_reserved_take_back_locked(engine_t *engine, uint64_t size)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	uint64_t bytes = ENGINE_STORE_BYTES * engine->vd->vdev_children;
	if (engine_pages_reserved_runs_locked(engine) == 0 || size > bytes)
		return (B_FALSE);
	uint64_t p = engine_pages_reserved_take_locked(engine);
	zfs_range_tree_remove(engine->store_extents, p, bytes);
	zfs_range_tree_add(engine->free, p, bytes);
	engine->store_reserve_returns++;
	engine->bulk_runs_cached++;
	engine->admit_held_takes++;
	pr19262_printf("PR19262_ADMIT_HELD_TAKE p=%llu size=%llu held=%llu "
	    "takes=%llu synced=%llu\n", (u_longlong_t)p, (u_longlong_t)size,
	    (u_longlong_t)engine_pages_reserved_runs_locked(engine),
	    (u_longlong_t)engine->admit_held_takes,
	    (u_longlong_t)spa_last_synced_txg(engine->spa));
	return (B_TRUE);
}

static void
engine_pages_reserved_check_cb(void *arg, uint64_t p, uint64_t size)
{
	engine_t *engine = arg;
	uint64_t bytes = ENGINE_STORE_BYTES * engine->vd->vdev_children;
	VERIFY0(p % (ENGINE_STORE_PAGE * engine->vd->vdev_children));
	VERIFY0(size % bytes);
	VERIFY(zfs_range_tree_contains(engine->store_extents, p, size));
	zfs_range_tree_verify_not_present(engine->free, p, size);
}

/* The reserved runs are whole runs inside store_extents, none in free. */
static void
engine_pages_reserved_check_locked(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->lock));
	if (engine->store_reserved != NULL)
		zfs_range_tree_walk(engine->store_reserved,
		    engine_pages_reserved_check_cb, engine);
}

static int
engine_pages_grow(engine_mbatch_t *b)
{
	engine_t *engine = b->spa->spa_reloc;
	if (engine->pages->pending[1] != 0)
		return (engine_pages_reconcile(b->spa, B_TRUE));
	uint64_t bytes = ENGINE_STORE_BYTES * engine->vd->vdev_children;
	uint64_t range[2] = { UINT64_MAX, bytes };
	engine_walk_arg_t walk = { engine, range };
	boolean_t standby = B_FALSE, reserved = B_FALSE;
	mutex_enter(&engine->lock);
	/* A run admission counted and set aside comes first. */
	range[0] = engine_pages_reserved_take_locked(engine);
	if (range[0] != UINT64_MAX) {
		reserved = B_TRUE;
		engine->store_reserve_uses++;
	} else {
		zfs_range_tree_walk(engine->free, engine_pages_pick, &walk);
	}
	if (range[0] == UINT64_MAX && engine->store_standby_valid) {
		/* Already in store_extents: the standby run is the arena. */
		range[0] = engine->store_standby;
		engine->store_standby_valid = B_FALSE;
		engine->store_standby_uses++;
		standby = B_TRUE;
	}
	if (range[0] == UINT64_MAX) {
		engine->store_grow_enospc++;
		mutex_exit(&engine->lock);
		pr19262_printf("PR19262_M_GROW_ENOSPC capacity=%llu "
		    "available=%llu standby_uses=%llu\n",
		    (u_longlong_t)engine->pages->capacity,
		    (u_longlong_t)engine->pages->available,
		    (u_longlong_t)engine->store_standby_uses);
		return (ENOSPC);
	}
	if (!standby && !reserved) {
		zfs_range_tree_remove(engine->free, range[0], bytes);
		zfs_range_tree_add(engine->store_extents, range[0], bytes);
	}
	uint64_t held = engine_pages_reserved_runs_locked(engine);
	uint64_t uses = engine->store_reserve_uses;
	mutex_exit(&engine->lock);
	int error = engine_pages_add_arena(engine, range[0], ENGINE_STORE_PAGES,
	    NULL,
	    B_TRUE);
	if (error == 0 && reserved)
		pr19262_printf("PR19262_M_GROW_RESERVED p=%llu capacity=%llu "
		    "reserved_left=%llu reserve_uses=%llu\n",
		    (u_longlong_t)range[0],
		    (u_longlong_t)engine->pages->capacity, (u_longlong_t)held,
		    (u_longlong_t)uses);
	if (error != 0) {
		mutex_enter(&engine->lock);
		if (reserved) {
			/* Still in store_extents: it stays reserved. */
			zfs_range_tree_add(engine->store_reserved, range[0],
			    bytes);
			engine->store_reserve_uses--;
		} else if (standby && !engine->store_standby_valid) {
			/* Still in store_extents: it stays the standby. */
			engine->store_standby = range[0];
			engine->store_standby_valid = B_TRUE;
		} else {
			zfs_range_tree_remove(engine->store_extents, range[0],
			    bytes);
			zfs_range_tree_add(engine->free, range[0], bytes);
		}
		mutex_exit(&engine->lock);
		return (error);
	}
	engine_marena_t *a = engine->pages->arenas;
	engine->pages->pending[0] = range[0];
	engine->pages->pending[1] = a->pages;
	VERIFY0(random_get_pseudo_bytes((void *)(engine->pages->pending + 2),
	    2 * sizeof (uint64_t)));
	if (engine->pages->pending[2] == 0 && engine->pages->pending[3] == 0)
		engine->pages->pending[2] = 1;
	memcpy(a->uuid, engine->pages->pending + 2, sizeof (a->uuid));
	return (engine_pages_finish(b->spa, a, b->generation));
}

static void
engine_pages_remove_pending(engine_t *engine, engine_marena_t *a)
{
	VERIFY3P(a, ==, engine->pages->arenas);
	VERIFY3U(a->first + a->pages, ==, engine->pages->capacity);
	uint64_t count = a->first;
	engine_mnode_t **cache = pr19262_alloc(count * sizeof (*cache),
	    KM_SLEEP);
	uint8_t *bits = pr19262_alloc(count, KM_SLEEP);
	memcpy(cache, engine->pages->cache, count * sizeof (*cache));
	memcpy(bits, engine->pages->bits, count);
	for (uint64_t i = count; i < engine->pages->capacity; i++)
		if (engine->pages->cache[i] != NULL)
			engine_mnode_free(engine->pages->cache[i]);
	kmem_free(engine->pages->cache,
	    engine->pages->capacity * sizeof (*engine->pages->cache));
	kmem_free(engine->pages->bits, engine->pages->capacity);
	engine->pages->cache = cache;
	engine->pages->bits = bits;
	engine->pages->capacity = count;
	engine->pages->free_hint = 0;
	engine->pages->arenas = a->next;
	kmem_free(a->slots, 2 * a->pages);
	kmem_free(a, sizeof (*a));
}

/* A PREPARE may have formatted bytes; never infer that from P membership. */
static int
engine_pages_pending_inspect(spa_t *spa, engine_marena_t *a)
{
	engine_t *engine = spa->spa_reloc;
	if (a->formatted)
		return (0);
	engine_mnode_t *n = engine_mnode_alloc();
	engine_mnode_t *wire = engine_mnode_alloc();
	engine_mnode_t *directory = NULL;
	abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
	uint8_t *bits = pr19262_zalloc(a->pages, KM_SLEEP);
	bits[0] = bits[1] = 1;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	boolean_t formatted = B_FALSE;
	uint64_t generation = 0, serial = engine->pages->serial;
	engine_mref_t candidate = { 0 };
	engine_mref_t forward = { 0 };
	engine_mcontrol_t highest = { 0 };
	boolean_t conflict = B_FALSE;
	int error = 0;
	uint_t readable = 0;
	/* Cold reconcile runs from spa_load with no config lock held. */
	spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
	for (uint_t page = 0; error == 0 && page < 3; page++) {
		for (uint_t c = 0; error == 0 &&
		    c < engine->vd->vdev_children; c++) {
			uint64_t p = a->p + page * stripe;
			error = engine_store_io_at(engine->vd->vdev_child[c], p,
			    0, 1, abd, B_FALSE);
			if (error == ENXIO) {
				/* An absent member is inspected on return. */
				error = 0;
				continue;
			}
			if (error != 0)
				break;
			readable++;
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
			    h->top != engine->vd->vdev_guid ||
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
				    &engine->pages->directory,
				    sizeof (published)) == 0 &&
				    memcmp(pending, engine->pages->pending,
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
				    h->low[2] != engine->vd->vdev_children ||
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
				    memcmp(data, engine->pages->pending,
				    sizeof (engine->pages->pending)) != 0 ||
				    memcmp(data + 4, &engine->pages->directory,
				    sizeof (engine->pages->directory)) != 0) {
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
					wire = engine_mnode_alloc();
					directory->sealed = B_TRUE;
				}
				bits[2] = 1;
			}
			formatted = B_TRUE;
		}
	}
	spa_config_exit(spa, SCL_ZIO, FTAG);
	if (error == 0 && readable == 0)
		error = ENXIO;
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
			engine->pages->cache[a->first + 2] = directory;
			directory = NULL;
		}
		memcpy(engine->pages->bits + a->first, bits, a->pages);
		engine->pages->free_hint = 0;
		engine->pages->serial = serial;
	}
	kmem_free(bits, a->pages);
	if (directory != NULL)
		engine_mnode_free(directory);
	engine_mnode_free(wire);
	abd_free(abd);
	engine_mnode_free(n);
	return (error);
}

static int
engine_pages_reconcile(spa_t *spa, boolean_t write)
{
	engine_t *engine = spa->spa_reloc;
	if (engine->pages->pending[1] == 0)
		return (0);
	engine_marena_t *a = engine->pages->arenas;
	if (!a->pending || a->p != engine->pages->pending[0])
		return (ENGINE_EINVAL(15));
	if (write) {
		int error = engine_pages_pending_inspect(spa, a);
		if (error != 0)
			return (error);
	}
	uint64_t bytes = a->pages * ENGINE_STORE_PAGE *
	    engine->vd->vdev_children;
	mutex_enter(&engine->lock);
	boolean_t claimed = zfs_range_tree_contains(engine->store_extents,
	    a->p, bytes);
	boolean_t free = engine_store_extent(engine, a->p, bytes);
	if (free) {
		zfs_range_tree_remove(engine->free, a->p, bytes);
		zfs_range_tree_add(engine->store_extents, a->p, bytes);
	}
	mutex_exit(&engine->lock);
	if (!write)
		return (0);
	if (free || claimed)
		return (engine_pages_finish(spa, a, engine->pages->generation));
	if (a->formatted)
		return (ECKSUM);
	/* A lone, unformatted PREPARE cannot override a recovered owner. */
	engine_mcontrol_t cancel = { .directory = engine->pages->directory,
	    .floor = engine->pages->floor,
	    .certified = engine->pages->certified };
	int error = engine_pages_reserve_control(spa, engine->pages->generation,
	    &cancel);
	if (error == 0)
		error = engine_pages_reserve_control(spa,
		    engine->pages->generation, &cancel);
	if (error != 0)
		return (error);
	memset(engine->pages->pending, 0, sizeof (engine->pages->pending));
	engine_pages_remove_pending(engine, a);
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

#ifndef _KERNEL
/* Real slot I/O: skipping must not erase the predecessor on a shrink. */
static int
engine_pages_slots_witness(spa_t *spa)
{
	engine_t *engine = spa->spa_reloc;
	engine_mbatch_t b = { .spa = spa,
	    .generation = engine->pages->generation + 1 };
	int error = engine_pages_grow(&b);
	if (error != 0)
		return (error);
	engine_marena_t *a = engine->pages->arenas;
	VERIFY3U(a->p, !=, engine->arena);
	VERIFY0(engine->pages->bits[a->first + 3]);
	/* A reserved, unwritten page is enough to test the durable masks. */
	mutex_enter(&engine->lock);
	engine->pages->bits[a->first + 3] = 1;
	engine->pages->available--;
	mutex_exit(&engine->lock);
	VERIFY0(engine_pages_reserve(spa, b.generation));
	VERIFY0(engine_pages_reserve(spa, b.generation));
	uint64_t local = a->serial, global = engine->pages->serial;
	uint64_t skipped = engine->slot_cost.skipped;
	for (uint_t i = 0; i < 4; i++)
		VERIFY0(engine_pages_reserve(spa, b.generation));
	VERIFY3U(a->serial, ==, local);
	VERIFY3U(engine->pages->serial, ==, global + 4);
	VERIFY3U(engine->slot_cost.skipped, >=, skipped + 4);
	mutex_enter(&engine->lock);
	engine->pages->bits[a->first + 3] = 0;
	engine->pages->available++;
	engine->pages->free_hint = 0;
	mutex_exit(&engine->lock);
	boolean_t relaxed = engine->pages->gc_relaxed;
	engine->pages->gc_relaxed = B_TRUE;
	uint8_t *disk = kmem_zalloc(a->pages, KM_SLEEP);
	uint64_t generation = 0, serial = 0;
	VERIFY0(engine_pages_reserve(spa, b.generation));
	VERIFY3U(a->serial, ==, local + 1);
	VERIFY0(engine_pages_reservations_arena(spa, a, disk,
	    &generation, &serial, NULL, B_TRUE));
	VERIFY3U(disk[3], ==, 1);
	memset(disk, 0, a->pages);
	VERIFY0(engine_pages_reserve(spa, b.generation));
	VERIFY3U(a->serial, ==, local + 2);
	VERIFY0(engine_pages_reservations_arena(spa, a, disk,
	    &generation, &serial, NULL, B_TRUE));
	VERIFY0(disk[3]);
	engine->pages->gc_relaxed = relaxed;
	kmem_free(disk, a->pages);
	/* Any observed I/O error must send the next reserve back to disk. */
	uint64_t reads = engine->slot_cost.reads;
	atomic_inc_64(&engine->io_errors);
	VERIFY0(engine_pages_reserve(spa, b.generation));
	VERIFY3U(engine->slot_cost.reads, >, reads);
	pr19262_printf("PR19262_M_SLOTS PASS gap=4 retained_first=1 "
	    "released_second=1 error_invalidated=1 local=%llu global=%llu "
	    "skipped=%llu cached=%llu checked=%llu\n",
	    (u_longlong_t)a->serial, (u_longlong_t)engine->pages->serial,
	    (u_longlong_t)engine->slot_cost.skipped,
	    (u_longlong_t)engine->slot_cost.cached,
	    (u_longlong_t)engine->slot_cost.checked);
	pr19262_flush();
	return (0);
}
#endif

/* Private userspace witness: execute the real operation, including errors. */
static int
engine_pages_growth_probe(spa_t *spa, boolean_t advance,
    pr19262_mgrowth_probe_t *out)
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->store_lock));
	if (engine->pages->capacity == 0) {
		memset(out, 0, sizeof (*out));
		return (ENOTSUP);
	}
	int error = 0;
#ifndef _KERNEL
	if (pr19262_env("PR19262_M_SLOTS_WITNESS") != NULL) {
		error = engine_pages_slots_witness(spa);
		if (error != 0)
			return (error);
	}
#endif
	if (advance) {
		if (engine->pages->generation == UINT64_MAX)
			error = EOVERFLOW;
		else if (engine->pages->pending[1] != 0)
			error = engine_pages_reconcile(spa, B_TRUE);
		else {
			engine_mbatch_t b = { .spa = spa,
			    .generation = engine->pages->generation + 1 };
			error = engine_pages_grow(&b);
		}
	}
	memset(out, 0, sizeof (*out));
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	engine_marena_t *operation = NULL;
	for (engine_marena_t *a = engine->pages->arenas; a != NULL;
	    a = a->next) {
		if ((a->pending && a->p == engine->pages->pending[0]) ||
		    (!a->pending && a->p + 2 * stripe ==
		    engine->pages->directory.p)) {
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
	memcpy(out->directory, &engine->pages->directory,
	    sizeof (out->directory));
	out->serial = engine->pages->serial;
	out->control_serial = engine->pages->control_serial;
	out->capacity = engine->pages->capacity;
	out->floor = engine->pages->floor;
	out->certified = engine->pages->certified;
	uint64_t count = engine->pages->directory_pages == NULL ? 0 :
	    zfs_range_tree_numsegs(engine->pages->directory_pages);
	engine_munion_t u = { .ranges = pr19262_zalloc(MAX(1, count) * 16,
	    KM_SLEEP) };
	if (engine->pages->directory_pages != NULL) {
		out->directory_bytes =
		    zfs_range_tree_space(engine->pages->directory_pages);
		zfs_range_tree_walk(engine->pages->directory_pages,
		    engine_pages_union_record, &u);
	}
	engine_pages_probe_hash(u.ranges, MAX(1, u.at) * 8,
	    out->directory_hash);
	kmem_free(u.ranges, MAX(1, count) * 16);
	uint8_t *bits = pr19262_alloc(engine->pages->capacity, KM_SLEEP);
	mutex_enter(&engine->lock);
	memcpy(bits, engine->pages->bits, engine->pages->capacity);
	mutex_exit(&engine->lock);
	engine_pages_probe_hash(bits, engine->pages->capacity,
	    out->bitmap_hash);
	/*
	 * Cache offsets follow discovery order, so also hash the same
	 * allocation bytes in physical arena order: ascending P, each
	 * arena prefixed by its BE64 base and page count.
	 */
	uint64_t arenas = 0, pages = 0;
	for (engine_marena_t *a = engine->pages->arenas; a != NULL;
	    a = a->next) {
		arenas++;
		pages += a->pages;
	}
	uint64_t size = arenas * 16 + pages, at = 0;
	uint8_t *canonical = pr19262_alloc(size, KM_SLEEP);
	engine_marena_t *previous = NULL;
	for (uint64_t i = 0; i < arenas; i++) {
		engine_marena_t *next = NULL;
		for (engine_marena_t *a = engine->pages->arenas; a != NULL;
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
	kmem_free(bits, engine->pages->capacity);
	return (error);
}

/* Rows of released journaled owners stay as tombstones until they expire. */
static boolean_t
engine_pages_tombstone_needed(engine_t *engine, const engine_mrow_t *row)
{
	if (row->key[3] != 0)
		return (B_FALSE);
	engine_tomb_t key = { .v = row->key[1], .incarnation = row->key[2] };
	mutex_enter(&engine->lock);
	engine_tomb_t *t = avl_find(&engine->tombstones, &key, NULL);
	boolean_t needed = t != NULL && !t->expired;
	mutex_exit(&engine->lock);
	return (needed);
}

static uint64_t *
engine_pages_expired_tombstones(engine_t *engine, uint_t *count)
{
	*count = 0;
	mutex_enter(&engine->lock);
	uint_t n = 0;
	for (engine_tomb_t *t = avl_first(&engine->tombstones); t != NULL;
	    t = AVL_NEXT(&engine->tombstones, t))
		n += t->expired;
	uint64_t *keys = n == 0 ? NULL :
	    pr19262_alloc(n * 2 * sizeof (uint64_t), KM_SLEEP);
	uint_t at = 0;
	for (engine_tomb_t *t = avl_first(&engine->tombstones); t != NULL;
	    t = AVL_NEXT(&engine->tombstones, t)) {
		if (!t->expired)
			continue;
		keys[2 * at] = t->v;
		keys[2 * at + 1] = t->incarnation;
		at++;
	}
	mutex_exit(&engine->lock);
	*count = n;
	return (keys);
}

/* The stored tree no longer carries these rows. */
static void
engine_pages_expire_tombstones(engine_t *engine, const uint64_t *keys,
    uint_t count)
{
	mutex_enter(&engine->lock);
	for (uint_t i = 0; i < count; i++) {
		engine_tomb_t key = { .v = keys[2 * i],
		    .incarnation = keys[2 * i + 1] };
		engine_tomb_t *t = avl_find(&engine->tombstones, &key, NULL);
		if (t != NULL && t->expired) {
			avl_remove(&engine->tombstones, t);
			kmem_free(t, sizeof (*t));
		}
	}
	mutex_exit(&engine->lock);
}

/*
 * Store this root's catalog: rows (count of them, in key order, taken
 * over) against the tree of the last stored root.  A row the last root
 * stored byte for byte needs no insert: the tree already holds it.
 */
/*
 * Debug ztest still keeps the stored rows twice, as a sorted mirror, and
 * checks every step of the tree walk against it; elsewhere the walk alone
 * stands in for the mirror (160 B per allocation).
 */
#if defined(ZFS_DEBUG) && !defined(_KERNEL)
#define	ENGINE_MIRROR	1
#else
#define	ENGINE_MIRROR	0
#endif

/*
 * Clean leaves kept in memory, in pages (0: all); debug ztest keeps few,
 * so its witnesses read leaves back all the time.
 */
#if defined(ZFS_DEBUG) && !defined(_KERNEL)
static uint64_t zfs_pr19262_page_cache = 256;
#else
static uint64_t zfs_pr19262_page_cache = 16384;
#endif

static boolean_t
engine_pages_evictable(engine_mnode_t *n)
{
	return (n != NULL && !n->dirty && n->sealed &&
	    engine_mheader(n)->type == ENGINE_M_LEAF);
}

/*
 * After a stored root: clean leaves beyond the budget leave memory, least
 * recently used first, down to three quarters of it.  Branches, blobs and
 * directories stay; a later lookup reads an evicted leaf back and checks
 * it against its reference like any page.
 */
static uint_t
engine_pages_evict(engine_t *engine)
{
	ASSERT(MUTEX_HELD(&engine->store_lock));
	uint64_t budget = zfs_pr19262_page_cache;
	const char *env = pr19262_env("PR19262_PAGE_CACHE");
	if (env != NULL)
		budget = pr19262_strtoull(env, NULL, 10);
	if (budget == 0)
		return (0);
	uint64_t n = 0;
	for (uint64_t i = 0; i < engine->pages->capacity; i++)
		n += engine_pages_evictable(engine->pages->cache[i]);
	if (n <= budget)
		return (0);
	uint64_t *ages = pr19262_zalloc(n * 2 * sizeof (uint64_t), KM_SLEEP);
	uint64_t at = 0;
	for (uint64_t i = 0; i < engine->pages->capacity; i++) {
		engine_mnode_t *node = engine->pages->cache[i];
		if (!engine_pages_evictable(node))
			continue;
		ages[2 * at] = node->used;
		ages[2 * at + 1] = i;
		at++;
	}
	VERIFY3U(at, ==, n);
	qsort(ages, n, 2 * sizeof (uint64_t), engine_dirty_compare);
	uint64_t drop = n - (budget - budget / 4);
	for (uint64_t j = 0; j < drop; j++) {
		uint64_t i = ages[2 * j + 1];
		engine_mnode_free(engine->pages->cache[i]);
		engine->pages->cache[i] = NULL;
	}
	engine->pages_evicted += drop;
	kmem_free(ages, n * 2 * sizeof (uint64_t));
	return ((uint_t)drop);
}

/* Check incremental catalog stores against the full rows (tests). */
static int zfs_pr19262_store_verify = 0;

static void
engine_store_check_failed(engine_t *engine, const char *cond, int line)
{
	engine->store_check_failures++;
	cmn_err(CE_WARN, "pr19262: catalog store check failed: %s "
	    "(line %d, failures %llu)", cond, line,
	    (u_longlong_t)engine->store_check_failures);
}

/* Debug ztest stops at a failed check; a verifying kernel warns. */
#define	ENGINE_STORE_CHECK(cond)					\
	do {								\
		if (ENGINE_MIRROR)					\
			VERIFY(cond);					\
		else if (!(cond))					\
			engine_store_check_failed(engine, #cond, \
			    __LINE__);	\
	} while (0)

/*
 * An in-order walk of a stored tree's rows, tombstones skipped: what the
 * last store wrote, read from the cached pages.  The tree is a previous
 * generation's; a batch copies a page before it changes it and frees only
 * its own, so the walk sees it unchanged while the batch builds the next.
 */
#define	ENGINE_M_DEPTH	16
typedef struct engine_mcursor {
	engine_mbatch_t *b;
	uint_t depth;
	engine_mnode_t *node[ENGINE_M_DEPTH];
	uint_t at[ENGINE_M_DEPTH];
} engine_mcursor_t;

static int
engine_mcursor_down(engine_mcursor_t *c, const engine_mref_t *ref)
{
	for (;;) {
		if (c->depth == ENGINE_M_DEPTH)
			return (ENGINE_EINVAL(39));
		engine_mnode_t *n;
		int error = engine_mget(c->b, ref, &n);
		if (error != 0)
			return (error);
		engine_mheader_t *h = engine_mheader(n);
		if (h->count == 0 || (h->type != ENGINE_M_LEAF &&
		    h->type != ENGINE_M_BRANCH))
			return (ENGINE_EINVAL(40));
		c->node[c->depth] = n;
		c->at[c->depth] = 0;
		c->depth++;
		if (h->type == ENGINE_M_LEAF)
			return (0);
		ref = &((engine_mchild_t *)engine_mpayload(n))[0].ref;
	}
}

/* The current row, or NULL past the last. */
static const engine_mrow_t *
engine_mcursor_row(const engine_mcursor_t *c)
{
	if (c->depth == 0)
		return (NULL);
	uint_t d = c->depth - 1;
	return (&((engine_mrow_t *)engine_mpayload(c->node[d]))[c->at[d]]);
}

/* One row on, climbing out of finished nodes; depth 0 past the last. */
static int
engine_mcursor_step(engine_mcursor_t *c)
{
	while (c->depth > 0) {
		uint_t d = c->depth - 1;
		engine_mnode_t *n = c->node[d];
		engine_mheader_t *h = engine_mheader(n);
		if (++c->at[d] < h->count) {
			if (h->type == ENGINE_M_LEAF)
				return (0);
			engine_mchild_t *child = engine_mpayload(n);
			return (engine_mcursor_down(c, &child[c->at[d]].ref));
		}
		c->depth--;
	}
	return (0);
}

static int
engine_mcursor_skip(engine_mcursor_t *c)
{
	int error = 0;
	const engine_mrow_t *r;
	while (error == 0 && (r = engine_mcursor_row(c)) != NULL &&
	    r->data[6] == 3)
		error = engine_mcursor_step(c);
	return (error);
}

/* Past the current row, and past any tombstones after it. */
static int
engine_mcursor_next(engine_mcursor_t *c)
{
	if (c->depth == 0)
		return (ENGINE_EINVAL(41));
	int error = engine_mcursor_step(c);
	return (error != 0 ? error : engine_mcursor_skip(c));
}

/*
 * At the first row at or past the key, tombstones skipped.  A branch
 * child's high is its last key, so the first child whose high is not
 * before the key holds the row.
 */
static int
engine_mcursor_seek(engine_mcursor_t *c, const engine_mref_t *root,
    const uint64_t key[4])
{
	c->depth = 0;
	if (root->p == UINT64_MAX)
		return (0);
	const engine_mref_t *ref = root;
	for (;;) {
		if (c->depth == ENGINE_M_DEPTH)
			return (ENGINE_EINVAL(39));
		engine_mnode_t *n;
		int error = engine_mget(c->b, ref, &n);
		if (error != 0)
			return (error);
		engine_mheader_t *h = engine_mheader(n);
		if (h->count == 0 || (h->type != ENGINE_M_LEAF &&
		    h->type != ENGINE_M_BRANCH))
			return (ENGINE_EINVAL(40));
		boolean_t leaf = h->type == ENGINE_M_LEAF;
		engine_mrow_t *rows = engine_mpayload(n);
		engine_mchild_t *children = engine_mpayload(n);
		uint_t lo = 0, hi = h->count;
		while (lo < hi) {
			uint_t mid = lo + (hi - lo) / 2;
			if (engine_mkey_compare(leaf ? rows[mid].key :
			    children[mid].high, key) < 0)
				lo = mid + 1;
			else
				hi = mid;
		}
		uint_t d = c->depth++;
		c->node[d] = n;
		if (lo == h->count) {
			/* All of it is before the key: past its last row. */
			c->at[d] = h->count - 1;
			error = engine_mcursor_step(c);
			return (error != 0 ? error : engine_mcursor_skip(c));
		}
		c->at[d] = lo;
		if (leaf)
			return (engine_mcursor_skip(c));
		ref = &children[lo].ref;
	}
}

static int
engine_mcursor_open(engine_mcursor_t *c, const engine_mref_t *root)
{
	c->depth = 0;
	if (root->p == UINT64_MAX)
		return (0);
	int error = engine_mcursor_down(c, root);
	return (error != 0 ? error : engine_mcursor_skip(c));
}

/*
 * Before an incremental store's merge: the uncached pages its seeks will
 * reach first, found through the cached branches, are read at once from
 * the first member instead of one by one in the sync thread.  A copy the
 * checks refuse, or a member that cannot read, leaves the page to the
 * merge's own lookup, which tries every member.  Out of line: the page
 * store's frame stays as it was.
 */
noinline static uint_t
engine_pages_prefetch(engine_mbatch_t *b, const engine_dirty_snap_t *snap)
{
	engine_t *engine = b->spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->store_lock));
	if (engine->pages->root.p == UINT64_MAX || snap->count == 0)
		return (0);
	engine_mref_t *refs = pr19262_zalloc(snap->count * sizeof (*refs),
	    KM_SLEEP);
	uint_t n = 0;
	uint64_t last = UINT64_MAX;
	for (uint_t i = 0; i < snap->count; i++) {
		const uint64_t *k = &snap->keys[4 * i];
		uint64_t key[4] = { 1, k[0], k[1], 0 };
		const engine_mref_t *ref = &engine->pages->root;
		for (uint_t depth = 0; depth < ENGINE_M_DEPTH; depth++) {
			uint64_t index;
			if (!engine_mindex(engine, ref->p, &index))
				break;
			engine_mnode_t *node = engine->pages->cache[index];
			if (node == NULL) {
				if (index != last)
					refs[n++] = *ref;
				last = index;
				break;
			}
			engine_mheader_t *h = engine_mheader(node);
			if (h->type != ENGINE_M_BRANCH || h->count == 0 ||
			    h->generation != ref->generation)
				break;
			engine_mchild_t *children = engine_mpayload(node);
			uint_t lo = 0, hi = h->count;
			while (lo < hi) {
				uint_t mid = lo + (hi - lo) / 2;
				if (engine_mkey_compare(children[mid].high,
				    key) < 0)
					lo = mid + 1;
				else
					hi = mid;
			}
			if (lo == h->count)
				break;
			ref = &children[lo].ref;
		}
	}
	uint_t installed = 0;
	vdev_t *vd = engine->vd->vdev_child[0];
	if (n != 0) {
		engine_mnode_t **nodes = pr19262_zalloc(n * sizeof (*nodes),
		    KM_SLEEP);
		abd_t **abds = pr19262_zalloc(n * sizeof (*abds), KM_SLEEP);
		int *errors = pr19262_zalloc(n * sizeof (*errors), KM_SLEEP);
		spa_config_enter(b->spa, SCL_ZIO, FTAG, RW_READER);
		zio_t *pio = zio_root(b->spa, NULL, NULL, ZIO_FLAG_CANFAIL);
		for (uint_t j = 0; j < n; j++) {
			nodes[j] = engine_mnode_alloc();
			abds[j] = abd_get_from_buf(nodes[j]->data,
			    ENGINE_STORE_PAGE);
			zio_t *zio;
			errors[j] = engine_store_zio(pio, vd, refs[j].p, 0, 1,
			    abds[j], B_FALSE, &zio);
			if (errors[j] == 0)
				zio_nowait(zio);
		}
		/* A failed read shows in its page's checks, not here. */
		(void) zio_wait(pio);
		spa_config_exit(b->spa, SCL_ZIO, FTAG);
		for (uint_t j = 0; j < n; j++) {
			uint64_t index;
			abd_free(abds[j]);
			if (errors[j] == 0 &&
			    engine_mindex(engine, refs[j].p, &index) &&
			    engine->pages->cache[index] == NULL &&
			    engine_mpage_accept(b->spa, &refs[j], nodes[j])) {
				nodes[j]->sealed = B_TRUE;
				nodes[j]->used = b->generation;
				engine->pages->cache[index] = nodes[j];
				installed++;
			} else {
				engine_mnode_free(nodes[j]);
			}
		}
		kmem_free(errors, n * sizeof (*errors));
		kmem_free(abds, n * sizeof (*abds));
		kmem_free(nodes, n * sizeof (*nodes));
	}
	kmem_free(refs, snap->count * sizeof (*refs));
	return (installed);
}

/*
 * One step of a store's merge of the last tree's rows (at the cursor) with
 * its own, in key order: an old row alone leaves, as a tombstone while a
 * journal segment may still bind a released journaled owner; an equal row
 * stays; a new or changed row goes in.
 */
static int
engine_pages_merge(engine_mbatch_t *b, engine_mref_t *tree,
    engine_mcursor_t *cur, const engine_mrow_t *old, int order,
    const engine_mrow_t *rows, uint_t *after, uint64_t *live)
{
	engine_t *engine = b->spa->spa_reloc;
	int error = 0;
	if (order < 0) {
		engine_mrow_t removed = *old;
		ENGINE_STORE_CHECK(*live > 0);
		*live -= *live > 0;
		error = engine_mcursor_next(cur);
		if (error != 0)
			return (error);
		if (engine_pages_tombstone_needed(engine, &removed)) {
			removed.data[6] = 3;
			b->tombstones++;
			return (engine_mput(b, tree, &removed));
		}
		return (engine_mdel(b, tree, removed.key));
	}
	if (order == 0 && memcmp(old, &rows[*after],
	    sizeof (engine_mrow_t)) == 0) {
#if defined(ZFS_DEBUG) && !defined(_KERNEL)
		/* Debug ztest proves the walk: a no-op insert. */
		uint64_t changes = b->changes;
		error = engine_mput(b, tree, &rows[*after]);
		VERIFY(error != 0 || b->changes == changes);
#endif
		(*after)++;
		return (error != 0 ? error : engine_mcursor_next(cur));
	}
	if (order == 0)
		error = engine_mcursor_next(cur);
	else
		(*live)++;
	if (error == 0)
		error = engine_mput(b, tree, &rows[(*after)++]);
	return (error);
}

/* Debug ztest: a seek lands where the last store's rows say. */
static void
engine_pages_mirror_seek(const engine_mcursor_t *c, const uint64_t key[4])
{
	engine_t *engine = c->b->spa->spa_reloc;
	uint_t lo = 0, hi = engine->pages->count;
	while (lo < hi) {
		uint_t mid = lo + (hi - lo) / 2;
		if (engine_mkey_compare(engine->pages->rows[mid].key, key) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	const engine_mrow_t *r = engine_mcursor_row(c);
	VERIFY((r == NULL) == (lo == engine->pages->count));
	VERIFY(r == NULL || memcmp(r, &engine->pages->rows[lo],
	    sizeof (*r)) == 0);
}

/* Is the row one of the owner's a snapshot entry names? */
static boolean_t
engine_mrow_of(const engine_mrow_t *r, const uint64_t *k)
{
	return (r->key[0] == 1 && r->key[1] == k[0] && r->key[2] == k[1]);
}

/*
 * A store of this txg's root.  With snap->all the rows are every listed
 * owner's and the whole last tree is walked; otherwise they are only the
 * rows of the listed owners the snapshot names, and the store seeks each
 * named owner's rows in the last tree.  Debug ztest passes the full rows
 * (full) with an incremental store and checks the new tree against them.
 */
static int
engine_pages_store(spa_t *spa, const uberblock_t *ub, nvlist_t *nv,
    engine_mrow_t *rows, uint_t count, engine_mrow_t *full, uint_t full_count,
    const engine_dirty_snap_t *snap, uint64_t desc[16])
{
	engine_t *engine = spa->spa_reloc;
	ASSERT(MUTEX_HELD(&engine->store_lock));
	VERIFY(full == NULL || !snap->all);
	engine_store_cost_t cost = { 0 };
	hrtime_t start = gethrtime(), reserving = start, reserved = start;
	engine_pages_init(engine);
	int error = 0;
	if (engine->pages->pending[1] != 0)
		error = engine_pages_reconcile(spa, B_TRUE);
	/* An incremental store bounds its rows by the tree's plus its own. */
	if (error == 0 && (snap->all ? count : engine->pages->live + count) >
	    engine_pages_row_limit(engine))
		error = ENGINE_EINVAL(9);
	if (error == 0 && engine->pages->generation == UINT64_MAX)
		error = EOVERFLOW;
	if (error != 0) {
		kmem_free(rows, MAX(1, count) * sizeof (*rows));
		if (full != NULL)
			kmem_free(full, MAX(1, full_count) * sizeof (*full));
		return (error);
	}
	engine_mbatch_t *b = kmem_zalloc(sizeof (*b), KM_SLEEP);
	b->spa = spa;
	b->generation = ++engine->pages->generation;
	engine_mref_t tree = engine->pages->root;
	engine_mcursor_t cur = { .b = b };
	uint_t after = 0, evicted = 0, prefetched = 0;
	uint64_t live = engine->pages->live;
	if (snap->all) {
		/* The rows the last store wrote, walked from its tree. */
		error = engine_mcursor_open(&cur, &engine->pages->root);
		uint_t before = 0;
		while (error == 0) {
			const engine_mrow_t *old = engine_mcursor_row(&cur);
			if (ENGINE_MIRROR) {
				VERIFY3U(before, <=, engine->pages->count);
				VERIFY((old == NULL) ==
				    (before == engine->pages->count));
				VERIFY(old == NULL || memcmp(old,
				    &engine->pages->rows[before],
				    sizeof (*old)) == 0);
			}
			if (old == NULL && after == count)
				break;
			int order = old == NULL ? 1 : (after == count ? -1 :
			    engine_mkey_compare(old->key, rows[after].key));
			/* Debug ztest: no row changes but a marked owner's. */
			if (ENGINE_MIRROR && order != 0)
				VERIFY(engine_dirty_covers(snap,
				    order < 0 ? old->key : rows[after].key));
			if (ENGINE_MIRROR && order == 0 && memcmp(old,
			    &rows[after], sizeof (engine_mrow_t)) != 0)
				VERIFY(engine_dirty_covers(snap, old->key));
			before += order <= 0;
			error = engine_pages_merge(b, &tree, &cur, old, order,
			    rows, &after, &live);
		}
		if (error == 0 && ENGINE_MIRROR)
			VERIFY3U(live, ==, count);
		live = count;
	} else {
		prefetched = engine_pages_prefetch(b, snap);
		/* Each named owner's rows in the last tree, and its own. */
		for (uint_t i = 0; error == 0 && i < snap->count; i++) {
			const uint64_t *k = &snap->keys[4 * i];
			uint64_t start[4] = { 1, k[0], k[1], 0 };
			error = engine_mcursor_seek(&cur, &engine->pages->root,
			    start);
			if (error == 0 && ENGINE_MIRROR)
				engine_pages_mirror_seek(&cur, start);
			while (error == 0) {
				const engine_mrow_t *old =
				    engine_mcursor_row(&cur);
				boolean_t mine = old != NULL &&
				    engine_mrow_of(old, k);
				boolean_t own = after < count &&
				    engine_mrow_of(&rows[after], k);
				if (!mine && !own)
					break;
				int order = !mine ? 1 : (!own ? -1 :
				    engine_mkey_compare(old->key,
				    rows[after].key));
				error = engine_pages_merge(b, &tree, &cur,
				    old, order, rows, &after, &live);
			}
		}
		if (error == 0)
			VERIFY3U(after, ==, count);
	}
	/* Tombstones whose journal segments are gone leave the tree. */
	uint_t expired = 0;
	uint64_t *expired_keys = engine_pages_expired_tombstones(engine,
	    &expired);
	for (uint_t i = 0; error == 0 && i < expired; i++) {
		uint64_t key[4] = { 1, expired_keys[2 * i],
		    expired_keys[2 * i + 1], 0 };
		error = engine_mdel(b, &tree, key);
	}
	/* The incremental tree holds exactly the full rows. */
	if (error == 0 && full != NULL) {
		engine_mcursor_t check = { .b = b };
		uint_t at = 0;
		boolean_t same = B_TRUE;
		const engine_mrow_t *r;
		error = engine_mcursor_open(&check, &tree);
		while (error == 0 && same &&
		    (r = engine_mcursor_row(&check)) != NULL) {
			same = at < full_count &&
			    memcmp(r, &full[at++], sizeof (*r)) == 0;
			error = engine_mcursor_next(&check);
		}
		if (error == 0) {
			ENGINE_STORE_CHECK(same && at == full_count &&
			    live == full_count);
			engine->store_checked++;
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
		/* A snapshot for this store carries no nvlist of owners. */
		(void) nvlist_remove_all(nv, "owners");
		fnvlist_add_uint64_array(nv, "page-tree", (uint64_t *)&tree, 6);
		fnvlist_add_uint64(nv, "page-generation", b->generation);
		error = nvlist_pack(nv, &packed, &size, NV_ENCODE_XDR,
		    KM_SLEEP);
	}
	if (error == 0) {
		nb = howmany(size, ENGINE_M_PAYLOAD);
		if (nb == 0 || nb > engine->pages->capacity)
			error = ENOSPC;
		else
			blobs = pr19262_zalloc(nb * sizeof (*blobs), KM_SLEEP);
	}
	for (uint_t i = 0; error == 0 && i < nb; i++)
		error = engine_mnew(b, ENGINE_M_BLOB, 0, &blobs[i]);
	if (error == 0) {
		kmem_free(packed, size); packed = NULL;
		fnvlist_remove(nv, "bitmap");
		mutex_enter(&engine->lock);
		fnvlist_add_byte_array(nv, "bitmap", engine->bitmap,
		    ENGINE_STORE_PAGES);
		mutex_exit(&engine->lock);
		size = 0;
		error = nvlist_pack(nv, &packed, &size, NV_ENCODE_XDR,
		    KM_SLEEP);
		if (error == 0 && howmany(size, ENGINE_M_PAYLOAD) != nb)
			error = ENGINE_EINVAL(23);
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
		reserving = gethrtime();
		/* Independent of BIND errors; reserve before any page write. */
		error = engine_pages_reserve(spa, b->generation);
		/* Fence both legacy control slots before codec7 page writes. */
		if (error == 0)
			error = engine_pages_reserve(spa, b->generation);
		if (error == 0)
			engine_pages_fault(2, b->generation, 0, UINT64_MAX);
		reserved = gethrtime();
	}
	/*
	 * Without a page-cut hook, every member writes all dirty pages and
	 * then flushes at once; the hook cuts after a page or the flush of
	 * the first member alone and keeps the member-by-member order.
	 */
	if (error == 0 && pr19262_env("PR19262_M_PAGE_FAULT") == NULL) {
		uint_t n = 0, children = engine->vd->vdev_children;
		engine_member_page_t *pv = pr19262_alloc(
		    MAX(1, engine->pages->capacity) * sizeof (*pv), KM_SLEEP);
		for (uint64_t i = 0; error == 0 &&
		    i < engine->pages->capacity; i++) {
			engine_mnode_t *n_i = engine->pages->cache[i];
			uint64_t index;
			if (!engine_mdirty(b, n_i))
				continue;
			VERIFY(n_i->sealed);
			if (!engine_mindex(engine, engine_mheader(n_i)->p,
			    &index)) {
				error = ENGINE_EINVAL(3);
				break;
			}
			pv[n].arena = engine_mheader(n_i)->p;
			pv[n].page = 0;
			pv[n++].abd = abd_get_from_buf(n_i->data,
			    ENGINE_STORE_PAGE);
		}
		if (error == 0) {
			engine_quorum_t q = { 0 };
			int *errors = kmem_alloc(children * sizeof (int),
			    KM_SLEEP);
			spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
			engine_members_write(spa, pv, n, B_TRUE, B_TRUE,
			    errors, &cost);
			spa_config_exit(spa, SCL_ZIO, FTAG);
			for (uint_t c = 0; c < children; c++)
				engine_quorum_add(engine, &q, errors[c]);
			kmem_free(errors, children * sizeof (int));
			error = engine_quorum_result(engine, &q);
		}
		for (uint_t i = 0; i < n; i++)
			abd_free(pv[i].abd);
		kmem_free(pv, MAX(1, engine->pages->capacity) * sizeof (*pv));
	} else if (error == 0) {
		engine_quorum_t q = { 0 };
		spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
		for (uint_t c = 0; c < engine->vd->vdev_children; c++) {
			int member = 0;
			for (uint64_t i = 0; i < engine->pages->capacity; i++) {
				engine_mnode_t *n = engine->pages->cache[i];
				if (!engine_mdirty(b, n))
					continue;
				VERIFY(n->sealed);
				abd_t *abd = abd_get_from_buf(n->data,
				    ENGINE_STORE_PAGE);
				int e = engine_mio(
				    engine->vd->vdev_child[c],
				    engine_mheader(n)->p, abd, B_TRUE);
				abd_free(abd);
				if (e != 0 && (member == 0 || e != ENXIO))
					member = e;
				else if (e == 0 && c == 0)
					engine_pages_fault(3, b->generation,
					    c, i);
			}
			int e = engine_store_flush(engine->vd->vdev_child[c]);
			if (e != 0 && (member == 0 || e != ENXIO))
				member = e;
			else if (e == 0 && c == 0)
				engine_pages_fault(4, b->generation, c,
				    UINT64_MAX);
			/* A member fails as a whole; the quorum decides. */
			engine_quorum_add(engine, &q, member);
		}
		spa_config_exit(spa, SCL_ZIO, FTAG);
		error = engine_quorum_result(engine, &q);
	}
	if (error == 0) {
		if (engine->pages->rows != NULL) {
			uint64_t bytes = MAX(1, engine->pages->count) *
			    sizeof (engine_mrow_t);
			kmem_free(engine->pages->rows, bytes);
			engine->pages->rows = NULL;
			engine->pages->count = 0;
		}
		if (ENGINE_MIRROR && snap->all) {
			engine->pages->rows = rows; rows = NULL;
			engine->pages->count = count;
		} else if (ENGINE_MIRROR) {
			engine->pages->rows = full; full = NULL;
			engine->pages->count = full_count;
		}
		engine->pages->live = live;
		engine->pages->root = tree;
		engine->pages->levels = 0;
		if (tree.p != UINT64_MAX) {
			engine_mnode_t *top;
			if (engine_mget(b, &tree, &top) == 0)
				engine->pages->levels =
				    engine_mheader(top)->level + 1;
		}
		for (uint64_t i = 0; i < engine->pages->capacity; i++) {
			engine_mnode_t *n = engine->pages->cache[i];
			if (engine_mdirty(b, n))
				n->dirty = B_FALSE;
		}
		evicted = engine_pages_evict(engine);
		uint64_t d[16] = { ENGINE_STORE_MAGIC,
		    engine->vd->vdev_guid, engine->arena, engine->arena_bytes,
		    root.p, 1, ub->ub_txg, size, 0, 0, 0, 0,
		    engine->vd->vdev_ashift, engine->vd->vdev_children,
		    engine->total, 7 };
		memcpy(d + 8, &root.hash, sizeof (root.hash));
		memcpy(desc, d, sizeof (d));
		engine_pages_expire_tombstones(engine, expired_keys, expired);
		/* Pages allocated in the arenas, and those held in memory. */
		uint64_t allocated = 0, cached = 0;
		for (uint64_t i = 0; i < engine->pages->capacity; i++) {
			allocated += engine->pages->bits[i] != 0;
			cached += engine->pages->cache[i] != NULL;
		}
		engine->store_cost.stores++;
		engine->store_cost.pages += b->pages;
		engine->store_cost.runs += cost.runs;
		engine->store_cost.tree += reserving - start;
		engine->store_cost.reserve += reserved - reserving;
		engine->store_cost.write += cost.write;
		engine->store_cost.flush += cost.flush;
		pr19262_printf("PR19262_PAGED_SYNC txg=%llu newpages=%u "
		    "changes=%u deletes=%u tombstones=%u expired=%u rows=%u "
		    "root=%llu generation=%llu levels=%llu allocated=%llu "
		    "cached=%llu joins=%u dirty=%u all=%u evicted=%u "
		    "prefetched=%u runs=%llu tree_us=%llu reserve_us=%llu "
		    "write_us=%llu flush_us=%llu\n",
		    (u_longlong_t)ub->ub_txg, b->pages, b->changes, b->deletes,
		    b->tombstones, expired, (uint_t)live, (u_longlong_t)root.p,
		    (u_longlong_t)b->generation,
		    (u_longlong_t)engine->pages->levels,
		    (u_longlong_t)allocated,
		    (u_longlong_t)cached, b->joins, snap->count,
		    (uint_t)snap->all, evicted, prefetched,
		    (u_longlong_t)cost.runs,
		    (u_longlong_t)((reserving - start) / USEC2NSEC(1)),
		    (u_longlong_t)((reserved - reserving) / USEC2NSEC(1)),
		    (u_longlong_t)(cost.write / USEC2NSEC(1)),
		    (u_longlong_t)(cost.flush / USEC2NSEC(1)));
	}
	if (expired_keys != NULL)
		kmem_free(expired_keys, expired * 2 * sizeof (uint64_t));
	if (packed != NULL)
		kmem_free(packed, size);
	if (blobs != NULL)
		kmem_free(blobs, nb * sizeof (*blobs));
	if (rows != NULL)
		kmem_free(rows, MAX(1, count) * sizeof (*rows));
	if (full != NULL)
		kmem_free(full, MAX(1, full_count) * sizeof (*full));
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
	/* Dead version rows: none once deletes replace tombstoning. */
	uint_t dead;
	uint64_t levels;
} engine_mreader_t;

static int
engine_mcollect(engine_mreader_t *r, const engine_mrow_t *row)
{
	engine_t *engine = r->spa->spa_reloc;
	if (row->key[0] != 1 || row->key[1] != row->data[0] ||
	    row->key[2] != row->data[3] || row->data[6] > 3 ||
	    (row->key[3] != 0 && row->key[3] != row->data[15]) ||
	    (r->have_last && engine_mkey_compare(r->last, row->key) >= 0))
		return (ENGINE_EINVAL(16));
	memcpy(r->last, row->key, sizeof (r->last));
	r->have_last = B_TRUE;
	if (row->data[6] == 3) {
		/* Keep released owner identities for journal folding. */
		if (row->key[3] != 0) {
			r->dead++;
			return (0);
		}
		if (r->tomb_count == r->tomb_capacity) {
			uint_t cap = r->tomb_capacity == 0 ? 64 :
			    r->tomb_capacity * 2;
			uint64_t *tombs = pr19262_alloc(cap * 2 *
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
		uint_t cap = MIN(wanted, engine_pages_row_limit(engine));
		if (cap == r->capacity)
			return (EOVERFLOW);
		engine_mrow_t *rows = pr19262_alloc(cap * sizeof (*rows),
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
	engine_t *engine = r->spa->spa_reloc;
	uint64_t index;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	if (ref->generation == 0 || ref->generation > r->generation ||
	    !engine_mindex(engine, ref->p, &index) ||
	    zfs_range_tree_contains(r->seen, ref->p, stripe))
		return (ENGINE_EINVAL(17));
	engine_mnode_t *n;
	int error = engine_mread(r->spa, ref, &n);
	if (error != 0)
		return (error);
	engine_mheader_t *h = engine_mheader(n);
	zfs_range_tree_add(r->seen, ref->p, stripe);
	if (level == UINT64_MAX)
		r->levels = h->level + 1;
	if ((level != UINT64_MAX && level != h->level) ||
	    (h->type != ENGINE_M_LEAF && h->type != ENGINE_M_BRANCH)) {
		error = ENGINE_EINVAL(24);
	} else if (h->type == ENGINE_M_LEAF) {
		engine_mrow_t *rows = engine_mpayload(n);
		if (engine_mkey_compare(rows[0].key, h->low) != 0 ||
		    engine_mkey_compare(rows[h->count - 1].key, h->high) != 0)
			error = ENGINE_EINVAL(25);
		for (uint_t i = 0; error == 0 && i < h->count; i++)
			error = engine_mcollect(r, &rows[i]);
	} else {
		engine_mchild_t *children = engine_mpayload(n);
		uint64_t previous[4] = { 0 };
		for (uint_t i = 0; error == 0 && i < h->count; i++) {
			uint64_t clow[4] = { 0 }, chigh[4] = { 0 };
			if (children[i].ref.generation > h->generation)
				error = ENGINE_EINVAL(26);
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
					error = ENGINE_EINVAL(27);
			}
			memcpy(previous, chigh, sizeof (previous));
		}
		if (error == 0 && engine_mkey_compare(previous, h->high) != 0)
			error = ENGINE_EINVAL(28);
	}
	memcpy(low, h->low, sizeof (h->low));
	memcpy(high, h->high, sizeof (h->high));
	engine_mnode_free(n);
	return (error);
}

static int
engine_mowners(engine_mreader_t *r, nvlist_t *nv)
{
	nvlist_t **owners = pr19262_alloc(MAX(1, r->count) * sizeof (*owners),
	    KM_SLEEP);
	uint_t nr = 0, at = 0;
	int error = 0;
	while (at < r->count && error == 0) {
		uint_t end = at + 1;
		engine_mrow_t *row_cur = &r->rows[at];
		if (row_cur->key[3] != 0) {
			error = ENGINE_EINVAL(29);
			break;
		}
		while (end < r->count &&
		    r->rows[end].key[1] == row_cur->key[1] &&
		    r->rows[end].key[2] == row_cur->key[2])
			end++;
		uint64_t *versions = kmem_alloc((end - at) * 8 *
		    sizeof (uint64_t), KM_SLEEP);
		for (uint_t i = at; i < end; i++) {
			if (memcmp(row_cur->data, r->rows[i].data,
			    8 * sizeof (uint64_t)) != 0 ||
			    (i != at && r->rows[i].data[14] != 1))
				error = ENGINE_EINVAL(30);
			memcpy(versions + (i - at) * 8, r->rows[i].data + 8,
			    8 * sizeof (uint64_t));
		}
		if (error == 0) {
			owners[nr] = fnvlist_alloc();
			fnvlist_add_uint64_array(owners[nr], "V",
			    row_cur->data, 8);
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
	engine_t *engine = spa->spa_reloc;
	if (allocator) {
		int error = engine_pages_manifest(spa);
		if (error != 0)
			return (error);
		/* Pages of roots below the floor may already be reused. */
		if (ub->ub_txg < engine->pages->floor) {
			pr19262_printf("PR19262_FLOOR_REFUSED ub=%llu "
			    "floor=%llu\n", (u_longlong_t)ub->ub_txg,
			    (u_longlong_t)engine->pages->floor);
			return (SET_ERROR(ESTALE));
		}
	}
	const uint64_t *d = ub->ub_pr19262;
	engine_mreader_t *r = kmem_zalloc(sizeof (*r), KM_SLEEP);
	r->spa = spa;
	r->seen = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64, NULL, 0, 0);
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	engine_mref_t ref = { .p = d[4] };
	memcpy(&ref.hash, d + 8, sizeof (ref.hash));
	char *packed = kmem_alloc(d[7], KM_SLEEP);
	uint64_t offset = 0;
	int error = 0;
	while (error == 0 && ref.p != UINT64_MAX) {
		uint64_t index;
		if (!engine_mindex(engine, ref.p, &index) ||
		    zfs_range_tree_contains(r->seen, ref.p, stripe)) {
			error = ENGINE_EINVAL(31);
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
			error = ENGINE_EINVAL(32);
		else {
			zfs_range_tree_add(r->seen, ref.p, stripe);
			memcpy(packed + offset, engine_mpayload(n), h->count);
			offset += h->count;
			memcpy(&ref, h->reserved, sizeof (ref));
			if (ref.p != UINT64_MAX && ref.generation == 0)
				error = ENGINE_EINVAL(33);
		}
		engine_mnode_free(n);
	}
	nvlist_t *nv = NULL;
	if (error == 0 && offset != d[7])
		error = ENGINE_EINVAL(34);
	if (error == 0)
		error = nvlist_unpack(packed, d[7], &nv, KM_SLEEP);
	kmem_free(packed, d[7]);
	uint64_t *tree, generation;
	uint_t nt;
	if (error == 0 && (nvlist_exists(nv, "owners") ||
	    nvlist_lookup_uint64_array(nv, "page-tree", &tree, &nt) != 0 ||
	    nt != 6 || nvlist_lookup_uint64(nv, "page-generation",
	    &generation) != 0 || generation != r->generation))
		error = ENGINE_EINVAL(35);
	if (error == 0) {
		memcpy(&ref, tree, sizeof (ref));
		if (ref.p != UINT64_MAX) {
			uint64_t low[4], high[4];
			error = engine_mwalk(r, &ref, UINT64_MAX, low, high);
		}
	}
	if (error == 0 && allocator)
		pr19262_printf("PR19262_PAGES_READ root=%llu rows=%u "
		    "tombstones=%u dead=%u levels=%llu pages=%llu\n",
		    (u_longlong_t)ub->ub_txg, r->count, r->tomb_count, r->dead,
		    (u_longlong_t)r->levels,
		    (u_longlong_t)(zfs_range_tree_space(r->seen) / stripe));
	uchar_t *bitmap;
	uint_t nb;
	if (error == 0 && (nvlist_lookup_byte_array(nv, "bitmap", &bitmap,
	    &nb) != 0 || nb != ENGINE_STORE_PAGES))
		error = ENGINE_EINVAL(36);
	for (uint_t i = 0; error == 0 && i < ENGINE_STORE_PAGES; i++)
		if (bitmap[i] > 1 ||
		    (zfs_range_tree_contains(r->seen,
		    engine->arena + i * stripe, stripe) && !bitmap[i]))
			error = ENGINE_EINVAL(37);
	if (error == 0 && (!bitmap[0] || !bitmap[1]))
		error = ENGINE_EINVAL(38);
	if (error == 0 && allocator)
		engine->pages->legacy_dead = r->dead;
	if (error == 0 && allocator) {
		uint8_t *reserved = pr19262_zalloc(engine->pages->capacity,
		    KM_SLEEP);
		uint64_t latest = 0;
		uint64_t serial = 0;
		error = engine_pages_reservations(spa, reserved, &latest,
		    &serial);
		if (error == 0 && latest < r->generation)
			error = ECKSUM;
		for (uint64_t i = 0; error == 0 &&
		    i < engine->pages->capacity; i++)
			if (!reserved[i] && zfs_range_tree_contains(r->seen,
			    engine_mphysical(engine, i), stripe))
				error = ECKSUM;
		if (error == 0) {
			engine->pages->serial = MAX(engine->pages->serial,
			    serial);
			/* Control masks are the allocator; roots only read. */
			memcpy(bitmap, reserved, ENGINE_STORE_PAGES);
			memcpy(engine->pages->bits, reserved,
			    engine->pages->capacity);
			engine->pages->free_hint = 0;
			engine_pages_recount_free(engine);
			fnvlist_add_uint64(nv, "page-reservation-generation",
			    latest);
		}
		kmem_free(reserved, engine->pages->capacity);
	}
	if (error == 0 && allocator)
		fnvlist_add_uint64(nv, "page-levels", r->levels);
	if (error == 0)
		error = engine_mowners(r, nv);
	if (error == 0 && allocator)
		error = engine_pages_control_check(spa, B_FALSE);
	if (error == 0) {
		for (engine_marena_t *a = engine->pages->arenas; a != NULL;
		    a = a->next)
			zfs_range_tree_add(r->seen, a->p, 2 * stripe);
		if (engine->pages->directory_pages != NULL)
			zfs_range_tree_walk(engine->pages->directory_pages,
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
	engine_t *engine = spa->spa_reloc;
	for (uint_t attempt = 0; attempt < 3; attempt++) {
		int error = engine_pages_read_once(spa, ub, out, seen,
		    allocator);
		if (error != EAGAIN || !allocator || engine->bitmap != NULL)
			return (error);
		/* Nothing has been admitted to the physical allocator yet. */
		engine_pages_clear(engine);
		pr19262_printf("PR19262_M_CONTROL_RETRY attempt=%u\n",
		    attempt + 1);
	}
	return (EAGAIN);
}

/*
 * Private R02 manifest scratch.  It deliberately does not reuse
 * engine->pages: recovery's page cache and allocator masks are mutable
 * engine state.  These objects are discarded before the caller drops its
 * topology/store locks.
 */
typedef struct engine_r02_arena {
	uint64_t p;
	uint64_t pages;
	uint64_t uuid[2];
	uint8_t *bits;
} engine_r02_arena_t;

typedef struct engine_r02_entry {
	list_node_t node;
	pr19262_r02_manifest_entry_t entry;
	/* Reseed only: the control serial bound by this entry's hash. */
	uint64_t serial;
} engine_r02_entry_t;

typedef struct engine_r02_manifest_private {
	spa_t *spa;
	/* Config-writer mode: the reseed caller owns SCL_ALL. */
	engine_store_leaf_io_mode_t mode;
	/* The reseed collector also binds pending-arena control slots. */
	boolean_t reseed;
	engine_r02_arena_t *arenas;
	uint_t count;
	uint_t capacity;
	uint64_t generation;
	uint64_t reservation_generation;
	engine_mcontrol_t control;
	zfs_range_tree_t *all;
	zfs_range_tree_t *catalog;
	zfs_range_tree_t *tree;
	zfs_range_tree_t *control_pages;
	zfs_range_tree_t *directory;
	/* Journal extents are reserved for collision checks, not allocation. */
	zfs_range_tree_t *journal;
	list_t entries;
	uint64_t roots;
	uint64_t pending;
	uint64_t journal_heads;
	uint64_t journal_binds;
	uint64_t journal_forwards;
	uint64_t journal_pending;
} engine_r02_manifest_private_t;

typedef enum engine_r02_page_kind {
	ENGINE_R02_CATALOG,
	ENGINE_R02_TREE,
	ENGINE_R02_CONTROL,
	ENGINE_R02_DIRECTORY
} engine_r02_page_kind_t;

static int
engine_r02_entry_add(engine_r02_manifest_private_t *m,
    pr19262_r02_manifest_kind_t kind, uint64_t physical, uint64_t pages,
    uint64_t generation, const zio_cksum_t *hash)
{
	for (engine_r02_entry_t *e = list_head(&m->entries); e != NULL;
	    e = list_next(&m->entries, e)) {
		if (e->entry.kind == kind && e->entry.physical == physical &&
		    e->entry.pages == pages)
			return (0);
	}
	engine_r02_entry_t *e = kmem_zalloc(sizeof (*e), KM_SLEEP);
	e->entry.kind = kind;
	e->entry.physical = physical;
	e->entry.pages = pages;
	e->entry.generation = generation;
	if (hash != NULL)
		memcpy(&e->entry.hash, hash, sizeof (e->entry.hash));
	list_insert_tail(&m->entries, e);
	return (0);
}

/*
 * A reseed copies, for each physical control slot, the highest-serial valid
 * copy (the observer keeps its first-valid closure).  Equal serials with
 * different bytes are a conflict.
 */
static int
engine_r02_control_newest(engine_r02_manifest_private_t *m, uint64_t p,
    uint64_t serial, uint64_t generation, const zio_cksum_t *hash)
{
	for (engine_r02_entry_t *e = list_head(&m->entries); e != NULL;
	    e = list_next(&m->entries, e)) {
		if (e->entry.kind != PR19262_R02_MANIFEST_CONTROL ||
		    e->entry.physical != p)
			continue;
		if (e->serial == 0 || serial > e->serial) {
			e->serial = serial;
			e->entry.generation = generation;
			memcpy(&e->entry.hash, hash, sizeof (e->entry.hash));
		} else if (serial == e->serial &&
		    memcmp(&e->entry.hash, hash, sizeof (*hash)) != 0) {
			return (SET_ERROR(ECKSUM));
		}
		return (0);
	}
	return (SET_ERROR(ENOENT));
}

static pr19262_r02_manifest_kind_t
engine_r02_entry_kind(engine_r02_page_kind_t kind)
{
	switch (kind) {
	case ENGINE_R02_CATALOG:
		return (PR19262_R02_MANIFEST_CATALOG);
	case ENGINE_R02_TREE:
		return (PR19262_R02_MANIFEST_TREE);
	case ENGINE_R02_CONTROL:
		return (PR19262_R02_MANIFEST_CONTROL);
	default:
		return (PR19262_R02_MANIFEST_DIRECTORY);
	}
}

static int
engine_r02_extent(engine_t *engine, uint64_t p, uint64_t pages)
{
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	if (stripe == 0 || pages < 3 || pages > ENGINE_M_PAYLOAD ||
	    pages > engine->total / stripe || p % stripe != 0 ||
	    p >= engine->total || pages > (engine->total - p) / stripe)
		return (ENGINE_EINVAL(70));
	return (0);
}

static zfs_range_tree_t *
engine_r02_kind_tree(engine_r02_manifest_private_t *m,
    engine_r02_page_kind_t kind)
{
	switch (kind) {
	case ENGINE_R02_CATALOG:
		return (m->catalog);
	case ENGINE_R02_TREE:
		return (m->tree);
	case ENGINE_R02_CONTROL:
		return (m->control_pages);
	default:
		return (m->directory);
	}
}

static int
engine_r02_mark(engine_r02_manifest_private_t *m,
    engine_r02_page_kind_t kind, uint64_t p)
{
	engine_t *engine = m->spa->spa_reloc;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	zfs_range_tree_t *kind_tree = engine_r02_kind_tree(m, kind);
	if (zfs_range_tree_contains(kind_tree, p, stripe))
		return (0);
	/* A raw page cannot play two typed roles in one validated closure. */
	if (zfs_range_tree_contains(m->journal, p, stripe))
		return (ENGINE_EINVAL(99));
	if (zfs_range_tree_contains(m->all, p, stripe))
		return (ENGINE_EINVAL(71));
	zfs_range_tree_add(kind_tree, p, stripe);
	zfs_range_tree_add(m->all, p, stripe);
	return (0);
}

/* Exactly one raw leaf read, with no engine error accounting. */
static int
engine_r02_read_member(engine_r02_manifest_private_t *m, vdev_t *vd,
    uint64_t p, engine_mnode_t *n)
{
	abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
	int error = engine_store_manifest_read(vd, p, 0, abd, m->mode);
	abd_free(abd);
	return (error);
}

static int
engine_r02_read_ref(engine_r02_manifest_private_t *m,
    const engine_mref_t *ref, engine_r02_page_kind_t kind,
    engine_mnode_t **result)
{
	engine_t *engine = m->spa->spa_reloc;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	if (stripe == 0 || ref->p % stripe != 0 || ref->p >= engine->total ||
	    stripe > engine->total - ref->p)
		return (ENGINE_EINVAL(72));
	engine_mnode_t *n = engine_mnode_alloc();
	int error = ECKSUM;
	abd_t *abd = abd_get_from_buf(n->data, ENGINE_STORE_PAGE);
	for (uint_t c = 0; c < engine->vd->vdev_children; c++) {
		int e = engine_store_manifest_read(engine->vd->vdev_child[c],
		    ref->p, 0, abd, m->mode);
		if (e != 0 || !engine_mpage_accept(m->spa, ref, n))
			continue;
		error = 0;
		break;
	}
	abd_free(abd);
	if (error == 0)
		error = engine_r02_mark(m, kind, ref->p);
	if (error == 0) {
		engine_mheader_t *h = engine_mheader(n);
		error = engine_r02_entry_add(m, engine_r02_entry_kind(kind),
		    ref->p, 1, h->generation, &h->hash);
	}
	if (error != 0) {
		engine_mnode_free(n);
		return (error);
	}
	n->sealed = B_TRUE;
	*result = n;
	return (0);
}

static boolean_t
engine_r02_index(const engine_r02_manifest_private_t *m, uint64_t p,
    uint_t *which, uint64_t *index)
{
	engine_t *engine = m->spa->spa_reloc;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	if (stripe == 0)
		return (B_FALSE);
	for (uint_t i = 0; i < m->count; i++) {
		const engine_r02_arena_t *a = &m->arenas[i];
		if (p < a->p || (p - a->p) % stripe != 0)
			continue;
		uint64_t page = (p - a->p) / stripe;
		if (page < a->pages) {
			*which = i;
			*index = page;
			return (B_TRUE);
		}
	}
	return (B_FALSE);
}

static boolean_t
engine_r02_allocated(const engine_r02_manifest_private_t *m,
    const uchar_t *initial, uint64_t p)
{
	uint_t which;
	uint64_t index;
	if (!engine_r02_index(m, p, &which, &index) ||
	    !m->arenas[which].bits[index])
		return (B_FALSE);
	return (which != 0 || initial == NULL || initial[index] == 1);
}

/*
 * Decode the two allocation slots into caller-owned bits.  This is a local
 * clone of the on-disk validator; unlike recovery it updates no member
 * serial, cache statistic, allocator mask, or error counter.
 */
static int
engine_r02_slots(engine_r02_manifest_private_t *m, uint64_t arena,
    uint64_t pages, const uint64_t uuid[2], uint8_t *bitmap,
    uint64_t *generation, uint64_t *serial_high, engine_mcontrol_t *control)
{
	engine_t *engine = m->spa->spa_reloc;
	uint_t children = engine->vd->vdev_children;
	uint64_t stripe = ENGINE_STORE_PAGE * children;
	uint64_t slots = 2 * children;
	if (children == 0 || stripe == 0 || slots > SIZE_MAX / pages ||
	    slots > SIZE_MAX / sizeof (uint64_t))
		return (ENGINE_EINVAL(73));
	uint64_t *serials = pr19262_zalloc(slots * sizeof (*serials), KM_SLEEP);
	uint8_t *masks = pr19262_zalloc(slots * pages, KM_SLEEP);
	engine_mnode_t *n = engine_mnode_alloc();
	boolean_t found = B_FALSE, conflict = B_FALSE;
	int error = 0;
	for (uint_t slot = 0; error == 0 && slot < 2; slot++) {
		for (uint_t c = 0; error == 0 && c < children; c++) {
			uint64_t p = arena + slot * stripe;
			if (engine_r02_read_member(m, engine->vd->vdev_child[c],
			    p, n) != 0)
				continue;
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
			    h->pool != spa_guid(m->spa) ||
			    h->top != engine->vd->vdev_guid ||
			    h->generation == 0 ||
			    h->level != 0 || h->type != ENGINE_M_BLOB ||
			    h->count != pages || h->low[0] != arena ||
			    h->low[1] != pages * ENGINE_STORE_PAGE ||
			    h->low[2] != children || h->high[0] != pages ||
			    h->p != p || (h->magic == ENGINE_M_ALLOC_MAGIC &&
			    h->high[1] == 0) || !bits[0] || !bits[1])
				continue;
			if ((h->reserved[10] != 0 || h->reserved[11] != 0) &&
			    memcmp(h->reserved + 10, uuid,
			    sizeof (uint64_t) * 2) != 0) {
				error = ECKSUM;
				break;
			}
			boolean_t valid = B_TRUE;
			for (uint_t i = 0; i < pages; i++)
				valid &= bits[i] <= 1;
			if (!valid)
				continue;
			error = engine_r02_mark(m, ENGINE_R02_CONTROL, p);
			if (error != 0)
				break;
			uint64_t serial = h->high[1] == 0 ? h->generation :
			    h->high[1];
			error = engine_r02_entry_add(m,
			    PR19262_R02_MANIFEST_CONTROL, p, 1, h->generation,
			    &h->hash);
			if (error == 0 && m->reseed)
				error = engine_r02_control_newest(m, p, serial,
				    h->generation, &h->hash);
			if (error != 0)
				break;
			uint64_t at = slot * children + c;
			memcpy(masks + at * pages, bits, pages);
			serials[at] = MAX(serial, 1);
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
	uint64_t top = 0;
	for (uint_t i = 0; i < slots; i++)
		top = MAX(top, serials[i]);
	for (uint_t i = 0; i < slots; i++) {
		if (serials[i] == 0 || serials[i] + 1 < top)
			continue;
		for (uint_t j = 0; j < pages; j++)
			bitmap[j] |= masks[i * pages + j];
	}
	engine_mnode_free(n);
	kmem_free(masks, slots * pages);
	kmem_free(serials, slots * sizeof (*serials));
	if (error != 0)
		return (error);
	return (conflict ? ECKSUM : (found ? 0 : ENOENT));
}

static int
engine_r02_add_arena(engine_r02_manifest_private_t *m, uint64_t p,
    uint64_t pages, const uint64_t uuid[2], boolean_t initial)
{
	engine_t *engine = m->spa->spa_reloc;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	int error = engine_r02_extent(engine, p, pages);
	if (error != 0)
		return (error);
	for (uint_t i = 0; i < m->count; i++) {
		engine_r02_arena_t *a = &m->arenas[i];
		if (p < a->p + a->pages * stripe && a->p < p + pages * stripe)
			return (ENGINE_EINVAL(74));
	}
	uint8_t *bits = kmem_zalloc(pages, KM_SLEEP);
	uint64_t generation = 0, serial = 0;
	engine_mcontrol_t *control = initial ? &m->control : NULL;
	error = engine_r02_slots(m, p, pages, uuid, bits, &generation,
	    &serial, control);
	if (error != 0) {
		kmem_free(bits, pages);
		return (error);
	}
	if (initial && (m->control.codec != ENGINE_M_ALLOC_MAGIC ||
	    m->control.serial == 0 || generation == 0)) {
		kmem_free(bits, pages);
		return (ENOTSUP);
	}
	if (m->count == m->capacity) {
		uint64_t wanted = m->capacity == 0 ? 4 :
		    (uint64_t)m->capacity * 2;
		uint64_t limit = MIN(UINT_MAX, engine->total / stripe);
		uint_t capacity = MIN(wanted, limit);
		if (capacity == m->capacity || (capacity != 0 &&
		    SIZE_MAX / capacity < sizeof (*m->arenas))) {
			kmem_free(bits, pages);
			return (EOVERFLOW);
		}
		engine_r02_arena_t *arenas = pr19262_alloc(capacity *
		    sizeof (*arenas), KM_SLEEP);
		if (m->arenas != NULL) {
			memcpy(arenas, m->arenas, m->count * sizeof (*arenas));
			kmem_free(m->arenas, m->capacity * sizeof (*arenas));
		}
		m->arenas = arenas;
		m->capacity = capacity;
	}
	engine_r02_arena_t *a = &m->arenas[m->count++];
	a->p = p;
	a->pages = pages;
	memcpy(a->uuid, uuid, sizeof (a->uuid));
	a->bits = bits;
	m->reservation_generation = MAX(m->reservation_generation, generation);
	error = engine_r02_entry_add(m, PR19262_R02_MANIFEST_ACTIVE_ARENA,
	    p, pages, generation, NULL);
	if (error != 0)
		return (error);
	if (initial)
		m->generation = generation;
	return (0);
}

static int
engine_r02_manifest_open(engine_r02_manifest_private_t *m)
{
	engine_t *engine = m->spa->spa_reloc;
	uint64_t zero[2] = { 0 };
	int error = engine_r02_add_arena(m, engine->arena, ENGINE_STORE_PAGES,
	    zero, B_TRUE);
	if (error != 0)
		return (error);
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	engine_mref_t ref = m->control.directory;
	while (!engine_mref_empty(&ref)) {
		if (ref.generation == 0 || ref.generation > m->generation ||
		    zfs_range_tree_contains(m->directory, ref.p, stripe))
			return (ENGINE_EINVAL(75));
		engine_mnode_t *n;
		error = engine_r02_read_ref(m, &ref, ENGINE_R02_DIRECTORY, &n);
		if (error != 0)
			return (error);
		engine_mheader_t *h = engine_mheader(n);
		uint64_t *data = engine_mpayload(n);
		if (h->type != ENGINE_M_DIRECTORY || data[0] >= ref.p ||
		    ref.p - data[0] != 2 * stripe ||
		    (data[2] == 0 && data[3] == 0)) {
			error = ENGINE_EINVAL(76);
		} else {
			error = engine_r02_add_arena(m, data[0], data[1],
			    data + 2, B_FALSE);
		}
		engine_mref_t next;
		memcpy(&next, data + 4, sizeof (next));
		if (error == 0 && !engine_mref_empty(&next) &&
		    next.generation > ref.generation)
			error = ENGINE_EINVAL(77);
		engine_mnode_free(n);
		if (error != 0)
			return (error);
		ref = next;
	}
	uint64_t empty[4] = { 0 };
	if (memcmp(m->control.pending, empty, sizeof (empty)) != 0) {
		if (m->control.pending[1] == 0 ||
		    (m->control.pending[2] == 0 && m->control.pending[3] == 0))
			return (ENGINE_EINVAL(78));
		error = engine_r02_extent(engine, m->control.pending[0],
		    m->control.pending[1]);
		if (error != 0)
			return (error);
		for (uint_t i = 0; i < m->count; i++) {
			engine_r02_arena_t *a = &m->arenas[i];
			if (m->control.pending[0] < a->p + a->pages * stripe &&
			    a->p < m->control.pending[0] +
			    m->control.pending[1] * stripe)
				return (ENGINE_EINVAL(79));
		}
		m->pending = 1;
		error = engine_r02_entry_add(m,
		    PR19262_R02_MANIFEST_PENDING_ARENA, m->control.pending[0],
		    m->control.pending[1], m->control.serial, NULL);
		if (error != 0)
			return (error);
		/*
		 * A reseed also carries whichever pending-arena allocation slot
		 * already validates for that arena and uuid, so recovery
		 * selects the same pending state on the exact new leaf.  The
		 * read-only observer keeps its original closure and counts.
		 */
		if (m->reseed) {
			uint8_t *bits = kmem_zalloc(m->control.pending[1],
			    KM_SLEEP);
			uint64_t generation = 0, serial = 0;
			error = engine_r02_slots(m, m->control.pending[0],
			    m->control.pending[1], m->control.pending + 2, bits,
			    &generation, &serial, NULL);
			kmem_free(bits, m->control.pending[1]);
			if (error == ENOENT)
				error = 0;
			if (error != 0)
				return (error);
		}
	}
	for (uint_t i = 0; i < m->count; i++)
		if (!m->arenas[i].bits[0] || !m->arenas[i].bits[1])
			return (ENGINE_EINVAL(80));
	return (0);
}

typedef struct engine_r02_tree_reader {
	engine_r02_manifest_private_t *manifest;
	uint64_t generation;
	const uchar_t *bitmap;
	zfs_range_tree_t *seen;
	uint64_t last[4];
	boolean_t have_last;
} engine_r02_tree_reader_t;

typedef struct engine_r02_root_check {
	const engine_r02_manifest_private_t *manifest;
	const uchar_t *bitmap;
	boolean_t bad;
} engine_r02_root_check_t;

static void
engine_r02_root_check_page(void *arg, uint64_t p, uint64_t size)
{
	engine_r02_root_check_t *check = arg;
	engine_t *engine = check->manifest->spa->spa_reloc;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	if (stripe == 0 || size % stripe != 0 || p > UINT64_MAX - size) {
		check->bad = B_TRUE;
		return;
	}
	for (uint64_t at = p, end = p + size; at < end; at += stripe)
		if (!engine_r02_allocated(check->manifest, check->bitmap, at))
			check->bad = B_TRUE;
}

static int
engine_r02_row(engine_r02_tree_reader_t *r, const engine_mrow_t *row)
{
	if (row->key[0] != 1 || row->key[1] != row->data[0] ||
	    row->key[2] != row->data[3] || row->data[6] > 3 ||
	    (row->key[3] != 0 && row->key[3] != row->data[15]) ||
	    (r->have_last && engine_mkey_compare(r->last, row->key) >= 0))
		return (ENGINE_EINVAL(81));
	memcpy(r->last, row->key, sizeof (r->last));
	r->have_last = B_TRUE;
	return (0);
}

static int
engine_r02_walk(engine_r02_tree_reader_t *r, const engine_mref_t *ref,
    uint64_t level, uint64_t low[4], uint64_t high[4])
{
	engine_t *engine = r->manifest->spa->spa_reloc;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	if (ref->generation == 0 || ref->generation > r->generation ||
	    !engine_r02_allocated(r->manifest, r->bitmap, ref->p) ||
	    zfs_range_tree_contains(r->seen, ref->p, stripe))
		return (ENGINE_EINVAL(82));
	engine_mnode_t *n;
	int error = engine_r02_read_ref(r->manifest, ref, ENGINE_R02_TREE, &n);
	if (error != 0)
		return (error);
	engine_mheader_t *h = engine_mheader(n);
	zfs_range_tree_add(r->seen, ref->p, stripe);
	if ((level != UINT64_MAX && level != h->level) ||
	    (h->type != ENGINE_M_LEAF && h->type != ENGINE_M_BRANCH)) {
		error = ENGINE_EINVAL(83);
	} else if (h->type == ENGINE_M_LEAF) {
		engine_mrow_t *rows = engine_mpayload(n);
		if (engine_mkey_compare(rows[0].key, h->low) != 0 ||
		    engine_mkey_compare(rows[h->count - 1].key, h->high) != 0)
			error = ENGINE_EINVAL(84);
		for (uint_t i = 0; error == 0 && i < h->count; i++)
			error = engine_r02_row(r, &rows[i]);
	} else {
		engine_mchild_t *children = engine_mpayload(n);
		uint64_t previous[4] = { 0 };
		for (uint_t i = 0; error == 0 && i < h->count; i++) {
			uint64_t clow[4] = { 0 }, chigh[4] = { 0 };
			if (children[i].ref.generation > h->generation)
				error = ENGINE_EINVAL(85);
			else
				error = engine_r02_walk(r, &children[i].ref,
				    h->level - 1, clow, chigh);
			if (error == 0) {
				boolean_t bad = engine_mkey_compare(chigh,
				    children[i].high) != 0;
				bad |= i == 0 && engine_mkey_compare(clow,
				    h->low) != 0;
				bad |= i != 0 && engine_mkey_compare(previous,
				    clow) >= 0;
				if (bad)
					error = ENGINE_EINVAL(86);
			}
			memcpy(previous, chigh, sizeof (previous));
		}
		if (error == 0 && engine_mkey_compare(previous, h->high) != 0)
			error = ENGINE_EINVAL(87);
	}
	memcpy(low, h->low, sizeof (h->low));
	memcpy(high, h->high, sizeof (h->high));
	engine_mnode_free(n);
	return (error);
}

static int
engine_r02_root(engine_r02_manifest_private_t *m, const uberblock_t *ub,
    void *arg, int (*add)(void *, const uberblock_t *))
{
	engine_t *engine = m->spa->spa_reloc;
	const uint64_t *d = ub->ub_pr19262;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	if (d[15] < 6 || d[7] == 0 || d[7] > ENGINE_STORE_BYTES ||
	    stripe == 0 || d[4] % stripe != 0 || d[4] >= engine->total ||
	    stripe > engine->total - d[4])
		return (ENOTSUP);
	engine_mref_t ref = { .p = d[4] };
	memcpy(&ref.hash, d + 8, sizeof (ref.hash));
	zfs_range_tree_t *seen = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
	    NULL, 0, 0);
	char *packed = kmem_alloc(d[7], KM_SLEEP);
	uint64_t offset = 0, generation = 0;
	int error = 0;
	while (error == 0 && ref.p != UINT64_MAX) {
		if (ref.generation != 0 && ref.generation > generation) {
			error = ENGINE_EINVAL(88);
			break;
		}
		if (zfs_range_tree_contains(seen, ref.p, stripe)) {
			error = ENGINE_EINVAL(89);
			break;
		}
		engine_mnode_t *n;
		error = engine_r02_read_ref(m, &ref, ENGINE_R02_CATALOG, &n);
		if (error != 0)
			break;
		engine_mheader_t *h = engine_mheader(n);
		if (offset == 0)
			generation = h->generation;
		if (h->type != ENGINE_M_BLOB || h->generation != generation ||
		    h->low[0] != offset || h->high[0] != d[7] ||
		    offset > d[7] || h->count > d[7] - offset ||
		    !engine_r02_allocated(m, NULL, ref.p)) {
			error = ENGINE_EINVAL(90);
		} else {
			memcpy(packed + offset, engine_mpayload(n), h->count);
			offset += h->count;
			zfs_range_tree_add(seen, ref.p, stripe);
			memcpy(&ref, h->reserved, sizeof (ref));
			if (ref.p != UINT64_MAX && ref.generation == 0)
				error = ENGINE_EINVAL(91);
		}
		engine_mnode_free(n);
	}
	nvlist_t *nv = NULL;
	if (error == 0 && offset != d[7])
		error = ENGINE_EINVAL(92);
	if (error == 0)
		error = nvlist_unpack(packed, d[7], &nv, KM_SLEEP);
	kmem_free(packed, d[7]);
	uint64_t *tree, generation_nv;
	uchar_t *bitmap;
	uint_t nt, nb;
	if (error == 0 && (nvlist_exists(nv, "owners") ||
	    nvlist_lookup_uint64_array(nv, "page-tree", &tree, &nt) != 0 ||
	    nt != 6 || nvlist_lookup_uint64(nv, "page-generation",
	    &generation_nv) != 0 || generation_nv != generation ||
	    nvlist_lookup_byte_array(nv, "bitmap", &bitmap, &nb) != 0 ||
	    nb != ENGINE_STORE_PAGES))
		error = ENGINE_EINVAL(93);
	for (uint_t i = 0; error == 0 && i < ENGINE_STORE_PAGES; i++)
		if (bitmap[i] > 1)
			error = ENGINE_EINVAL(94);
	if (error == 0 && (!bitmap[0] || !bitmap[1]))
		error = ENGINE_EINVAL(95);
	if (error == 0 && m->reservation_generation < generation)
		error = ECKSUM;
	if (error == 0) {
		engine_r02_root_check_t check = {
			.manifest = m, .bitmap = bitmap
		};
		zfs_range_tree_walk(seen, engine_r02_root_check_page, &check);
		if (check.bad)
			error = ENGINE_EINVAL(99);
	}
	engine_mref_t tree_ref;
	if (error == 0)
		memcpy(&tree_ref, tree, sizeof (tree_ref));
	if (error == 0 && tree_ref.p != UINT64_MAX) {
		engine_r02_tree_reader_t reader = {
			.manifest = m, .generation = generation,
			.bitmap = bitmap,
			.seen = zfs_range_tree_create(NULL, ZFS_RANGE_SEG64,
			    NULL, 0, 0)
		};
		uint64_t low[4], high[4];
		error = engine_r02_walk(&reader, &tree_ref, UINT64_MAX, low,
		    high);
		zfs_range_tree_vacate(reader.seen, NULL, NULL);
		zfs_range_tree_destroy(reader.seen);
	}
	uint64_t txg, pool, top, *mos, *checkpoint;
	uint_t nm, nc;
	if (error == 0 && (nvlist_lookup_uint64(nv, "txg", &txg) != 0 ||
	    nvlist_lookup_uint64(nv, "pool", &pool) != 0 ||
	    nvlist_lookup_uint64(nv, "top", &top) != 0 ||
	    nvlist_lookup_uint64_array(nv, "mos", &mos, &nm) != 0 ||
	    nvlist_lookup_uint64_array(nv, "checkpoint", &checkpoint, &nc) !=
	    0 || nm != sizeof (blkptr_t) / sizeof (uint64_t) ||
	    nc != sizeof (uberblock_t) / sizeof (uint64_t) || txg != d[6] ||
	    pool != spa_guid(m->spa) || top != d[1] ||
	    memcmp(mos, &ub->ub_rootbp, sizeof (blkptr_t)) != 0))
		error = ENGINE_EINVAL(96);
	if (error == 0) {
		uberblock_t pin;
		memcpy(&pin, checkpoint, sizeof (pin));
		if (pin.ub_txg != 0) {
			if (uberblock_verify(&pin) != 0 || pin.ub_txg > txg ||
			    pin.ub_checkpoint_txg != pin.ub_txg ||
			    pin.ub_pr19262[6] >= d[6])
				error = ENGINE_EINVAL(97);
			else if (!engine_native_checkpoint(&pin))
				error = add(arg, &pin);
		}
	}
	if (error == 0)
		error = engine_r02_entry_add(m, PR19262_R02_MANIFEST_ROOT,
		    d[4], d[5], d[6], (const zio_cksum_t *)(d + 8));
	if (error == 0)
		m->roots++;
	nvlist_free(nv);
	zfs_range_tree_vacate(seen, NULL, NULL);
	zfs_range_tree_destroy(seen);
	return (error);
}

typedef struct engine_r02_check {
	const engine_r02_manifest_private_t *manifest;
	boolean_t bad;
} engine_r02_check_t;

static void
engine_r02_check_page(void *arg, uint64_t p, uint64_t size)
{
	engine_r02_check_t *check = arg;
	engine_t *engine = check->manifest->spa->spa_reloc;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	if (stripe == 0 || size % stripe != 0 || p > UINT64_MAX - size) {
		check->bad = B_TRUE;
		return;
	}
	for (uint64_t at = p, end = p + size; at < end; at += stripe)
		if (!engine_r02_allocated(check->manifest, NULL, at))
			check->bad = B_TRUE;
}

static int
engine_r02_manifest_check(engine_r02_manifest_private_t *m)
{
	engine_r02_check_t check = { .manifest = m };
	zfs_range_tree_walk(m->all, engine_r02_check_page, &check);
	return (check.bad ? ENGINE_EINVAL(98) : 0);
}

static void
engine_r02_manifest_counts(const engine_r02_manifest_private_t *m,
    pr19262_r02_manifest_t *out)
{
	engine_t *engine = m->spa->spa_reloc;
	uint64_t stripe = ENGINE_STORE_PAGE * engine->vd->vdev_children;
	memset(out, 0, sizeof (*out));
	out->roots = m->roots;
	out->catalog_pages = zfs_range_tree_space(m->catalog) / stripe;
	out->tree_pages = zfs_range_tree_space(m->tree) / stripe;
	out->control_pages = zfs_range_tree_space(m->control_pages) / stripe;
	out->directory_pages = zfs_range_tree_space(m->directory) / stripe;
	out->active_arenas = m->count;
	out->pending_arenas = m->pending;
	out->journal_heads = m->journal_heads;
	out->journal_binds = m->journal_binds;
	out->journal_forwards = m->journal_forwards;
	out->journal_pending = m->journal_pending;
}

static void
engine_r02_manifest_fini(engine_r02_manifest_private_t *m)
{
	for (uint_t i = 0; i < m->count; i++)
		kmem_free(m->arenas[i].bits, m->arenas[i].pages);
	if (m->arenas != NULL)
		kmem_free(m->arenas, m->capacity * sizeof (*m->arenas));
	engine_r02_entry_t *entry;
	while ((entry = list_remove_head(&m->entries)) != NULL)
		kmem_free(entry, sizeof (*entry));
	list_destroy(&m->entries);
	zfs_range_tree_vacate(m->all, NULL, NULL);
	zfs_range_tree_destroy(m->all);
	zfs_range_tree_vacate(m->catalog, NULL, NULL);
	zfs_range_tree_destroy(m->catalog);
	zfs_range_tree_vacate(m->tree, NULL, NULL);
	zfs_range_tree_destroy(m->tree);
	zfs_range_tree_vacate(m->control_pages, NULL, NULL);
	zfs_range_tree_destroy(m->control_pages);
	zfs_range_tree_vacate(m->directory, NULL, NULL);
	zfs_range_tree_destroy(m->directory);
	zfs_range_tree_vacate(m->journal, NULL, NULL);
	zfs_range_tree_destroy(m->journal);
}

static int
engine_pages_install(engine_t *engine, nvlist_t *nv)
{
	uint64_t *tree, generation;
	uint_t nt, count;
	if (nvlist_lookup_uint64_array(nv, "page-tree", &tree, &nt) != 0 ||
	    nt != 6 || nvlist_lookup_uint64(nv, "page-generation",
	    &generation) != 0)
		return (ENGINE_EINVAL(18));
	engine_mrow_t *rows;
	int error = engine_mrows(engine, nv, &rows, &count);
	if (error != 0)
		return (error);
	engine_pages_tree_clear(engine);
	memcpy(engine->pages->bits, engine->bitmap, ENGINE_STORE_PAGES);
	engine->pages->free_hint = 0;
	engine_pages_recount_free(engine);
	memcpy(&engine->pages->root, tree, sizeof (engine->pages->root));
	engine->pages->generation = generation;
	uint64_t reserved;
	if (nvlist_lookup_uint64(nv, "page-reservation-generation",
	    &reserved) == 0)
		engine->pages->generation = MAX(generation, reserved);
	engine->pages->live = count;
	if (ENGINE_MIRROR) {
		engine->pages->rows = rows; engine->pages->count = count;
	} else {
		kmem_free(rows, MAX(1, count) * sizeof (*rows));
	}
	uint64_t levels;
	if (nvlist_lookup_uint64(nv, "page-levels", &levels) == 0)
		engine->pages->levels = levels;
	return (0);
}

_Static_assert(sizeof (engine_mheader_t) == 256, "private page header size");
_Static_assert(sizeof (engine_mref_t) == 48, "private page reference size");
_Static_assert(sizeof (engine_mrow_t) == 160, "private page record size");
