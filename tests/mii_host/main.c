// Host driver for sdk/mii/mii.c's cores (tests/test_mii.py). Miis, IDs and
// text are hex on the command line (Windows hands argv over in its own code
// page). Each command prints one line per result.
//   mii_host fields MII          every field as name=value, then pack=MII again
//   mii_host check MII           the result code and why
//   mii_host default SEX         a Mii
//   mii_host random SEX AGE SEED N   N Miis, each with its check result
//   mii_host name UTF8           units kept, and the name read back as UTF-8
//   mii_host id TICKS MAC        mii_id_make, then flags, time, home word
//   mii_host store MII           the 76-byte form
//   mii_host import DATA         a 74- or 76-byte file's result and Mii
//   mii_host db FILE OP...       RFL_DB.dat ops, written back if all attach:
//     get S | put S MII | add MII | rm S | find ID | count | iso | home WORD
//     hput MII | hrm I | hget I | hlist | hfind ID
//   mii_host remote IN OUT OP... a Remote's two copies: get S | put S MII |
//     rm S | secret; OUT gets both copies of the built block

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mii.h"

static size_t unhex(const char *s, uint8_t *out, size_t max) {
	size_t n = 0;
	unsigned v;

	while (s[0] && s[1] && n < max && sscanf(s, "%2x", &v) == 1) {
		out[n++] = (uint8_t) v;
		s += 2;
	}
	return n;
}

static void hex(const uint8_t *p, size_t n) {
	while (n--)
		printf("%02x", *p++);
}

static void mii_arg(const char *s, mii *m) {
	uint8_t raw[MII_SIZE];

	if (unhex(s, raw, sizeof(raw)) != MII_SIZE) {
		fprintf(stderr, "a Mii is %d bytes\n", MII_SIZE);
		exit(2);
	}
	mii_unpack(raw, m);
}

static void print_mii(const mii *m) {
	uint8_t raw[MII_SIZE];

	mii_pack(m, raw);
	hex(raw, sizeof(raw));
}

#define P(f) printf(#f "=%u\n", (unsigned) m.f)

static int cmd_fields(const char *arg) {
	mii m;
	int i;

	mii_arg(arg, &m);
	P(sex); P(birth_month); P(birth_day); P(fav_color); P(favorite); P(height); P(build);
	P(face_type); P(face_color); P(face_tex); P(local_only); P(kind);
	P(hair_type); P(hair_color); P(hair_flip);
	P(brow_type); P(brow_rotate); P(brow_color); P(brow_scale); P(brow_y); P(brow_x);
	P(eye_type); P(eye_rotate); P(eye_y); P(eye_color); P(eye_scale); P(eye_x);
	P(nose_type); P(nose_scale); P(nose_y);
	P(mouth_type); P(mouth_color); P(mouth_scale); P(mouth_y);
	P(glass_type); P(glass_color); P(glass_scale); P(glass_y);
	P(mustache); P(beard_type); P(beard_color); P(beard_scale); P(beard_y);
	P(mole); P(mole_scale); P(mole_y); P(mole_x);
	printf("name=");
	for (i = 0; i < MII_NAME_MAX; ++i)
		printf("%04x", m.name[i]);
	printf("\ncreator=");
	for (i = 0; i < MII_NAME_MAX; ++i)
		printf("%04x", m.creator[i]);
	printf("\nid=");
	hex(m.id, MII_ID_SIZE);
	printf("\npack=");
	print_mii(&m);
	printf("\n");
	return 0;
}

static void print_check(const mii *m) {
	const char *why = NULL;
	int r = mii_check(m, &why);

	printf("%d %s", r, why ? why : "-");
}

static uint8_t *read_file(const char *path, size_t *size) {
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(n ? (size_t) n : 1);
	if (fread(buf, 1, (size_t) n, f) != (size_t) n) {
		fclose(f);
		free(buf);
		return NULL;
	}
	fclose(f);
	*size = (size_t) n;
	return buf;
}

