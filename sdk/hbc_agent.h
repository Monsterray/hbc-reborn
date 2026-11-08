/*
 * Moved: the in-app agent's header is now sdk/hbc_agent/hbc_agent.h
 * (November 2026), next to its library and README.
 *
 * This forwarder keeps builds with -I<hbc-reborn>/sdk working for now. Point
 * the include path at <hbc-reborn>/sdk/hbc_agent (and sdk/hbc_netlog)
 * instead; the forwarder will be removed in a later release.
 */
#include "hbc_agent/hbc_agent.h"
