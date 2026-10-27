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
 * bins at 256/512/1024 points: 20, 10 or 5 MHz wide around the cursor. Bin i is
 * the FFT tone centred at f0 + i * bw MHz.
 *
 * Modes: (none) = app on /dev/fb0; --replay FILE = play a PCAP or a spectrum CSV;
 * --review = recording browser; --dump N = N sweeps as text; --burst MHZ SEC =
 * single-channel recorder; --fake = synthetic radio for the PC build; --shot FILE
 * = render one frame to a PNG or PPM and exit. --help lists them all.
 * --pm-active switches off the driver's power saving and exits (launch.sh runs
 * it while still associated); --needs-radio tells launch.sh whether the other
 * options need the radio, so it only takes Wi-Fi down when they do.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#ifdef __linux__
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/wireless.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#endif

#include "font.h"
#include "pcap.h"

#ifndef MIYOOSDR_VERSION
#define MIYOOSDR_VERSION "dev"
#endif

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

/* ------------------------------------------------- signal-to-sound (audio) */
/* A tone whose pitch and loudness track audio_norm (0..1), so you can hear a
 * signal rise as you sweep the cursor over it or walk toward its source. */
static volatile float audio_norm;
static volatile int audio_quiet; /* fade the tone out (screens with no level to follow) */
static int audio_on;

#ifdef MIYOO
#include <pthread.h>
#include <dlfcn.h>
#include <mi_ao.h>
#include <mi_sys.h>
#define AO_DEV 0
#define AO_CHN 0
#define AO_FRAMES 512
#define AO_RATE 48000

static void *ao_lib, *sys_lib;
static pthread_t ao_thread;
static volatile int ao_running;
static MI_S32 (*p_MI_SYS_Init)(void);
static MI_S32 (*p_MI_AO_SetPubAttr)(MI_AUDIO_DEV, MI_AUDIO_Attr_t *);
static MI_S32 (*p_MI_AO_Enable)(MI_AUDIO_DEV);
static MI_S32 (*p_MI_AO_Disable)(MI_AUDIO_DEV);
static MI_S32 (*p_MI_AO_EnableChn)(MI_AUDIO_DEV, MI_AO_CHN);
static MI_S32 (*p_MI_AO_DisableChn)(MI_AUDIO_DEV, MI_AO_CHN);
static MI_S32 (*p_MI_AO_SendFrame)(MI_AUDIO_DEV, MI_AO_CHN, MI_AUDIO_Frame_t *, MI_S32);
static MI_S32 (*p_MI_AO_SetVolume)(MI_AUDIO_DEV, MI_S32);
static MI_S32 (*p_MI_AO_SetMute)(MI_AUDIO_DEV, MI_BOOL);

static int volume_db(int vol) /* Onion's log volume curve */
{
	if (vol < 0) vol = 0;
	if (vol > 20) vol = 20;
	return vol == 0 ? -60 : (int)lround(48.0 * log10(1.0 + vol)) - 60;
}

static int system_volume_db(void)
{
	char buf[2048];
	FILE *f = fopen("/mnt/SDCARD/system.json", "r");
	int vol = 10;
	if (f == NULL)
		f = fopen("/appconfigs/system.json", "r");
	if (f != NULL) {
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		char *v;
		buf[n] = 0;
		fclose(f);
		v = strstr(buf, "\"vol\"");
		if (v != NULL && (v = strchr(v, ':')) != NULL)
			vol = atoi(v + 1);
	}
	return volume_db(vol);
}

static void *ao_loop(void *arg)
{
	static int16_t buf[AO_FRAMES * 2];
	static double phase, hz = 400, amp;
	MI_AUDIO_Frame_t frame;
	int i;
	(void)arg;
	while (ao_running) {
		float n = audio_norm;
		double thz, tamp;
		if (n < 0) n = 0;
		if (n > 1) n = 1;
		thz = 220.0 + n * n * 1600.0; /* 220..1820 Hz, squared so peaks sing */
		tamp = audio_quiet ? 0.0 : 0.05 + 0.6 * n;
		for (i = 0; i < AO_FRAMES; i++) {
			int16_t s;
			hz += (thz - hz) * 0.001; /* glide to avoid clicks */
			amp += (tamp - amp) * 0.001;
			phase += 2.0 * M_PI * hz / AO_RATE;
			if (phase > 2.0 * M_PI)
				phase -= 2.0 * M_PI;
			s = (int16_t)(sin(phase) * amp * 26000.0);
			buf[2 * i] = s;
			buf[2 * i + 1] = s;
		}
		memset(&frame, 0, sizeof(frame));
		frame.eBitwidth = E_MI_AUDIO_BIT_WIDTH_16;
		frame.eSoundmode = E_MI_AUDIO_SOUND_MODE_STEREO;
		frame.u32Len = sizeof(buf);
		frame.apVirAddr[0] = buf;
		while (ao_running && p_MI_AO_SendFrame(AO_DEV, AO_CHN, &frame, 100) != 0)
			usleep(2000);
	}
	return NULL;
}

static void *load_lib(const char *name)
{
	char path[128];
	void *h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
	if (h == NULL) {
		snprintf(path, sizeof(path), "/config/lib/%s", name);
		h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
	}
	return h;
}

static int audio_start(void)
{
	MI_AUDIO_Attr_t attr;
	MI_S32 vol;

	if (ao_running)
		return 0;
	load_lib("libcam_os_wrapper.so");
	load_lib("libmi_common.so");
	sys_lib = load_lib("libmi_sys.so");
	ao_lib = load_lib("libmi_ao.so");
	if (sys_lib == NULL || ao_lib == NULL)
		return -1;
	p_MI_SYS_Init = (MI_S32 (*)(void))dlsym(sys_lib, "MI_SYS_Init");
	p_MI_AO_SetPubAttr = (MI_S32 (*)(MI_AUDIO_DEV, MI_AUDIO_Attr_t *))dlsym(ao_lib, "MI_AO_SetPubAttr");
	p_MI_AO_Enable = (MI_S32 (*)(MI_AUDIO_DEV))dlsym(ao_lib, "MI_AO_Enable");
	p_MI_AO_Disable = (MI_S32 (*)(MI_AUDIO_DEV))dlsym(ao_lib, "MI_AO_Disable");
	p_MI_AO_EnableChn = (MI_S32 (*)(MI_AUDIO_DEV, MI_AO_CHN))dlsym(ao_lib, "MI_AO_EnableChn");
	p_MI_AO_DisableChn = (MI_S32 (*)(MI_AUDIO_DEV, MI_AO_CHN))dlsym(ao_lib, "MI_AO_DisableChn");
	p_MI_AO_SendFrame = (MI_S32 (*)(MI_AUDIO_DEV, MI_AO_CHN, MI_AUDIO_Frame_t *, MI_S32))dlsym(ao_lib, "MI_AO_SendFrame");
	p_MI_AO_SetVolume = (MI_S32 (*)(MI_AUDIO_DEV, MI_S32))dlsym(ao_lib, "MI_AO_SetVolume");
	p_MI_AO_SetMute = (MI_S32 (*)(MI_AUDIO_DEV, MI_BOOL))dlsym(ao_lib, "MI_AO_SetMute");
	if (!p_MI_SYS_Init || !p_MI_AO_SetPubAttr || !p_MI_AO_Enable || !p_MI_AO_EnableChn || !p_MI_AO_SendFrame)
		return -1;
	p_MI_SYS_Init();
	memset(&attr, 0, sizeof(attr));
	attr.eSamplerate = (MI_AUDIO_SampleRate_e)AO_RATE;
	attr.eBitwidth = E_MI_AUDIO_BIT_WIDTH_16;
	attr.eWorkmode = E_MI_AUDIO_MODE_I2S_MASTER;
	attr.eSoundmode = E_MI_AUDIO_SOUND_MODE_STEREO;
	attr.u32FrmNum = 6;
	attr.u32PtNumPerFrm = AO_FRAMES;
	attr.u32ChnCnt = 2;
	if (p_MI_AO_SetPubAttr(AO_DEV, &attr) != 0 || p_MI_AO_Enable(AO_DEV) != 0) {
		fprintf(stderr, "audio: MI_AO setup failed (audioserver still running?)\n");
		return -1;
	}
	if (p_MI_AO_EnableChn(AO_DEV, AO_CHN) != 0) {
		fprintf(stderr, "audio: MI_AO channel setup failed\n");
		if (p_MI_AO_Disable)
			p_MI_AO_Disable(AO_DEV);
		return -1;
	}
	vol = system_volume_db();
	if (p_MI_AO_SetVolume)
		p_MI_AO_SetVolume(AO_DEV, vol < -20 ? -20 : vol);
	if (p_MI_AO_SetMute)
		p_MI_AO_SetMute(AO_DEV, FALSE);
	ao_running = 1;
	if (pthread_create(&ao_thread, NULL, ao_loop, NULL) != 0) {
		ao_running = 0;
		if (p_MI_AO_DisableChn)
			p_MI_AO_DisableChn(AO_DEV, AO_CHN);
		if (p_MI_AO_Disable)
			p_MI_AO_Disable(AO_DEV);
		return -1;
	}
	return 0;
}

static void audio_stop(void)
{
	if (!ao_running)
		return;
	ao_running = 0;
	pthread_join(ao_thread, NULL);
	if (p_MI_AO_DisableChn)
		p_MI_AO_DisableChn(AO_DEV, AO_CHN);
	if (p_MI_AO_Disable)
		p_MI_AO_Disable(AO_DEV);
}
#else
static int audio_start(void) { return -1; }
static void audio_stop(void) {}
#endif

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
		if (write(fd_rr_cfg, b, n) != n) {
			io_error("read_reg address", addr);
			return 0;
		}
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
	if (eq == NULL) {
		io_error("read_reg reply", addr);
		return 0;
	}
	return (unsigned)strtoul(eq + 1, NULL, 16);
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
	long errors;
	pm_active();
	spin_us(20000);
	fd_rr_cfg = open(PROC "read_reg", O_WRONLY);
	fd_rr = open(PROC "read_reg", O_RDONLY);
	fd_wr = open(PROC "write_reg", O_WRONLY);
	fd_mon = open(PROC "monitor", O_WRONLY);
	if (fd_rr_cfg < 0 || fd_rr < 0 || fd_wr < 0) {
		fprintf(stderr, "no RTL8188FU proc entries (%s)\n", strerror(errno));
		pm_restore();
		return -1;
	}
	if (fd_mon < 0 && !raw_tune) { /* the raw RF path once hung the chip's USB link */
		fprintf(stderr, "no proc monitor entry to tune with; refusing raw RF tuning without --raw-tune\n");
		pm_restore();
		return -1;
	}
	errors = io_errors;
	orig_808 = reg_read32(0x808);
	orig_c50 = reg_read32(0xc50) & 0xff;
	if (io_errors != errors) { /* zeros here would be written back on exit */
		fprintf(stderr, "cannot read the PSD registers; leaving the radio alone\n");
		pm_restore();
		return -1;
	}
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