static int write_file(const char *path, const uint8_t *buf, size_t size) {
	FILE *f = fopen(path, "wb");
	int ok = f && fwrite(buf, 1, size, f) == size;

	if (f)
		fclose(f);
	return ok ? 0 : 1;
}

static int cmd_db(int argc, char **argv) {
	mii_db db;
	size_t size = 0;
	uint8_t *file = read_file(argv[0], &size);
	int i, r;

	if (!file)
		return 2;
	r = mii_db_attach(&db, file, size);
	printf("attach %d\n", r);
	if (r < 0)
		return 0;
	for (i = 1; i < argc; ++i) {
		const char *op = argv[i];
		mii m;
		uint8_t id[MII_ID_SIZE];

		if (!strcmp(op, "get") || !strcmp(op, "hget")) {
			int n = atoi(argv[++i]);
			r = op[0] == 'h' ? mii_hidden_get(&db, n, &m) : mii_db_get(&db, n, &m);
			printf("%s %d ", op, r);
			if (r >= 0)
				print_mii(&m);
			printf("\n");
		} else if (!strcmp(op, "put")) {
			int n = atoi(argv[++i]);
			mii_arg(argv[++i], &m);
			printf("put %d\n", mii_db_put(&db, n, &m));
		} else if (!strcmp(op, "add")) {
			mii_arg(argv[++i], &m);
			printf("add %d\n", mii_db_add(&db, &m));
		} else if (!strcmp(op, "hput")) {
			mii_arg(argv[++i], &m);
			printf("hput %d\n", mii_hidden_put(&db, &m));
		} else if (!strcmp(op, "rm")) {
			printf("rm %d\n", mii_db_remove(&db, atoi(argv[++i])));
		} else if (!strcmp(op, "hrm")) {
			printf("hrm %d\n", mii_hidden_remove(&db, atoi(argv[++i])));
		} else if (!strcmp(op, "find") || !strcmp(op, "hfind")) {
			unhex(argv[++i], id, sizeof(id));
			printf("%s %d\n", op, op[0] == 'h' ? mii_hidden_find(&db, id) : mii_db_find(&db, id));
		} else if (!strcmp(op, "count")) {
			printf("count %d %d\n", mii_db_count(&db), mii_hidden_count(&db));
		} else if (!strcmp(op, "iso")) {
			printf("iso %d\n", mii_db_isolated(&db));
		} else if (!strcmp(op, "home")) {
			db.home = (uint32_t) strtoul(argv[++i], NULL, 16);
			printf("home %08x\n", (unsigned) db.home);
		} else if (!strcmp(op, "hlist")) {
			int k;
			printf("hlist");
			for (k = mii_hidden_first(&db); k >= 0; k = mii_hidden_next(&db, k))
				printf(" %d", k);
			printf("\n");
		} else {
			fprintf(stderr, "unknown op %s\n", op);
			return 2;
		}
	}
	printf("dirty %u %u\n", (unsigned) db.dirty_lo, (unsigned) db.dirty_hi);
	return write_file(argv[0], file, size);
}

