/*
 * MiYooSDR: a live 2.4 GHz spectrum and waterfall on the Miyoo Mini Plus, read
 * from its own RTL8188FU Wi-Fi chip. EXPERIMENTAL: this started as a spike to
 * find out whether the idea works at all, and the code still shows it. It is
 * receive-only (it never transmits) and restores Wi-Fi when it exits.
 *
 * Radio: the chip's baseband has a PSD (power spectral density) engine. Write a
 * tone index to BB 0x808[9:0], raise 0x808[22], and 0x8B4[15:0] holds the linear
 * power of that FFT bin (read it with the bit still high). 0x808[15:14] selects
 * the FFT size, 128..1024 points over 40 MHz (312.5 .. 39 kHz per bin); a result
 * is ready under 100 us after the trigger at 128 points and in 400-800 us at
 * 1024. Registers go through the vendor driver's /proc read_reg/write_reg; the
 * channel through its proc "monitor" entry ("ch offset bw"). launch.sh stops
 * Wi-Fi management first so nothing scans or retunes behind our back.
 *
 * Views: full band = channels 1, 5, 9, 13, middle 20 MHz of each (tones -32..31
 * at 128 points), 256 bins over 2402.0 .. 2481.7 MHz. Zoom = one channel, 128
 * bins at 256/512/1024 points: 20, 10 or 5 MHz wide around the cursor.
 *
 * Modes: (none) = app on /dev/fb0; --dump N = N sweeps as text; --probe = PSD
 * latency per FFT size; --pm-active = switch off the driver's power saving and
 * exit (launch.sh runs it while still associated); --fake = synthetic radio for
 * the PC build; --shot FILE = render one frame to a PPM and exit; --zoom MHZ LEVEL
 * = start zoomed.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/wireless.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#endif

#include "font.h"

#define PROC "/proc/net/rtl8188fu/wlan0/"
#define MAXBINS 256

static volatile sig_atomic_t quit;
static int fake;

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* the Mini's usleep is ~9 ms at best, so short waits spin */
static void spin_us(int us)
{
	double end = now_s() + us / 1e6;
	while (now_s() < end)
		;
}

/* ------------------------------------------------------------------ radio */

static int fd_rr_cfg = -1, fd_rr = -1, fd_wr = -1, fd_mon = -1;
static unsigned rr_addr = ~0u;
static unsigned orig_808, orig_c50, orig_rf18, rf18_base, psd_cfg;
static int radio_ok, raw_tune, radio_dead, tuned_ch = -1;
static int psd_wait_us[4] = { 150, 250, 500, 1000 }; /* per FFT size, measured */
static long reg_ops, io_errors, sweeps;

/* after a USB timeout the driver drops the device; stop instead of hammering it */
static void io_error(const char *what, unsigned addr)
{
	if (io_errors++ == 0)
		fprintf(stderr, "first I/O error (%s %x) at sweep %ld: %s\n", what, addr, sweeps, strerror(errno));
	if (io_errors > 20 && !radio_dead) {
		radio_dead = 1;
		quit = 1;
		fprintf(stderr, "radio stopped responding, giving up\n");
	}
}

static void reg_write(unsigned addr, unsigned val, int len)
{
	char b[48];
	int n = snprintf(b, sizeof(b), "%x %x %x", addr, val, len);
	reg_ops++;
	if (write(fd_wr, b, n) != n)
		io_error("write_reg", addr);
}

static unsigned reg_read32(unsigned addr)
{
	char b[96], *eq;
	ssize_t n;

	if (addr != rr_addr) {
		n = snprintf(b, sizeof(b), "%x 4", addr);
		if (write(fd_rr_cfg, b, n) != n)
			return 0;
		rr_addr = addr;
	}
	reg_ops++;
	lseek(fd_rr, 0, SEEK_SET);
	n = read(fd_rr, b, sizeof(b) - 1);
	if (n <= 0) {
		io_error("read_reg", addr);
		return 0;
	}
	b[n] = 0;
	eq = strchr(b, '=');
	return eq ? (unsigned)strtoul(eq + 1, NULL, 16) : 0;
}

static void rf_write(unsigned reg, unsigned val)
{
	reg_write(0x840, (((reg & 0xff) << 20) | (val & 0xfffff)) & 0x0fffffff, 4);
}

