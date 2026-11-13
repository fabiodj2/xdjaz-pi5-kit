// azshim.so: LD_PRELOAD shim for the stock XDJ-AZ player (EP147) on a Pi.
//
// Path redirection: the player talks to the AZ's own kernel through files
// that do not exist on a Pi (/proc/udev_usb1 and friends, written by the
// AZ's device-mount.sh). procfs takes no new files, so opens of those paths
// go to a writable directory instead, where our host side can write the
// same lines the AZ kernel would.
//
// On the deck each of those files is an event channel: the player polls it
// once a second and acts on what it reads ("mount /media/usb/sda1 exfat
// protect:0", "umount /media/usb/sda1"). Here the host appends one line per
// event (under flock), and a read-only open takes the first line off the file
// and hands it over through a pipe, so each event is read once and in order.
//
// EP147 imports open64, fopen and fopen64 (GLIBC_2.17). They are replaced
// here without dlsym: the shim is built on the Pi's glibc 2.41, where dlsym
// carries a version the guest's glibc 2.29 does not have, so the shim must
// only need symbols that exist at GLIBC_2.17 (syscall, fdopen, ...).
//
// SysV semaphores: the player's cipher module creates its lock with mode 000
// and relies on root bypassing the permission check (CAP_IPC_OWNER). bwrap's
// uid 0 has no capabilities, so the mode is widened to 0600.
//
// OneLibrary: the player opens PIONEER/rekordbox/exportLibrary.db (SQLCipher)
// with a passphrase it derives from the deck's provisioning files, which we
// do not have, so sqlite3_key arrives empty. For that database only, an empty
// key is replaced by the OneLibrary key read from KEYFILE (bound in by the
// launcher; the key is never in this source). EP147 imports sqlite3_key, not
// _v2, so ours calls libsqlcipher's sqlite3_key_v2 the way the real one does.
//
// SubCpu (UCOM): the deck's controls come from microcontrollers behind
// /dev/subucom_spi{1,2,3,5}.0 (see bridge/UCOM-PROTOCOL.md). Opening
// one connects a SOCK_SEQPACKET socket to our bridge daemon instead and sends
// the device name; each packet the bridge sends is one full RX frame, which
// the player reads as from the device. The device's 'p' ioctls are answered
// here, and the TX frame ioctl is passed on to the bridge as a packet.
//
// Beat grid ticks: the detailed waveform's tick strips are an image built one
// column per beat (gridMaker.makeGridImage, 0x1DFFE10) and drawn scaled with
// the whole UI; at our JUCE scale of 0.75 a tick is 0.75 px and shimmers as it
// scrolls. At start the shim patches that loop to write two columns per beat.
//
// Waveform frame rate: the repaint timer (MainDisplayUpdateTimer's callback,
// 0x212FB40) only repaints once two vsync periods have passed since the last
// repaint, i.e. 30 fps; patched to one period, with the paint timer armed for
// right after the update (see fps_patches). Off unless AZ_60FPS=1 while on
// test.
//
// EP147 1.30 only: every site of a patch must hold its original instruction,
// or that patch is not applied. AZ_GRID_WIDE=0 leaves the ticks alone.
//
// Check with:  objdump -T azshim.so | grep GLIBC_2.[2-9]   (must be empty)

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
#include <link.h>
#include <elf.h>
#include <stdint.h>
#include <time.h>

struct redirect {
	const char *from;	// prefix in the player's view
	const char *to;		// replacement prefix
};

static const struct redirect redirects[] = {
	{ "/proc/udev_", "/tmp/udev/udev_" },
	// the serial number on the player's information screen is the "Serial" line of cpuinfo;
	// run-probe.sh writes a copy with the kit's own (AZ_SERIAL)
	{ "/proc/cpuinfo", "/tmp/cpuinfo" },
};

