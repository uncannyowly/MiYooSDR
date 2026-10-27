/* Test driver: builds the app with its main() renamed so tests can reach the
 * internals headlessly (no framebuffer, keys fed through a pipe). Each scenario
 * prints its observations for tests/test_app.py.
 */
#define main miyoosdr_main
#include "../src/main.c"
#undef main

#include <pthread.h>

static int key_pipe[2] = { -1, -1 };

static void keys_open(void)
{
	if (pipe(key_pipe)) {
		perror("pipe");
		exit(1);
	}
	fcntl(key_pipe[0], F_SETFL, O_NONBLOCK);
	in_fd = key_pipe[0];
}

static void key(int code, int value)
{
	struct input_event ev[2];
	memset(ev, 0, sizeof(ev));
	ev[0].type = EV_KEY;
	ev[0].code = code;
	ev[0].value = value;
	ev[1].type = EV_SYN;
	if (write(key_pipe[1], ev, sizeof(ev)) != (ssize_t)sizeof(ev))
		exit(1);
}

static void tap(int code)
{
	key(code, 1);
	key(code, 0);
}

/* Is the string drawn somewhere on the canvas in one colour? Glyph pixels must
 * share a colour that the pixels around them (nearly all) do not have, so a
 * plain area does not count as any text.
 */
static int find_text(const char *s)
{
	int len = (int)strlen(s), x, y, k, r, b;
	for (y = 0; y + FONT_H <= SH; y++)
		for (x = 0; x + len * FONT_W <= SW; x++) {
			uint32_t colour = 0;
			int found = 0, ok = 1, around = 0, same = 0;
			for (k = 0; ok && k < len; k++)
				for (r = 0; ok && r < FONT_H; r++)
					for (b = 0; ok && b < FONT_W; b++) {
						uint32_t px = canvas[(y + r) * SW + x + k * FONT_W + b];
						if (!((font_bits[(unsigned char)s[k] & 0x7f][r] >> (7 - b)) & 1)) {
							around++;
							same += found && px == colour;
							continue;
						}
						if (!found) {
							colour = px;
							found = 1;
						}
						ok = px == colour;
					}
			if (ok && found && same * 10 < around)
				return 1;
		}
	return 0;
}

/* Presses a screen has to see after it opened (loading drops earlier ones). */
static void *press_b_twice(void *arg)
{
	(void)arg;
	usleep(400000);
	tap(KEY_LEFTCTRL);
	usleep(400000);
	tap(KEY_LEFTCTRL);
	return NULL;
}

static void *leave_scope_soon(void *arg) /* L2 after 0.7 s */
{
	(void)arg;
	usleep(700000);
	tap(KEY_TAB);
	return NULL;
}

static void *record_then_leave(void *arg) /* X at 0.2 s, X at 0.6 s, L2 at 0.8 s */
{
	(void)arg;
	usleep(200000);
	tap(KEY_LEFTSHIFT);
	usleep(400000);
	tap(KEY_LEFTSHIFT);
	usleep(200000);
	tap(KEY_TAB);
	return NULL;
}

/* 3000 samples a second: quiet at 45 dB, hits of 1 ms at 70 dB 20 ms apart */
static void feed_burst_pattern(struct scope_state *s, long *n, double from, double to, double burst_at)
{
	for (; *n < (long)(to * 3000); (*n)++) {
		double t = *n / 3000.0, in = t - burst_at;
		int hit = burst_at >= 0 && in >= 0 && in < 0.2 && fmod(in, 0.020) < 0.001 - 1e-9;
		(void)from;
		scope_feed(s, *n, t, hit ? 70.0f : 45.0f);
	}
}

static int load_or_report(const char *path, struct recording *r)
{
	char error[160];
	if (recording_load(path, r, error, sizeof(error)) == 0)
		return 0;
	printf("error: %s\n", error);
	return -1;
}

static int untouched(float *v, int n)
{
	int i, count = 0;
	for (i = 0; i < n; i++)
		count += v[i] == -999.0f;
	return count;
}

