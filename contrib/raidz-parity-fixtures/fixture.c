// SPDX-License-Identifier: CDDL-1.0
/* Neutral libzpool input: no migration, mapping or epoch-table calls. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/sha2.h>
#include <sys/spa_impl.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_tx.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_destroy.h>
#include <sys/txg.h>
#include <sys/vdev_impl.h>
#include <sys/fs/zfs.h>
#include <libzpool.h>
#include <libzutil.h>

#define	POOL "jointfixture"
#define	WIDTH 5
#define	MEMBER_SIZE (256ULL << 20)
#define	SEED 0x726169647a323032ULL

static struct {
	uint64_t record;
	uint64_t retained;
	uint64_t head;
	uint64_t robj;
	uint64_t hobj;
	unsigned int dropped;
} spec;
static const char *dir;

static void
path_for(char *buf, size_t size, const char *name)
{
	int n = snprintf(buf, size, "%s/%s", dir, name);
	VERIFY3S(n, >, 0);
	VERIFY3U(n, <, size);
}

static void
pattern(uint8_t *buf, uint64_t off, uint64_t len, uint64_t tag)
{
	for (uint64_t i = 0; i < len; i += 8) {
		uint64_t x = (SEED ^ tag) +
		    ((off + i) / 8) * 0x9e3779b97f4a7c15ULL;

		x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
		x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
		x ^= x >> 31;
		for (unsigned int j = 0; j < 8; j++)
			buf[i + j] = x >> (j * 8);
	}
}

static void
save_spec(void)
{
	char path[4096];
	FILE *f;

	path_for(path, sizeof (path), "objects.txt");
	f = fopen(path, "w");
	VERIFY3P(f, !=, NULL);
	VERIFY3S(fprintf(f, "1 %llu %llu %llu %llu %llu %u\n",
	    (u_longlong_t)spec.record, (u_longlong_t)spec.retained,
	    (u_longlong_t)spec.head, (u_longlong_t)spec.robj,
	    (u_longlong_t)spec.hobj, spec.dropped), >, 0);
	VERIFY0(fflush(f));
	VERIFY0(fsync(fileno(f)));
	VERIFY0(fclose(f));
}

static void
load_spec(void)
{
	char path[4096];
	FILE *f;
	unsigned int version;
	u_longlong_t r, s, h, ro, ho;

	path_for(path, sizeof (path), "objects.txt");
	f = fopen(path, "r");
	VERIFY3P(f, !=, NULL);
	VERIFY3S(fscanf(f, "%u %llu %llu %llu %llu %llu %u",
	    &version, &r, &s, &h, &ro, &ho, &spec.dropped), ==, 7);
	VERIFY3U(version, ==, 1);
	spec.record = r;
	spec.retained = s;
	spec.head = h;
	spec.robj = ro;
	spec.hobj = ho;
	VERIFY(spec.record == 16384 || spec.record == 131072);
	VERIFY3U(spec.retained + spec.head, <=, 128ULL << 20);
	VERIFY0(fclose(f));
}

static void
create_pool(void)
{
	nvlist_t *child[WIDTH];
	nvlist_t *raid = fnvlist_alloc();
	nvlist_t *root = fnvlist_alloc();
	nvlist_t *props = fnvlist_alloc();
	char name[32];
	char path[4096];

	for (int i = 0; i < WIDTH; i++) {
		int fd;

		(void) snprintf(name, sizeof (name), "v%d", i);
		path_for(path, sizeof (path), name);
		fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
		VERIFY3S(fd, >=, 0);
		VERIFY0(ftruncate(fd, MEMBER_SIZE));
		VERIFY0(close(fd));
		child[i] = fnvlist_alloc();
		fnvlist_add_string(child[i], ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
		fnvlist_add_string(child[i], ZPOOL_CONFIG_PATH, path);
		fnvlist_add_uint64(child[i], ZPOOL_CONFIG_ASHIFT, 12);
	}
	fnvlist_add_string(raid, ZPOOL_CONFIG_TYPE, VDEV_TYPE_RAIDZ);
	fnvlist_add_uint64(raid, ZPOOL_CONFIG_NPARITY, 1);
	fnvlist_add_nvlist_array(raid, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)child, WIDTH);
	fnvlist_add_string(root, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(root, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)&raid, 1);
	/* Same prerequisite on both prototypes, no epoch table yet. */
	fnvlist_add_uint64(props, "feature@raidz_parity_epochs", 0);
	fnvlist_add_string(props, "cachefile", "none");
	VERIFY0(spa_create(POOL, root, props, NULL, NULL, NULL));
	fnvlist_free(props);
	fnvlist_free(root);
	fnvlist_free(raid);
	for (int i = 0; i < WIDTH; i++)
		fnvlist_free(child[i]);
}

