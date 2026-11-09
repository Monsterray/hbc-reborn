/* What a -DUSBGUARD_NO_OGC build of sdk/usbguard asks of the test program, and
 * the one internal call the test reaches. Not part of the public API.
 *
 * The test program provides the EHCI registers at their Wii addresses
 * (0x0d800064 HW_AHBPROT, 0x0d040000 the EHCI controller). */
#ifndef USBGUARD_TEST_H
#define USBGUARD_TEST_H

#include <stdint.h>
#include "usbguard.h"

uint32_t usbguard_host_read32(uint32_t addr);
void usbguard_host_write32(uint32_t addr, uint32_t value);

/* What usbguard_reload_ios runs: no retry, and a result of 0 or more passes
 * even if usbguard_free cut the request off. */
int32_t usbguard_test_reload_call(usbguard_fn fn, void *arg, const usbguard_opts *opts);

#endif