// Returns path, or buf filled with the redirected path.
static const char *redirect(const char *path, char *buf, size_t len)
{
	if (!path)
		return path;
	for (size_t i = 0; i < sizeof(redirects) / sizeof(redirects[0]); i++) {
		size_t n = strlen(redirects[i].from);
		if (strncmp(path, redirects[i].from, n) == 0 &&
		    (size_t)snprintf(buf, len, "%s%s", redirects[i].to, path + n) < len)
			return buf;
	}
	return path;
}

#define EVENT_PREFIX	"/proc/udev_"

// A read of an event file: its first line, through a pipe (empty: no event).
static int event_open(const char *path, int flags)
{
	char buf[4096];
	int p[2];
	int fd = (int)syscall(SYS_openat, AT_FDCWD, path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (pipe2(p, flags & O_CLOEXEC) < 0) {
		close(fd);
		return -1;
	}
	flock(fd, LOCK_EX);
	ssize_t n = pread(fd, buf, sizeof(buf), 0);
	if (n > 0) {
		char *nl = memchr(buf, '\n', n);
		ssize_t first = nl ? nl - buf : n, rest = nl ? n - first - 1 : 0;
		if (write(p[1], buf, first) < 0)
			first = 0;
		if (pwrite(fd, buf + n - rest, rest, 0) < 0 || ftruncate(fd, rest) < 0)
			rest = 0;
	}
	flock(fd, LOCK_UN);
	close(fd);
	close(p[1]);
	return p[0];
}

static int sys_open(const char *path, int flags, mode_t mode)
{
	char buf[512];
	const char *to = redirect(path, buf, sizeof(buf));
	if (to != path && (flags & O_ACCMODE) == O_RDONLY &&
	    strncmp(path, EVENT_PREFIX, strlen(EVENT_PREFIX)) == 0)
		return event_open(to, flags);
	return (int)syscall(SYS_openat, AT_FDCWD, to, flags, mode);
}

#define UCOM_PREFIX	"/dev/subucom_"
#define BRIDGE_SOCK	"/tmp/azbridge.sock"
#define MAXFD	4096

static unsigned char ucom_fds[MAXFD / 8];

static int is_ucom(int fd)
{
	return fd >= 0 && fd < MAXFD && (ucom_fds[fd / 8] & (1 << (fd % 8)));
}

static int ucom_open(const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX, .sun_path = BRIDGE_SOCK };
	const char *dev = path + strlen("/dev/");
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (fd >= MAXFD || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
	    send(fd, dev, strlen(dev), 0) < 0) {
		close(fd);
		errno = ENOENT;		// as without the device
		return -1;
	}
	ucom_fds[fd / 8] |= 1 << (fd % 8);
	return fd;
}

int open64(const char *path, int flags, ...)
{
	if (strncmp(path, UCOM_PREFIX, strlen(UCOM_PREFIX)) == 0)
		return ucom_open(path);

	mode_t mode = 0;
	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	return sys_open(path, flags, mode);
}

int open(const char *path, int flags, ...)
	__attribute__((alias("open64")));

// fopen = open + fdopen, with glibc's mode letters: r w a, then + b e x.
FILE *fopen64(const char *path, const char *mode)
{
	int flags, fd;
	FILE *f;

	switch (mode[0]) {
	case 'r': flags = 0; break;
	case 'w': flags = O_CREAT | O_TRUNC; break;
	case 'a': flags = O_CREAT | O_APPEND; break;
	default: errno = EINVAL; return NULL;
	}
	int rw = 0;
	for (const char *m = mode + 1; *m && *m != ','; m++) {
		if (*m == '+') rw = 1;
		else if (*m == 'e') flags |= O_CLOEXEC;
		else if (*m == 'x') flags |= O_EXCL;
	}
	flags |= rw ? O_RDWR : (mode[0] == 'r' ? O_RDONLY : O_WRONLY);

	fd = sys_open(path, flags, 0666);
	if (fd < 0)
		return NULL;
	f = fdopen(fd, mode);
	if (!f) {
		int e = errno;
		close(fd);
		errno = e;
	}
	return f;
}

