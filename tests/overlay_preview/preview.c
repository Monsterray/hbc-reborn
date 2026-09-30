// Render the agent overlay on the PC, from the same ov_ui.c and ov_draw.c
// the Wii runs, with made-up data and a scripted set of presses. Each
// capture is written as raw YUYV; preview.py turns them into PNG files.
//
//   cc -O2 -I sdk/hbc_agent tests/overlay_preview/preview.c
//      sdk/hbc_agent/ov_ui.c sdk/hbc_agent/ov_draw.c -o preview   (one line)
//   ./preview OUTDIR        (python3 tests/overlay_preview/preview.py does both)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ov_ui.h"

#define W 640
#define H 480

static uint8_t game[W * H * 2], frame[W * H * 2];
static ov_ext ext;
static const char *outdir;

// A stand-in game screen: sky, ground and two blocks.
static void make_game(void) {
	ov_canvas c = { game, W, H, W * 2, 0, 0, W, H };

	ov_fill(&c, 0, 0, W, H, ov_rgb(0x5c, 0x8f, 0xc8), 255);
	ov_fill(&c, 0, 300, W, 180, ov_rgb(0x4d, 0x7a, 0x3a), 255);
	ov_fill(&c, 80, 220, 160, 80, ov_rgb(0xb0, 0x6a, 0x3c), 255);
	ov_fill(&c, 420, 150, 90, 150, ov_rgb(0x8a, 0x6d, 0x3b), 255);
}

static void act(int action, int arg, void *user) {
	(void) user;
	printf("  act %d %d\n", action, arg);
	switch (action) {
	case OVA_LOG_PC: ext.log_pc = arg; break;
	case OVA_CRASH_STAY: ext.crash_stay = arg; break;
	case OVA_HBCPY: ext.hbcpy = arg; break;
	case OVA_RUMBLE: ext.remote[arg].rumble = !ext.remote[arg].rumble; break;
	case OVA_FIND: ext.remote[arg].finding = true; break;
	case OVA_SENSOR_ABOVE: ext.sensor_above = arg; break;
	case OVA_IR_SENS: ext.ir_sens = arg; break;
	case OVA_RUMBLE_ALL: ext.rumble_all = arg; break;
	case OVA_CAL_START: ext.cal_progress = 100; break;
	case OVA_SHOT: snprintf(ext.toast, sizeof(ext.toast), "Saved sd:/screenshots/agent_app-001.bmp"); break;
	}
}

static void capture(ov_ui *ui, const char *name) {
	ov_canvas c = { frame, W, H, W * 2, 0, 0, W, H };
	char path[256];
	FILE *f;

	ov_dim_copy(&c, game, ui->menu ? 150 : 200);
	ov_draw(ui, &ext, &c);
	snprintf(path, sizeof(path), "%s/%s.yuyv", outdir, name);
	f = fopen(path, "wb");
	if (!f) {
		perror(path);
		exit(1);
	}
	fwrite(frame, 1, sizeof(frame), f);
	fclose(f);
	printf("%s: menu %d open %d dev %d wm %d exit %d closing %d focus %d items %d\n", name,
		   ui->menu, ui->open_t, ui->dev_t, ui->wm_t, ui->exit_t, ui->closing, ui->focus, ui->n);
}

// Press each key in turn, letting animations finish after each.
static void keys(ov_ui *ui, const char *seq) {
	int i;

	for (i = 0; i < 20; ++i)
		ov_step(ui, &ext, 0, act, NULL);
	for (; *seq; ++seq) {
		unsigned k = 0;

		switch (*seq) {
		case 'u': k = OV_UP; break;
		case 'd': k = OV_DOWN; break;
		case 'l': k = OV_LEFT; break;
		case 'r': k = OV_RIGHT; break;
		case 'a': k = OV_A; break;
		case 'b': k = OV_B; break;
		case 'h': k = OV_HOME; break;
		}
		ov_step(ui, &ext, k, act, NULL);
		for (i = 0; i < 20; ++i)
			ov_step(ui, &ext, 0, act, NULL);
	}
}

