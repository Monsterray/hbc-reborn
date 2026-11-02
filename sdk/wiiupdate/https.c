// An HTTPS GET (wiiupdate.h): TLS 1.2 through the mbedTLS in mbedtls/,
// following redirects and streaming the body to a callback, so a download
// of any size never has to be held twice. From libwiiupdate's
// wiiupdate_http.c.
//
// Certificates are checked against the roots in certs.h, and the host
// name too; their dates are not (hbc_mbedtls_config.h), as many Wiis' clocks
// are wrong.

#define _GNU_SOURCE	// strcasestr
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <time.h>

#include <gccore.h>
#include <network.h>
#include <poll.h>
#include <ogc/lwp_watchdog.h>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/platform_time.h>

#include "wiiupdate.h"
#include "certs.h"

#define TIMEOUT_MS 15000
#define MAX_REDIRECTS 5
#define URL_MAX 2048

// mbedTLS's clock (MBEDTLS_PLATFORM_MS_TIME_ALT): only for its timeouts.
mbedtls_ms_time_t mbedtls_ms_time(void) {
	return (mbedtls_ms_time_t) ticks_to_millisecs(gettime());
}

// The Wii has no random number generator the PPC can reach: the jitter of
// the time base around memory work, which varies with the bus and caches.
static int wii_entropy(void *data, unsigned char *out, size_t len, size_t *olen) {
	static volatile u32 scratch[256];
	size_t i;
	int k, n;

	for (i = 0; i < len; i++) {
		u32 acc = 0;

		for (k = 0; k < 16; k++) {
			u64 t0 = gettime(), t1;

			for (n = 0; n < (int) (t0 & 31) + 8; n++)
				scratch[(t0 + n * 37) & 255] += (u32) t0 ^ n;
			t1 = gettime();
			acc = (acc << 3 | acc >> 29) ^ (u32) (t1 - t0) ^ (u32) t1;
		}
		out[i] = (unsigned char) (acc ^ acc >> 8 ^ acc >> 16 ^ acc >> 24);
	}
	*olen = len;
	return 0;
}

// ---- A connection ----

typedef struct {
	s32 fd;
	volatile int *cancel;
	mbedtls_ssl_context ssl;
	u8 buf[4096];
	size_t pos, len;
	bool eof;
} conn;

static struct {
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt cas;
	mbedtls_ssl_config conf;
} tls;

// In short slices, so a cancel is noticed quickly.
static int wait_fd(conn *c, int events) {
	int waited;

	for (waited = 0; waited < TIMEOUT_MS; waited += 250) {
		struct pollsd p = { .socket = c->fd, .events = events, .revents = 0 };
		int r;

		if (*c->cancel)
			return -1;
		r = net_poll(&p, 1, 250);
		if (r < 0)
			return -1;
		if (r > 0)
			return 0;
	}
	return -1;
}

static int sock_send(void *ctx, const unsigned char *buf, size_t len) {
	conn *c = ctx;
	int r;

	if (wait_fd(c, POLLOUT) < 0)
		return MBEDTLS_ERR_SSL_TIMEOUT;
	r = net_send(c->fd, buf, len > 16384 ? 16384 : len, 0);
	if (r == -EAGAIN)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r;
}

static int sock_recv(void *ctx, unsigned char *buf, size_t len) {
	conn *c = ctx;
	int r;

	if (wait_fd(c, POLLIN) < 0)
		return MBEDTLS_ERR_SSL_TIMEOUT;
	r = net_recv(c->fd, buf, len, 0);
	if (r == -EAGAIN)
		return MBEDTLS_ERR_SSL_WANT_READ;
	return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r;
}

