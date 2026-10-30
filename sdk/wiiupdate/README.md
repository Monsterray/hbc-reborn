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
| `https.c` | An HTTPS GET over libogc's sockets: TLS 1.2 through mbedTLS, the server checked against `certs.h`, redirects followed, a body streamed to a callback |
| `release.c` | Reads GitHub's `/releases` reply: the highest-numbered release that is not a draft or prerelease and has a WAD for this title, and its notes |
| `wad.c` | Checks a WAD held in memory and installs it through ES: the ticket, the TMD, each content in 32 KiB pieces, then the finish, cancelling on any failure and reading the stored version back |
| `certs.h` | The root certificates GitHub's API and download hosts chain to, as DER (`tools/gen_certs.py` makes it) |
| `hbc_mbedtls_config.h`, `mbedtls/` | mbedTLS 3.6.7, cut down to what a TLS 1.2 client with ECDHE and AES-GCM needs |

The channel's side is `channel/channelapp/source/update.c` and `essig.c`:

1. Once the network is up, a thread asks
   `api.github.com/repos/UPDATE_OWNER/UPDATE_REPO/releases` (both set in
   `channel/channelapp/config.h`) for the latest releases. Only the
   installed channel asks; a DOL started any other way does not.
2. If the newest release with a `-OHBC.wad` asset (`-LULZ.wad` for a build
   over the original title ID) is newer than this build, HBC asks with its
   usual dialog, showing the release notes.
3. On yes, the WAD is downloaded into memory with a progress bar, checked
   (its title must be this channel's, its version newer than the installed
   one), and installed with a second progress bar. `essig.c` lifts IOS's
   signature check for just that install, under AHBPROT, and puts back the
   bytes it changed.
4. HBC then exits through its reload stub, which starts the new version.

A failed download or install leaves the installed channel as it was and
says why.

## Publishing a release HBC will offer

Attach the retail WAD to the GitHub release, its name ending in the title
ID's four characters: `hbc-1.12.11-OHBC.wad`. The tag must carry the
version (`v1.12.11`); drafts, prereleases and tags with no number are
skipped. The WAD's TMD title version must be higher than the installed
one, which `channel/title/Makefile` makes from `CHANNEL_VERSION`.

## Tests

`tests/test_wiiupdate.py` builds `release.c` on the PC and checks what it
picks from a sample reply. The download and install run on the Wii; see
REVIEW.md for what has been checked.

## License

HBC's own files here are under HBC's license (GPL-2.0-or-later).
`mbedtls/` is Mbed TLS, under Apache-2.0 or GPL-2.0-or-later
(`mbedtls/LICENSE`); only `include/` and `library/` are kept, unchanged.
`certs.h` holds public root certificates from Mozilla's CA list.
