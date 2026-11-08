# wiiupdate

How the installed channel updates itself from this project's GitHub
releases. It is a rewrite of
[libwiiupdate](https://github.com/quatric/libwiiupdate) for this tree,
built straight into the channel like `sdk/mii`, with no submodule or
prebuilt library. libwiiupdate replaces an app's files on the SD card; HBC
is a channel, so here the release's WAD is installed as a title instead,
with the steps RiftWii's channel installer uses.

| File | What it does |
| --- | --- |
| `https.c` | An HTTPS GET over libogc's sockets: TLS 1.2 through mbedTLS, the server checked against `certs.h`, redirects followed, a body streamed to a callback, a cancel flag |
| `release.c` | Reads GitHub's `/releases` reply: the highest-numbered release that is not a draft or prerelease and has a WAD for this title with a signature beside it, and its notes |
| `verify.c` | Checks the detached signature on a WAD against a key the caller built in |
| `wad.c` | Checks a WAD held in memory and installs it through ES: the ticket, the TMD, each content in 32 KiB pieces, then the finish, cancelling on any failure and reading the stored version back |
| `certs.h` | The root certificates GitHub's API and download hosts chain to, as DER (`tools/gen_certs.py` makes it) |
| `hbc_mbedtls_config.h`, `mbedtls/` | Mbed TLS 3.6.7 (`mbedtls/VERSION`), cut down to what a TLS 1.2 client with ECDHE and AES-GCM, and an ECDSA P-256 check, need |

## What makes an update safe to install

TLS only says that GitHub sent the file. Anyone who can push to the
repository or its CI could then put code with full hardware access on every
Wii, so the channel installs nothing that the project's key has not signed.

1. **A signature, from a key built into HBC.** Each release carries the WAD
   and, beside it, its detached signature, `NAME.wad.sig`: 64 bytes, ECDSA
   on P-256 over the SHA-256 of `HBC-WAD-SIGNATURE-1`, a 0 byte and the
   WAD (`tools/wadsign.py`). HBC checks it against `UPDATE_PUBLIC_KEY` in
   `channel/channelapp/config.h` after the download and before it touches
   anything. A release with no signature asset is not offered at all. With
   the key left at all zeros, no signature is good and nothing installs.
2. **The title ID, three times.** The WAD's ticket and its TMD must both
   carry this channel's title ID (`MY_TITLEID`), checked in `wad.c` before
   ES sees either, so a WAD can not add or replace the ticket of another
   title, the System Menu's included. Its version must also be newer than
   the installed one.
3. **IOS is patched in one place.** The signature check that ES applies to
   a release's fakesigned ticket and TMD is lifted by `nand_sigcheck_lift()`
   in `channel/channelapp/source/nandperm.c`, only around the install,
   under the lock and the one MEM2_PROT window that the play log's and the
   Mii faces' NAND permission patch use too. MEM2_PROT goes back to what it
   was when the last patch of either kind is undone.
4. **It can be turned off, and it stops cleanly.** Options has *Check for
   updates*, on or off (`check_updates` in `settings.xml`). A check or a
   download is cancelled, and its thread joined, when an app is launched
   and when HBC exits; the power button stops a download. An install, once
   begun, is let finish, as stopping it halfway is the dangerous thing.

The channel's side is `channel/channelapp/source/update.c`:

1. Once the network is up (and the setting is on), a thread asks
   `api.github.com/repos/UPDATE_OWNER/UPDATE_REPO/releases` (both set in
   `config.h`) for the latest releases. Only the installed channel asks; a
   DOL started any other way does not.
2. If the newest release with a `-OHBC.wad` asset (`-LULZ.wad` for a build
   over the original title ID) and its `.sig` is newer than this build, HBC
   asks with its usual dialog, showing the release notes.
3. On yes, the signature and the WAD are downloaded into memory, with a
   progress bar, then the WAD is checked (title, version) and its signature
   verified, and it is installed with a second progress bar.
4. HBC then exits through its reload stub, which starts the new version.

A failed download, signature or install leaves the installed channel as it
was and says why.

## Publishing a release HBC will offer

Once, make the signing key and put its public half into `config.h`:

```sh
python3 tools/wadsign.py keygen hbc-update.pem    # keep this file private
```

It prints the `UPDATE_PUBLIC_KEY` line for `config.h`. Every build that
should be able to install updates carries that key; a release is only as
trustworthy as the place `hbc-update.pem` is kept, which is not the CI.

For each release, sign the retail WAD on the machine that has the key:

```sh
python3 tools/wadsign.py sign hbc-update.pem hbc-1.13.1-OHBC.wad
python3 tools/wadsign.py verify hbc-update.pem hbc-1.13.1-OHBC.wad
```

and attach `hbc-1.13.1-OHBC.wad` and `hbc-1.13.1-OHBC.wad.sig` (and the
`-LULZ` pair, if there is one) to the release. The tag must carry the
version (`v1.13.1`); drafts, prereleases and tags with no number are
skipped. The WAD's TMD title version must be higher than the installed
one, which `channel/title/Makefile` makes from `CHANNEL_VERSION`.

## Tests

`tests/test_wiiupdate.py` builds `release.c` on the PC and checks what it
picks from a sample reply, and builds `verify.c` with the vendored mbedTLS
and checks that only a good signature by the right key is taken (a changed
WAD or signature, another signer, the unset key and a bare signature
without the prefix are all refused). The download and install run on the
Wii; see REVIEW.md for what has been checked.

## License

HBC's own files here are under HBC's license (GPL-2.0-or-later).
`mbedtls/` is Mbed TLS, under Apache-2.0 or GPL-2.0-or-later
(`mbedtls/LICENSE`); only `include/` and `library/` are kept, unchanged.
`certs.h` holds public root certificates from Mozilla's CA list.