/* bin i of the view is the FFT tone centred at f0 + i * bw */
static double bin_mhz(int i) { return vw.f0 + i * vw.bw; }

static int mhz_bin(double f) /* nearest bin, kept inside the view */
{
	double i = (f - vw.f0) / vw.bw;
	if (!(i > 0)) /* also NaN */
		return 0;
	if (i >= vw.nb - 1)
		return vw.nb - 1;
	return (int)lround(i);
}

static void view_full(void)
{
	memset(&vw, 0, sizeof(vw));
	vw.nb = 256;
	vw.f0 = 2402.0;
	vw.bw = 0.3125;
	vw.dc_lo = 1;
	vw.dc_hi = 0;
}

#define BAND_LO 2400.0 /* zoom targets stay inside the 2.4 GHz band */
#define BAND_HI 2484.0

/* zoom on target MHz, 2.5-7.5 MHz off the LO so the DC leak stays away from it.
 * The tones stay inside the FFT's +-N/2: beyond that the PSD index wraps and
 * would show the far side of the LO under these frequency labels. */
static void view_zoom(double target, int level)
{
	int ch, best = 1, half;
	double bestscore = 1e9;

	if (level < 1) level = 1;
	if (level > 3) level = 3;
	if (!(target >= BAND_LO)) /* also NaN */
		target = BAND_LO;
	if (target > BAND_HI)
		target = BAND_HI;
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
	half = (128 << level) / 2;
	vw.tone0 = (int)lround((target - ch_mhz(best)) / vw.bw) - 64;
	if (vw.tone0 < -half)
		vw.tone0 = -half;
	if (vw.tone0 > half - vw.nb)
		vw.tone0 = half - vw.nb;
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
static int igi = 0x28, sweep_gain = 0x28, have_hold, have_avg, have_base;
static unsigned view_gen; /* bumped by every view change, so a sweep can tell it was overtaken */
static long zeros, stuck_sweeps, remedies;
static char status_msg[200];
static double status_until;

static void poll_input(void);
static void status(const char *msg);
static void update_occupancy(void);
static void burst_reset(void);

static float to_db(unsigned v, float prev, int gain)
{
	if (v == 0) { /* occasional dropout: keep the previous reading */
		zeros++;
		return prev;
	}
	return (float)(10.0 * log10((double)v) + gain);
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

/* One sweep of the current view, at the gain it started with (L1/R1 apply from
 * the next sweep). Buttons are read in between; if one changes the view, the
 * readings so far belong to no view, so they are dropped and this returns 0. */
static int do_sweep(void)
{
	int j, k, frozen = 0, gain = igi;
	unsigned gen = view_gen;

	if (vw.level == 0) {
		for (j = 0; j < 4 && !quit; j++) {
			float *out = sweep_db + j * 64;
			if (!fake) {
				set_igi(gain);
				tune(full_ch[j]);
				(void)psd_point(0, 0); /* settle */
			}
			for (k = -32; k < 32; k++) {
				int idx = j * 64 + k + 32;
				unsigned v = fake ? fake_point(full_ch[j], k, 0) : psd_point(k, 0);
				raw_v[idx] = v;
				out[k + 32] = to_db(v, sweeps ? sweep_db[idx] : (float)gain, gain);
			}
			bridge(out, 64, 32 - 2, 32 + 2); /* LO leak with notches beside it */
			frozen += distinct_count(raw_v + j * 64, 64) <= 3;
			poll_input();
			if (view_gen != gen)
				return 0;
		}
		frozen = frozen >= 3;
	} else {
		if (!fake) {
			set_igi(gain);
			if (tuned_ch != vw.ch) {
				tune(vw.ch);
				(void)psd_point(0, vw.sel);
			}
		}
		for (k = 0; k < vw.nb && !quit; k++) {
			unsigned v = fake ? fake_point(vw.ch, vw.tone0 + k, vw.sel) : psd_point(vw.tone0 + k, vw.sel);
			raw_v[k] = v;
			sweep_db[k] = to_db(v, sweeps ? sweep_db[k] : (float)gain, gain);
			if ((k & 31) == 31) {
				poll_input();
				if (view_gen != gen)
					return 0;
			}
		}
		bridge(sweep_db, vw.nb, vw.dc_lo, vw.dc_hi);
		frozen = distinct_count(raw_v, vw.nb) <= 3;
	}
	sweep_gain = gain;
	check_stuck(frozen);
	if (fake) /* about the Mini's sweep rates, so timing on the PC looks like the real thing */
		usleep(vw.level ? 50000 : 200000);

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
	if (vw.level == 0)
		update_occupancy();
	sweeps++;
	return 1;
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

/* x of frequency f: bin i fills bin_x(i)..bin_x(i + 1) with its tone in the middle */
static int mhz_x(double f) { return PX0 + (int)lround(((f - vw.f0) / vw.bw + 0.5) * PW / vw.nb); }

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

static uint8_t wf_level(float db, int i) /* waterfall colour index of a reading in bin i */
{
	float t = enhance ? (db - base_db[i]) / 24.0f : (db - db_low) / DB_SPAN;
	return (uint8_t)(t < 0 ? 0 : t > 1 ? 255 : t * 255);
}

static void push_waterfall(void)
{
	int i;
	wf_head = (wf_head + WHGT - 1) % WHGT;
	for (i = 0; i < vw.nb; i++)
		wf[wf_head][i] = wf_level(sweep_db[i], i);
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
	burst_reset();
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
		int xa = mhz_x(ch_mhz(band_chs[c]) - 11.0);
		int xb = mhz_x(ch_mhz(band_chs[c]) + 11.0);
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

/* per Wi-Fi-channel activity (dB above the noise floor), fed by full-band sweeps */
static int disp_mode; /* 0 waterfall, 1 channel occupancy */
static int review_request;  /* SELECT+START asks to open a saved recording */
static float occ[15], occ_pk[15];

static void update_occupancy(void)
{
	int ch, i;
	for (ch = 1; ch <= 13; ch++) {
		double c = ch_mhz(ch);
		float sum = 0, mx = 0;
		int cnt = 0;
		for (i = 0; i < vw.nb; i++) {
			double f = bin_mhz(i);
			if (f >= c - 10 && f <= c + 10) {
				float e = sweep_db[i] - base_db[i];
				sum += e;
				cnt++;
				if (e > mx)
					mx = e;
			}
		}
		if (cnt) {
			occ[ch] = 0.6f * occ[ch] + 0.4f * (sum / cnt);
			occ_pk[ch] *= 0.95f;
			if (mx > occ_pk[ch])
				occ_pk[ch] = mx;
		}
	}
}

static void draw_occupancy(void)
{
	int ch, i, y, bw_px = PW / 13, top = WY0 + 16, bottom = WY0 + WHGT - 16, hpx = bottom - top;
	int best = 1;
	float bestv = 1e9f;
	char s[40];
	for (ch = 1; ch <= 13; ch++) {
		int x0 = PX0 + (ch - 1) * bw_px + 2, x1 = PX0 + ch * bw_px - 2, py;
		float frac = occ[ch] / 30.0f, pfr = occ_pk[ch] / 30.0f;
		uint32_t col;
		if (frac < 0) frac = 0;
		if (frac > 1) frac = 1;
		if (pfr > 1) pfr = 1;
		col = pal_color(0.15f + 0.85f * frac);
		for (y = bottom - (int)(frac * hpx); y <= bottom; y++)
			for (i = x0; i < x1; i++)
				canvas[y * SW + i] = col;
		py = bottom - (int)(pfr * hpx); /* peak-hold tick */
		if (pfr > 0.02f)
			for (i = x0; i < x1; i++)
				canvas[py * SW + i] = C_HOLD;
		snprintf(s, sizeof(s), "%d", ch);
		text((x0 + x1) / 2 - (ch < 10 ? 4 : 8), bottom + 3, (ch == 1 || ch == 6 || ch == 11) ? C_TEXT : C_DIM, s);
		if (occ[ch] < bestv) {
			bestv = occ[ch];
			best = ch;
		}
	}
	text(PX0, WY0, C_DIM, "CHANNEL OCCUPANCY (dB over noise)");
	snprintf(s, sizeof(s), vw.level ? "zoom out for live channels" : "clearest: ch %d", best);
	text(PX0 + PW - (int)strlen(s) * 8, WY0, vw.level ? C_DIM : C_LIVE, s);
}

/* what uses frequency f (MHz): Bluetooth LE (40 channels 2 MHz apart from 2402; 37, 38
 * and 39 are the advertising channels that beacons and most remotes use), Wi-Fi
 * (channels 1-13, 20 MHz wide) and Zigbee / 802.15.4 (channels 11-26, 2 MHz wide) */
static int ble_channel(double f, int *adv)
{
	int rf = (int)lround((f - 2402.0) / 2.0), idx;
	if (rf < 0 || rf > 39 || fabs(f - (2402.0 + 2.0 * rf)) > 1.0)
		return -1;
	idx = rf == 0 ? 37 : rf == 12 ? 38 : rf == 39 ? 39 : rf < 12 ? rf - 1 : rf - 2;
	*adv = idx >= 37;
	return idx;
}

static int wifi_channel(double f)
{
	int ch = (int)lround((f - 2407.0) / 5.0);
	if (ch < 1) ch = 1;
	if (ch > 13) ch = 13;
	return fabs(f - ch_mhz(ch)) <= 10.0 ? ch : -1;
}

static int zigbee_channel(double f)
{
	int k = (int)lround((f - 2405.0) / 5.0) + 11;
	return k >= 11 && k <= 26 && fabs(f - (2405.0 + 5.0 * (k - 11))) <= 1.5 ? k : -1;
}

/* bursts at the cursor: the level rises 8 dB over the background, then falls back
 * under 5 dB. Counts them, with the time since the last one, the typical spacing
 * and the last one's length; time resolution is one sweep */
#define BURST_ON 8.0f
#define BURST_OFF 5.0f
static struct {
	int on, cursor, level;
	long count;
	double last_start, last_len, interval;
} bst;
static int in_review;

static void burst_reset(void)
{
	memset(&bst, 0, sizeof(bst));
	bst.cursor = cursor;
	bst.level = vw.level;
}

static void burst_update(double t)
{
	float e = -1e9f;
	int i;
	if (!have_base)
		return;
	if (bst.cursor != cursor || bst.level != vw.level)
		burst_reset();
	for (i = cursor - 1; i <= cursor + 1; i++) /* the cursor's bin and its neighbours */
		if (i >= 0 && i < vw.nb && sweep_db[i] - base_db[i] > e)
			e = sweep_db[i] - base_db[i];
	if (!bst.on && e > BURST_ON) {
		bst.on = 1;
		if (bst.count) {
			double iv = t - bst.last_start;
			bst.interval = bst.count > 1 ? 0.7 * bst.interval + 0.3 * iv : iv;
		}
		bst.last_start = t;
		bst.count++;
	} else if (bst.on && e < BURST_OFF) {
		bst.on = 0;
		bst.last_len = t - bst.last_start;
	}
}

static void fmt_secs(char *s, size_t n, double v)
{
	if (v < 1.0)
		snprintf(s, n, "%.0fms", v * 1000);
	else
		snprintf(s, n, v < 100 ? "%.1fs" : "%.0fs", v);
}

/* the column right of the lower panel */
static void draw_bursts(void)
{
	int x = PX0 + PW + 6, y = WY0 + 26;
	char s[16];
	text(x, y, C_DIM, "BURSTS");
	snprintf(s, sizeof(s), "%ld", bst.count);
	text(x, y + 16, bst.on ? C_LIVE : C_TEXT, s);
	text(x, y + 40, C_DIM, "last");
	if (bst.count)
		fmt_secs(s, sizeof(s), now_s() - bst.last_start);
	else
		snprintf(s, sizeof(s), "-");
	text(x, y + 56, C_TEXT, s);
	text(x, y + 80, C_DIM, "every");
	if (bst.count > 1)
		fmt_secs(s, sizeof(s), bst.interval);
	else
		snprintf(s, sizeof(s), "-");
	text(x, y + 96, C_TEXT, s);
	text(x, y + 120, C_DIM, "length");
	if (bst.last_len > 0)
		fmt_secs(s, sizeof(s), bst.last_len);
	else
		snprintf(s, sizeof(s), "-");
	text(x, y + 136, C_TEXT, s);
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
	if (audio_on)
		text(SW - 92, 2, C_LIVE, "SND");
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
		x = mhz_x(f);
		if (x >= PX0 + PW)
			break;
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
			x = mhz_x(ch_mhz(chs[i]));
			snprintf(s, sizeof(s), "ch%d", chs[i]);
			text(x - 12, SY0 + SHGT + 18, C_AXIS, s);
		}
	} else {
		snprintf(s, sizeof(s), "tuned ch%d (%.0f MHz)", vw.ch, ch_mhz(vw.ch));
		text(PX0, SY0 + SHGT + 18, C_AXIS, s);
	}

	/* spectrum */
	if (sweeps || in_review) {
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

	/* Bluetooth LE advertising channels 37/38/39: ticks on the bottom edge */
	{
		static const double adv_mhz[3] = { 2402.0, 2426.0, 2480.0 };
		for (i = 0; i < 3; i++) {
			if (adv_mhz[i] < vw.f0 || adv_mhz[i] >= vw.f0 + span)
				continue;
			x = mhz_x(adv_mhz[i]);
			fill(x - 1, SY0 + SHGT - 7, 3, 7, C_HOLD);
			if (vw.level == 0)
				text(x - 12, SY0 + SHGT + 18, cscale(C_HOLD, 0.7f), "adv");
		}
	}

	/* cursor */
	if (cursor >= vw.nb)
		cursor = vw.nb - 1;
	x = bin_x(cursor) + PW / vw.nb / 2;
	for (y = SY0; y < SY0 + SHGT; y += 3)
		canvas[y * SW + x] = C_CURSOR;
	snprintf(s, sizeof(s), vw.level ? "%.2f" : "%.1f", bin_mhz(cursor));
	text(PX0 + PW + 6, SY0 + 2, C_CURSOR, s);
	if (sweeps || in_review) {
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

	/* what the cursor frequency is used for */
	{
		double fc = bin_mhz(cursor);
		int adv = 0, b = ble_channel(fc, &adv), w = wifi_channel(fc), z = zigbee_channel(fc), yy = SY0 + 122;
		if (b >= 0) {
			snprintf(s, sizeof(s), adv ? "ADV %d" : "BLE %d", b);
			text(PX0 + PW + 6, yy, adv ? C_HOLD : C_TEXT, s);
			yy += 16;
		}
		if (w > 0) {
			snprintf(s, sizeof(s), "WiFi %d", w);
			text(PX0 + PW + 6, yy, C_TEXT, s);
			yy += 16;
		}
		if (z > 0) {
			snprintf(s, sizeof(s), "ZB %d", z);
			text(PX0 + PW + 6, yy, C_TEXT, s);
		}
	}

	/* lower panel: waterfall (newest on top) or channel occupancy */
	if (disp_mode == 1) {
		draw_occupancy();
	} else {
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
	}
	if (in_review) /* review draws its own title and footer */
		return;
	draw_bursts();

	/* help / status line (80 characters fit) */
	if (sel_down)
		text(4, SH - 17, C_ZOOM, "SELECT +  Y style  A palette  B occupancy  X sound  START review  (tap: peak)");
	else if (status_until > now_s())
		text(4, SH - 17, C_TEXT, status_msg);
	else
		text(4, SH - 17, C_DIM, "R2/L2 zoom  SEL peak  Y enhance  A hold  B clear  X rec  L1/R1 gain  START png");
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
static uint32_t keys_down[4]; /* key codes 0..127 currently held */

#define KEY_SHIFTED 0x10000 /* next_key(): pressed while SELECT is held */

/* Every screen reads buttons through here. Returns the next press, or -1 when
 * there is none. Holding a key repeats only for the d-pad and L1/R1, so holding
 * X does not toggle recording on and off. SELECT is a shift layer: keys pressed
 * while it is held come back with KEY_SHIFTED, and a tap on its own comes back
 * as KEY_RIGHTCTRL when it is released. */
static int next_key(void)
{
#ifdef __linux__
	static struct input_event ev[16];
	static int n, k;
	while (in_fd >= 0) {
		const struct input_event *e;
		int code, val;
		if (k >= n) {
			ssize_t got = read(in_fd, ev, sizeof(ev));
			if (got <= 0)
				return -1;
			n = (int)(got / (ssize_t)sizeof(ev[0]));
			k = 0;
		}
		e = &ev[k++];
		code = e->code;
		val = e->value;
		if (e->type != EV_KEY)
			continue;
		if (code < 128) {
			if (val)
				keys_down[code >> 5] |= 1u << (code & 31);
			else
				keys_down[code >> 5] &= ~(1u << (code & 31));
		}
		if (code == KEY_RIGHTCTRL) {
			if (val == 1) {
				sel_down = 1;
				sel_combo = 0;
			} else if (val == 0) {
				sel_down = 0;
				if (!sel_combo)
					return KEY_RIGHTCTRL;
			}
			continue;
		}
		if (val == 0 || (val == 2 && code != KEY_UP && code != KEY_DOWN && code != KEY_LEFT &&
		                 code != KEY_RIGHT && code != KEY_E && code != KEY_T))
			continue;
		if (sel_down) {
			sel_combo = 1;
			return code | KEY_SHIFTED;
		}
		return code;
	}
#endif
	return -1;
}

/* Sleep until a button is pressed or max_s seconds pass (signals end it too). */
static void wait_input(double max_s)
{
	struct pollfd p;
	int ms = max_s <= 0 ? 0 : (int)(max_s * 1000 + 0.5);
	if (in_fd < 0) {
		usleep((useconds_t)ms * 1000);
		return;
	}
	p.fd = in_fd;
	p.events = POLLIN;
	p.revents = 0;
	poll(&p, 1, ms);
}

/* Launched over SSH while Onion's MainUI is up, the app would share the screen
 * and the buttons with it: A in a replay would also start MainUI's highlighted
 * game. So MainUI is paused and the buttons grabbed while the app has the screen,
 * and both are handed back on exit, the screen as MainUI left it. A normal launch
 * from Onion's menu finds no MainUI running and changes nothing. */
static pid_t ui_pid;
static uint8_t *fb_saved;
static int in_grabbed;

static pid_t find_process(const char *name)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	pid_t found = 0;
	if (d == NULL)
		return 0;
	while (!found && (e = readdir(d)) != NULL) {
		char path[300], comm[64];
		FILE *f;
		if (e->d_name[0] < '1' || e->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
		if ((f = fopen(path, "r")) == NULL)
			continue;
		if (fgets(comm, sizeof(comm), f) != NULL) {
			comm[strcspn(comm, "\n")] = 0;
			if (!strcmp(comm, name))
				found = (pid_t)atoi(e->d_name);
		}
		fclose(f);
	}
	closedir(d);
	return found;
}

static void ui_resume(void)
{
	if (ui_pid > 0) {
		kill(ui_pid, SIGCONT);
		fprintf(stderr, "MainUI resumed\n");
		ui_pid = 0;
	}
}

static void on_fatal(int sig) /* never leave MainUI frozen */
{
	if (ui_pid > 0)
		kill(ui_pid, SIGCONT);
	signal(sig, SIG_DFL);
	raise(sig);
}

static int fb_open(void)
{
#ifdef __linux__
	const char *fb_path = getenv("MIYOOSDR_FB"), *in_path = getenv("MIYOOSDR_INPUT");
	static const int fatal[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL };
	unsigned i;

	fb_fd = open(fb_path ? fb_path : "/dev/fb0", O_RDWR);
	if (fb_fd < 0)
		return -1;
	ui_pid = find_process("MainUI");
	if (ui_pid > 0) {
		for (i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++)
			signal(fatal[i], on_fatal);
		kill(ui_pid, SIGSTOP);
	}
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0)
		goto fail;
	orig_var = var;
	fb_size = fix.smem_len;
	fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED) {
		fb_mem = NULL;
		goto fail;
	}
	if (ui_pid > 0 && (fb_saved = malloc(fb_size)) != NULL) /* before the mode change can touch it */
		memcpy(fb_saved, fb_mem, fb_size);
	var.bits_per_pixel = 32;
	var.yres_virtual = var.yres * 2;
	var.yoffset = 0;
	if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &var) < 0) {
		var = orig_var;
		ioctl(fb_fd, FBIOPUT_VSCREENINFO, &var);
	}
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0 ||
	    var.bits_per_pixel != 32)
		goto fail;
	if (fix.smem_len != fb_size) { /* the mode change moved the buffer */
		munmap(fb_mem, fb_size);
		free(fb_saved);
		fb_saved = NULL;
		fb_size = fix.smem_len;
		fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
		if (fb_mem == MAP_FAILED) {
			fb_mem = NULL;
			goto fail;
		}
	}
	pages = (var.yres_virtual >= var.yres * 2 && fb_size >= (size_t)fix.line_length * var.yres * 2) ? 2 : 1;
	in_fd = open(in_path ? in_path : "/dev/input/event0", O_RDONLY | O_NONBLOCK);
	if (ui_pid > 0 && in_fd >= 0 && ioctl(in_fd, EVIOCGRAB, 1) == 0)
		in_grabbed = 1;
	if (ui_pid > 0)
		fprintf(stderr, "MainUI (pid %d) paused%s while MiYooSDR has the screen\n", (int)ui_pid,
		        in_grabbed ? ", buttons grabbed" : "");
	return 0;
fail:
	if (fb_mem) {
		ioctl(fb_fd, FBIOPUT_VSCREENINFO, &orig_var);
		munmap(fb_mem, fb_size);
		fb_mem = NULL;
	}
	free(fb_saved);
	fb_saved = NULL;
	ui_resume();
	return -1;
#else
	return -1;
#endif
}

