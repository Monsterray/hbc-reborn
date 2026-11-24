// Memory benchmark for HBC's transfer paths, printed over the network log.
// Measures memcpy, CRC-32 and zlib with data and zlib state in MEM1 or MEM2,
// and CRC-32 streamed through the locked cache.
// usage (argv from Wiiload): membench.dol sd:/hbcbench/sample.bin

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ogcsys.h>
#include <ogc/cache.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>
#include <zlib.h>

#include "hbc_netlog.h"

#define BIG (4 << 20)
#define FRAME (64 * 1024)
#define LC_CHUNK 4096

static double mbps(u32 bytes, u64 ticks) {
	return bytes / 1e6 / (ticks_to_microsecs(ticks) / 1e6);
}

// A bump allocator so zlib's state can be placed in a chosen memory.
static u8 *zpool;
static u32 zpool_used, zpool_size;

static voidpf zalloc_pool(voidpf opaque, uInt items, uInt size) {
	u32 n = (items * size + 31) & ~31;
	(void) opaque;
	if (zpool_used + n > zpool_size)
		return Z_NULL;
	zpool_used += n;
	return zpool + zpool_used - n;
}

static void zfree_pool(voidpf opaque, voidpf p) {
	(void) opaque;
	(void) p;
}

// MEM2 by carving the top of the MEM2 arena, like HBC's blob allocator.
static void *mem2_alloc(u32 size) {
	u32 hi = (u32) SYS_GetArena2Hi();
	u32 p = (hi - size) & ~31u;
	SYS_SetArena2Hi((void *) p);
	return (void *) p;
}

static void bench_copy_crc(const char *name, u8 *a, u8 *b) {
	u64 t;
	uLong crc;
	int i;

	memset(a, 0x5a, BIG);
	t = gettime();
	for (i = 0; i < 4; ++i)
		memcpy(b, a, BIG);
	printf("  memcpy  %-10s %7.1f MB/s\n", name, mbps(4 * BIG, diff_ticks(t, gettime())));

	t = gettime();
	crc = crc32(0, a, BIG);
	printf("  crc32   %-10s %7.1f MB/s (%08lx)\n", name, mbps(BIG, diff_ticks(t, gettime())), crc);
}

// CRC-32 of a MEM2 buffer, DMAed through the locked cache in double-buffered
// 4 KiB chunks: load chunk n+1 while checksumming chunk n.
static void bench_lc_crc(u8 *src) {
	u8 *lc = LCGetBase();
	uLong crc = crc32(0, Z_NULL, 0);
	u32 off, n = BIG / LC_CHUNK, i;
	u64 t;

	LCEnable();
	DCFlushRange(src, BIG);
	t = gettime();
	LCLoadData(lc, src, LC_CHUNK);
	for (i = 0; i < n; ++i) {
		u8 *cur = lc + (i & 1) * LC_CHUNK;
		if (i + 1 < n) {
			off = (i + 1) * LC_CHUNK;
			LCLoadData(lc + ((i + 1) & 1) * LC_CHUNK, src + off, LC_CHUNK);
			LCQueueWait(1);
		} else {
			LCQueueWait(0);
		}
		crc = crc32(crc, cur, LC_CHUNK);
	}
	printf("  crc32   %-10s %7.1f MB/s (%08lx)\n", "LC<-MEM2",
		   mbps(BIG, diff_ticks(t, gettime())), crc);
	LCDisable();
}

