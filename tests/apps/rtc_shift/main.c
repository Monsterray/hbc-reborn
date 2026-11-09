// Move the Wii's RTC (its battery-backed clock) by a number of seconds, then
// exit back to the loader. For undoing a bad clock change on the bench Wii.
// usage (argv from Wiiload): rtc_shift.dol SECONDS   (negative moves it back)
//
// The Wii's time is the RTC plus SYSCONF's counter bias; this changes only
// the RTC, as DEV > Sync clock does, and prints before and after.

#include <stdio.h>
#include <stdlib.h>
#include <ogcsys.h>
#include <ogc/exi.h>

extern u32 __SYS_GetRTC(u32 *gctime);

static bool rtc_write(u32 value) {
	u32 cmd = 0xa0000000;
	bool ok;

	if (!EXI_Lock(EXI_CHANNEL_0, EXI_DEVICE_1, NULL))
		return false;
	if (!EXI_Select(EXI_CHANNEL_0, EXI_DEVICE_1, EXI_SPEED8MHZ)) {
		EXI_Unlock(EXI_CHANNEL_0);
		return false;
	}
	ok = EXI_ImmEx(EXI_CHANNEL_0, &cmd, 4, EXI_WRITE) && EXI_ImmEx(EXI_CHANNEL_0, &value, 4, EXI_WRITE);
	ok = EXI_Deselect(EXI_CHANNEL_0) && ok;
	EXI_Unlock(EXI_CHANNEL_0);
	return ok;
}

int main(int argc, char **argv) {
	u32 rtc, bias = 0, back;
	long shift;

	if (argc < 2 || __SYS_GetRTC(&rtc) != 1)
		return 1;
	shift = strtol(argv[1], NULL, 10);
	CONF_GetCounterBias(&bias);
	printf("rtc %u, bias %u, Wii time %u s since 2000\n", rtc, bias, rtc + bias);
	if (!rtc_write(rtc + (u32) shift) || __SYS_GetRTC(&back) != 1)
		return 2;
	printf("shifted by %ld: rtc %u, Wii time %u\n", shift, back, back + bias);
	return 0;
}