static int keys_held(void)
{
	return (keys_down[0] | keys_down[1] | keys_down[2] | keys_down[3]) != 0;
}

static void fb_close(void)
{
#ifdef __linux__
	if (fb_mem) {
		memset(fb_mem, 0, fb_size);
		var.yoffset = 0;
		ioctl(fb_fd, FBIOPAN_DISPLAY, &var);
		ioctl(fb_fd, FBIOPUT_VSCREENINFO, &orig_var);
		if (fb_saved) /* MainUI's own frame, so it looks as it did when paused */
			memcpy(fb_mem, fb_saved, fb_size);
		munmap(fb_mem, fb_size);
		fb_mem = NULL;
	}
	free(fb_saved);
	fb_saved = NULL;
	if (in_grabbed) { /* let go only once the buttons are up, or MainUI gets a stray release */
		double until = now_s() + 1.5;
		while (keys_held() && now_s() < until) {
			wait_input(0.05);
			while (next_key() >= 0)
				;
		}
		ioctl(in_fd, EVIOCGRAB, 0);
		in_grabbed = 0;
	}
	ui_resume();
#endif
}

static void present(void)
{
#ifdef __linux__
	int target = pages == 2 ? page ^ 1 : 0, y, x, w, h;
	int ox = ((int)var.xres - SW) / 2, oy = ((int)var.yres - SH) / 2;
	size_t stride = fix.line_length;
	uint8_t *base;

	if (!fb_mem)
		return;
	if (ox < 0) ox = 0;
	if (oy < 0) oy = 0;
	w = (int)var.xres - ox < SW ? (int)var.xres - ox : SW;
	h = (int)var.yres - oy < SH ? (int)var.yres - oy : SH;
	base = fb_mem + (size_t)target * var.yres * stride;
	/* the panel is mounted upside down: rotate 180 degrees */
	for (y = 0; y < h; y++) {
		const uint32_t *src = canvas + (size_t)(SH - 1 - y) * SW + SW - 1;
		uint32_t *dst = (uint32_t *)(base + (size_t)(y + oy) * stride) + ox;
		for (x = 0; x < w; x++)
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
	unsigned char row[SW * 3];
	int x, y, ok;
	if (!f)
		return -1;
	ok = fprintf(f, "P6\n%d %d\n255\n", SW, SH) > 0;
	for (y = 0; ok && y < SH; y++) {
		for (x = 0; x < SW; x++) {
			uint32_t c = canvas[y * SW + x];
			row[3 * x] = (unsigned char)(c >> 16);
			row[3 * x + 1] = (unsigned char)(c >> 8);
			row[3 * x + 2] = (unsigned char)c;
		}
		ok = fwrite(row, 1, sizeof(row), f) == sizeof(row);
	}
	if (fclose(f) != 0)
		ok = 0;
	return ok ? 0 : -1;
}

/* PNG: zlib stream holding one fixed-Huffman deflate block (RFC 1951) with LZ77
 * matches. Screens are mostly flat colour, so trying the previous pixel and the
 * previous row besides a hash of the next three bytes finds nearly every match.
 * No zlib needed, and the bytes come out the same on the PC and the Miyoo. */
struct bit_out {
	uint8_t *p;
	uint32_t acc;
	int n;
};

static void put_bits(struct bit_out *w, uint32_t v, int n)
{
	w->acc |= v << w->n;
	w->n += n;
	while (w->n >= 8) {
		*w->p++ = (uint8_t)w->acc;
		w->acc >>= 8;
		w->n -= 8;
	}
}

static void put_code(struct bit_out *w, uint32_t code, int len) /* Huffman codes go MSB first */
{
	uint32_t r = 0;
	int i;
	for (i = 0; i < len; i++)
		r |= ((code >> i) & 1u) << (len - 1 - i);
	put_bits(w, r, len);
}

static void put_symbol(struct bit_out *w, int v)
{
	if (v < 144)
		put_code(w, 0x30 + v, 8);
	else if (v < 256)
		put_code(w, 0x190 + v - 144, 9);
	else if (v < 280)
		put_code(w, v - 256, 7);
	else
		put_code(w, 0xc0 + v - 280, 8);
}

static void put_match(struct bit_out *w, int len, int dist)
{
	static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
	static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	                                       3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
	static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257,
	                                        385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
	                                        12289, 16385, 24577 };
	static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7,
	                                        8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
	int c = 28, d = 29;
	while (len_base[c] > len)
		c--;
	put_symbol(w, 257 + c);
	put_bits(w, (uint32_t)(len - len_base[c]), len_extra[c]);
	while (dist_base[d] > dist)
		d--;
	put_code(w, (uint32_t)d, 5);
	put_bits(w, (uint32_t)(dist - dist_base[d]), dist_extra[d]);
}