static void bench_zlib(const char *name, u8 *state, u32 state_size, const u8 *in,
					   u32 len, u8 *out) {
	static const int levels[] = { 1, 3, 6 };
	u32 li, off;

	for (li = 0; li < sizeof(levels) / sizeof(levels[0]); ++li) {
		z_stream z;
		u32 wire = 0;
		u64 t;

		memset(&z, 0, sizeof(z));
		zpool = state;
		zpool_size = state_size;
		zpool_used = 0;
		z.zalloc = zalloc_pool;
		z.zfree = zfree_pool;
		if (deflateInit(&z, levels[li]) != Z_OK) {
			printf("  deflateInit failed (state %u bytes)\n", state_size);
			return;
		}
		t = gettime();
		for (off = 0; off < len; off += FRAME) {
			u32 n = len - off < FRAME ? len - off : FRAME;
			deflateReset(&z);
			z.next_in = (Bytef *) in + off;
			z.avail_in = n;
			z.next_out = out;
			z.avail_out = FRAME + 1024;
			deflate(&z, Z_FINISH);
			wire += z.total_out;
		}
		printf("  deflate L%d %-9s %7.2f MB/s, %u -> %u bytes (%.1f%%)\n", levels[li], name,
			   mbps(len, diff_ticks(t, gettime())), len, wire, 100.0 * wire / len);
		deflateEnd(&z);
	}

	// Inflate: compress the frames once at level 1, then time decompression.
	{
		z_stream d, c;
		u8 *packed = out + FRAME + 1024;
		u64 total = 0, t;

		memset(&c, 0, sizeof(c));
		memset(&d, 0, sizeof(d));
		zpool = state;
		zpool_size = state_size;
		zpool_used = 0;
		c.zalloc = d.zalloc = zalloc_pool;
		c.zfree = d.zfree = zfree_pool;
		deflateInit(&c, 1);
		inflateInit(&d);
		for (off = 0; off < len; off += FRAME) {
			u32 n = len - off < FRAME ? len - off : FRAME;
			deflateReset(&c);
			c.next_in = (Bytef *) in + off;
			c.avail_in = n;
			c.next_out = packed;
			c.avail_out = FRAME + 1024;
			deflate(&c, Z_FINISH);

			t = gettime();
			inflateReset(&d);
			d.next_in = packed;
			d.avail_in = c.total_out;
			d.next_out = out;
			d.avail_out = FRAME;
			inflate(&d, Z_FINISH);
			total += diff_ticks(t, gettime());
		}
		printf("  inflate   %-9s %7.2f MB/s\n", name, mbps(len, total));
		deflateEnd(&c);
		inflateEnd(&d);
	}
}

int main(int argc, char **argv) {
	u8 *m1a, *m1b, *m2a, *m2b, *sample, *out1, *out2, *st1, *st2;
	u32 len = 0;
	const u32 state = 512 * 1024;
	FILE *f;

	// MEM1 first: the heap starts there in a small app like this one.
	m1a = memalign(32, BIG);
	m1b = memalign(32, BIG);
	st1 = memalign(32, state);
	out1 = memalign(32, 2 * (FRAME + 1024));
	m2a = mem2_alloc(BIG);
	m2b = mem2_alloc(BIG);
	st2 = mem2_alloc(state);
	out2 = mem2_alloc(2 * (FRAME + 1024));

	hbc_netlog_init();
	printf("membench: MEM1 buffers at %p/%p, MEM2 at %p/%p\n", m1a, st1, m2a, st2);
	if (((u32) m1a >> 28) != 8 || ((u32) m2a >> 28) != 9)
		printf("membench: warning, buffers are not where they were meant to be\n");

	bench_copy_crc("MEM1->MEM1", m1a, m1b);
	bench_copy_crc("MEM2->MEM2", m2a, m2b);
	bench_copy_crc("MEM2->MEM1", m2a, m1b);
	bench_lc_crc(m2a);
	bench_lc_crc(m1a);

	if (argc > 1 && fatInitDefault() && (f = fopen(argv[1], "rb"))) {
		sample = m1a;
		len = fread(sample, 1, BIG, f);
		fclose(f);
		memcpy(m2a, sample, len);
		printf("membench: zlib on %s (%u bytes), 64 KiB frames\n", argv[1], len);
		bench_zlib("MEM1", st1, state, m1a, len, out1);
		bench_zlib("MEM2", st2, state, m2a, len, out2);
	} else {
		printf("membench: no sample file; skipping zlib\n");
	}

	printf("membench: done\n");
	exit(0);
}