int main(int argc, char **argv) {
	static ov_ui ui;
	int i;

	outdir = argc > 1 ? argv[1] : ".";
	make_game();

	snprintf(ext.app, sizeof(ext.app), "agent_app 1.0");
	snprintf(ext.clock, sizeof(ext.clock), "14:32");
	snprintf(ext.date, sizeof(ext.date), "14:32, Tue 29 Sep");
	snprintf(ext.playing, sizeof(ext.playing), "12 min");
	snprintf(ext.network, sizeof(ext.network), "192.168.8.213");
	snprintf(ext.sd, sizeof(ext.sd), "3.1 GB free");
	ext.mem_free_kb[0] = 23156;
	ext.mem_total_kb[0] = 24576;
	ext.mem_free_kb[1] = 53002;
	ext.mem_total_kb[1] = 65536;
	ext.remote[0] = (ov_remote) { true, 3, 75, "Nunchuk", true, true, false };
	ext.remote[1] = (ov_remote) { true, 1, 25, "Classic Controller", false, false, false };
	ext.log_pc = ext.hbcpy = ext.rumble_all = ext.has_save = ext.can_leds = true;
	ext.ir_sens = 3;
	snprintf(ext.auto_off, sizeof(ext.auto_off), "5 min");
	snprintf(ext.slot[1], sizeof(ext.slot[1]), "Shot");
	ext.show_net = ext.show_log_pc = ext.show_crash = true;
	ext.exit_mask = 15;
	snprintf(ext.slot[0], sizeof(ext.slot[0]), "HBC");
	snprintf(ext.menu[0].title, sizeof(ext.menu[0].title), "The Homebrew Channel");
	{
		static const char *items[][2] = {
			{ "Version", "1.7.0" }, { "IOS", "IOS58 v24.31" }, { "Network", "192.168.8.213" },
			{ "About", NULL }, { "Launch BootMii", NULL }, { "Exit to System Menu", NULL },
			{ "Shutdown", NULL },
		};
		int i;

		for (i = 0; i < 7; ++i) {
			ov_menu_item *it = &ext.menu[0].item[i];

			snprintf(it->label, sizeof(it->label), "%s", items[i][0]);
			if (items[i][1]) {
				snprintf(it->value, sizeof(it->value), "%s", items[i][1]);
				it->flags = OV_ITEM_INFO;
			} else {
				it->flags = OV_ITEM_CLOSE;
			}
		}
		ext.menu[0].count = 7;
	}
	ext.log_text = "agent_app: agent 0, fat 1, mode 'stay'\nagent_app: network 0\n"
				   "agent_app: running, 5 s\nagent_app: running, 10 s\n"
				   "a very long line that goes on and on to show how the log view wraps its text\n";
	snprintf(ext.test[0], sizeof(ext.test[0]), "Buttons\tA, B held");
	snprintf(ext.test[1], sizeof(ext.test[1]), "Pointer\tx 312, y 188");
	snprintf(ext.test[2], sizeof(ext.test[2]), "Tilt\tpitch 12, roll -4");
	snprintf(ext.test[3], sizeof(ext.test[3]), "MotionPlus\tyaw 0.3, pitch -0.1");
	snprintf(ext.test[4], sizeof(ext.test[4]), "Nunchuk\tstick 0.40, 0.90");
	snprintf(ext.cal_result, sizeof(ext.cal_result), "Done: gyro rest offsets measured.");

	ov_init(&ui, W, H);
	for (i = 0; i < 6; ++i)
		ov_step(&ui, &ext, 0, act, NULL);
	capture(&ui, "00-opening");
	keys(&ui, "");
	capture(&ui, "01-bar");
	keys(&ui, "a");
	capture(&ui, "02-exit");
	keys(&ui, "bll");
	ov_step(&ui, &ext, OV_A, act, NULL);
	for (i = 0; i < 4; ++i)
		ov_step(&ui, &ext, 0, act, NULL);
	capture(&ui, "03-dev-sliding");
	keys(&ui, "");
	capture(&ui, "04-dev-actions");
	keys(&ui, "ra");
	capture(&ui, "05-dev-info");
	keys(&ui, "la" "dr" "a");
	capture(&ui, "06-dev-log");
	keys(&ui, "bb" "rrrr" "a");
	capture(&ui, "07-wiimote");
	keys(&ui, "a");
	capture(&ui, "08-wiimote-find");
	ext.remote[0].finding = false;
	keys(&ui, "ra");
	capture(&ui, "09-more");
	keys(&ui, "dla");
	capture(&ui, "10-test");
	keys(&ui, "bda");
	capture(&ui, "11-calibrate");
	keys(&ui, "bb" "d" "a");
	capture(&ui, "12-settings");
	keys(&ui, "bb" "l" "a");
	capture(&ui, "13-shot-toast");
	ext.toast[0] = 0;
	keys(&ui, "ll" "a");
	capture(&ui, "14-slot-menu");
	keys(&ui, "b");
	ext.toast[0] = 0;
	keys(&ui, "h");
	for (i = 0; i < 40 && ov_step(&ui, &ext, 0, act, NULL); ++i)
		;
	printf("closed after HOME: %s\n", i < 40 ? "yes" : "no");
	return 0;
}