#define LZ_HASH_BITS 15

/* Deflate n bytes of rows `stride` long into dst (room for n + n / 8 + 16 bytes). */
static size_t deflate_fixed(const uint8_t *src, uint32_t n, uint32_t stride, uint8_t *dst)
{
	int32_t *head = malloc(sizeof(int32_t) << LZ_HASH_BITS);
	struct bit_out w = { dst, 0, 0 };
	uint32_t i = 0;
	if (head == NULL)
		return 0;
	memset(head, 0xff, sizeof(int32_t) << LZ_HASH_BITS);
	put_bits(&w, 1, 1); /* BFINAL */
	put_bits(&w, 1, 2); /* BTYPE 01: fixed Huffman codes */
	while (i < n) {
		int best = 0, bestd = 0, c;
		if (i + 3 <= n) {
			uint32_t h = ((uint32_t)src[i] << 16 | (uint32_t)src[i + 1] << 8 | src[i + 2]) * 2654435761u >>
			             (32 - LZ_HASH_BITS);
			int64_t cand[3];
			cand[0] = head[h];
			cand[1] = (int64_t)i - 3;
			cand[2] = (int64_t)i - stride;
			head[h] = (int32_t)i;
			for (c = 0; c < 3; c++) {
				uint32_t from, max = n - i < 258 ? n - i : 258, len = 0;
				if (cand[c] < 0 || i - cand[c] > 32768)
					continue;
				from = (uint32_t)cand[c];
				while (len < max && src[from + len] == src[i + len])
					len++;
				if ((int)len > best) {
					best = (int)len;
					bestd = (int)(i - from);
				}
			}
		}
		if (best >= 3) {
			put_match(&w, best, bestd);
			i += (uint32_t)best;
		} else {
			put_symbol(&w, src[i++]);
		}
	}
	put_symbol(&w, 256); /* end of block */
	if (w.n)
		*w.p++ = (uint8_t)w.acc;
	free(head);
	return (size_t)(w.p - dst);
}

static uint32_t adler32(const uint8_t *p, uint32_t n)
{
	uint32_t a = 1, b = 0;
	while (n) { /* 5552 bytes cannot overflow the sums before reducing them */
		uint32_t k = n < 5552 ? n : 5552;
		n -= k;
		while (k--) {
			a += *p++;
			b += a;
		}
		a %= 65521;
		b %= 65521;
	}
	return b << 16 | a;
}

static uint32_t crc_tab[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n)
{
	size_t i;
	if (!crc_tab[1]) {
		uint32_t k, v;
		int b;
		for (k = 0; k < 256; k++) {
			v = k;
			for (b = 0; b < 8; b++)
				v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
			crc_tab[k] = v;
		}
	}
	for (i = 0; i < n; i++)
		c = crc_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return c;
}

static void put_be32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)(v >> 24);
	b[1] = (uint8_t)(v >> 16);
	b[2] = (uint8_t)(v >> 8);
	b[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t b[4];
	uint32_t c;
	put_be32(b, len);
	fwrite(b, 1, 4, f);
	fwrite(type, 1, 4, f);
	if (len)
		fwrite(data, 1, len, f);
	c = crc32_update(0xFFFFFFFFu, (const uint8_t *)type, 4);
	c = crc32_update(c, data, len) ^ 0xFFFFFFFFu;
	put_be32(b, c);
	fwrite(b, 1, 4, f);
}

static int write_png(const char *path)
{
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	const uint32_t row = 1 + SW * 3, raw = row * SH;
	uint8_t *rb = malloc(raw), *z = malloc(2 + raw + raw / 8 + 16 + 4), *p, ihdr[13];
	size_t zn;
	int x, y, ok = -1, failed;
	FILE *f;

	if (!rb || !z)
		goto out;
	for (y = 0; y < SH; y++) {
		uint8_t *r = rb + (size_t)y * row;
		*r++ = 0; /* filter: none */
		for (x = 0; x < SW; x++) {
			uint32_t c = canvas[y * SW + x];
			*r++ = (uint8_t)(c >> 16);
			*r++ = (uint8_t)(c >> 8);
			*r++ = (uint8_t)c;
		}
	}
	p = z;
	*p++ = 0x78; /* zlib header: deflate, 32 KB window */
	*p++ = 0x01;
	zn = deflate_fixed(rb, raw, row, p);
	if (zn == 0)
		goto out;
	p += zn;
	put_be32(p, adler32(rb, raw));
	p += 4;
	f = fopen(path, "wb");
	if (!f)
		goto out;
	put_be32(ihdr, SW);
	put_be32(ihdr + 4, SH);
	ihdr[8] = 8;  /* bits per channel */
	ihdr[9] = 2;  /* RGB */
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	fwrite(sig, 1, 8, f);
	png_chunk(f, "IHDR", ihdr, 13);
	png_chunk(f, "IDAT", z, (uint32_t)(p - z));
	png_chunk(f, "IEND", NULL, 0);
	failed = ferror(f);
	ok = fclose(f) == 0 && !failed ? 0 : -1;
out:
	free(rb);
	free(z);
	return ok;
}

static int write_image(const char *path)
{
	size_t l = strlen(path);
	return l > 4 && !strcmp(path + l - 4, ".png") ? write_png(path) : write_ppm(path);
}

static void status(const char *msg);

/* dir/prefix-YYYYMMDD-HHMMSS.ext, with -2, -3 ... added when that second is
 * taken, so quick repeats never overwrite each other */
static void timestamped_name(char *out, size_t n, const char *dir, const char *prefix, const char *ext)
{
	time_t t = time(NULL);
	struct tm tm;
	struct stat st;
	int k;
	localtime_r(&t, &tm);
	mkdir(dir, 0755);
	for (k = 1; k < 100; k++) {
		char suffix[8] = "";
		if (k > 1)
			snprintf(suffix, sizeof(suffix), "-%d", k);
		snprintf(out, n, "%s/%s-%04d%02d%02d-%02d%02d%02d%s.%s", dir, prefix, tm.tm_year + 1900, tm.tm_mon + 1,
		         tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, suffix, ext);
		if (stat(out, &st) != 0)
			return;
	}
}

