// Pipelined, checksummed and optionally compressed file transfers for the
// developer protocol (HBCF ops 'p' and 'g', docs/devnet.md).
//
// Data moves in frames of at most 64 KiB: u32 raw length, u32 wire length,
// u32 CRC-32 of the raw bytes, then the wire bytes. A wire length below the
// raw length means the frame is one zlib stream. The loader thread runs the
// network side and a worker thread runs SD and zlib, passing a ring of MEM2
// buffers through two message queues, so IOS services Wi-Fi and SDIO at the
// same time and the PPC compresses while the radio is busy.

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/message.h>
#include <zlib.h>

#include "../config.h"
#include "devnet.h"
#include "devstream.h"
#include "tcp.h"
#include "zmem.h"

#define FRAME_MAX (64 * 1024)
#define FRAME_HDR 12
#define SLOTS 4
#define WORKER_STACK (32 * 1024)
// Each slot holds a raw and a wire buffer, each 32-byte aligned for SD DMA
// with headroom for the frame header in front.
#define HEADROOM 64
#define SLOT_BYTES (2 * (HEADROOM + FRAME_MAX))
#define BACKOFF_FRAMES 8
// Level 1 measured best on a Wii: level 6 cut a 5.3 MB ELF download from
// 2.96 to 2.78 MB on the wire but spent 1.6 s of CPU, lowering 0.91 MB/s
// to 0.82 MB/s.
#define DEFLATE_LEVEL 1

typedef struct {
	u32 raw_len, wire_len, crc;
	s32 err;       // terminator status when raw_len is 0
	u8 *raw;       // FRAME_HDR bytes of headroom precede each buffer
	u8 *wire;
	u8 *data;      // what goes on (or came off) the wire: raw or wire
} slot_t;

static slot_t slots[SLOTS];
static u8 *pool;
static mqbox_t q_free, q_full;
static lwp_t worker;
static volatile bool abort_flag;

static struct {
	FILE *f;
	u32 size;
	bool compress;
	s32 result;
	devstream_stats *stats;
} job;