/* RF registers are only readable through the driver's dump */
static int rf_read_dump(unsigned reg, unsigned *val)
{
	char line[256];
	FILE *f = fopen(PROC "rf_reg_dump", "r");
	int found = 0;

	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		unsigned base, v[4];
		if (sscanf(line, " 0x%x 0x%x 0x%x 0x%x 0x%x", &base, &v[0], &v[1], &v[2], &v[3]) == 5 &&
		    reg >= base && reg < base + 4) {
			*val = v[reg - base];
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

/*
 * The driver's power saving has private ioctls: "pm_set ips=N" (idle power
 * save) and "pm_set lps=N" (802.11 power save, 0 = always awake), what
 * `iwpriv wlan0 pm_set ...` sends. Onion has no iwpriv, so this looks the
 * command up and sends it the same way.
 */
static int wext_priv_set(const char *ifname, const char *name, const char *arg)
{
#ifdef __linux__
	static struct iw_priv_args priv[256];
	struct iwreq wrq;
	char buf[128];
	int sock, n, k, ret = -1;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return -1;
	memset(&wrq, 0, sizeof(wrq));
	strncpy(wrq.ifr_name, ifname, IFNAMSIZ - 1);
	wrq.u.data.pointer = priv;
	wrq.u.data.length = 256;
	if (ioctl(sock, SIOCGIWPRIV, &wrq) < 0) {
		fprintf(stderr, "SIOCGIWPRIV: %s\n", strerror(errno));
		goto out;
	}
	n = wrq.u.data.length;
	for (k = 0; k < n && strcmp(priv[k].name, name); k++)
		;
	if (k == n) {
		fprintf(stderr, "private ioctl %s not found\n", name);
		goto out;
	}
	snprintf(buf, sizeof(buf), "%s", arg);
	memset(&wrq, 0, sizeof(wrq));
	strncpy(wrq.ifr_name, ifname, IFNAMSIZ - 1);
	wrq.u.data.pointer = buf;
	wrq.u.data.length = (unsigned short)(strlen(buf) + 1);
	if (priv[k].cmd < SIOCDEVPRIVATE) { /* sub-ioctl: find the parent with the same args */
		int j;
		for (j = 0; j < n; j++)
			if (priv[j].name[0] == 0 && priv[j].set_args == priv[k].set_args &&
			    priv[j].get_args == priv[k].get_args)
				break;
		if (j == n)
			goto out;
		wrq.u.data.flags = (unsigned short)priv[k].cmd;
		k = j;
	}
	ret = ioctl(sock, priv[k].cmd, &wrq);
	if (ret < 0)
		fprintf(stderr, "%s %s: %s\n", name, arg, strerror(errno));
out:
	close(sock);
	return ret;
#else
	return -1;
#endif
}

static int read_param(const char *name, int dflt)
{
	char path[128];
	int v = dflt;
	FILE *f;
	snprintf(path, sizeof(path), "/sys/module/8188fu/parameters/%s", name);
	f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = dflt;
		fclose(f);
	}
	return v;
}

static void pm_active(void)
{
	int a = wext_priv_set("wlan0", "pm_set", "lps=0");
	int b = wext_priv_set("wlan0", "pm_set", "ips=0");
	fprintf(stderr, "power save off: lps=0 -> %d, ips=0 -> %d\n", a, b);
}

static void pm_restore(void)
{
	char a[16];
	snprintf(a, sizeof(a), "ips=%d", read_param("rtw_ips_mode", 1));
	wext_priv_set("wlan0", "pm_set", a);
	snprintf(a, sizeof(a), "lps=%d", read_param("rtw_power_mgnt", 2));
	wext_priv_set("wlan0", "pm_set", a);
}

static int radio_open(void)
{
	pm_active();
	spin_us(20000);
	fd_rr_cfg = open(PROC "read_reg", O_WRONLY);
	fd_rr = open(PROC "read_reg", O_RDONLY);
	fd_wr = open(PROC "write_reg", O_WRONLY);
	fd_mon = open(PROC "monitor", O_WRONLY);
	if (fd_rr_cfg < 0 || fd_rr < 0 || fd_wr < 0) {
		fprintf(stderr, "no RTL8188FU proc entries (%s)\n", strerror(errno));
		return -1;
	}
	orig_808 = reg_read32(0x808);
	orig_c50 = reg_read32(0xc50) & 0xff;
	if (!rf_read_dump(0x18, &orig_rf18))
		orig_rf18 = 0x00c0b;
	rf18_base = orig_rf18 & ~0xffu;
	/* keep the BB table's other PSD bits (0x808 = 0xFC00); size goes in 15:14 */
	psd_cfg = (orig_808 & 0xFFBF0000u) | (0x0Fu << 10);
	fprintf(stderr, "radio: 0x808=%x 0xc50=%x rf18=%x\n", orig_808, orig_c50, orig_rf18);
	radio_ok = 1;
	return 0;
}

/* default: the driver's own channel switch (proc "monitor": "ch offset bw");
 * --raw-tune pokes RF 0x18 through the LSSI port instead (faster, but a raw
 * test run hung the chip's USB link) */
static void tune(int ch)
{
	if (raw_tune || fd_mon < 0) {
		rf_write(0x18, rf18_base | (unsigned)ch);
	} else {
		char b[16];
		int n = snprintf(b, sizeof(b), "%d 0 0", ch);
		reg_ops++;
		if (write(fd_mon, b, n) != n)
			io_error("monitor", (unsigned)ch);
	}
	tuned_ch = ch;
	spin_us(300);
}

static void radio_restore(void)
{
	if (!radio_ok)
		return;
	if (!radio_dead) {
		reg_write(0x808, orig_808, 4);
		reg_write(0xc50, orig_c50, 1);
		if (raw_tune || fd_mon < 0)
			rf_write(0x18, orig_rf18);
		else
			tune((int)(orig_rf18 & 0xff));
		pm_restore();
	}
	radio_ok = 0;
}

static void set_igi(int igi)
{
	reg_write(0xc50, (orig_c50 & 0x80) | (igi & 0x7f), 1);
}

/* sel: FFT size 128 << sel */
static unsigned psd_point(int tone, int sel)
{
	unsigned cfg = (psd_cfg & ~(3u << 14)) | ((unsigned)sel << 14);
	unsigned t = (unsigned)tone & ((128u << sel) - 1);
	reg_write(0x808, cfg | t, 4);
	reg_write(0x808, cfg | t | (1u << 22), 4);
	spin_us(psd_wait_us[sel]);
	return reg_read32(0x8b4) & 0xffff;
}

/* ------------------------------------------------------------- fake radio */

static unsigned fake_rng = 12345;
static double frand(void)
{
	fake_rng = fake_rng * 1103515245u + 12345u;
	return (fake_rng >> 8) / 16777216.0;
}

static unsigned fake_point(int ch, int tone, int sel)
{
	double bw = 0.3125 / (1 << sel), f = 2407 + 5 * ch + tone * bw, p = 25 * (0.5 + frand());
	double t = now_s();
	if (frand() < 0.03 && fabs(f - 2462) < 10) /* bursty Wi-Fi on ch 11 */
		p += 400 * frand();
	if (fmod(t, 1.0) < 0.5 && frand() < 0.5 && fabs(f - 2447.3) < 0.5) /* a "remote" */
		p += 3000 * (0.6 + 0.4 * frand()) * exp(-pow((f - 2447.3) / 0.25, 2));
	if (abs(tone) < 2 << sel)
		p += 400;
	return (unsigned)p;
}

/* ------------------------------------------------------------------ views */

struct view {
	int level;   /* 0 = full band, 1..3 = zoom 20/10/5 MHz */
	int nb;      /* bins shown */
	double f0;   /* MHz of bin 0 */
	double bw;   /* MHz per bin */
	int sel;     /* FFT size 128 << sel */
	int ch;      /* zoom: channel; full band: unused */
	int tone0;   /* zoom: tone of bin 0 */
	int dc_lo, dc_hi; /* bins bridged over the LO leak (dc_lo > dc_hi: none) */
};

static const int full_ch[4] = { 1, 5, 9, 13 };
static struct view vw;

static double ch_mhz(int ch) { return 2407.0 + 5.0 * ch; }

static void view_full(void)
{
	memset(&vw, 0, sizeof(vw));
	vw.nb = 256;
	vw.f0 = 2402.0;
	vw.bw = 0.3125;
	vw.dc_lo = 1;
	vw.dc_hi = 0;
}

/* zoom on target MHz, 2.5-7.5 MHz off the LO so the DC leak stays away from it */
static void view_zoom(double target, int level)
{
	int ch, best = 1;
	double bestscore = 1e9;

	if (level < 1) level = 1;
	if (level > 3) level = 3;
	for (ch = 1; ch <= 13; ch++) {
		double off = target - ch_mhz(ch), score = fabs(fabs(off) - 5.0);
		if (score < bestscore) {
			bestscore = score;
			best = ch;
		}
	}
	memset(&vw, 0, sizeof(vw));
	vw.level = level;
	vw.nb = 128;
	vw.sel = level;
	vw.bw = 0.3125 / (1 << level);
	vw.ch = best;
	vw.tone0 = (int)lround((target - ch_mhz(best)) / vw.bw) - 64;
	vw.f0 = ch_mhz(best) + vw.tone0 * vw.bw;
	{ /* LO leakage covers roughly +-0.7 MHz */
		int dcw = (int)ceil(0.7 / vw.bw);
		vw.dc_lo = -dcw - vw.tone0;
		vw.dc_hi = dcw - vw.tone0;
	}
}

/* ------------------------------------------------------------------ sweep */

static float sweep_db[MAXBINS], hold_db[MAXBINS], avg_db[MAXBINS], base_db[MAXBINS];
static unsigned raw_v[MAXBINS];
static int igi = 0x28, have_hold, have_avg, have_base;
static long zeros, stuck_sweeps, remedies;
static char status_msg[200];
static double status_until;

static void poll_input(void);
static void status(const char *msg);

static float to_db(unsigned v, float prev)
{
	if (v == 0) { /* occasional dropout: keep the previous reading */
		zeros++;
		return prev;
	}
	return (float)(10.0 * log10((double)v) + igi);
}

static void bridge(float *out, int nb, int lo, int hi)
{
	int i;
	float a, b;
	if (lo > hi || hi < 0 || lo >= nb)
		return;
	a = out[lo - 1 >= 0 ? lo - 1 : (hi + 1 < nb ? hi + 1 : 0)];
	b = out[hi + 1 < nb ? hi + 1 : (lo - 1 >= 0 ? lo - 1 : 0)];
	for (i = lo < 0 ? 0 : lo; i <= hi && i < nb; i++)
		out[i] = a + (b - a) * (float)(i - lo + 1) / (float)(hi - lo + 2);
}

static int distinct_count(const unsigned *v, int n)
{
	int i, j, d = 0;
	for (i = 0; i < n; i++) {
		for (j = 0; j < i && v[j] != v[i]; j++)
			;
		if (j == i && ++d > 8)
			break;
	}
	return d;
}

/*
 * Sometimes the PSD engine freezes and every tone reads the same value. Two
 * frozen sweeps in a row: try a fix, rotating through the candidates, and log
 * which one helped (the cause is not known yet).
 */
static void check_stuck(int frozen)
{
	static int run, last_remedy = -1;
	char m[96];

	if (fake)
		return;
	if (!frozen) {
		if (last_remedy >= 0) {
			fprintf(stderr, "stuck: recovered after remedy %d at sweep %ld\n", last_remedy, sweeps);
			last_remedy = -1;
		}
		run = 0;
		return;
	}
	stuck_sweeps++;
	if (++run < 2)
		return;
	run = 0;
	last_remedy = (int)(remedies++ % 3);
	fprintf(stderr, "stuck at sweep %ld: remedy %d\n", sweeps, last_remedy);
	switch (last_remedy) {
	case 0: pm_active(); break;
	case 1: tune(tuned_ch == 7 ? 3 : 7); tuned_ch = -1; break;
	case 2: reg_write(0x808, orig_808, 4); spin_us(2000); break;
	}
	snprintf(m, sizeof(m), "radio readings frozen, trying fix %d", last_remedy);
	status(m);
}

static void do_sweep(void)
{
	int j, k, frozen = 0;

	if (vw.level == 0) {
		for (j = 0; j < 4 && !quit; j++) {
			float *out = sweep_db + j * 64;
			if (!fake) {
				set_igi(igi);
				tune(full_ch[j]);
				(void)psd_point(0, 0); /* settle */
			}
			for (k = -32; k < 32; k++) {
				int idx = j * 64 + k + 32;
				unsigned v = fake ? fake_point(full_ch[j], k, 0) : psd_point(k, 0);
				raw_v[idx] = v;
				out[k + 32] = to_db(v, sweeps ? sweep_db[idx] : (float)igi);
			}
			bridge(out, 64, 32 - 2, 32 + 2); /* LO leak with notches beside it */
			frozen += distinct_count(raw_v + j * 64, 64) <= 3;
			poll_input();
		}
		frozen = frozen >= 3;
	} else {
		if (!fake) {
			set_igi(igi);
			if (tuned_ch != vw.ch) {
				tune(vw.ch);
				(void)psd_point(0, vw.sel);
			}
		}
		for (k = 0; k < vw.nb && !quit; k++) {
			unsigned v = fake ? fake_point(vw.ch, vw.tone0 + k, vw.sel) : psd_point(vw.tone0 + k, vw.sel);
			raw_v[k] = v;
			sweep_db[k] = to_db(v, sweeps ? sweep_db[k] : (float)igi);
			if ((k & 31) == 31)
				poll_input();
		}
		bridge(sweep_db, vw.nb, vw.dc_lo, vw.dc_hi);
		frozen = distinct_count(raw_v, vw.nb) <= 3;
	}
	check_stuck(frozen);

	for (j = 0; j < vw.nb; j++) {
		float s = sweep_db[j];
		if (!have_hold || s > hold_db[j])
			hold_db[j] = s;
		avg_db[j] = have_avg ? 0.9f * avg_db[j] + 0.1f * s : s;
		/* background: follows dips fast and rises slowly, so bursts stand out */
		if (!have_base)
			base_db[j] = s;
		else
			base_db[j] += (s < base_db[j] ? 0.2f : 0.02f) * (s - base_db[j]);
	}
	have_hold = have_avg = have_base = 1;
	sweeps++;
}

/* one log line: raw PSD spread of the last sweep plus chip state */
static void diag(const char *tag)
{
	unsigned mn = ~0u, mx = 0;
	int i;

	if (fake)
		return;
	for (i = 0; i < vw.nb; i++) {
		if (raw_v[i] < mn) mn = raw_v[i];
		if (raw_v[i] > mx) mx = raw_v[i];
	}
	fprintf(stderr, "diag %s sweep %ld view %d: raw min %u max %u distinct>=%d stuck %ld | 808=%x 8b4=%x c50=%x ioerr %ld\n",
	        tag, sweeps, vw.level, mn, mx, distinct_count(raw_v, vw.nb), stuck_sweeps, reg_read32(0x808),
	        reg_read32(0x8b4), reg_read32(0xc50), io_errors);
}

/* --------------------------------------------------------------- display */

#define SW 640
#define SH 480
static uint32_t canvas[SW * SH];

#define C_BG 0x0b0f14
#define C_PANEL 0x111821
#define C_GRID 0x1f2a36
#define C_AXIS 0x4a5a6b
#define C_TEXT 0xc8d3df
#define C_DIM 0x7d8b99
#define C_LIVE 0x3ddc84
#define C_HOLD 0xffcc33
#define C_AVG 0x56b6ff
#define C_CURSOR 0xff5c8a
#define C_REC 0xff4040
#define C_ZOOM 0x8a7dff

static void fill(int x, int y, int w, int h, uint32_t c)
{
	int i, j;
	for (j = y < 0 ? 0 : y; j < y + h && j < SH; j++)
		for (i = x < 0 ? 0 : x; i < x + w && i < SW; i++)
			canvas[j * SW + i] = c;
}

static void text(int x, int y, uint32_t c, const char *s)
{
	for (; *s; s++, x += FONT_W) {
		int r, b;
		unsigned ch = (unsigned char)*s & 0x7f;
		for (r = 0; r < FONT_H; r++)
			for (b = 0; b < FONT_W; b++)
				if ((font_bits[ch][r] >> (7 - b)) & 1) {
					int px = x + b, py = y + r;
					if (px >= 0 && px < SW && py >= 0 && py < SH)
						canvas[py * SW + px] = c;
				}
	}
}

static void line(int x0, int y0, int x1, int y1, uint32_t c)
{
	int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	for (;;) {
		if (x0 >= 0 && x0 < SW && y0 >= 0 && y0 < SH)
			canvas[y0 * SW + x0] = c;
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

/* plot geometry */
#define PX0 64
#define PW 512
#define SY0 22
#define SHGT 186
#define WY0 244
#define WHGT 216
#define DB_SPAN 48.0f

static float db_low = 40.0f, enh_low = -8.0f;
static int scaled, cursor = 128, show_hold = 1, enhance;
static int spec_style = 1;      /* 0 line, 1 filled, 2 filled + Wi-Fi bands */
static int palette;             /* index into palettes[] */
static int sel_down, sel_combo; /* SELECT used as a shift layer */
static uint8_t wf[WHGT][MAXBINS];
static int wf_head, wf_rows;
static uint32_t lut[256];

static int bin_x(int i) { return PX0 + i * PW / vw.nb; }

/* colour palettes: five {pos,r,g,b} stops each (0..1), shared by the filled
 * spectrum and the waterfall */
#define NPAL 4
static const char *pal_name[NPAL] = { "Ember", "Spectrum", "Ice", "Matrix" };
static const float palettes[NPAL][5][4] = {
	{ {0.00f,0.03f,0.04f,0.10f},{0.30f,0.20f,0.05f,0.45f},{0.60f,0.85f,0.25f,0.30f},{0.85f,1.00f,0.65f,0.10f},{1.00f,1.00f,0.98f,0.75f} },
	{ {0.00f,0.02f,0.02f,0.12f},{0.25f,0.00f,0.55f,0.90f},{0.50f,0.10f,0.85f,0.35f},{0.75f,0.95f,0.90f,0.10f},{1.00f,1.00f,0.15f,0.10f} },
	{ {0.00f,0.02f,0.03f,0.07f},{0.35f,0.05f,0.20f,0.60f},{0.70f,0.20f,0.75f,0.95f},{0.90f,0.70f,0.95f,1.00f},{1.00f,1.00f,1.00f,1.00f} },
	{ {0.00f,0.00f,0.03f,0.00f},{0.40f,0.00f,0.30f,0.05f},{0.75f,0.10f,0.85f,0.20f},{0.92f,0.60f,1.00f,0.50f},{1.00f,0.90f,1.00f,0.80f} },
};

static uint32_t rgbf(float r, float g, float b)
{
	int R = r < 0 ? 0 : r > 1 ? 255 : (int)(r * 255);
	int G = g < 0 ? 0 : g > 1 ? 255 : (int)(g * 255);
	int B = b < 0 ? 0 : b > 1 ? 255 : (int)(b * 255);
	return (uint32_t)(R << 16 | G << 8 | B);
}

static uint32_t pal_color(float t) /* 0..1 through the active palette */
{
	const float (*st)[4] = palettes[palette];
	int s = 0;
	float u;
	if (t < 0) t = 0;
	if (t > 1) t = 1;
	while (s < 3 && t > st[s + 1][0])
		s++;
	u = (t - st[s][0]) / (st[s + 1][0] - st[s][0]);
	return rgbf(st[s][1] + u * (st[s + 1][1] - st[s][1]),
	            st[s][2] + u * (st[s + 1][2] - st[s][2]),
	            st[s][3] + u * (st[s + 1][3] - st[s][3]));
}

static void make_lut(void)
{
	int i;
	for (i = 0; i < 256; i++)
		lut[i] = pal_color(i / 255.0f);
}

static uint32_t cscale(uint32_t c, float f) /* dim or brighten a colour */
{
	int r = (int)(((c >> 16) & 0xff) * f);
	int g = (int)(((c >> 8) & 0xff) * f);
	int b = (int)((c & 0xff) * f);
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	return (uint32_t)(r << 16 | g << 8 | b);
}

/* what gets drawn: absolute dB, or dB above the background in enhance mode */
static float shown(const float *v, int i) { return enhance ? v[i] - base_db[i] : v[i]; }
static float low(void) { return enhance ? enh_low : db_low; }

static int db_to_y(float db)
{
	float t = (db - low()) / DB_SPAN;
	if (t < 0) t = 0;
	if (t > 1) t = 1;
	return SY0 + SHGT - 1 - (int)(t * (SHGT - 1));
}

static void push_waterfall(void)
{
	int i;
	wf_head = (wf_head + WHGT - 1) % WHGT;
	for (i = 0; i < vw.nb; i++) {
		float t = enhance ? (sweep_db[i] - base_db[i]) / 24.0f : (sweep_db[i] - db_low) / DB_SPAN;
		wf[wf_head][i] = (uint8_t)(t < 0 ? 0 : t > 1 ? 255 : t * 255);
	}
	if (wf_rows < WHGT)
		wf_rows++;
}

static void autoscale(void)
{
	float tmp[MAXBINS];
	int i, j, n = vw.nb;
	memcpy(tmp, sweep_db, sizeof(float) * n);
	for (i = 1; i < n; i++)
		for (j = i; j > 0 && tmp[j - 1] > tmp[j]; j--) {
			float t = tmp[j]; tmp[j] = tmp[j - 1]; tmp[j - 1] = t;
		}
	db_low = floorf(tmp[n / 2] / 3.0f) * 3.0f - 6.0f;
	scaled = 1;
}

static void reset_traces(void)
{
	have_hold = have_avg = have_base = 0;
	wf_rows = 0;
	scaled = 0;
}

static FILE *rec;
static char rec_name[160];
static double rate;

/* standard 2.4 GHz Wi-Fi channels shaded behind the spectrum in the bands view */
static const int band_chs[3] = { 1, 6, 11 };
static const uint32_t band_tint[3] = { 0x101d33, 0x10301c, 0x2f2710 };

static void draw_band_tint(void)
{
	int c, i, y;
	if (vw.level != 0)
		return;
	for (c = 0; c < 3; c++) {
		int xa = PX0 + (int)((ch_mhz(band_chs[c]) - 11.0 - vw.f0) / vw.bw * PW / vw.nb);
		int xb = PX0 + (int)((ch_mhz(band_chs[c]) + 11.0 - vw.f0) / vw.bw * PW / vw.nb);
		if (xa < PX0) xa = PX0;
		if (xb > PX0 + PW) xb = PX0 + PW;
		for (y = SY0; y < SY0 + SHGT; y++)
			for (i = xa; i < xb; i++)
				canvas[y * SW + i] = band_tint[c];
	}
}

/* filled multi-colour spectrum: a vertical gradient under each bin, brightest
 * at the trace, with a lit top edge */
static void draw_fill(const float *v)
{
	int i, bottom = SY0 + SHGT - 1, w = PW / vw.nb, k, y;
	for (i = 0; i < vw.nb; i++) {
		float frac = (shown(v, i) - low()) / DB_SPAN;
		int yt;
		uint32_t top, *dst;
		if (frac < 0) frac = 0;
		if (frac > 1) frac = 1;
		yt = bottom - (int)(frac * (SHGT - 1));
		top = pal_color(frac);
		for (y = bottom; y >= yt; y--) {
			float g = yt == bottom ? 1.0f : (float)(bottom - y) / (bottom - yt);
			uint32_t col = cscale(top, 0.22f + 0.78f * g);
			dst = canvas + y * SW + PX0 + i * w;
			for (k = 0; k < w; k++)
				dst[k] = col;
		}
		dst = canvas + yt * SW + PX0 + i * w;
		for (k = 0; k < w; k++)
			dst[k] = cscale(top, 1.4f);
	}
}

static void render(void)
{
	char s[128];
	int i, x, y;
	double span = vw.nb * vw.bw, step, f;

	fill(0, 0, SW, SH, C_BG);
	fill(PX0, SY0, PW, SHGT, C_PANEL);
	if (spec_style == 2)
		draw_band_tint();

	/* title bar */
	text(4, 2, C_TEXT, "MiYooSDR");
	if (vw.level == 0)
		snprintf(s, sizeof(s), "FULL 2402-2482  %.1f/s  gain %02X%s", rate, igi, enhance ? "  ENHANCE" : "");
	else
		snprintf(s, sizeof(s), "ZOOM %.0f MHz, %.0f kHz/bin  %.1f/s  gain %02X%s", span, vw.bw * 1000, rate, igi,
		         enhance ? "  ENH" : "");
	text(76, 2, vw.level ? C_ZOOM : C_DIM, s);
	if (rec) {
		fill(SW - 52, 6, 8, 8, C_REC);
		text(SW - 40, 2, C_REC, "REC");
	}

	/* dB grid */
	for (i = 0; i <= 48; i += 6) {
		float db = low() + i;
		y = db_to_y(db);
		line(PX0, y, PX0 + PW - 1, y, C_GRID);
		if (i % 12 == 0) {
			snprintf(s, sizeof(s), enhance ? "%+3.0f" : "%3.0f", db);
			text(PX0 - 34, y - 8, C_DIM, s);
		}
	}
	/* frequency grid */
	step = vw.level == 0 ? 10 : vw.level == 1 ? 2 : vw.level == 2 ? 1 : 0.5;
	for (f = ceil(vw.f0 / step) * step; f < vw.f0 + span; f += step) {
		x = PX0 + (int)((f - vw.f0) / vw.bw * PW / vw.nb);
		line(x, SY0, x, SY0 + SHGT - 1, C_GRID);
		/* label every other grid line when zoomed */
		if (vw.level == 0 || fmod(f / step + 0.25, 2.0) < 0.5) {
			snprintf(s, sizeof(s), "%.0f", f);
			text(x - (int)strlen(s) * 4, SY0 + SHGT + 2, C_DIM, s);
		}
	}
	if (vw.level == 0) {
		static const int chs[3] = { 1, 6, 11 };
		for (i = 0; i < 3; i++) {
			x = PX0 + (int)((ch_mhz(chs[i]) - vw.f0) / vw.bw * PW / vw.nb);
			snprintf(s, sizeof(s), "ch%d", chs[i]);
			text(x - 12, SY0 + SHGT + 18, C_AXIS, s);
		}
	} else {
		snprintf(s, sizeof(s), "tuned ch%d (%.0f MHz)", vw.ch, ch_mhz(vw.ch));
		text(PX0, SY0 + SHGT + 18, C_AXIS, s);
	}

	/* spectrum */
	if (sweeps) {
		if (spec_style == 0) {
			for (i = 1; i < vw.nb; i++)
				line(bin_x(i - 1), db_to_y(shown(sweep_db, i - 1)), bin_x(i), db_to_y(shown(sweep_db, i)), C_LIVE);
		} else {
			draw_fill(sweep_db);
			for (i = 0; i <= 48; i += 12) { /* dotted dB lines back over the fill */
				y = db_to_y(low() + i);
				for (x = PX0; x < PX0 + PW; x += 4)
					canvas[y * SW + x] = C_GRID;
			}
		}
		if (have_hold && show_hold)
			for (i = 1; i < vw.nb; i++)
				line(bin_x(i - 1), db_to_y(shown(hold_db, i - 1)), bin_x(i), db_to_y(shown(hold_db, i)), C_HOLD);
	}

	/* cursor */
	if (cursor >= vw.nb)
		cursor = vw.nb - 1;
	x = bin_x(cursor) + PW / vw.nb / 2;
	for (y = SY0; y < SY0 + SHGT; y += 3)
		canvas[y * SW + x] = C_CURSOR;
	snprintf(s, sizeof(s), vw.level ? "%.2f" : "%.1f", vw.f0 + (cursor + 0.5) * vw.bw);
	text(PX0 + PW + 6, SY0 + 2, C_CURSOR, s);
	if (sweeps) {
		snprintf(s, sizeof(s), enhance ? "now%+4.0f" : "now %3.0f", shown(sweep_db, cursor));
		text(PX0 + PW + 6, SY0 + 22, C_LIVE, s);
		snprintf(s, sizeof(s), enhance ? "max%+4.0f" : "max %3.0f", shown(hold_db, cursor));
		text(PX0 + PW + 6, SY0 + 40, C_HOLD, s);
		snprintf(s, sizeof(s), "bg %4.0f", base_db[cursor]);
		text(PX0 + PW + 6, SY0 + 58, C_AVG, s);
	}
	snprintf(s, sizeof(s), "%s", spec_style == 0 ? "Line" : spec_style == 1 ? "Fill" : "Bands");
	text(PX0 + PW + 6, SY0 + 82, C_DIM, s);
	text(PX0 + PW + 6, SY0 + 98, C_DIM, pal_name[palette]);

	/* waterfall, newest row on top */
	for (y = 0; y < wf_rows; y++) {
		const uint8_t *row = wf[(wf_head + y) % WHGT];
		uint32_t *dst = canvas + (WY0 + y) * SW + PX0;
		int w = PW / vw.nb;
		for (i = 0; i < vw.nb; i++) {
			int k;
			for (k = 0; k < w; k++)
				dst[w * i + k] = lut[row[i]];
		}
	}
	for (y = WY0; y < WY0 + WHGT; y += 3)
		canvas[y * SW + x] = C_CURSOR;
	text(PX0 + PW + 6, WY0, C_DIM, "newest");
	text(PX0 + PW + 6, WY0 + WHGT - 16, C_DIM, "older");

	/* help / status line */
	if (sel_down)
		text(4, SH - 17, C_ZOOM, "SELECT:  Y = view style   A = palette   (release to jump to peak)");
	else if (status_until > now_s())
		text(4, SH - 17, C_TEXT, status_msg);
	else
		text(4, SH - 17, C_DIM, "R2/L2 zoom  Dpad cursor  SEL peak (+Y style +A palette)  Y enh  A hold  B clr  X rec  START shot");
}

/* --------------------------------------------------------- output / input */

static int fb_fd = -1, pages = 1, page;
static uint8_t *fb_mem;
static size_t fb_size;
#ifdef __linux__
static struct fb_var_screeninfo var, orig_var;
static struct fb_fix_screeninfo fix;
#endif
static int in_fd = -1;

static int fb_open(void)
{
#ifdef __linux__
	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) < 0)
		return -1;
	orig_var = var;
	var.bits_per_pixel = 32;
	var.yres_virtual = var.yres * 2;
	var.yoffset = 0;
	if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &var) < 0) {
		var = orig_var;
		ioctl(fb_fd, FBIOPUT_VSCREENINFO, &var);
	}
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0 ||
	    var.bits_per_pixel != 32)
		return -1;
	fb_size = fix.smem_len;
	fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED) {
		fb_mem = NULL;
		return -1;
	}
	pages = (var.yres_virtual >= var.yres * 2 && fb_size >= (size_t)fix.line_length * var.yres * 2) ? 2 : 1;
	in_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	return 0;