/* START: shots/sdr-<date>-<time>.png */
static void save_screenshot(void)
{
	char name[96], m[128];
	timestamped_name(name, sizeof(name), "shots", "sdr", "png");
	if (write_png(name) == 0) {
		sync();
		snprintf(m, sizeof(m), "screenshot saved: %s", name);
		status(m);
	} else {
		status("cannot write the screenshot");
	}
}

/* settings.txt next to the app: the view you left, restored on the next plain start
 * (started with options, as in tests, it is neither read nor written) */
static void settings_save(void)
{
	FILE *f = fopen("settings.txt", "w");
	if (!f)
		return;
	fprintf(f, "# MiYooSDR settings, written on exit\n");
	fprintf(f, "palette=%d\nstyle=%d\npanel=%d\nenhance=%d\nhold=%d\n", palette, spec_style, disp_mode, enhance,
	        show_hold);
	fprintf(f, "zoom=%d\nzoom_mhz=%.4f\ncursor_mhz=%.4f\n", vw.level, bin_mhz(vw.nb / 2), bin_mhz(cursor));
	fclose(f);
	sync();
}

static void settings_load(void)
{
	FILE *f = fopen("settings.txt", "r");
	char line[96], key[32];
	int zoom = 0;
	double v, zoom_mhz = 0, cursor_mhz = 0;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%31[a-z_]=%lf", key, &v) != 2)
			continue;
		if (!strcmp(key, "palette") && v >= 0 && v < NPAL) palette = (int)v;
		else if (!strcmp(key, "style") && v >= 0 && v <= 2) spec_style = (int)v;
		else if (!strcmp(key, "panel")) disp_mode = v != 0;
		else if (!strcmp(key, "enhance")) enhance = v != 0;
		else if (!strcmp(key, "hold")) show_hold = v != 0;
		else if (!strcmp(key, "zoom") && v >= 0 && v <= 3) zoom = (int)v;
		else if (!strcmp(key, "zoom_mhz")) zoom_mhz = v;
		else if (!strcmp(key, "cursor_mhz")) cursor_mhz = v;
	}
	fclose(f);
	if (zoom && zoom_mhz > 2395 && zoom_mhz < 2490)
		view_zoom(zoom_mhz, zoom);
	if (cursor_mhz >= bin_mhz(0) - vw.bw / 2 && cursor_mhz <= bin_mhz(vw.nb - 1) + vw.bw / 2)
		cursor = mhz_bin(cursor_mhz);
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
	timestamped_name(rec_name, sizeof(rec_name), "rec", "sdr", "csv");
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
	fprintf(rec, "%.3f,%d,%d", now_s() - t0, sweep_gain, vw.level);
	for (i = 0; i < vw.nb; i++)
		fprintf(rec, ",%d", (int)lrintf(sweep_db[i] * 10));
	fputc('\n', rec);
}

static void set_view_zoom(double target, int level)
{
	char m[96];
	if (level <= 0) {
		double f = bin_mhz(cursor);
		view_full();
		cursor = mhz_bin(f);
		snprintf(m, sizeof(m), "full band");
	} else {
		view_zoom(target, level);
		cursor = mhz_bin(target);
		snprintf(m, sizeof(m), "zoom %.0f MHz around %.2f MHz (ch %d)", vw.nb * vw.bw, bin_mhz(cursor), vw.ch);
	}
	view_gen++;
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
	snprintf(m, sizeof(m), "peak %.2f MHz, %.0f dB over background", bin_mhz(best), bestv);
	status(m);
}

static int shot_req;

static void on_button(int code)
{
#ifdef __linux__
	double fcur = bin_mhz(cursor);
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
	case KEY_LEFTCTRL: /* SEL + B: waterfall <-> channel occupancy */
		disp_mode ^= 1;
		status(disp_mode ? "channel occupancy" : "waterfall");
		break;
	case KEY_LEFTSHIFT: /* SEL + X: signal-to-sound */
		if (audio_on) {
			audio_stop();
			audio_on = 0;
			status("audio off");
		} else if (audio_start() == 0) {
			audio_on = 1;
			status("audio: pitch follows the cursor level");
		} else {
			status("audio unavailable");
		}
		break;
	case KEY_ENTER: /* SEL + START: open a saved recording */
		review_request = 1;
		break;
	}
}

static void poll_input(void)
{
#ifdef __linux__
	int code;
	while ((code = next_key()) >= 0) {
		if (code == KEY_RIGHTCTRL) /* SELECT tapped on its own */
			find_peak();
		else if (code & KEY_SHIFTED)
			sel_layer(code & ~KEY_SHIFTED);
		else
			on_button(code);
	}
#endif
}

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

/* ----------------------------------------------------------- review mode */
/* Packet playback uses capture timestamps and metadata, rather than inventing
 * a spectrum from decoded frames. It also works without opening the radio. */
#define PACKET_X 24
#define PACKET_W 592
#define TL_TOP 68 /* traffic timeline panel: rows TL_TOP .. TL_TOP + TL_H - 1 */
#define TL_H 78

static int timeline_column(double t, double duration)
{
	int x = duration > 0 ? (int)(t / duration * PACKET_W) : 0;
	return x < 0 ? 0 : x >= PACKET_W ? PACKET_W - 1 : x;
}

/* Shown before a load that can take seconds from the SD card. */
static void loading_screen(const char *path)
{
	char s[128];
	const char *name = strrchr(path, '/');
	fill(0, 0, SW, SH, C_BG);
	snprintf(s, sizeof(s), "Loading %.90s ...", name ? name + 1 : path);
	text(24, SH / 2 - 8, C_TEXT, s);
	present();
}

static size_t packet_at_time(const struct packet_capture *capture, double pos)
{
	size_t lo = 0, hi = capture->count;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (capture->packets[mid].time <= pos) lo = mid + 1;
		else hi = mid;
	}
	return lo ? lo - 1 : 0;
}

static void packet_signal(const struct packet_record *packet, char *out, size_t size)
{
	if (packet->signal == PACKET_SIGNAL_UNKNOWN) snprintf(out, size, "---");
	else snprintf(out, size, "%d%s", packet->signal, packet->signal == 0 ? "?" : "");
}

static void packet_rate(const struct packet_record *packet, char *out, size_t size)
{
	if (packet->mcs != PACKET_MCS_UNKNOWN) snprintf(out, size, "MCS %u", packet->mcs);
	else if (packet->rate_half) snprintf(out, size, "%g%s Mb/s", packet->rate_half / 2.0,
	                                  packet->rate_half == 93 ? "?" : "");
	else snprintf(out, size, "---");
}

static void packet_address(const uint8_t *address, int valid, char *out, size_t size)
{
	if (!valid) snprintf(out, size, "---");
	else snprintf(out, size, "%02x:%02x:%02x:%02x:%02x:%02x", address[0], address[1], address[2],
	              address[3], address[4], address[5]);
}

static void packet_render(const struct packet_capture *capture, const char *path, size_t selected,
                          double pos, double speed, int playing,
                          unsigned bins[3][PACKET_W], unsigned peak)
{
	const struct packet_record *packet = &capture->packets[selected];
	static const uint32_t colours[3] = { C_LIVE, C_AVG, C_HOLD };
	char s[256], signal[24], rate_text[32], tx[24], rx[24], channel[32], sequence[16];
	const char *name = strrchr(path, '/');
	size_t first = selected > 3 ? selected - 3 : 0, r;
	int x, category;
	fill(0, 0, SW, SH, C_BG);
	text(12, 6, C_ZOOM, "PACKET REPLAY");
	snprintf(s, sizeof(s), "%.3f / %.3f s  x%g  %s", pos, capture->duration, speed,
	         playing ? "PLAY" : pos >= capture->duration ? "END" : "PAUSE");
	text(200, 6, playing ? C_LIVE : C_TEXT, s);
	snprintf(s, sizeof(s), "%.76s", name ? name + 1 : path);
	text(12, 26, C_DIM, s);
	text(24, 48, C_TEXT, "TRAFFIC: packets / time bin");
	text(376, 48, C_LIVE, "MGMT"); text(440, 48, C_AVG, "CTRL"); text(504, 48, C_HOLD, "DATA");
	fill(PACKET_X, TL_TOP, PACKET_W, TL_H, C_PANEL);
	for (x = 0; x < PACKET_W; x++) { /* stacked: management, control, data */
		int bottom = TL_TOP + TL_H - 1, filled = 0;
		unsigned cumulative = 0;
		for (category = 0; category < 3; category++) {
			int height;
			if (!bins[category][x])
				continue;
			cumulative += bins[category][x];
			height = (int)lround(cumulative * (TL_H - 1.0) / peak);
			if (height <= filled) /* every category with frames shows at least a pixel */
				height = filled + 1;
			if (height > TL_H - 1)
				height = TL_H - 1;
			if (height > filled)
				line(PACKET_X + x, bottom - filled, PACKET_X + x, bottom - height + 1, colours[category]);
			filled = height;
		}
	}
	x = timeline_column(pos, capture->duration);
	line(PACKET_X + x, TL_TOP - 2, PACKET_X + x, TL_TOP + TL_H + 1, C_CURSOR);
	snprintf(s, sizeof(s), "0 s       peak %u/bin; bin %.2f ms", peak, capture->duration * 1000 / PACKET_W);
	text(24, TL_TOP + TL_H + 4, C_DIM, s);
	snprintf(s, sizeof(s), "%.3f s", capture->duration); text(536, TL_TOP + TL_H + 4, C_DIM, s);
	snprintf(s, sizeof(s), "Frame %zu/%zu   %s   %u bytes", selected + 1, capture->count,
	         packet_type_name(packet), packet->length);
	text(24, 176, C_TEXT, s);
	packet_signal(packet, signal, sizeof(signal));
	packet_rate(packet, rate_text, sizeof(rate_text));
	if (packet->frequency) snprintf(channel, sizeof(channel), "%u MHz", packet->frequency);
	else snprintf(channel, sizeof(channel), "---");
	snprintf(s, sizeof(s), "%s   Signal %s dBm   Rate %s", channel, signal, rate_text);
	text(24, 196, C_AVG, s);
	packet_address(packet->transmitter, packet->has_transmitter, tx, sizeof(tx));
	packet_address(packet->receiver, packet->has_receiver, rx, sizeof(rx));
	snprintf(s, sizeof(s), "TX %s   RX %s", tx, rx); text(24, 216, C_DIM, s);
	if (packet->sequence != PACKET_SEQUENCE_UNKNOWN) snprintf(sequence, sizeof(sequence), "%u", packet->sequence);
	else snprintf(sequence, sizeof(sequence), "---");
	snprintf(s, sizeof(s), "Seq %s   %s%s%s", sequence, packet->protected_frame ? "PROTECTED  " : "",
	         packet->retry ? "RETRY  " : "", packet->bad_fcs ? "BAD FCS" : "");
	text(24, 236, packet->bad_fcs ? C_REC : C_DIM, s);
	text(24, 262, C_DIM, " TIME (s)  FRAME TYPE       BYTES    SIGNAL   RATE");
	if (first + 7 > capture->count) first = capture->count > 7 ? capture->count - 7 : 0;
	for (r = first; r < capture->count && r < first + 7; r++) {
		int y = 282 + (int)(r - first) * 20;
		const struct packet_record *p = &capture->packets[r];
		packet_signal(p, signal, sizeof(signal)); packet_rate(p, rate_text, sizeof(rate_text));
		if (r == selected) fill(16, y - 1, 608, 19, C_PANEL);
		snprintf(s, sizeof(s), "%9.4f  %-15.15s %5u  %7s   %s", p->time, packet_type_name(p),
		         p->length, signal, rate_text);
		text(24, y, r == selected ? C_LIVE : C_DIM, s);
	}
	if (status_until > now_s()) {
		text(24, 426, C_HOLD, status_msg);
	} else if (capture->limited || capture->skipped) {
		snprintf(s, sizeof(s), "%s%zu skipped; showing %zu frames", capture->limited ? "LIMIT: " : "",
		         capture->skipped, capture->count);
		text(24, 426, C_HOLD, s);
	} else text(24, 426, C_DIM, "Driver metadata; ? = suspect value. Timeline shows packet counts.");
	text(8, 448, C_TEXT, "A play/pause  Left/Right packet  L1/R1 speed  Up/Down seek 1s");
	text(8, 466, C_DIM, "B back  MENU quit  START screenshot");
}