static int tls_init(void) {
	char personal[64];
	size_t i, at;

	mbedtls_entropy_init(&tls.entropy);
	mbedtls_ctr_drbg_init(&tls.drbg);
	mbedtls_x509_crt_init(&tls.cas);
	mbedtls_ssl_config_init(&tls.conf);
	mbedtls_entropy_add_source(&tls.entropy, wii_entropy, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);

	snprintf(personal, sizeof(personal), "hbc %llu %lld", gettime(), (long long) time(NULL));
	if (mbedtls_ctr_drbg_seed(&tls.drbg, mbedtls_entropy_func, &tls.entropy,
							  (const u8 *) personal, strlen(personal)))
		return -1;
	for (i = 0, at = 0; i < sizeof(WIIUPDATE_CA_SIZES) / sizeof(WIIUPDATE_CA_SIZES[0]); ++i) {
		if (mbedtls_x509_crt_parse_der_nocopy(&tls.cas, WIIUPDATE_CA_DER + at,
											  WIIUPDATE_CA_SIZES[i]) < 0)
			return -1;
		at += WIIUPDATE_CA_SIZES[i];
	}
	if (mbedtls_ssl_config_defaults(&tls.conf, MBEDTLS_SSL_IS_CLIENT,
									MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT))
		return -1;
	mbedtls_ssl_conf_authmode(&tls.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&tls.conf, &tls.cas, NULL);
	mbedtls_ssl_conf_rng(&tls.conf, mbedtls_ctr_drbg_random, &tls.drbg);
	return 0;
}

static void tls_free(void) {
	mbedtls_ssl_config_free(&tls.conf);
	mbedtls_x509_crt_free(&tls.cas);
	mbedtls_ctr_drbg_free(&tls.drbg);
	mbedtls_entropy_free(&tls.entropy);
}

static int conn_open(conn *c, const char *host) {
	struct sockaddr_in sa;
	struct hostent *he;
	int r;

	c->fd = -1;
	c->pos = c->len = 0;
	c->eof = false;
	mbedtls_ssl_init(&c->ssl);
	if (mbedtls_ssl_setup(&c->ssl, &tls.conf) || mbedtls_ssl_set_hostname(&c->ssl, host))
		return -1;

	he = net_gethostbyname(host);
	if (*c->cancel || !he || he->h_addrtype != AF_INET || !he->h_addr_list[0])
		return -1;
	c->fd = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (c->fd < 0)
		return -1;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(443);
	memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));
	if (net_connect(c->fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
		net_close(c->fd);
		c->fd = -1;
		return -1;
	}
	mbedtls_ssl_set_bio(&c->ssl, c, sock_send, sock_recv, NULL);

	while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0)
		if (*c->cancel || (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE))
			return -1;
	return 0;
}

static void conn_close(conn *c) {
	if (c->fd >= 0) {
		mbedtls_ssl_close_notify(&c->ssl);
		net_close(c->fd);
		c->fd = -1;
	}
	mbedtls_ssl_free(&c->ssl);
}

static int conn_write(conn *c, const char *data, size_t len) {
	size_t sent = 0;

	while (sent < len) {
		int r = mbedtls_ssl_write(&c->ssl, (const u8 *) data + sent, len - sent);

		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (r <= 0 || *c->cancel)
			return -1;
		sent += r;
	}
	return 0;
}

// Bytes in the buffer, refilled when empty; 0 at the end, < 0 on error.
static int conn_fill(conn *c) {
	if (c->pos < c->len)
		return (int) (c->len - c->pos);
	if (c->eof)
		return 0;
	for (;;) {
		int r;

		if (*c->cancel)
			return -1;
		r = mbedtls_ssl_read(&c->ssl, c->buf, sizeof(c->buf));
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
			c->eof = true;
			return 0;
		}
		if (r < 0)
			return -1;
		c->pos = 0;
		c->len = r;
		return r;
	}
}

// One line without its CR LF; a longer line than fits is cut.
static int conn_line(conn *c, char *out, size_t max) {
	size_t n = 0;

	for (;;) {
		char ch;

		if (conn_fill(c) <= 0)
			return -1;
		ch = (char) c->buf[c->pos++];
		if (ch == '\n')
			break;
		if (n + 1 < max)
			out[n++] = ch;
	}
	if (n && out[n - 1] == '\r')
		n--;
	out[n] = 0;
	return (int) n;
}

// Exactly len bytes to the sink, or up to the end when len < 0.
static int conn_body(conn *c, s64 len, wiiupdate_sink sink, void *ctx) {
	while (len != 0) {
		int avail = conn_fill(c);
		size_t n;

		if (avail < 0)
			return -1;
		if (avail == 0)
			return len < 0 ? 0 : -1;
		n = (size_t) avail;
		if (len > 0 && (s64) n > len)
			n = (size_t) len;
		if (sink(ctx, c->buf + c->pos, n) < 0)
			return -1;
		c->pos += n;
		if (len > 0)
			len -= n;
	}
	return 0;
}