#else
	return -1;
#endif
}

static void fb_close(void)
{
#ifdef __linux__
	if (fb_mem) {
		memset(fb_mem, 0, fb_size);
		var.yoffset = 0;
		ioctl(fb_fd, FBIOPAN_DISPLAY, &var);
		ioctl(fb_fd, FBIOPUT_VSCREENINFO, &orig_var);
		munmap(fb_mem, fb_size);
	}
#endif
}

static void present(void)
{
#ifdef __linux__
	int target = pages == 2 ? page ^ 1 : 0, y, x;
	int ox = ((int)var.xres - SW) / 2, oy = ((int)var.yres - SH) / 2;
	uint8_t *base;

	if (!fb_mem)
		return;
	if (ox < 0) ox = 0;
	if (oy < 0) oy = 0;
	base = fb_mem + (size_t)target * var.yres * fix.line_length;
	/* the panel is mounted upside down: rotate 180 degrees */
	for (y = 0; y < SH && y + oy < (int)var.yres; y++) {
		const uint32_t *src = canvas + (size_t)(SH - 1 - y) * SW + SW - 1;
		uint32_t *dst = (uint32_t *)(base + (size_t)(y + oy) * fix.line_length) + ox;
		for (x = 0; x < SW && x + ox < (int)var.xres; x++)
			dst[x] = *src-- | 0xFF000000u;
	}
	if (pages == 2) {
		var.yoffset = target * var.yres;
		ioctl(fb_fd, FBIOPAN_DISPLAY, &var);
		page = target;
	}
#endif
}