static int packet_review(const char *path, const char *shot, double start)
{
	static unsigned bins[3][PACKET_W];
	struct packet_capture capture;
	char error[160];
	unsigned peak = 0;
	double pos, last, speed = 1;
	size_t selected, i, drawn = (size_t)-1;
	int playing = !shot, back = 0, result = 0, dirty = 1, drawn_x = -1, status_was = 0;
	if (!shot)
		loading_screen(path);
	if (pcap_load(path, &capture, error, sizeof(error)) != 0) {
		fprintf(stderr, "PCAP: %s\n", error); status(error); return 1;
	}
	memset(bins, 0, sizeof(bins));
	pos = start < capture.duration ? start : capture.duration;
	selected = packet_at_time(&capture, pos);
	for (i = 0; i < capture.count; i++) {
		const struct packet_record *p = &capture.packets[i];
		int bin = timeline_column(p->time, capture.duration);
		unsigned total;
		int category = p->type < 3 ? p->type : 2;
		bins[category][bin]++;
		total = bins[0][bin] + bins[1][bin] + bins[2][bin];
		if (total > peak) peak = total;
	}
	if (shot) {
		packet_render(&capture, path, selected, pos, speed, playing, bins, peak);
		result = write_image(shot) != 0;
		goto done;
	}
	audio_quiet = 1; /* no level to follow here */
	while (next_key() >= 0) /* presses made while it loaded */
		;
	last = now_s();
	while (!quit && !back) {
		double t = now_s();
		int code, status_now = status_until > t;
		if (playing) {
			pos += (t - last) * speed;
			if (pos >= capture.duration) { pos = capture.duration; playing = 0; dirty = 1; }
			selected = packet_at_time(&capture, pos);
		}
		last = t;
		if (selected != drawn || timeline_column(pos, capture.duration) != drawn_x || status_now != status_was)
			dirty = 1;
		status_was = status_now;
		if (dirty) { /* redraw only on change, so a paused replay leaves the CPU alone */
			packet_render(&capture, path, selected, pos, speed, playing, bins, peak);
			present();
			drawn = selected;
			drawn_x = timeline_column(pos, capture.duration);
			dirty = 0;
		}
		while (!back && !quit && (code = next_key()) >= 0) {
			dirty = 1;
			switch (code & ~KEY_SHIFTED) {
			case KEY_SPACE:
				if (!playing && selected + 1 == capture.count && pos >= capture.duration) {
					pos = 0; selected = 0;
				}
				playing = !playing; break;
			case KEY_LEFT:
				playing = 0; if (selected) selected--;
				pos = capture.packets[selected].time; break;
			case KEY_RIGHT:
				playing = 0; if (selected + 1 < capture.count) selected++;
				pos = capture.packets[selected].time; break;
			case KEY_UP: case KEY_DOWN:
				pos += (code & ~KEY_SHIFTED) == KEY_UP ? 1 : -1;
				if (pos < 0) pos = 0;
				if (pos > capture.duration) pos = capture.duration;
				selected = packet_at_time(&capture, pos); break;
			case KEY_E: if (speed > 0.25) speed /= 2; break;
			case KEY_T: if (speed < 8) speed *= 2; break;
			case KEY_ENTER: save_screenshot(); break;
			case KEY_LEFTCTRL: back = 1; break;
			case KEY_ESC: quit = 1; break;
			}
		}
		if (!dirty && !back && !quit)
			wait_input(playing ? 1.0 / 30 : status_now ? status_until - now_s() + 0.01 : 0.5);
	}
	audio_quiet = 0;
done:
	fprintf(stderr, "replay: %zu frames, %.6f seconds, %zu skipped%s\n", capture.count,
	        capture.duration, capture.skipped, capture.limited ? " (record limit reached)" : "");
	pcap_free(&capture);
	return result;
}

/* Spectrum CSV playback, for sweep recordings (X) and burst captures. A sweep
 * recording starts a "# view" section each time the view changed while it was
 * recording, and every row is shown on its own section's frequency axis. */
#ifndef REVIEW_MAX_VALUES
#define REVIEW_MAX_VALUES (4u << 20) /* readings kept: 8 MB, about an hour of full band */
#endif
#define REVIEW_MAX_SECTIONS 256

struct rec_section {
	int level, nb, ch;
	double f0, bw;
	size_t first, rows;           /* rows first .. first + rows - 1 */
	float mn[MAXBINS], mx[MAXBINS];
};

struct recording {
	struct rec_section *sections;
	size_t nsections;
	double *ts;                   /* per row: seconds from the first row, never decreasing */
	uint32_t *start;              /* per row: index of its first reading */
	uint16_t *section;            /* per row */
	int16_t *values;              /* dB*10 readings */
	size_t rows, row_cap, nvalues, value_cap;
	int truncated;
	double duration;
};

static void recording_free(struct recording *r)
{
	free(r->sections);
	free(r->ts);
	free(r->start);
	free(r->section);
	free(r->values);
	memset(r, 0, sizeof(*r));
}

static int recording_grow(struct recording *r, int nb)
{
	if (r->rows == r->row_cap) {
		size_t cap = r->row_cap ? r->row_cap * 2 : 1024;
		double *ts = realloc(r->ts, cap * sizeof(*ts));
		uint32_t *start = ts ? realloc(r->start, cap * sizeof(*start)) : NULL;
		uint16_t *section = start ? realloc(r->section, cap * sizeof(*section)) : NULL;
		if (ts) r->ts = ts;
		if (start) r->start = start;
		if (!section) return -1;
		r->section = section;
		r->row_cap = cap;
	}
	if (r->nvalues + (size_t)nb > r->value_cap) {
		size_t cap = r->value_cap ? r->value_cap * 2 : 65536;
		int16_t *values;
		while (cap < r->nvalues + (size_t)nb) cap *= 2;
		if (cap > REVIEW_MAX_VALUES) cap = REVIEW_MAX_VALUES;
		values = realloc(r->values, cap * sizeof(*values));
		if (!values) return -1;
		r->values = values;
		r->value_cap = cap;
	}
	return 0;
}

static int bad_section(const struct rec_section *s)
{
	return s->level < 0 || s->level > 3 || s->nb < 0 || s->nb > MAXBINS || s->ch < 0 || s->ch > 14 ||
	       !(s->f0 >= 2300 && s->f0 <= 2600) || !(s->bw > 0.001 && s->bw <= 1);
}

/* Returns 0, or -1 with a message. Rows that do not fit their section (a wrong
 * number of readings, or no time) are skipped; past REVIEW_MAX_VALUES readings
 * the rest of the file is left out and truncated is set. */
static int recording_load(const char *path, struct recording *r, char *error, size_t error_size)
{
	static char line[8192];
	struct rec_section cur;
	FILE *f;
	int leading = 2, pending = 1, i;
	size_t k;
	double last = 0;

	memset(r, 0, sizeof(*r));
	memset(&cur, 0, sizeof(cur));
	cur.f0 = 2402.0;
	cur.bw = 0.3125;
	if ((f = fopen(path, "r")) == NULL) {
		snprintf(error, error_size, "Cannot open recording: %s", strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		char *p, *end;
		int nb = 0;
		double t;
		if (line[0] == '#') {
			if ((p = strstr(line, "# view ")) != NULL) { /* sweep recording: a new section */
				leading = 3;
				pending = 1;
				memset(&cur, 0, sizeof(cur));
				if ((p = strstr(line, "level=")) != NULL) cur.level = atoi(p + 6);
				if ((p = strstr(line, "bins=")) != NULL) cur.nb = atoi(p + 5);
				if ((p = strstr(line, "f0=")) != NULL) cur.f0 = strtod(p + 3, NULL);
				if ((p = strstr(line, "bw=")) != NULL) cur.bw = strtod(p + 3, NULL);
				if ((p = strstr(line, "ch=")) != NULL) cur.ch = atoi(p + 3);
			} else if ((p = strstr(line, "bin i =")) != NULL) { /* burst capture */
				leading = 2;
				pending = 1;
				memset(&cur, 0, sizeof(cur));
				cur.f0 = strtod(p + 7, NULL);
				cur.bw = 0.3125;
				if ((p = strstr(line, "channel ")) != NULL) cur.ch = atoi(p + 8);
			}
			if (bad_section(&cur)) {
				snprintf(error, error_size, "Unrecognised recording header");
				goto fail;
			}
			continue;
		}
		for (p = line; *p; p++)
			nb += *p == ',';
		nb += 1 - leading;
		t = strtod(line, &end);
		if (end == line || !isfinite(t) || nb < 1 || nb > MAXBINS || (cur.nb && nb != cur.nb))
			continue; /* torn or foreign row */
		if (r->nvalues + (size_t)nb > REVIEW_MAX_VALUES) {
			r->truncated = 1;
			break;
		}
		if (pending) {
			struct rec_section *grown;
			if (r->nsections == REVIEW_MAX_SECTIONS) {
				r->truncated = 1;
				break;
			}
			grown = realloc(r->sections, (r->nsections + 1) * sizeof(*grown));
			if (!grown) {
				snprintf(error, error_size, "Out of memory loading recording");
				goto fail;
			}
			r->sections = grown;
			cur.nb = nb;
			cur.first = r->rows;
			cur.rows = 0;
			r->sections[r->nsections++] = cur;
			pending = 0;
		}
		if (recording_grow(r, nb)) {
			snprintf(error, error_size, "Out of memory loading recording");
			goto fail;
		}
		if (!r->rows)
			last = t;
		r->ts[r->rows] = t < last ? last : t; /* a torn last line cannot run time backwards */
		last = r->ts[r->rows];
		r->start[r->rows] = (uint32_t)r->nvalues;
		r->section[r->rows] = (uint16_t)(r->nsections - 1);
		p = line;
		for (i = 0; i < leading && p; i++)
			if ((p = strchr(p, ',')) != NULL) p++;
		for (i = 0; i < nb; i++) {
			long v = p ? strtol(p, NULL, 10) : 0;
			r->values[r->nvalues++] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
			if (p && (p = strchr(p, ',')) != NULL) p++;
		}
		r->sections[r->nsections - 1].rows++;
		r->rows++;
	}
	fclose(f);
	f = NULL;
	if (r->rows == 0) {
		snprintf(error, error_size, "No readings in recording");
		goto fail;
	}
	for (k = 1; k < r->rows; k++)
		r->ts[k] -= r->ts[0];
	r->ts[0] = 0;
	r->duration = r->ts[r->rows - 1];
	for (k = 0; k < r->nsections; k++) { /* each section's background (min) and max hold */
		struct rec_section *s = &r->sections[k];
		size_t row;
		for (i = 0; i < s->nb; i++) {
			s->mn[i] = 1e9f;
			s->mx[i] = -1e9f;
		}
		for (row = s->first; row < s->first + s->rows; row++)
			for (i = 0; i < s->nb; i++) {
				float db = r->values[r->start[row] + i] / 10.0f;
				if (db < s->mn[i]) s->mn[i] = db;
				if (db > s->mx[i]) s->mx[i] = db;
			}
	}
	return 0;
fail:
	if (f)
		fclose(f);
	recording_free(r);
	return -1;
}

static size_t recording_row(const struct recording *r, double pos)
{
	size_t lo = 0, hi = r->rows;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (r->ts[mid] <= pos) lo = mid + 1;
		else hi = mid;
	}
	return lo ? lo - 1 : 0;
}