static spa_t *
import_pool(void)
{
	importargs_t ia = { 0 };
	char *paths[1] = { (char *)dir };
	nvlist_t *config = NULL;
	spa_t *spa;
	libpc_handle_t handle = {
		.lpc_ops = &libzpool_config_ops,
		.lpc_printerr = B_TRUE
	};

	ia.path = paths;
	ia.paths = 1;
	ia.can_be_active = B_TRUE;
	VERIFY0(zpool_find_config(&handle, POOL, &config, &ia));
	VERIFY0(spa_import((char *)POOL, config, NULL, ZFS_IMPORT_SKIP_MMP));
	fnvlist_free(config);
	VERIFY0(spa_open(POOL, &spa, FTAG));
	return (spa);
}

static uint64_t
write_object(objset_t *os, uint64_t bytes, uint64_t tag)
{
	dmu_tx_t *tx = dmu_tx_create(os);
	uint64_t object;
	uint8_t *buf = malloc(spec.record);

	VERIFY3P(buf, !=, NULL);
	dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	object = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS,
	    spec.record, DMU_OT_NONE, 0, tx);
	dmu_tx_commit(tx);
	for (uint64_t off = 0; off < bytes; off += spec.record) {
		pattern(buf, off, spec.record, tag);
		tx = dmu_tx_create(os);
		dmu_tx_hold_write(tx, object, off, spec.record);
		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
		dmu_write(os, object, off, spec.record, buf, tx,
		    DMU_READ_NO_PREFETCH);
		dmu_tx_commit(tx);
	}
	free(buf);
	return (object);
}

static void
verify_object(const char *dataset, uint64_t object, uint64_t bytes,
    uint64_t tag, boolean_t hole)
{
	objset_t *os;
	uint8_t *expected = calloc(1, spec.record);
	uint8_t *actual = malloc(spec.record);
	SHA2_CTX ctx;
	uint8_t digest[SHA256_DIGEST_LENGTH];
	char hex[SHA256_DIGEST_LENGTH * 2 + 1];

	VERIFY3P(expected, !=, NULL);
	VERIFY3P(actual, !=, NULL);
	VERIFY0(dmu_objset_own(dataset, DMU_OST_ANY, B_TRUE, B_TRUE,
	    FTAG, &os));
	SHA2Init(SHA256, &ctx);
	for (uint64_t off = 0; off < bytes; off += spec.record) {
		if (!hole)
			pattern(expected, off, spec.record, tag);
		VERIFY0(dmu_read(os, object, off, spec.record, actual,
		    DMU_READ_NO_PREFETCH));
		VERIFY0(memcmp(actual, expected, spec.record));
		SHA2Update(&ctx, actual, spec.record);
	}
	SHA2Final(digest, &ctx);
	for (unsigned int i = 0; i < sizeof (digest); i++)
		(void) snprintf(hex + i * 2, 3, "%02x", digest[i]);
	dmu_objset_disown(os, B_TRUE, FTAG);
	free(expected);
	free(actual);
	(void) printf("VERIFY dataset=%s object=%llu bytes=%llu "
	    "hole=%u sha256=%s\n", dataset, (u_longlong_t)object,
	    (u_longlong_t)bytes, hole, hex);
}

