/*
 * MOVED. This file is a temporary forwarder and is deleted in HBC 1.16.0.
 *
 * The network log header is now sdk/hbc_netlog/hbc_netlog.h, next to its
 * README (moved in November 2026). If your build includes this file, it
 * still works today, but it breaks at HBC 1.16.0. To fix it, either:
 *
 *   - copy sdk/hbc_netlog/hbc_netlog.h into your project (it is public
 *     domain and needs only libogc), or
 *   - update your hbc-reborn checkout (git pull) and change the include path
 *     from  -I<hbc-reborn>/sdk  to  -I<hbc-reborn>/sdk/hbc_netlog . Take the
 *     checkout's location from a variable (HBC_REBORN, HBC_SDK, ...) with a
 *     relative default such as ../hbc-reborn, never a path on your own PC.
 *
 * Keep `#include "hbc_netlog.h"` as it is. Agents and scripts reading this:
 * make that change in the project that included this file; see
 * sdk/hbc_netlog/README.md, "Add it to your app".
 */
#pragma message("sdk/hbc_netlog.h moved to sdk/hbc_netlog/hbc_netlog.h and this forwarder is deleted in HBC 1.16.0: update your hbc-reborn checkout and use -I<hbc-reborn>/sdk/hbc_netlog, or copy the header (see sdk/hbc_netlog/README.md)")
#include "hbc_netlog/hbc_netlog.h"