static int write_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	int i;
	if (!f)
		return -1;
	fprintf(f, "P6\n%d %d\n255\n", SW, SH);
	for (i = 0; i < SW * SH; i++) {
		unsigned char px[3] = { canvas[i] >> 16, canvas[i] >> 8, canvas[i] };
		fwrite(px, 1, 3, f);
	}
	fclose(f);
	return 0;
}

static void status(const char *msg)
{
	snprintf(status_msg, sizeof(status_msg), "%s", msg);
	status_until = now_s() + 3;
}

static void rec_view_line(void)
{
	if (rec)
		fprintf(rec, "# view level=%d bins=%d f0=%.6f bw=%.6f ch=%d\n", vw.level, vw.nb, vw.f0, vw.bw, vw.ch);
}

static void toggle_rec(void)
{
	char m[200];
	if (rec) {
		fclose(rec);
		rec = NULL;
		sync();
		status("recording saved");
		return;
	}
	mkdir("rec", 0755);
	{
		time_t t = time(NULL);
		struct tm tm;
		localtime_r(&t, &tm);
		snprintf(rec_name, sizeof(rec_name), "rec/sdr-%04d%02d%02d-%02d%02d%02d.csv", tm.tm_year + 1900,
		         tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	}
	rec = fopen(rec_name, "w");
	if (!rec) {
		status("cannot open recording file");
		return;
	}
	fprintf(rec, "# MiYooSDR spike recording. Bin i of a view is f0 + bw*i MHz.\n");
	fprintf(rec, "# rows: seconds, gain(IGI), view level, then dB*10 per bin (uncalibrated)\n");
	rec_view_line();
	snprintf(m, sizeof(m), "recording to %s", rec_name);
	status(m);
}

static void record_sweep(double t0)
{
	int i;
	if (!rec)
		return;
	fprintf(rec, "%.3f,%d,%d", now_s() - t0, igi, vw.level);
	for (i = 0; i < vw.nb; i++)
		fprintf(rec, ",%d", (int)lrintf(sweep_db[i] * 10));
	fputc('\n', rec);
}

static void set_view_zoom(double target, int level)
{
	char m[96];
	if (level <= 0) {
		double f = vw.f0 + (cursor + 0.5) * vw.bw;
		view_full();
		cursor = (int)((f - vw.f0) / vw.bw);
		if (cursor < 0) cursor = 0;
		if (cursor >= vw.nb) cursor = vw.nb - 1;
		snprintf(m, sizeof(m), "full band");
	} else {
		view_zoom(target, level);
		cursor = (int)((target - vw.f0) / vw.bw);
		if (cursor < 0) cursor = 0;
		if (cursor >= vw.nb) cursor = vw.nb - 1;
		snprintf(m, sizeof(m), "zoom %.0f MHz around %.2f MHz (ch %d)", vw.nb * vw.bw, target, vw.ch);
	}
	reset_traces();
	rec_view_line();
	status(m);
}

static void find_peak(void)
{
	int i, best = -1;
	float bestv = -1e9f;
	char m[96];
	for (i = 0; i < vw.nb; i++) {
		float e = hold_db[i] - base_db[i];
		if (i >= vw.dc_lo && i <= vw.dc_hi)
			continue;
		if (vw.level == 0 && (i % 64 >= 30 && i % 64 <= 34))
			continue;
		if (e > bestv) {
			bestv = e;
			best = i;
		}
	}
	if (best < 0)
		return;
	cursor = best;
	snprintf(m, sizeof(m), "peak %.2f MHz, %.0f dB over background", vw.f0 + (best + 0.5) * vw.bw, bestv);
	status(m);
}

static int shot_req;

static void on_button(int code)
{
#ifdef __linux__
	double fcur = vw.f0 + (cursor + 0.5) * vw.bw;
	switch (code) {
	case KEY_SPACE: /* A */
		show_hold = !show_hold;
		status(show_hold ? "max hold shown" : "max hold hidden");
		break;
	case KEY_LEFTCTRL: /* B */
		reset_traces();
		status("cleared");
		break;
	case KEY_LEFTSHIFT: toggle_rec(); break; /* X */
	case KEY_LEFTALT: /* Y */
		enhance = !enhance;
		wf_rows = 0;
		status(enhance ? "enhance: dB above the background" : "absolute levels");
		break;
	case KEY_E: if (igi > 0x1c) igi -= 2; break; /* L1 */
	case KEY_T: if (igi < 0x6e) igi += 2; break; /* R1 */
	case KEY_UP: if (enhance) enh_low += 3; else db_low += 3; break;
	case KEY_DOWN: if (enhance) enh_low -= 3; else db_low -= 3; break;
	case KEY_LEFT:
		if (cursor > 0)
			cursor--;
		else if (vw.level) /* pan */
			set_view_zoom(fcur - vw.nb * vw.bw / 4, vw.level);
		break;
	case KEY_RIGHT:
		if (cursor < vw.nb - 1)
			cursor++;
		else if (vw.level)
			set_view_zoom(fcur + vw.nb * vw.bw / 4, vw.level);
		break;
	case KEY_BACKSPACE: /* R2: zoom in at the cursor */
		if (vw.level < 3)
			set_view_zoom(fcur, vw.level + 1);
		break;
	case KEY_TAB: /* L2: zoom out */
		if (vw.level > 0)
			set_view_zoom(fcur, vw.level - 1);
		break;
	case KEY_ENTER: shot_req = 1; break;    /* START */
	case KEY_ESC: quit = 1; break;          /* MENU */
	}
#endif
}

/* actions on the SELECT shift layer */
static void sel_layer(int code)
{
	char m[48];
	switch (code) {
	case KEY_LEFTALT: /* SEL + Y: cycle spectrum style */
		spec_style = (spec_style + 1) % 3;
		status(spec_style == 0 ? "view: line" : spec_style == 1 ? "view: filled" : "view: filled + Wi-Fi bands");
		break;
	case KEY_SPACE: /* SEL + A: cycle palette */
		palette = (palette + 1) % NPAL;
		make_lut();
		snprintf(m, sizeof(m), "palette: %s", pal_name[palette]);
		status(m);
		break;
	}
}

static void poll_input(void)
{
#ifdef __linux__
	struct input_event ev[16];
	ssize_t n;
	if (in_fd < 0)
		return;
	while ((n = read(in_fd, ev, sizeof(ev))) > 0) {
		int k;
		for (k = 0; k < (int)(n / sizeof(ev[0])); k++) {
			int code = ev[k].code, val = ev[k].value;
			if (ev[k].type != EV_KEY)
				continue;
			if (code == KEY_RIGHTCTRL) { /* SELECT: shift layer, or peak on a tap */
				if (val == 1) {
					sel_down = 1;
					sel_combo = 0;
				} else if (val == 0) {
					sel_down = 0;
					if (!sel_combo)
						find_peak();
				}
				continue;
			}
			if (!val) /* ignore key releases for everything else */
				continue;
			if (sel_down) {
				sel_layer(code);
				sel_combo = 1;
			} else {
				on_button(code);
			}
		}
	}
#endif
}

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

/* -------------------------------------------------------------------- main */

static void print_row(const char *tag, const float *v)
{
	int i;
	printf("%s", tag);
	for (i = 0; i < vw.nb; i++)
		printf(" %.0f", v[i]);
	printf("\n");
}

int main(int argc, char **argv)
{
	const char *shot = NULL;
	int dump = 0, probe = 0, racap = 0, burst_sec = 0, i, shot_after = 12, zlevel = 0;
	double t0, last_rate_t, last_flush, last_diag = 0, zf = 0, burst_mhz = 0;
	long last_sweeps = 0;
	struct sigaction sa;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--fake"))
			fake = 1;
		else if (!strcmp(argv[i], "--dump") && i + 1 < argc)
			dump = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
			shot = argv[++i];
		else if (!strcmp(argv[i], "--probe"))
			probe = 1;
		else if (!strcmp(argv[i], "--raw-tune"))
			raw_tune = 1;
		else if (!strcmp(argv[i], "--zoom") && i + 2 < argc) {
			zf = atof(argv[++i]);
			zlevel = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--burst") && i + 2 < argc) {
			burst_mhz = atof(argv[++i]);
			burst_sec = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--racap")) {
			racap = 1;
		} else if (!strcmp(argv[i], "--style") && i + 1 < argc) {
			spec_style = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--pal") && i + 1 < argc) {
			palette = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--pm-active")) {
			pm_active();
			return 0;
		}
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	if (!fake && radio_open() != 0)
		return 2;
	if (!fake)
		igi = (int)(orig_c50 & 0x7f);
	make_lut();
	view_full();
	if (zlevel) {
		view_zoom(zf, zlevel);
		cursor = (int)((zf - vw.f0) / vw.bw);
	}

	if (burst_sec) { /* single-channel high-rate power-vs-time recorder */
		int ch = (int)lround((burst_mhz - 2407.0) / 5.0), k;
		double f0, t0b, tnow;
		long nrows = 0;
		char name[160];
		time_t tt;
		struct tm tm;
		FILE *f;
		if (ch < 1) ch = 1;
		if (ch > 13) ch = 13;
		f0 = ch_mhz(ch) - 64 * 0.3125;
		mkdir("rec", 0755);
		tt = time(NULL);
		localtime_r(&tt, &tm);
		snprintf(name, sizeof(name), "rec/burst-%04d%02d%02d-%02d%02d%02d.csv", tm.tm_year + 1900, tm.tm_mon + 1,
		         tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
		f = fopen(name, "w");
		if (!f) {
			fprintf(stderr, "cannot open %s\n", name);
			radio_restore();
			return 4;
		}
		fprintf(f, "# MiYooSDR burst capture: channel %d, bin i = %.4f + 0.3125*i MHz, 128 bins, 128-pt FFT\n", ch, f0);
		fprintf(f, "# columns: seconds, gain(IGI), then dB*10 per bin (uncalibrated)\n");
		set_igi(igi);
		tune(ch);
		(void)psd_point(0, 0);
		t0b = now_s();
		while ((tnow = now_s()) - t0b < burst_sec && !quit && !radio_dead) {
			fprintf(f, "%.4f,%d", tnow - t0b, igi);
			for (k = -64; k < 64; k++) {
				unsigned v = psd_point(k, 0);
				double db = v ? 10.0 * log10((double)v) + igi : 0;
				fprintf(f, ",%d", (int)lrint(db * 10));
			}
			fputc('\n', f);
			nrows++;
		}
		fclose(f);
		sync();
		fprintf(stderr, "burst: %ld sweeps in %.1f s (%.0f/s) on ch %d (%.1f MHz) -> %s\n", nrows, now_s() - t0b,
		        nrows / (now_s() - t0b), ch, ch_mhz(ch), name);
		radio_restore();
		return 0;
	}
	if (racap) { /* feasibility probe: can we read a sample buffer, does the LA block respond? */
		unsigned old106, old7c0, old9a0, base, first = 0, w0, w1;
		int k, varied = 0;
		printf("racap feasibility probe (read-biased, restores afterwards)\n");
		printf("LA/MAC regs: 0x7c0=%08x 0x7cc=%08x 0x9a0=%08x 0x808=%08x\n",
		       reg_read32(0x7c0), reg_read32(0x7cc), reg_read32(0x9a0), reg_read32(0x808));
		old106 = reg_read32(0x106) & 0xff;
		reg_write(0x106, 0x69, 1); /* enable packet-buffer access (as dump_TX_FIFO does) */
		base = reg_read32(0x209) & 0xff;
		printf("packet-buffer access: 0x106 %x->69, base page 0x209=%x; words at 0x144/0x148:\n", old106, base);
		for (k = 0; k < 12; k++) {
			reg_write(0x140, (unsigned)k, 4);
			w0 = reg_read32(0x144);
			w1 = reg_read32(0x148);
			printf("  [%2d] %08x %08x\n", k, w0, w1);
			if (k == 0)
				first = w0;
			else if (w0 != first)
				varied = 1;
		}
		reg_write(0x106, old106, 1);
		printf("=> packet-buffer readback %s\n", varied ? "VARIES (mechanism responds)" : "constant (no usable readback)");
		old7c0 = reg_read32(0x7c0);
		old9a0 = reg_read32(0x9a0);
		reg_write(0x7c0, (old7c0 & ~0xffu) | 0x1u, 4); /* try to enable the LA block */
		w0 = reg_read32(0x7c0);
		printf("=> LA enable test: wrote 0x7c0 bit0=1, reads %08x (bit0=%u) %s\n", w0, w0 & 1u,
		       (w0 & 1u) ? "-> LA block accepts enable" : "-> enable did not stick, LA likely absent");
		reg_write(0x7c0, old7c0, 4);
		reg_write(0x9a0, old9a0, 4);
		printf("restored: 0x7c0=%08x 0x9a0=%08x, ioerr %ld\n", reg_read32(0x7c0), reg_read32(0x9a0), io_errors);
		radio_restore();
		return 0;
	}
	if (probe) { /* per FFT size: is a read W us after the trigger still the old result? */
		static const int waits[6] = { 100, 200, 400, 800, 1600, 3200 };
		int sz, w, r;
		tune(6);
		for (sz = 0; sz < 4; sz++) {
			unsigned cfg = (psd_cfg & ~(3u << 14)) | ((unsigned)sz << 14);
			printf("fft %4d stale/10 at", 128 << sz);
			for (w = 0; w < 6; w++) {
				int stale = 0;
				for (r = 0; r < 10; r++) {
					unsigned tt = (unsigned)(r * 37 + w * 11 + 5) & ((128u << sz) - 1), a, b;
					reg_write(0x808, cfg | tt, 4);
					reg_write(0x808, cfg | tt | (1u << 22), 4);
					spin_us(waits[w]);
					a = reg_read32(0x8b4) & 0xffff;
					spin_us(6000);
					b = reg_read32(0x8b4) & 0xffff;
					stale += a != b;
				}
				printf("  %dus:%d", waits[w], stale);
			}
			printf("\n");
		}
		radio_restore();
		return 0;
	}
	if (dump) {
		double ts = now_s(), te;
		for (i = 0; i < dump && !quit; i++) {
			do_sweep();
			if (sweeps % 5 == 1) {
				char tag[32];
				snprintf(tag, sizeof(tag), "dump t=%.1f", now_s() - ts);
				diag(tag);
			}
		}
		te = now_s();
		printf("sweeps %ld in %.2f s (%.2f/s), reg ops %ld, zeros %ld, stuck %ld, igi %02x, view %d f0 %.3f bw %.4f\n",
		       sweeps, te - ts, sweeps / (te - ts), reg_ops, zeros, stuck_sweeps, igi, vw.level, vw.f0, vw.bw);
		print_row("last", sweep_db);
		print_row("hold", hold_db);
		print_row("avg", avg_db);
		radio_restore();
		return 0;
	}

	if (!shot && fb_open() != 0) {
		fprintf(stderr, "no framebuffer\n");
		radio_restore();
		return 3;
	}

	t0 = last_rate_t = last_flush = now_s();
	while (!quit) {
		do_sweep();
		if (now_s() - last_diag >= 2.0) {
			diag("ui");
			last_diag = now_s();
		}
		if (!scaled && sweeps >= 2)
			autoscale();
		push_waterfall();
		record_sweep(t0);
		if (now_s() - last_rate_t >= 1.0) {
			rate = (sweeps - last_sweeps) / (now_s() - last_rate_t);
			last_sweeps = sweeps;
			last_rate_t = now_s();
		}
		if (rec && now_s() - last_flush > 1.0) {
			fflush(rec);
			last_flush = now_s();
		}
		render();
		if (shot) {
			if (sweeps >= shot_after) {
				write_ppm(shot);
				break;
			}
			continue;
		}
		present();
		if (shot_req || sweeps == 20) { /* one automatic screenshot for checking the layout */
			write_ppm(shot_req ? "screenshot.ppm" : "first.ppm");
			if (shot_req)
				status("screenshot saved");
			shot_req = 0;
		}
	}

	if (rec)
		fclose(rec);
	if (radio_dead && fb_mem) {
		double until = now_s() + 4;
		snprintf(status_msg, sizeof(status_msg), "Wi-Fi chip stopped responding (it resets itself). Exiting.");
		status_until = until;
		render();
		present();
		while (now_s() < until)
			usleep(100000);
	}
	radio_restore();
	fb_close();
	fprintf(stderr, "exit after %ld sweeps, %ld zero readings, %ld frozen sweeps, %ld remedies\n", sweeps, zeros,
	        stuck_sweeps, remedies);
	return 0;
}