static void
populate(spa_t *spa)
{
	objset_t *os;

	VERIFY0(dsl_prop_set_int(POOL, "compression", ZPROP_SRC_LOCAL,
	    ZIO_COMPRESS_OFF));
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE,
	    FTAG, &os));
	if (spec.retained != 0)
		spec.robj = write_object(os, spec.retained, 1);
	txg_wait_synced(spa_get_dsl(spa), 0);
	dmu_objset_disown(os, B_TRUE, FTAG);
	if (spec.retained != 0) {
		VERIFY0(dmu_objset_snapshot_one(POOL, "retained"));
	}
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE,
	    FTAG, &os));
	if (spec.retained != 0)
		VERIFY0(dmu_free_long_range(os, spec.robj, 0,
		    DMU_OBJECT_END));
	if (spec.head != 0)
		spec.hobj = write_object(os, spec.head, 2);
	dmu_objset_disown(os, B_TRUE, FTAG);
	txg_wait_synced(spa_get_dsl(spa), 0);
}

int
main(int argc, char **argv)
{
	spa_t *spa;
	char cache[4096];
	boolean_t create;
	boolean_t drop;

	if (argc < 3 || (strcmp(argv[1], "create") != 0 &&
	    strcmp(argv[1], "verify") != 0 &&
	    strcmp(argv[1], "drop-retained") != 0)) {
		(void) fprintf(stderr, "usage: %s create DIR RECORD_KIB "
		    "RETAINED_PCT UNITS_128K | verify DIR | "
		    "drop-retained DIR\n",
		    argv[0]);
		return (2);
	}
	create = strcmp(argv[1], "create") == 0;
	drop = strcmp(argv[1], "drop-retained") == 0;
	dir = argv[2];
	if (dir[0] != '/' || strlen(dir) > 3900)
		return (2);
	if (create) {
		uint64_t pct;
		uint64_t total;
		char *end;

		if (argc != 6)
			return (2);
		spec.record = strtoull(argv[3], &end, 10) << 10;
		if (*end != '\0' || (spec.record != 16384 &&
		    spec.record != 131072))
			return (2);
		pct = strtoull(argv[4], &end, 10);
		if (*end != '\0' || (pct != 0 && pct != 10 && pct != 30 &&
		    pct != 100))
			return (2);
		total = strtoull(argv[5], &end, 10);
		if (*end != '\0' || total == 0 || total > 1024)
			return (2);
		total *= 128ULL << 10;
		if ((total * pct) % (100 * spec.record) != 0)
			return (2);
		spec.retained = total * pct / 100;
		spec.head = total - spec.retained;
	} else {
		if (argc != 3)
			return (2);
		load_spec();
	}
	path_for(cache, sizeof (cache), "zpool.cache");
	spa_config_path = cache;
	kernel_init(create || drop ? SPA_MODE_READ | SPA_MODE_WRITE :
	    SPA_MODE_READ);
	if (create) {
		create_pool();
		VERIFY0(spa_open(POOL, &spa, FTAG));
		populate(spa);
	} else {
		spa = import_pool();
	}
	if (drop && spec.retained != 0 && !spec.dropped) {
		VERIFY0(dsl_destroy_snapshot(POOL "@retained", B_FALSE));
		spec.dropped = 1;
		txg_wait_synced(spa_get_dsl(spa), 0);
	}
	if (spec.head != 0)
		verify_object(POOL, spec.hobj, spec.head, 2, B_FALSE);
	if (spec.retained != 0) {
		verify_object(POOL, spec.robj, spec.retained, 1, B_TRUE);
		if (!spec.dropped)
			verify_object(POOL "@retained", spec.robj,
			    spec.retained, 1, B_FALSE);
	}
	(void) printf("FIXTURE record=%llu retained=%llu head=%llu "
	    "native_alloc=%llu dropped=%u PASS\n",
	    (u_longlong_t)spec.record, (u_longlong_t)spec.retained,
	    (u_longlong_t)spec.head,
	    (u_longlong_t)spa->spa_root_vdev->vdev_stat.vs_alloc,
	    spec.dropped);
	spa_close(spa, FTAG);
	if (create || drop) {
		VERIFY0(spa_export(POOL, NULL, B_FALSE, B_FALSE));
		save_spec();
	}
	kernel_fini();
	return (0);
}