static void put_u32(u8 *p, u32 v) {
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static u32 get_u32(const u8 *p) {
	return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static bool setup(void) {
	int i;

	if (!pool) {
		pool = memalign(32, SLOTS * SLOT_BYTES + WORKER_STACK);
		if (!pool)
			return false;
	}

	for (i = 0; i < SLOTS; ++i) {
		u8 *base = pool + i * SLOT_BYTES;

		slots[i].raw = base + HEADROOM;
		slots[i].wire = base + 2 * HEADROOM + FRAME_MAX;
	}

	if (MQ_Init(&q_free, SLOTS) != MQ_ERROR_SUCCESSFUL)
		return false;
	if (MQ_Init(&q_full, SLOTS) != MQ_ERROR_SUCCESSFUL) {
		MQ_Close(q_free);
		return false;
	}
	for (i = 0; i < SLOTS; ++i)
		MQ_Send(q_free, &slots[i], MQ_MSG_BLOCK);

	abort_flag = false;
	return true;
}

static bool start_worker(void *(*fn)(void *)) {
	return LWP_CreateThread(&worker, fn, NULL, pool + SLOTS * SLOT_BYTES,
			WORKER_STACK, DEVNET_THREAD_PRIO) == 0;
}

static void teardown(void) {
	LWP_JoinThread(worker, NULL);
	MQ_Close(q_full);
	MQ_Close(q_free);
}

static slot_t *take(mqbox_t q) {
	mqmsg_t msg;

	MQ_Receive(q, &msg, MQ_MSG_BLOCK);
	return msg;
}

// Worker for uploads: inflate, verify and write each frame.
static void *put_worker(void *arg) {
	z_stream z = { 0 };  // zlib reads zalloc/zfree/opaque from here
	bool in_mem1 = zmem_use(&z);
	bool z_ok = inflateInit(&z) == Z_OK;
	s32 err = 0;
	(void) arg;

	while (true) {
		slot_t *slot = take(q_full);
		u64 t0 = gettime();

		if (!slot->raw_len) {
			if (slot->err && !err)
				err = slot->err;
			MQ_Send(q_free, slot, MQ_MSG_BLOCK);
			break;
		}

		slot->data = slot->wire;
		if (!err && slot->wire_len < slot->raw_len) {
			z.next_in = slot->wire;
			z.avail_in = slot->wire_len;
			z.next_out = slot->raw;
			z.avail_out = slot->raw_len;
			if (!z_ok || inflateReset(&z) != Z_OK ||
					inflate(&z, Z_FINISH) != Z_STREAM_END ||
					z.total_out != slot->raw_len)
				err = -EBADMSG;
			slot->data = slot->raw;
		}

		if (!err && crc32(0, slot->data, slot->raw_len) != slot->crc)
			err = -EBADMSG;
		u64 t2 = gettime();

		if (!err && fwrite(slot->data, 1, slot->raw_len, job.f) != slot->raw_len)
			err = errno ? -errno : -ENOSPC;

		job.stats->cpu += diff_ticks(t0, t2);
		job.stats->disk += diff_ticks(t2, gettime());
		MQ_Send(q_free, slot, MQ_MSG_BLOCK);
	}

	if (z_ok)
		inflateEnd(&z);
	if (in_mem1)
		zmem_release();
	job.result = err;
	return NULL;
}

s32 devstream_put(s32 s, const char *path, const char *part, u32 size,
				  devstream_stats *stats) {
	u8 hdr[FRAME_HDR];
	u32 left = size;
	s32 err = 0;

	job.f = fopen(part, "wb");
	if (!job.f)
		return -errno;
	job.stats = stats;
	job.result = 0;

	if (!setup()) {
		fclose(job.f);
		unlink(part);
		return -ENOMEM;
	}
	if (!start_worker(put_worker)) {
		MQ_Close(q_full);
		MQ_Close(q_free);
		fclose(job.f);
		unlink(part);
		return -ENOMEM;
	}

	while (left) {
		slot_t *slot = take(q_free);
		u64 t0 = gettime();

		if (devnet_aborted()) {
			err = -EINTR;
		} else if (!tcp_read(s, hdr, FRAME_HDR, NULL, NULL)) {
			err = -EIO;
		} else {
			slot->raw_len = get_u32(hdr);
			slot->wire_len = get_u32(hdr + 4);
			slot->crc = get_u32(hdr + 8);
			if (!slot->raw_len || slot->raw_len > FRAME_MAX || slot->raw_len > left ||
					!slot->wire_len || slot->wire_len > slot->raw_len)
				err = -EPROTO;
			else if (!tcp_read(s, slot->wire, slot->wire_len, NULL, NULL))
				err = -EIO;
		}
		stats->net += diff_ticks(t0, gettime());

		if (err) {
			MQ_Send(q_free, slot, MQ_MSG_BLOCK);
			break;
		}

		stats->wire += slot->wire_len;
		left -= slot->raw_len;
		MQ_Send(q_full, slot, MQ_MSG_BLOCK);
	}

	// Terminator: tells the worker to finish, with the network result.
	slot_t *end = take(q_free);
	end->raw_len = 0;
	end->err = err;
	MQ_Send(q_full, end, MQ_MSG_BLOCK);
	teardown();

	err = job.result;
	if (fclose(job.f) && !err)
		err = -errno;
	if (!err) {
		unlink(path);
		if (rename(part, path))
			err = -errno;
	} else {
		unlink(part);
	}
	return err;
}

// Worker for downloads: read, checksum and compress each frame.
static void *get_worker(void *arg) {
	z_stream z = { 0 };  // zlib reads zalloc/zfree/opaque from here
	bool in_mem1 = job.compress && zmem_use(&z);
	bool z_ok = job.compress && deflateInit(&z, DEFLATE_LEVEL) == Z_OK;
	u32 left = job.size, backoff = 0;
	s32 err = 0;
	(void) arg;

	while (left && !abort_flag && !devnet_aborted()) {
		slot_t *slot = take(q_free);
		u32 n = left > FRAME_MAX ? FRAME_MAX : left;
		u64 t0 = gettime();

		if (fread(slot->raw, 1, n, job.f) != n) {
			MQ_Send(q_free, slot, MQ_MSG_BLOCK);
			err = errno ? -errno : -EIO;
			break;
		}
		u64 t1 = gettime();

		slot->raw_len = n;
		slot->crc = crc32(0, slot->raw, n);
		slot->data = slot->raw;
		slot->wire_len = n;

		// Skip zlib for a while after a frame that did not shrink, so
		// incompressible data costs almost no CPU.
		if (z_ok && !backoff) {
			z.next_in = slot->raw;
			z.avail_in = n;
			z.next_out = slot->wire;
			z.avail_out = n - 1;
			if (deflateReset(&z) == Z_OK && deflate(&z, Z_FINISH) == Z_STREAM_END) {
				slot->wire_len = z.total_out;
				slot->data = slot->wire;
			} else {
				backoff = BACKOFF_FRAMES;
			}
		} else if (backoff) {
			backoff--;
		}

		job.stats->disk += diff_ticks(t0, t1);
		job.stats->cpu += diff_ticks(t1, gettime());
		left -= n;
		MQ_Send(q_full, slot, MQ_MSG_BLOCK);
	}

	if (z_ok)
		deflateEnd(&z);
	if (in_mem1)
		zmem_release();

	slot_t *end = take(q_free);
	end->raw_len = 0;
	end->err = err;
	MQ_Send(q_full, end, MQ_MSG_BLOCK);
	return NULL;
}

void devstream_get(s32 s, const char *path, bool compress, devstream_stats *stats) {
	struct stat st;
	u8 hdr[8];
	bool sending = true;

	if (stat(path, &st) || !S_ISREG(st.st_mode)) {
		devnet_reply(s, -ENOENT, NULL, 0);
		return;
	}

	job.f = fopen(path, "rb");
	if (!job.f) {
		devnet_reply(s, -errno, NULL, 0);
		return;
	}
	job.size = st.st_size;
	job.compress = compress;
	job.stats = stats;

	if (!setup()) {
		fclose(job.f);
		devnet_reply(s, -ENOMEM, NULL, 0);
		return;
	}
	if (!start_worker(get_worker)) {
		MQ_Close(q_full);
		MQ_Close(q_free);
		fclose(job.f);
		devnet_reply(s, -ENOMEM, NULL, 0);
		return;
	}

	put_u32(hdr, 0);
	put_u32(hdr + 4, job.size);
	u64 t0 = gettime();
	sending = devnet_send_all(s, hdr, sizeof(hdr));
	stats->net += diff_ticks(t0, gettime());

	while (true) {
		slot_t *slot = take(q_full);
		u8 *frame = slot->data - FRAME_HDR;
		bool done = !slot->raw_len;

		// The terminator frame carries the read status in its CRC field.
		put_u32(frame, slot->raw_len);
		put_u32(frame + 4, done ? 0 : slot->wire_len);
		put_u32(frame + 8, done ? (u32) slot->err : slot->crc);

		if (sending && devnet_aborted()) {
			sending = false;
			abort_flag = true;
		}
		if (sending) {
			u64 t1 = gettime();
			u32 len = FRAME_HDR + (done ? 0 : slot->wire_len);

			sending = devnet_send_all(s, frame, len);
			stats->net += diff_ticks(t1, gettime());
			stats->wire += len;
			if (!sending)
				abort_flag = true;
		}

		MQ_Send(q_free, slot, MQ_MSG_BLOCK);
		if (done)
			break;
	}

	teardown();
	fclose(job.f);
}
