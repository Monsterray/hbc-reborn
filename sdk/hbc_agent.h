/*
 * MOVED. This file is a temporary forwarder and is deleted in HBC 1.16.0.
 *
 * The in-app agent's header is now sdk/hbc_agent/hbc_agent.h, next to its
 * library and README (moved in November 2026). If your build includes this
 * file, it still works today, but it breaks at HBC 1.16.0. To fix it:
 *
 *   1. Update your hbc-reborn checkout (git pull), or point your build at a
 *      current one. Do not hard-code a path on your own PC: take the
 *      checkout's location from a variable (HBC_REBORN, HBC_AGENT_INC, ...)
 *      with a relative default such as ../hbc-reborn.
 *   2. Change the include path from  -I<hbc-reborn>/sdk
 *      to  -I<hbc-reborn>/sdk/hbc_agent -I<hbc-reborn>/sdk/hbc_netlog
 *      and link with  -L<hbc-reborn>/sdk/hbc_agent -lhbcagent .
 *   3. Keep `#include "hbc_agent.h"` as it is.
 *
 * Agents and scripts reading this: make that change in the project that
 * included this file; see sdk/hbc_agent/README.md, "Add it to your app".
 */
#pragma message("sdk/hbc_agent.h moved to sdk/hbc_agent/hbc_agent.h and this forwarder is deleted in HBC 1.16.0: update your hbc-reborn checkout and use -I<hbc-reborn>/sdk/hbc_agent -I<hbc-reborn>/sdk/hbc_netlog (see sdk/hbc_agent/README.md)")
#include "hbc_agent/hbc_agent.h"