FILE *fopen(const char *path, const char *mode)
	__attribute__((alias("fopen64")));

int close(int fd)
{
	if (is_ucom(fd))
		ucom_fds[fd / 8] &= ~(1 << (fd % 8));
	return (int)syscall(SYS_close, fd);
}

#define UCOM_TX		0x40107000	// _IOW('p', 0, 16): one TX frame

struct ucom_tx {
	unsigned int flags;	// 0x01000000, meaning unknown
	unsigned int len;
	unsigned long long buf;
};

int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);

	if (!is_ucom(fd))
		return (int)syscall(SYS_ioctl, fd, req, arg);
	if (req == UCOM_TX) {
		const struct ucom_tx *tx = arg;
		// a full socket (bridge busy) only loses LED updates
		send(fd, (const void *)(unsigned long)tx->buf, tx->len, MSG_DONTWAIT);
	} else if (_IOC_DIR(req) == _IOC_READ && arg) {
		// settings read back as 0, so the player sets each one (and we agree)
		memset(arg, 0, _IOC_SIZE(req));
	}
	return 0;
}

int semget(key_t key, int nsems, int semflg)
{
	if (semflg & IPC_CREAT)
		semflg |= 0600;
	return (int)syscall(SYS_semget, key, nsems, semflg);
}

// Wi-Fi wait: before setting up its Wi-Fi the player waits for
// wpa_supplicant by running "wpa_cli -i mlan0 status" (0x216BC38, 0x216BCA0)
// in a loop with no pause, through its exec_system (vfork + execl of
// /bin/sh). There is no mlan0 here, so it ran about 300 times a second and
// cost about one core. That one command now fails in the vfork child without
// an exec, after AZ_WPA_WAIT_MS (default 1000; the parent thread waits on
// the child). With the Wi-Fi broker below running, it answers this one too,
// with 0, and the player's Wi-Fi starts. Every other execl goes through
// unchanged.
#define WPA_STATUS "wpa_cli -i mlan0 status > /dev/null 2>&1"

// Wi-Fi broker (AZ_WIFI_SOCK, a unix socket path inside the sandbox): the
// player's Wi-Fi commands (wpa_supplicant/wpa_cli/iw/ifconfig on mlan0) go to
// azwifi.py on the host, which runs them on the host's wlan0 and writes any
// output the player reads (/tmp/shelloutput/..., /tmp/scan.log) itself. We
// send the whole command line, close our side, and get back the exit code as
// decimal text. Raw syscalls: this runs in exec_system's vfork child. Returns
// -1 if there is no broker (the command then runs as before).
static const char *const wifi_prefix[] = {
	"wpa_cli -i mlan0", "wpa_supplicant -i mlan0", "killall wpa_supplicant",
	"iw dev mlan0", "ifconfig mlan0",
};