/* Put row `row` on screen: its section's view and traces, and a waterfall of
 * that row and the ones recorded before it, newest on top. */
static void recording_show(const struct recording *r, size_t row)
{
	const struct rec_section *s = &r->sections[r->section[row]];
	size_t y, rows = row - s->first + 1;
	int i;
	memset(&vw, 0, sizeof(vw));
	vw.level = s->level;
	vw.nb = s->nb;
	vw.f0 = s->f0;
	vw.bw = s->bw;
	vw.ch = s->ch;
	vw.dc_lo = 1;
	vw.dc_hi = 0;
	if (cursor >= vw.nb)
		cursor = vw.nb - 1;
	for (i = 0; i < vw.nb; i++) {
		sweep_db[i] = r->values[r->start[row] + i] / 10.0f;
		base_db[i] = avg_db[i] = s->mn[i];
		hold_db[i] = s->mx[i];
	}
	have_hold = have_base = have_avg = 1;
	if (rows > WHGT)
		rows = WHGT;
	for (y = 0; y < rows; y++) {
		const int16_t *v = r->values + r->start[row - y];
		for (i = 0; i < vw.nb; i++)
			wf[y][i] = wf_level(v[i] / 10.0f, i);
	}
	wf_head = 0;
	wf_rows = (int)rows;
}

static void review_draw(const struct recording *r, size_t row, double speed, int playing, const char *name)
{
	char s[256];
	double t = r->ts[row];
	render();
	fill(0, 0, SW, 18, C_BG);
	snprintf(s, sizeof(s), "REVIEW  %.36s   %.1f / %.1f s   x%g   %s%s", name, t, r->duration, speed,
	         playing ? "PLAY" : row + 1 >= r->rows ? "END" : "PAUSE", r->truncated ? "  CUT SHORT" : "");
	text(4, 2, C_TEXT, s);
	fill(0, WY0 + WHGT, SW, SH - WY0 - WHGT, C_BG);
	if (status_until > now_s())
		text(4, WY0 + WHGT, C_HOLD, status_msg);
	else
		text(4, WY0 + WHGT, C_DIM, "A play  L/R step  L1/R1 speed  Up/Dn range  Y enhance  START png  B back");
	fill(PX0, SH - 3, PW, 2, C_GRID);
	fill(PX0 + (r->duration > 0 ? (int)(t / r->duration * (PW - 3)) : 0), SH - 4, 3, 4, C_CURSOR);
}

static int review_play(const char *path, const char *shot, double start)
{
	struct recording r;
	struct view save_vw = vw;
	const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	char error[160];
	int save_cursor = cursor, save_mode = disp_mode, save_enhance = enhance, save_scaled = scaled;
	float save_low = db_low, save_enh_low = enh_low;
	double pos, last, speed = 1;
	size_t row, drawn = (size_t)-1;
	int playing = !shot, back = 0, result = 0, dirty = 1, status_was = 0;

	if (!shot)
		loading_screen(path);
	if (recording_load(path, &r, error, sizeof(error)) != 0) {
		fprintf(stderr, "recording: %s\n", error);
		status(error);
		return 1;
	}
	in_review = 1;
	disp_mode = 0;
	recording_show(&r, 0);
	autoscale(); /* levels from the first row; Up/Down shift them */
	pos = start < r.duration ? start : r.duration;
	row = recording_row(&r, pos);
	if (shot) {
		recording_show(&r, row);
		review_draw(&r, row, speed, playing, name);
		result = write_image(shot) != 0;
		goto done;
	}
	while (next_key() >= 0) /* presses made while it loaded */
		;
	last = now_s();
	while (!quit && !back) {
		double t = now_s();
		int code, status_now = status_until > t;
		if (playing) {
			pos += (t - last) * speed;
			if (pos >= r.duration) { pos = r.duration; playing = 0; dirty = 1; }
			row = recording_row(&r, pos);
		}
		last = t;
		if (row != drawn || status_now != status_was)
			dirty = 1;
		status_was = status_now;
		if (dirty) {
			recording_show(&r, row);
			if (audio_on) { /* the tone follows the recording at the cursor */
				float nn = (shown(sweep_db, cursor) - low()) / DB_SPAN;
				audio_norm = nn < 0 ? 0 : nn > 1 ? 1 : nn;
			}
			review_draw(&r, row, speed, playing, name);
			present();
			drawn = row;
			dirty = 0;
		}
		while (!back && !quit && (code = next_key()) >= 0) {
			dirty = 1;
			switch (code & ~KEY_SHIFTED) {
			case KEY_SPACE:
				if (!playing && row + 1 >= r.rows) { pos = 0; row = 0; }
				playing = !playing; break;
			case KEY_LEFT: playing = 0; if (row) row--; pos = r.ts[row]; break;
			case KEY_RIGHT: playing = 0; if (row + 1 < r.rows) row++; pos = r.ts[row]; break;
			case KEY_UP: if (enhance) enh_low += 3; else db_low += 3; break;
			case KEY_DOWN: if (enhance) enh_low -= 3; else db_low -= 3; break;
			case KEY_E: if (speed > 0.25) speed /= 2; break;
			case KEY_T: if (speed < 8) speed *= 2; break;
			case KEY_LEFTALT: enhance = !enhance; break;
			case KEY_ENTER: save_screenshot(); break;
			case KEY_LEFTCTRL: back = 1; break;
			case KEY_ESC: quit = 1; break;
			}
		}
		if (!dirty && !back && !quit)
			wait_input(playing ? 1.0 / 30 : status_now ? status_until - now_s() + 0.01 : 0.5);
	}
done:
	in_review = 0;
	vw = save_vw; /* back to the live view and its settings */
	cursor = save_cursor;
	disp_mode = save_mode;
	enhance = save_enhance;
	scaled = save_scaled;
	db_low = save_low;
	enh_low = save_enh_low;
	recording_free(&r);
	reset_traces();
	return result;
}

/* The recording browser: spectrum CSVs and packet captures from rec/ and
 * research/, newest first. */
struct rec_entry {
	char path[300];
	time_t mtime;
	int packet;
};

static int has_suffix(const char *s, const char *suffix)
{
	size_t l = strlen(s), n = strlen(suffix);
	return l > n && !strcasecmp(s + l - n, suffix);
}

static int newest_first(const void *a, const void *b)
{
	const struct rec_entry *x = a, *y = b;
	if (x->mtime != y->mtime)
		return x->mtime < y->mtime ? 1 : -1;
	return strcmp(y->path, x->path);
}

static size_t list_recordings(struct rec_entry **out)
{
	static const char *dirs[] = { "rec", "research" };
	struct rec_entry *list = NULL;
	size_t n = 0, cap = 0;
	unsigned d;
	for (d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
		DIR *dir = opendir(dirs[d]);
		struct dirent *e;
		if (dir == NULL)
			continue;
		while ((e = readdir(dir)) != NULL) {
			struct stat st;
			struct rec_entry *it;
			int packet = has_suffix(e->d_name, ".pcap");
			if (!packet && !has_suffix(e->d_name, ".csv"))
				continue;
			if (n == cap) {
				struct rec_entry *grown = realloc(list, (cap ? cap * 2 : 64) * sizeof(*list));
				if (grown == NULL)
					break;
				list = grown;
				cap = cap ? cap * 2 : 64;
			}
			it = &list[n++];
			snprintf(it->path, sizeof(it->path), "%s/%s", dirs[d], e->d_name);
			it->packet = packet;
			it->mtime = stat(it->path, &st) == 0 ? st.st_mtime : 0;
		}
		closedir(dir);
	}
	if (n)
		qsort(list, n, sizeof(*list), newest_first);
	*out = list;
	return n;
}

#define BROWSE_ROWS 22

