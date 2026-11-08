/*
 * Moved: the network log header is now sdk/hbc_netlog/hbc_netlog.h
 * (November 2026), next to its README.
 *
 * This forwarder keeps builds with -I<hbc-reborn>/sdk working for now. Point
 * the include path at <hbc-reborn>/sdk/hbc_netlog instead, or copy
 * hbc_netlog.h into your project; the forwarder will be removed in a later
 * release.
 */
#include "hbc_netlog/hbc_netlog.h"