static int cmd_remote(int argc, char **argv) {
	mii_remote rm;
	uint8_t block[MII_REMOTE_BLOCK], both[2 * MII_REMOTE_BLOCK];
	size_t size = 0;
	uint8_t *mem = read_file(argv[0], &size);
	int i, r;

	if (!mem)
		return 2;
	r = mii_remote_parse(mem, size, &rm);
	printf("parse %d copy %d secret %03x\n", r, rm.copy, rm.secret);
	for (i = 2; i < argc; ++i) {
		const char *op = argv[i];
		mii m;

		if (!strcmp(op, "get")) {
			int n = atoi(argv[++i]);
			r = mii_remote_get(&rm, n, &m);
			printf("get %d ", r);
			if (r >= 0)
				print_mii(&m);
			printf("\n");
		} else if (!strcmp(op, "put")) {
			int n = atoi(argv[++i]);
			mii_arg(argv[++i], &m);
			printf("put %d\n", mii_remote_put(&rm, n, &m));
		} else if (!strcmp(op, "rm")) {
			printf("rm %d\n", mii_remote_remove(&rm, atoi(argv[++i])));
		} else if (!strcmp(op, "secret")) {
			rm.secret = (uint16_t) strtoul(argv[++i], NULL, 16);
		} else {
			fprintf(stderr, "unknown op %s\n", op);
			return 2;
		}
	}
	mii_remote_build(&rm, block);
	memcpy(both, block, sizeof(block));
	memcpy(both + MII_REMOTE_BLOCK, block, sizeof(block));
	return write_file(argv[1], both, sizeof(both));
}

int main(int argc, char **argv) {
	const char *cmd = argc > 1 ? argv[1] : "";
	mii m;

	if (!strcmp(cmd, "fields") && argc == 3)
		return cmd_fields(argv[2]);
	if (!strcmp(cmd, "check") && argc == 3) {
		mii_arg(argv[2], &m);
		print_check(&m);
		printf("\n");
		return 0;
	}
	if (!strcmp(cmd, "default") && argc == 3) {
		mii_default(&m, atoi(argv[2]));
		print_mii(&m);
		printf("\n");
		return 0;
	}
	if (!strcmp(cmd, "random") && argc == 6) {
		uint32_t seed = (uint32_t) strtoul(argv[4], NULL, 0);
		int n = atoi(argv[5]), k;

		for (k = 0; k < n; ++k) {
			mii_random(&m, atoi(argv[2]), atoi(argv[3]), &seed);
			m.id[0] = 0x80;          /* any ID, so the check looks at the parts */
			m.id[7] = 1;
			print_mii(&m);
			printf(" ");
			print_check(&m);
			printf("\n");
		}
		return 0;
	}
	if (!strcmp(cmd, "name") && argc == 3) {
		uint8_t in[256];
		char back[64];
		size_t n = unhex(argv[2], in, sizeof(in) - 1);
		int kept;

		in[n] = 0;
		kept = mii_set_name(m.name, (const char *) in);
		mii_get_name(m.name, back, sizeof(back));
		printf("%d ", kept);
		hex((const uint8_t *) back, strlen(back));
		printf("\n");
		return 0;
	}
	if (!strcmp(cmd, "id") && argc == 4) {
		uint8_t mac[6], id[MII_ID_SIZE];

		unhex(argv[3], mac, sizeof(mac));
		mii_id_make(id, strtoull(argv[2], NULL, 0), mac);
		hex(id, sizeof(id));
		printf(" %08x %u %08x\n", (unsigned) mii_id_flags(id), (unsigned) mii_id_time(id),
			   (unsigned) mii_id_home(mac));
		return 0;
	}
	if (!strcmp(cmd, "store") && argc == 3) {
		uint8_t raw[MII_SIZE], out[MII_STORE_SIZE];

		unhex(argv[2], raw, sizeof(raw));
		mii_store(raw, out);
		hex(out, sizeof(out));
		printf("\n");
		return 0;
	}
	if (!strcmp(cmd, "import") && argc == 3) {
		uint8_t data[128];
		size_t n = unhex(argv[2], data, sizeof(data));
		int r = mii_import(data, n, &m);

		printf("%d ", r);
		if (r >= 0)
			print_mii(&m);
		printf("\n");
		return 0;
	}
	if (!strcmp(cmd, "db") && argc >= 3)
		return cmd_db(argc - 2, argv + 2);
	if (!strcmp(cmd, "remote") && argc >= 4)
		return cmd_remote(argc - 2, argv + 2);
	fprintf(stderr, "usage: see the top of tests/mii_host/main.c\n");
	return 2;
}