// ---- GET ----

static int split_url(const char *url, char *host, size_t host_len, const char **path) {
	size_t n;
	char *port;

	if (strncasecmp(url, "https://", 8))
		return -1;
	url += 8;
	n = strcspn(url, "/?#");
	if (n == 0 || n >= host_len)
		return -1;
	memcpy(host, url, n);
	host[n] = 0;
	port = strchr(host, ':');
	if (port) {
		if (strcmp(port, ":443"))
			return -1;
		*port = 0;
	}
	*path = url[n] == '/' ? url + n : "/";
	return 0;
}

int wiiupdate_https_get(const char *start_url, const char *accept, wiiupdate_sink sink,
						void (*on_length)(void *ctx, s64 len), void *ctx,
						volatile int *cancel) {
	char *url = malloc(URL_MAX), *line = malloc(URL_MAX), *location = malloc(URL_MAX);
	conn *c = malloc(sizeof(*c));
	int result = -1, hop;
	bool tls_up = false;

	if (!url || !line || !location || !c)
		goto out;
	tls_up = true;
	if (tls_init() < 0)
		goto out;
	snprintf(url, URL_MAX, "%s", start_url);
	c->cancel = cancel;

	for (hop = 0; hop <= MAX_REDIRECTS && !*cancel; hop++) {
		char host[128];
		const char *path;
		int req, status = 0, n;
		bool chunked = false, bad = false;
		s64 length = -1;

		if (split_url(url, host, sizeof(host), &path) < 0)
			break;
		if (conn_open(c, host) < 0) {
			conn_close(c);
			break;
		}

		req = snprintf(line, URL_MAX,
					   "GET %s HTTP/1.1\r\n"
					   "Host: %s\r\n"
					   "User-Agent: TheHomebrewChannel\r\n"
					   "Accept: %s\r\n"
					   "Accept-Encoding: identity\r\n"
					   "Connection: close\r\n\r\n",
					   path, host, accept ? accept : "*/*");
		location[0] = 0;
		if (req >= URL_MAX || conn_write(c, line, req) < 0 || conn_line(c, line, URL_MAX) < 0 ||
				strncmp(line, "HTTP/1.", 7) || (status = atoi(line + 9)) < 100) {
			conn_close(c);
			break;
		}
		for (;;) {
			n = conn_line(c, line, URL_MAX);
			if (n < 0) {
				bad = true;
				break;
			}
			if (n == 0)
				break;
			if (!strncasecmp(line, "content-length:", 15)) {
				length = strtoll(line + 15, NULL, 10);
			} else if (!strncasecmp(line, "transfer-encoding:", 18) && strcasestr(line + 18, "chunked")) {
				chunked = true;
			} else if (!strncasecmp(line, "location:", 9)) {
				const char *v = line + 9;

				while (*v == ' ')
					v++;
				snprintf(location, URL_MAX, "%s", v);
			}
		}
		if (bad) {
			conn_close(c);
			break;
		}

		if (status >= 300 && status < 400 && location[0]) {
			conn_close(c);
			if (location[0] == '/') {
				// The same host, an absolute path.
				snprintf(line, URL_MAX, "https://%s%s", host, location);
				snprintf(url, URL_MAX, "%s", line);
			} else {
				snprintf(url, URL_MAX, "%s", location);
			}
			continue;
		}
		if (status != 200) {
			conn_close(c);
			break;
		}

		if (chunked) {
			if (on_length)
				on_length(ctx, -1);
			result = 0;
			for (;;) {
				s64 size;

				if (conn_line(c, line, URL_MAX) < 0) {
					result = -1;
					break;
				}
				size = strtoll(line, NULL, 16);
				if (size <= 0)
					break;
				if (conn_body(c, size, sink, ctx) < 0 || conn_line(c, line, URL_MAX) < 0) {
					result = -1;
					break;
				}
			}
		} else {
			if (on_length)
				on_length(ctx, length);
			result = conn_body(c, length, sink, ctx);
		}
		conn_close(c);
		break;
	}

out:
	if (tls_up)
		tls_free();
	free(c);
	free(location);
	free(line);
	free(url);
	return *cancel ? -1 : result;
}