static int wifi_broker(const char *cmd)
{
	size_t i, len;
	int hit = 0;
	for (i = 0; i < sizeof(wifi_prefix) / sizeof(*wifi_prefix); i++)
		if (!strncmp(cmd, wifi_prefix[i], strlen(wifi_prefix[i])))
			hit = 1;
	const char *sp = getenv("AZ_WIFI_SOCK");
	if (!hit || !sp || !*sp)
		return -1;
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	for (i = 0; sp[i] && i < sizeof(sa.sun_path) - 1; i++)
		sa.sun_path[i] = sp[i];
	int fd = (int)syscall(SYS_socket, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (syscall(SYS_connect, fd, &sa, sizeof(sa)) < 0) {
		syscall(SYS_close, fd);
		return -1;
	}
	len = strlen(cmd);
	for (i = 0; i < len;) {
		long w = syscall(SYS_write, fd, cmd + i, len - i);
		if (w <= 0)
			break;
		i += (size_t)w;
	}
	syscall(SYS_shutdown, fd, SHUT_WR);
	char buf[16];
	long n = 0, r;
	while (n < (long)sizeof(buf) &&
	    (r = syscall(SYS_read, fd, buf + n, sizeof(buf) - n)) > 0)
		n += r;
	syscall(SYS_close, fd);
	int rc = 0, got = 0;
	for (long k = 0; k < n && buf[k] >= '0' && buf[k] <= '9'; k++, got = 1)
		rc = rc * 10 + buf[k] - '0';
	return got ? rc & 0xff : -1;
}

int execl(const char *path, const char *arg, ...)
{
	const char *argv[16];
	int n = 0;
	va_list ap;

	argv[n++] = arg;
	va_start(ap, arg);
	while (argv[n - 1] && n < 16)
		argv[n++] = va_arg(ap, const char *);
	va_end(ap);
	if (argv[n - 1]) {
		errno = E2BIG;
		return -1;
	}
	if (n == 4 && !strcmp(path, "/bin/sh") && !strcmp(argv[1], "-c")) {
		int rc = wifi_broker(argv[2]);
		if (rc >= 0)
			_exit(rc);
	}
	if (n == 4 && !strcmp(path, "/bin/sh") && !strcmp(argv[1], "-c") &&
	    !strcmp(argv[2], WPA_STATUS)) {
		const char *v = getenv("AZ_WPA_WAIT_MS");
		long ms = 0;
		// no atol: glibc 2.41 headers turn it into __isoc23_strtol
		while (v && *v >= '0' && *v <= '9')
			ms = ms * 10 + *v++ - '0';
		if (!v)
			ms = 1000;
		struct timespec ts = { ms / 1000, ms % 1000 * 1000000 };
		nanosleep(&ts, NULL);
		_exit(1);
	}
	return execv(path, (char *const *)argv);
}

#define KEYFILE "/etc/azshim/onelibrary.key"
#define LIBDB "/exportLibrary.db"
#define SQLITE_ERROR 1

typedef struct sqlite3 sqlite3;
// weak: other programs started under the preload have no libsqlcipher
extern int sqlite3_key_v2(sqlite3 *, const char *, const void *, int)
	__attribute__((weak));
extern const char *sqlite3_db_filename(sqlite3 *, const char *)
	__attribute__((weak));

// Returns the key length (0 if KEYFILE is missing); trailing whitespace dropped.
static int onelibrary_key(char *buf, int len)
{
	int fd = (int)syscall(SYS_openat, AT_FDCWD, KEYFILE, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	int n = (int)read(fd, buf, len);
	close(fd);
	while (n > 0 && (unsigned char)buf[n - 1] <= ' ')
		n--;
	return n > 0 ? n : 0;
}

int sqlite3_key(sqlite3 *db, const void *pkey, int nkey)
{
	if (!sqlite3_key_v2)
		return SQLITE_ERROR;
	if (!pkey || nkey <= 0) {
		const char *f = sqlite3_db_filename ? sqlite3_db_filename(db, "main") : NULL;
		size_t fl = f ? strlen(f) : 0, ll = strlen(LIBDB);
		char key[128];
		int n;
		if (fl >= ll && strcmp(f + fl - ll, LIBDB) == 0 &&
		    (n = onelibrary_key(key, sizeof(key))) > 0) {
			fprintf(stderr, "azshim: %s: empty key -> OneLibrary key\n", f);
			return sqlite3_key_v2(db, "main", key, n);
		}
	}
	return sqlite3_key_v2(db, "main", pkey, nkey);
}

// ---- Blit guard ------------------------------------------------------------
//
// Without a compositor the player's frames go straight to the scanned-out
// buffer, so an image put while the scanout runs through its rows shows half
// old, half new: a tear. The stock player times its repaints from the vsync
// file (azvsync), but AZ-MODS' 60 FPS repaints on JUCE's own 16 ms timer,
// which drifts through the 16.88 ms frame, in two puts 2-3 ms apart.
//
// AZ_BLITGUARD=1 holds each XShmPutImage / XPutImage of the player whose rows
// the scanout is about to cross (or is crossing) until it has passed them,
// then sends it at once. The scanout row is the time since the last vblank
// over the line time; the vblank comes from the vsync file, which azvsync
// writes as vblank - period + offset (AZ_VSYNC_OFFSET_US, as given to it).
// Panel timings: AZ_FRAME_US (one frame), 625 lines of which 600 shown.
// AZ_BLIT_US is how long a put takes to land, so it must start that much
// ahead of the scanout; AZ_SCAN_SHIFT_US moves the model if the vblank
// timestamp is not the start of the scanout. The panel is mounted upside down
// and scans our picture from the bottom row up: AZ_SCAN_UP=1 (the default
// run-probe passes; 0 for a panel the right way up).
//
// AZ_BLITGUARD=2 locks whole frames instead: the player puts a frame as
// several images over ~3 ms (one per waveform lane), and a frame whose puts
// straddle the scanout's wrap shows its lanes in two different refreshes, so
// a lane jumps. The first put of a frame (one that comes more than 6 ms after
// the last) is held until AZ_FRAME_PHASE_US after the vblank, when the
// scanout has just passed the lanes, and the rest follow at once; a frame
// that comes late is not held (that would cost a whole refresh). The player's
// repaint period (AZ-MODS' azmod-ui60.ms) must be a little shorter than the
// refresh, so each frame arrives a little early and the hold takes up the
// difference.
//
// The real functions are found by name in the loaded libXext/libX11
// (dl_iterate_phdr and their GNU hash tables), as dlsym is out of reach (see
// the top of this file).
#define VSYNC_FILE "/sys/module/rockchipdrm/parameters/vsync_time"
#define LINES_TOTAL 625

struct sym_query { const char *lib, *name; void *addr; };

static unsigned long gnu_hash(const char *s)
{
	unsigned long h = 5381;
	for (; *s; s++)
		h = h * 33 + (unsigned char)*s;
	return h & 0xffffffff;
}

static int find_sym(struct dl_phdr_info *info, size_t size, void *data)
{
	struct sym_query *q = data;
	(void)size;
	if (!info->dlpi_name || !strstr(info->dlpi_name, q->lib))
		return 0;
	const ElfW(Sym) *symtab = NULL;
	const char *strtab = NULL;
	const uint32_t *gh = NULL, *sh = NULL;
	for (int i = 0; i < info->dlpi_phnum; i++) {
		if (info->dlpi_phdr[i].p_type != PT_DYNAMIC)
			continue;
		const ElfW(Dyn) *d = (const void *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
		for (; d->d_tag != DT_NULL; d++) {
			// in a loaded object these are already relocated addresses
			if (d->d_tag == DT_SYMTAB) symtab = (const void *)d->d_un.d_ptr;
			else if (d->d_tag == DT_STRTAB) strtab = (const void *)d->d_un.d_ptr;
			else if (d->d_tag == DT_GNU_HASH) gh = (const void *)d->d_un.d_ptr;
			else if (d->d_tag == DT_HASH) sh = (const void *)d->d_un.d_ptr;
		}
	}
	if (!symtab || !strtab || (!gh && !sh))
		return 0;
	if (!gh) {		// SysV hash only (the guest's Buildroot libraries): scan
		for (uint32_t i = 1; i < sh[1]; i++)
			if (symtab[i].st_shndx != SHN_UNDEF &&
			    strcmp(strtab + symtab[i].st_name, q->name) == 0) {
				q->addr = (void *)(info->dlpi_addr + symtab[i].st_value);
				return 1;
			}
		return 0;
	}
	uint32_t nbuckets = gh[0], symoffset = gh[1], bloom_size = gh[2];
	const uint32_t *buckets = gh + 4 + bloom_size * (sizeof(ElfW(Addr)) / 4);
	const uint32_t *chain = buckets + nbuckets;
	uint32_t h = gnu_hash(q->name);
	for (uint32_t i = buckets[h % nbuckets]; i >= symoffset && i; i++) {
		uint32_t ch = chain[i - symoffset];
		if ((ch | 1) == (h | 1) && strcmp(strtab + symtab[i].st_name, q->name) == 0 &&
		    symtab[i].st_shndx != SHN_UNDEF) {
			q->addr = (void *)(info->dlpi_addr + symtab[i].st_value);
			return 1;
		}
		if (ch & 1)
			break;
	}
	return 0;
}

static void *lib_sym(const char *lib, const char *name)
{
	struct sym_query q = { lib, name, NULL };
	dl_iterate_phdr(find_sym, &q);
	return q.addr;
}

static int guard_on = -1, vsync_fd = -1;
static long long frame_ns, offset_ns, blit_ns, shift_ns;
static int scan_up;
static long long phase_ns, last_put_ns;
#define LINES_SHOWN 600

// atoll would bind to glibc 2.38's __isoc23_strtoll (see the top of this file)
static long long dec(const char *s)
{
	long long v = 0;
	int neg = *s == '-';
	for (s += neg; *s >= '0' && *s <= '9'; s++)
		v = v * 10 + (*s - '0');
	return neg ? -v : v;
}

static long long env_us(const char *name, long long def)
{
	const char *v = getenv(name);
	return (v && *v ? dec(v) : def) * 1000;
}

static long long mono_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000000LL + t.tv_nsec;
}

// Once a second: /tmp/azshim-guard ("MODE [PHASE_US]", if present) switches
// the guard and its lock point at run time, and /tmp/azshim-guard.stat gets
// puts, waits and wait times.
#define GUARD_CTL "/tmp/azshim-guard"
static long long stat_t, stat_wait_ns, stat_max_ns;
static int stat_puts, stat_waits, stat_frames;
static long long stat_phase_ns, stat_phmin_ns, stat_phmax_ns;	// first puts' phase

static void guard_tick(void)
{
	long long now = mono_ns();
	if (now - stat_t < 1000000000LL)
		return;
	char b[160];
	int fd = (int)syscall(SYS_openat, AT_FDCWD, GUARD_CTL, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		ssize_t n = read(fd, b, sizeof(b) - 1);
		if (n > 0) {
			b[n] = 0;
			guard_on = b[0] - '0';
			char *sp = strchr(b, ' ');
			if (sp && dec(sp + 1) > 0)
				phase_ns = dec(sp + 1) * 1000;
		}
		close(fd);
	}
	if (stat_t) {
		int n = snprintf(b, sizeof(b), "on=%d phase_us=%lld puts=%d waits=%d wait_ms=%.1f max_ms=%.1f"
				 " first_put_phase_ms avg=%.1f min=%.1f max=%.1f\n",
				 guard_on, phase_ns / 1000, stat_puts, stat_waits, stat_wait_ns / 1e6, stat_max_ns / 1e6,
				 stat_frames ? stat_phase_ns / 1e6 / stat_frames : 0, stat_phmin_ns / 1e6, stat_phmax_ns / 1e6);
		fd = (int)syscall(SYS_openat, AT_FDCWD, GUARD_CTL ".stat",
				  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd >= 0) {
			if (write(fd, b, n) < 0) {}
			close(fd);
		}
	}
	stat_t = now;
	stat_puts = stat_waits = stat_frames = 0;
	stat_phase_ns = stat_phmax_ns = 0;
	stat_phmin_ns = 1LL << 40;
	stat_wait_ns = stat_max_ns = 0;
}

// Waits until rows y0..y1 can take a put without the scanout crossing them.
static void blit_wait(int y0, int y1)
{
	if (guard_on < 0) {
		const char *v = getenv("AZ_BLITGUARD");
		guard_on = v ? (int)dec(v) : 0;
		frame_ns = env_us("AZ_FRAME_US", 16878);
		offset_ns = env_us("AZ_VSYNC_OFFSET_US", 11000) % frame_ns;
		blit_ns = env_us("AZ_BLIT_US", 1500);
		shift_ns = env_us("AZ_SCAN_SHIFT_US", 0);
		phase_ns = env_us("AZ_FRAME_PHASE_US", 14600);
		const char *u = getenv("AZ_SCAN_UP");
		scan_up = u && strcmp(u, "1") == 0;
	}
	guard_tick();
	stat_puts++;
	if (!guard_on || y1 <= y0)
		return;
	if (scan_up) {		// in scan order
		int t = y0;
		y0 = LINES_SHOWN - y1;
		y1 = LINES_SHOWN - t;
	}
	if (vsync_fd < 0 &&
	    (vsync_fd = (int)syscall(SYS_openat, AT_FDCWD, VSYNC_FILE, O_RDONLY | O_CLOEXEC)) < 0)
		return;
	char buf[32];
	ssize_t n = pread(vsync_fd, buf, sizeof(buf) - 1, 0);
	if (n <= 0)
		return;
	buf[n] = 0;
	long long vb = dec(buf) - offset_ns + frame_ns + shift_ns;
	if (vb <= 0)
		return;
	double line_ns = (double)frame_ns / LINES_TOTAL;
	long long now = mono_ns();
	long long ph = ((now - vb) % frame_ns + frame_ns) % frame_ns;
	if (now - last_put_ns > 6000000) {	// a frame's first put
		stat_frames++;
		stat_phase_ns += ph;
		if (ph < stat_phmin_ns) stat_phmin_ns = ph;
		if (ph > stat_phmax_ns) stat_phmax_ns = ph;
	}
	if (guard_on == 2) {
		int first = now - last_put_ns > 6000000;
		last_put_ns = now;
		if (!first)
			return;
		long long early = phase_ns - ph;	// to the lock point, -T/2..T/2
		if (early > frame_ns / 2)
			early -= frame_ns;
		else if (early < -frame_ns / 2)
			early += frame_ns;
		if (early <= 0)
			return;			// late (or on time): send now
		struct timespec d = { early / 1000000000, early % 1000000000 };
		nanosleep(&d, NULL);
		last_put_ns = mono_ns();
		stat_waits++;
		stat_wait_ns += early;
		if (early > stat_max_ns)
			stat_max_ns = early;
		return;
	}
	double row = ph / line_ns;				// scanout now
	double ahead = blit_ns / line_ns;			// rows it covers while the put lands
	// the scanout must be past y1, or not reach y0 before the put has landed
	if (row >= y1 && row + ahead < LINES_TOTAL + y0)
		return;
	if (row < y0 && row + ahead < y0)
		return;
	double wait_rows = (row < y1 ? y1 - row : LINES_TOTAL - row + y1) + 2;
	long long wait = (long long)(wait_rows * line_ns);
	if (wait > frame_ns)
		return;
	struct timespec d = { wait / 1000000000, wait % 1000000000 };
	nanosleep(&d, NULL);
	stat_waits++;
	stat_wait_ns += wait;
	if (wait > stat_max_ns)
		stat_max_ns = wait;
}

typedef struct _XDisplay Display;
typedef unsigned long XID;
typedef int (*xshmput_fn)(Display *, XID, void *, void *, int, int, int, int,
			  unsigned int, unsigned int, int);
typedef int (*xput_fn)(Display *, XID, void *, void *, int, int, int, int,
		       unsigned int, unsigned int);
typedef int (*xflush_fn)(Display *);

int XShmPutImage(Display *dpy, XID d, void *gc, void *img, int sx, int sy, int dx, int dy,
		 unsigned int w, unsigned int h, int send_event)
{
	static xshmput_fn real;
	static xflush_fn flush;
	if (!real) {
		real = (xshmput_fn)lib_sym("libXext.so", "XShmPutImage");
		flush = (xflush_fn)lib_sym("libX11.so", "XFlush");
		if (!real)
			return 0;
	}
	blit_wait(dy, dy + (int)h);
	int r = real(dpy, d, gc, img, sx, sy, dx, dy, w, h, send_event);
	if (guard_on > 0 && flush)
		flush(dpy);		// out now, not at the next flush
	return r;
}

int XPutImage(Display *dpy, XID d, void *gc, void *img, int sx, int sy, int dx, int dy,
	      unsigned int w, unsigned int h)
{
	static xput_fn real;
	static xflush_fn flush;
	if (!real) {
		real = (xput_fn)lib_sym("libX11.so", "XPutImage");
		flush = (xflush_fn)lib_sym("libX11.so", "XFlush");
		if (!real) {
			fprintf(stderr, "azshim: XPutImage not found\n");
			return 0;
		}
	}
	blit_wait(dy, dy + (int)h);
	int r = real(dpy, d, gc, img, sx, sy, dx, dy, w, h);
	if (guard_on > 0 && flush)
		flush(dpy);
	return r;
}

struct code_patch {
	unsigned long addr;
	unsigned int from, to;	// AArch64 instructions
};

static const struct code_patch fps_patches[] = {
	{ 0x212FB94, 0x7100041f, 0x7100001f },	// cmp w0,#1 -> #0: repaint after 1 period
	{ 0x212FC44, 0x52800022, 0x52800002 },	// mov w2,#1 -> #0: wait to the next vsync
	// reserveToRepaint arms the paint timer to the next period boundary, so
	// at one update per period the update timer re-armed it before it fired
	// (20 fps); paint 1 ms after the update instead
	{ 0x212F980, 0x2a1303e1, 0x52800021 },	// mov w1,w19 -> mov w1,#1
};

static const struct code_patch grid_patches[] = {
	{ 0x1E00008, 0xb8004401, 0x28810401 },	// str w1,[x0],#4 -> stp w1,w1,[x0],#8
	{ 0x1E00010, 0x11000441, 0x11000841 },	// add w1,w2,#1 -> #2 (next free column)
	{ 0x1E001A4, 0xb9000001, 0x29000401 },	// mirror row: str w1,[x0] -> stp
	{ 0x1E003BC, 0xb9000016, 0x29005816 },	// mirror row: str w22,[x0] -> stp
	{ 0x1E001AC, 0x91001000, 0x91002000 },	// add x0,x0,#4 -> #8
	{ 0x1E001B4, 0x11000441, 0x11000841 },	// add w1,w2,#1 -> #2
};

static int is_ep147(void)
{
	char exe[256];
	long n = syscall(SYS_readlinkat, AT_FDCWD, "/proc/self/exe", exe, sizeof(exe) - 1);
	if (n < 6)
		return 0;
	exe[n] = 0;
	return strcmp(exe + n - 6, "/EP147") == 0;
}

// Applies a patch whose sites all lie in one page, if each holds its original.
// The env variable turns it off with "0", or on with "1" when def is 0.
static void apply_patch(const struct code_patch *p, size_t n, const char *env, int def)
{
	const char *opt = getenv(env);
	if (opt ? strcmp(opt, "0") == 0 : !def)
		return;
	for (size_t i = 0; i < n; i++)
		if (*(volatile unsigned int *)p[i].addr != p[i].from)
			return;		// another build: leave it alone
	unsigned long page = p[0].addr & ~0xFFFUL;
	if (mprotect((void *)page, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) < 0)
		return;
	for (size_t i = 0; i < n; i++)
		*(volatile unsigned int *)p[i].addr = p[i].to;
	__builtin___clear_cache((char *)page, (char *)page + 0x1000);
	mprotect((void *)page, 0x1000, PROT_READ | PROT_EXEC);
}

__attribute__((constructor)) static void patch_player(void)
{
	if (!is_ep147())
		return;
	apply_patch(grid_patches, sizeof(grid_patches) / sizeof(grid_patches[0]), "AZ_GRID_WIDE", 1);
	apply_patch(fps_patches, sizeof(fps_patches) / sizeof(fps_patches[0]), "AZ_60FPS", 0);
}