static void review_session(void)
{
	struct rec_entry *list = NULL;
	size_t count, sel = 0, i;
	int dirty = 1, status_was = 0;
	if (in_fd < 0)
		return;
	count = list_recordings(&list);
	while (!quit) {
		int code, chosen = -1, cancel = 0, status_now = status_until > now_s();
		audio_quiet = 1;
		if (status_now != status_was)
			dirty = 1;
		status_was = status_now;
		if (dirty) {
			size_t first = sel / BROWSE_ROWS * BROWSE_ROWS;
			char label[96];
			fill(0, 0, SW, SH, C_BG);
			snprintf(label, sizeof(label), "REVIEW: recordings and captures (%zu)   MiYooSDR %s", count,
			         MIYOOSDR_VERSION);
			text(4, 6, C_TEXT, label);
			if (count == 0) {
				text(24, 48, C_DIM, "Save spectrum recordings or copy PCAPs to rec/ or research/.");
				text(24, 72, C_DIM, "The capture tool's stock-monitor.pcap appears here automatically.");
			}
			for (i = first; i < count && i < first + BROWSE_ROWS; i++) {
				int y = 30 + (int)(i - first) * 18;
				if (i == sel)
					text(8, y, C_LIVE, ">");
				snprintf(label, sizeof(label), "[%s] %.68s", list[i].packet ? "PCAP" : "CSV ", list[i].path);
				text(24, y, i == sel ? C_LIVE : C_DIM, label);
			}
			if (status_now)
				text(4, 438, C_HOLD, status_msg);
			text(4, SH - 20, C_DIM, "Up/Down choose  Left/Right page  A open  B back  MENU quit");
			present();
			dirty = 0;
		}
		while (chosen < 0 && !cancel && !quit && (code = next_key()) >= 0) {
			dirty = 1;
			switch (code & ~KEY_SHIFTED) {
			case KEY_UP: if (sel > 0) sel--; break;
			case KEY_DOWN: if (sel + 1 < count) sel++; break;
			case KEY_LEFT: sel = sel > BROWSE_ROWS ? sel - BROWSE_ROWS : 0; break;
			case KEY_RIGHT: if (count) sel = sel + BROWSE_ROWS < count ? sel + BROWSE_ROWS : count - 1; break;
			case KEY_SPACE: if (count) chosen = (int)sel; break;
			case KEY_LEFTCTRL: cancel = 1; break;
			case KEY_ESC: quit = 1; break;
			}
		}
		if (chosen >= 0) {
			if (list[chosen].packet)
				packet_review(list[chosen].path, NULL, 0);
			else
				review_play(list[chosen].path, NULL, 0);
			dirty = 1;
			continue;
		}
		if (cancel)
			break;
		if (!dirty && !quit)
			wait_input(status_now ? status_until - now_s() + 0.01 : 0.5);
	}
	audio_quiet = 0;
	free(list);
}

/* Playback and browsing need no radio: --replay FILE (.pcap or .csv) and --review.
 * B in a replay opened with --replay goes to the browser, as it does elsewhere. */
static int run_offline(const char *replay_path, const char *shot, double replay_at)
{
	int result;
	make_lut();
	view_full();
	if (replay_path) {
		result = has_suffix(replay_path, ".csv") ? review_play(replay_path, shot, replay_at)
		                                         : packet_review(replay_path, shot, replay_at);
		if (shot || quit || result)
			return result;
	}
	review_session();
	return 0;
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

static void usage(FILE *out)
{
	fprintf(out,
	        "usage: miyoosdr [options]   (none: live spectrum on the Miyoo's screen)\n"
	        "  --replay FILE         play a packet capture (.pcap) or spectrum recording (.csv)\n"
	        "  --replay-at SECONDS   start the replay that far in\n"
	        "  --review              browse recordings and captures\n"
	        "  --shot FILE           render one frame to FILE (.png or .ppm) and exit\n"
	        "  --shot-after N        live sweeps before --shot renders (default 12)\n"
	        "  --zoom MHZ LEVEL      start zoomed: level 1, 2, 3 = 20, 10, 5 MHz\n"
	        "  --style N  --pal N    spectrum style 0-2, palette 0-%d\n"
	        "  --occ                 start on the channel occupancy panel\n"
	        "  --dump N              N sweeps as text\n"
	        "  --burst MHZ SECONDS   single-channel power-vs-time recording\n"
	        "  --probe  --racap      PSD timing and raw-capture probes\n"
	        "  --fake                synthetic radio, for PC builds\n"
	        "  --settings            use settings.txt even when started with options\n"
	        "  --raw-tune            tune by RF register writes (this once hung the chip)\n"
	        "  --needs-radio         exit 0 if the other options use the radio, 1 if not\n"
	        "  --version  --help\n",
	        NPAL - 1);
}

/* option values: whole or decimal numbers inside [lo, hi] */
static int arg_int(const char *s, long lo, long hi, int *out)
{
	char *end;
	long v;
	errno = 0;
	v = strtol(s, &end, 10);
	if (end == s || *end || errno || v < lo || v > hi)
		return -1;
	*out = (int)v;
	return 0;
}

static int arg_num(const char *s, double lo, double hi, double *out)
{
	char *end;
	double v = strtod(s, &end);
	if (end == s || *end || !isfinite(v) || v < lo || v > hi)
		return -1;
	*out = v;
	return 0;
}

int main(int argc, char **argv)
{
	const char *shot = NULL, *replay_path = NULL, *bad = NULL;
	int review_only = 0, replay_at_set = 0, pm_only = 0, needs_radio = 0, version = 0, shot_failed = 0;
	double replay_at = 0;
	int dump = 0, probe = 0, racap = 0, burst_sec = 0, i, shot_after = 12, zlevel = 0, use_settings = argc == 1;
	double t0, last_rate_t, last_flush, last_diag = 0, zf = 0, burst_mhz = 0;
	long last_sweeps = 0;
	struct sigaction sa;

	for (i = 1; i < argc && !bad; i++) {
		const char *a = argv[i];
		int left = argc - 1 - i; /* values after this option */
		if (!strcmp(a, "--fake")) fake = 1;
		else if (!strcmp(a, "--review")) review_only = 1;
		else if (!strcmp(a, "--probe")) probe = 1;
		else if (!strcmp(a, "--raw-tune")) raw_tune = 1;
		else if (!strcmp(a, "--racap")) racap = 1;
		else if (!strcmp(a, "--occ")) disp_mode = 1;
		else if (!strcmp(a, "--settings")) use_settings = 1; /* tests: settings.txt with options too */
		else if (!strcmp(a, "--pm-active")) pm_only = 1;
		else if (!strcmp(a, "--needs-radio")) needs_radio = 1;
		else if (!strcmp(a, "--version")) version = 1;
		else if (!strcmp(a, "--help")) { usage(stdout); return 0; }
		else if (!strcmp(a, "--shot")) { if (left < 1 || !*argv[i + 1]) bad = a; else shot = argv[++i]; }
		else if (!strcmp(a, "--replay")) { if (left < 1 || !*argv[i + 1]) bad = a; else replay_path = argv[++i]; }
		else if (!strcmp(a, "--replay-at")) {
			if (left < 1 || arg_num(argv[++i], 0, 1e9, &replay_at)) {
				fprintf(stderr, "--replay-at requires nonnegative finite seconds\n");
				return 2;
			}
			replay_at_set = 1;
		} else if (!strcmp(a, "--dump")) { if (left < 1 || arg_int(argv[++i], 1, 100000000, &dump)) bad = a; }
		else if (!strcmp(a, "--shot-after")) { if (left < 1 || arg_int(argv[++i], 1, 1000000, &shot_after)) bad = a; }
		else if (!strcmp(a, "--style")) { if (left < 1 || arg_int(argv[++i], 0, 2, &spec_style)) bad = a; }
		else if (!strcmp(a, "--pal")) { if (left < 1 || arg_int(argv[++i], 0, NPAL - 1, &palette)) bad = a; }
		else if (!strcmp(a, "--zoom")) {
			if (left < 2 || arg_num(argv[i + 1], BAND_LO, BAND_HI, &zf) || arg_int(argv[i + 2], 1, 3, &zlevel))
				bad = a;
			i += 2;
		} else if (!strcmp(a, "--burst")) {
			if (left < 2 || arg_num(argv[i + 1], BAND_LO, BAND_HI, &burst_mhz) ||
			    arg_int(argv[i + 2], 1, 86400, &burst_sec))
				bad = a;
			i += 2;
		} else {
			fprintf(stderr, "unknown option: %s (try --help)\n", a);
			return 2;
		}
	}
	if (bad) {
		fprintf(stderr, "%s: missing or out-of-range value (try --help)\n", bad);
		return 2;
	}
	if (replay_at_set && !replay_path) {
		fprintf(stderr, "--replay-at requires --replay FILE\n");
		return 2;
	}
	if (review_only && shot && !replay_path) {
		fprintf(stderr, "--shot needs --replay FILE in offline review\n");
		return 2;
	}
	if (needs_radio) /* asked by launch.sh, which only takes Wi-Fi down for the radio */
		return replay_path || review_only || fake || version ? 1 : 0;
	if (version) {
		printf("MiYooSDR %s\n", MIYOOSDR_VERSION);
		return 0;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	fprintf(stderr, "MiYooSDR %s\n", MIYOOSDR_VERSION);
	if (replay_path || review_only) {
		int result;
		if (!shot && fb_open() != 0) { fprintf(stderr, "no framebuffer\n"); fb_close(); return 3; }
		if (!shot && in_fd < 0) {
			fprintf(stderr, "no input device for replay controls\n"); fb_close(); return 3;
		}
		result = run_offline(replay_path, shot, replay_at);
		fb_close();
		return result;
	}
	if (pm_only) { pm_active(); return 0; }

	if (!fake && radio_open() != 0)
		return 2;
	if (!fake)
		igi = (int)(orig_c50 & 0x7f);
	view_full();
	if (use_settings) /* a plain start (from Onion): the view you left last time */
		settings_load();
	make_lut();
	if (zlevel) {
		view_zoom(zf, zlevel);
		cursor = mhz_bin(zf);
	}

	if (burst_sec) { /* single-channel high-rate power-vs-time recorder */
		int ch = (int)lround((burst_mhz - 2407.0) / 5.0), k;
		double f0, t0b, tnow;
		long nrows = 0;
		char name[160];
		FILE *f;
		if (ch < 1) ch = 1;
		if (ch > 13) ch = 13;
		f0 = ch_mhz(ch) - 64 * 0.3125;
		timestamped_name(name, sizeof(name), "rec", "burst", "csv");
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
		int fresh = do_sweep(); /* 0: a button changed the view mid-sweep */
		if (review_request) { /* SELECT+START: browse and replay recordings */
			review_request = 0;
			review_session();
			last_rate_t = last_flush = last_diag = now_s();
			continue;
		}
		if (fresh) {
			if (audio_on) { /* pitch tracks the level under the cursor */
				float nn = (shown(sweep_db, cursor) - low()) / DB_SPAN;
				audio_norm = nn < 0 ? 0 : nn > 1 ? 1 : nn;
			}
			if (!scaled && sweeps >= 2)
				autoscale();
			push_waterfall();
			record_sweep(t0);
			burst_update(now_s());
		}
		if (now_s() - last_diag >= 2.0) {
			diag("ui");
			last_diag = now_s();
		}
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
				shot_failed = write_image(shot) != 0;
				break;
			}
			continue;
		}
		present();
		if (shot_req) {
			save_screenshot();
			shot_req = 0;
		}
	}
	if (use_settings)
		settings_save();

	if (rec)
		fclose(rec);
	if (audio_on)
		audio_stop();
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
	return shot_failed;
}
