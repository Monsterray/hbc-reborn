// The detached signature on a release's WAD (wiiupdate.h): ECDSA on P-256
// over SHA-256, through the mbedTLS already here for HTTPS. The key is the
// caller's, built into HBC; a WAD is only worth installing if it was signed
// by the holder of the matching private key, however it reached the
// console.

#include <string.h>

#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>

#include "wiiupdate.h"

static const u8 domain[] = "HBC-WAD-SIGNATURE-1";	// with its 0 byte

int wiiupdate_verify(const u8 *key, const u8 *sig, size_t sig_len, const u8 *wad, u32 size) {
	mbedtls_ecp_group grp;
	mbedtls_ecp_point q;
	mbedtls_mpi r, s;
	mbedtls_sha256_context sha;
	u8 hash[32];
	int result = -1;

	if (sig_len != WIIUPDATE_SIG_SIZE || key[0] != 0x04)
		return -1;

	mbedtls_ecp_group_init(&grp);
	mbedtls_ecp_point_init(&q);
	mbedtls_mpi_init(&r);
	mbedtls_mpi_init(&s);
	mbedtls_sha256_init(&sha);

	if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) ||
			mbedtls_ecp_point_read_binary(&grp, &q, key, WIIUPDATE_KEY_SIZE) ||
			mbedtls_ecp_check_pubkey(&grp, &q) ||
			mbedtls_mpi_read_binary(&r, sig, 32) || mbedtls_mpi_read_binary(&s, sig + 32, 32))
		goto out;

	if (mbedtls_sha256_starts(&sha, 0) || mbedtls_sha256_update(&sha, domain, sizeof(domain)) ||
			mbedtls_sha256_update(&sha, wad, size) || mbedtls_sha256_finish(&sha, hash))
		goto out;

	if (!mbedtls_ecdsa_verify(&grp, hash, sizeof(hash), &q, &r, &s))
		result = 0;

out:
	mbedtls_sha256_free(&sha);
	mbedtls_mpi_free(&s);
	mbedtls_mpi_free(&r);
	mbedtls_ecp_point_free(&q);
	mbedtls_ecp_group_free(&grp);
	return result;
}