int main(int argc, char **argv)
{
	const char *what = argc > 1 ? argv[1] : "";
	int i;

	if (!strcmp(what, "zoom-mid-sweep")) { /* R2 arrives at the first poll inside a full-band sweep */
		fake = 1;
		view_full();
		make_lut();
		keys_open();
		tap(KEY_BACKSPACE);
		for (i = 0; i < MAXBINS; i++)
			hold_db[i] = -999.0f;
		have_hold = 0;
		do_sweep();
		printf("level %d untouched %d of %d\n", vw.level, untouched(hold_db, vw.nb), vw.nb);
		do_sweep();
		printf("next sweep untouched %d\n", untouched(hold_db, vw.nb));
	} else if (!strcmp(what, "gain-mid-sweep")) { /* R1 arrives at bin 31 of a zoomed sweep */
		double worst = 0;
		fake = 1;
		view_zoom(2440.0, 1);
		igi = 0x28;
		keys_open();
		tap(KEY_T);
		do_sweep();
		for (i = 0; i < vw.nb; i++) {
			double used = sweep_db[i] - 10.0 * log10((double)raw_v[i]);
			if (i >= vw.dc_lo && i <= vw.dc_hi) /* bridged over the LO leak */
				continue;
			if (fabs(used - 0x28) > worst)
				worst = fabs(used - 0x28);
		}
		printf("gain now %02X worst %.3f\n", igi, worst);
	} else if (!strcmp(what, "peak")) {
		view_full();
		for (i = 0; i < MAXBINS; i++)
			hold_db[i] = base_db[i] = 10.0f;
		hold_db[100] = 40.0f;
		have_hold = have_base = 1;
		find_peak();
		printf("%s\n", status_msg);
	} else if (!strcmp(what, "zoom-keeps-freq")) {
		double f;
		view_full();
		cursor = 100;
		f = vw.f0 + cursor * vw.bw; /* the frequency that bin measures */
		for (i = 0; i < 3; i++)
			on_button(KEY_BACKSPACE);
		printf("level %d cursor %.4f was %.4f half-bin %.4f\n", vw.level, vw.f0 + cursor * vw.bw, f, vw.bw / 2);
	} else if (!strcmp(what, "pan-left")) {
		set_view_zoom(2402.0, 1);
		for (i = 0; i < 6; i++) {
			cursor = 0;
			on_button(KEY_LEFT);
			printf("tone0 %d last %d half %d f0 %.3f\n", vw.tone0, vw.tone0 + vw.nb - 1, (128 << vw.sel) / 2, vw.f0);
		}
	} else if (!strcmp(what, "autorepeat") && argc > 2) { /* X held past the repeat delay */
		if (chdir(argv[2]))
			return 1;
		keys_open();
		key(KEY_LEFTSHIFT, 1);
		key(KEY_LEFTSHIFT, 2);
		key(KEY_LEFTSHIFT, 0);
		poll_input();
		printf("recording %d\n", rec != NULL);
		if (rec)
			toggle_rec();
	} else if (!strcmp(what, "replay-status") && argc > 2) {
		static unsigned bins[3][PACKET_W];
		struct packet_capture capture;
		char error[160];
		if (pcap_load(argv[2], &capture, error, sizeof(error)))
			return 1;
		status("screenshot saved: shots/x.png");
		packet_render(&capture, argv[2], 0, 0, 1, 0, bins, 0);
		printf("status shown %d\n", find_text("screenshot saved"));
		pcap_free(&capture);
	} else if (!strcmp(what, "shots") && argc > 2) {
		DIR *d;
		struct dirent *e;
		int n = 0;
		if (chdir(argv[2]))
			return 1;
		for (i = 0; i < 3; i++)
			save_screenshot();
		if ((d = opendir("shots")) != NULL) {
			while ((e = readdir(d)) != NULL)
				n += e->d_name[0] != '.';
			closedir(d);
		}
		printf("shots %d\n", n);
	} else if (!strcmp(what, "list") && argc > 2) {
		struct rec_entry *list;
		size_t n, k;
		if (chdir(argv[2]))
			return 1;
		n = list_recordings(&list);
		for (k = 0; k < n; k++)
			printf("%c %s\n", "CPS"[list[k].kind], list[k].path);
		free(list);
	} else if (!strcmp(what, "scope-replay") && argc > 3) { /* render one replay frame, read its burst line */
		make_lut();
		if (scope_replay(argv[2], "/dev/null", atof(argv[3])))
			return 1;
		printf("burst %d hits %d gap %d\n", find_text("LAST BURST #1: 181ms"), find_text("10 hits"),
		       find_text("every 20ms"));
	} else if (!strcmp(what, "replay-back") && argc > 2) { /* --replay FILE, then B, then B */
		pthread_t presser;
		char left;
		keys_open();
		pthread_create(&presser, NULL, press_b_twice, NULL);
		run_offline(argv[2], NULL, 0);
		pthread_join(presser, NULL);
		printf("browser %d drained %d\n", find_text("REVIEW:"), read(key_pipe[0], &left, 1) < 0);
	} else if (!strcmp(what, "csv-load") && argc > 2) {
		struct recording r;
		size_t k;
		if (load_or_report(argv[2], &r))
			return 0;
		printf("sections %zu rows %zu truncated %d duration %.3f\n", r.nsections, r.rows, r.truncated, r.duration);
		for (k = 0; k < r.nsections; k++)
			printf("section level %d nb %d f0 %.4f bw %.5f ch %d first %zu rows %zu\n", r.sections[k].level,
			       r.sections[k].nb, r.sections[k].f0, r.sections[k].bw, r.sections[k].ch, r.sections[k].first,
			       r.sections[k].rows);
		recording_free(&r);
	} else if (!strcmp(what, "csv-show") && argc > 3) {
		struct recording r;
		int y;
		if (load_or_report(argv[2], &r))
			return 0;
		recording_show(&r, (size_t)atoi(argv[3]));
		printf("view level %d nb %d f0 %.4f bw %.5f ch %d wf_rows %d\n", vw.level, vw.nb, vw.f0, vw.bw, vw.ch,
		       wf_rows);
		printf("wf");
		for (y = 0; y < wf_rows && y < 6; y++)
			printf(" %d", wf[(wf_head + y) % WHGT][0]);
		printf("\n");
		recording_free(&r);
	} else if (!strcmp(what, "csv-frame") && argc > 3) { /* the rows below the waterfall belong to review */
		struct recording r;
		int footer = 0;
		if (load_or_report(argv[2], &r))
			return 0;
		make_lut();
		in_review = 1;
		recording_show(&r, (size_t)atoi(argv[3]));
		render();
		for (i = (WY0 + WHGT) * SW; i < SW * SH; i++)
			footer += canvas[i] != C_BG;
		review_draw(&r, (size_t)atoi(argv[3]), 1, 0, "x.csv");
		printf("render footer pixels %d review help %d\n", footer, find_text("A play"));
		recording_free(&r);
	} else if (!strcmp(what, "persist")) { /* a steady 70 dB carrier in bin 10 over a 45 dB floor */
		int r, peak_row = -1, s;
		unsigned hot = 0;
		view_full();
		db_low = 40.0f;
		for (s = 0; s < 40; s++) {
			for (i = 0; i < vw.nb; i++)
				sweep_db[i] = i == 10 ? 70.0f : 45.0f;
			persist_add();
		}
		for (r = 0; r < WHGT; r++)
			if (persist[10][r] > hot) {
				hot = persist[10][r];
				peak_row = r;
			}
		printf("peak row %d of %d\n", peak_row, WHGT);
		for (s = 0; s < 200; s++) { /* the carrier goes away */
			for (i = 0; i < vw.nb; i++)
				sweep_db[i] = 45.0f;
			persist_add();
		}
		printf("decayed %u%%\n", hot ? (unsigned)(100.0 * persist[10][peak_row] / hot) : 999);
		db_low = 43.0f; /* a new level range starts the picture over */
		persist_add();
		printf("after range change %u\n", (unsigned)persist[10][peak_row]);
	} else if (!strcmp(what, "persist-panel")) {
		make_lut();
		view_full();
		disp_mode = 2;
		sweeps = 3;
		render();
		printf("title %d\n", find_text("PERSISTENCE"));
	} else if (!strcmp(what, "panels")) { /* SELECT + B, three times */
		for (i = 0; i < 3; i++) {
			sel_layer(KEY_LEFTCTRL);
			printf("%d ", disp_mode);
		}
		printf("\n");
	} else if (!strcmp(what, "scope-feed")) { /* two bursts of 10 hits, 1.3 s apart */
		struct scope_state s;
		long n = 0;
		memset(&s, 0, sizeof(s));
		feed_burst_pattern(&s, &n, 0, 1.0, -1);
		feed_burst_pattern(&s, &n, 1.0, 2.3, 1.0);
		feed_burst_pattern(&s, &n, 2.3, 3.0, 2.3);
		printf("bursts %ld hits %d gap %.2f len %.2f interval %.3f\n", s.bursts, s.last_hits, s.last_gap * 1000,
		       s.last_len * 1000, s.interval);
	} else if (!strcmp(what, "scope-sparse")) { /* 21 readings 0.5 ms apart across a 10 ms panel */
		struct scope_state s;
		int x, y, first = -1, last = -1, covered = 0;
		memset(&s, 0, sizeof(s));
		make_lut();
		db_low = 40.0f;
		for (i = 0; i <= 20; i++) {
			scope_ring[i].t = i * 0.0005;
			scope_ring[i].db10 = (int16_t)(i % 2 ? 700 : 500);
		}
		scope_trace(WY0, WHGT - 18, 0, 21, 0.0, 0.0105, &s);
		for (x = 0; x < PW; x++) {
			int drawn = 0;
			for (y = WY0; y < WY0 + WHGT - 18 && !drawn; y++) {
				uint32_t c = canvas[y * SW + PX0 + x];
				drawn = c != C_PANEL && c != C_GRID;
			}
			if (drawn) {
				covered++;
				if (first < 0)
					first = x;
				last = x;
			}
		}
		printf("covered %d of %d\n", covered, last - first + 1);
	} else if (!strcmp(what, "scope-enter")) { /* R2 at the 5 MHz zoom */
		view_zoom(2440.0, 3);
		cursor = 64;
		on_button(KEY_BACKSPACE);
		printf("scope request %d at %.4f\n", scope_request, scope_request_mhz);
	} else if (!strcmp(what, "scope-run")) {
		pthread_t presser;
		fake = 1;
		make_lut();
		keys_open();
		pthread_create(&presser, NULL, leave_scope_soon, NULL);
		scope_session(2447.3, NULL);
		pthread_join(presser, NULL);
		printf("samples %ld rate %.0f level %d cursor %.4f\n", scope_samples, scope_samples / scope_seconds,
		       vw.level, bin_mhz(cursor));
	} else if (!strcmp(what, "scope-rec") && argc > 2) {
		pthread_t presser;
		if (chdir(argv[2]))
			return 1;
		fake = 1;
		make_lut();
		keys_open();
		pthread_create(&presser, NULL, record_then_leave, NULL);
		scope_session(2447.3, NULL);
		pthread_join(presser, NULL);
		printf("samples %ld\n", scope_samples);
	} else if (!strcmp(what, "settings-panel") && argc > 2) {
		if (chdir(argv[2]))
			return 1;
		view_full();
		settings_load();
		printf("panel %d\n", disp_mode);
	} else {
		fprintf(stderr, "unknown scenario %s\n", what);
		return 2;
	}
	return 0;
}
