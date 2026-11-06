// aztouch: drops the panel touchscreen's one-report position glitches.
//
// The panel's touch controller (wch.cn USB2IIC_CTP_CONTROL) now and then
// reports one wrong position while a finger rests still, and the next report,
// ~10 ms later, returns to where the finger is (bits dropped from a coordinate:
// 1632 -> 1120 -> 1632). The AZ treats that as a drag on the waveform and
// seeks twice. aztouch grabs the device, holds every report that moves the
// contact until the next one arrives, and drops the pair when the next report
// moves straight back. Real motion is delayed by one report.
//
// The controller also drops off USB now and then and comes back as a new
// event node. aztouch then lifts any contact on its own device, waits for
// the touchscreen to return and carries on, so X keeps one device throughout.
//
//   aztouch [/dev/input/eventN]   (default: the first direct-touch device;
//                                  a fixed path is not found again after a drop)
//
// Build: gcc -O2 -Wall -o aztouch aztouch.c

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HOLD_MS	30	// a held report is let through if nothing follows
#define BACK	4	// "moved back": within this many units on each axis
#define MAXEV	64	// events per report

#define BIT(arr, n)	((arr)[(n) / 8] & (1 << ((n) % 8)))

struct frame {
	struct input_event ev[MAXEV];
	int n;
	int x, y;		// contact position after this report
	int moved;		// the report changes the position
	int edge;		// touch down/up or contact change: never held
};

static volatile sig_atomic_t stop;

static void on_signal(int sig) { (void)sig; stop = 1; }

static int is_touchscreen(int fd)
{
	unsigned char props[INPUT_PROP_MAX / 8 + 1] = { 0 };
	unsigned char abs[ABS_MAX / 8 + 1] = { 0 };
	return ioctl(fd, EVIOCGPROP(sizeof(props)), props) >= 0 &&
	       ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) >= 0 &&
	       BIT(props, INPUT_PROP_DIRECT) && BIT(abs, ABS_X);
}

static int open_touchscreen(const char *path, char *name, size_t len)
{
	if (path) {
		int fd = open(path, O_RDONLY);
		if (fd >= 0)
			ioctl(fd, EVIOCGNAME(len), name);
		return fd;
	}
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	int fd = -1;
	while (d && (e = readdir(d))) {
		char p[300];
		if (strncmp(e->d_name, "event", 5) != 0)
			continue;
		snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
		if ((fd = open(p, O_RDONLY)) < 0)
			continue;
		ioctl(fd, EVIOCGNAME(len), name);
		// skip our own output device when restarted
		if (!strstr(name, "aztouch") && is_touchscreen(fd))
			break;
		close(fd);
		fd = -1;
	}
	if (d)
		closedir(d);
	return fd;
}

// A uinput device with the same events, axes and properties as src.
static int clone_device(int src, const char *name)
{
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0)
		return -1;
	unsigned char types[EV_MAX / 8 + 1] = { 0 }, props[INPUT_PROP_MAX / 8 + 1] = { 0 };
	ioctl(src, EVIOCGBIT(0, sizeof(types)), types);
	ioctl(src, EVIOCGPROP(sizeof(props)), props);
	for (int p = 0; p <= INPUT_PROP_MAX; p++)
		if (BIT(props, p))
			ioctl(fd, UI_SET_PROPBIT, p);

	static const struct { int type, max; unsigned long set; } kinds[] = {
		{ EV_KEY, KEY_MAX, UI_SET_KEYBIT },
		{ EV_ABS, ABS_MAX, UI_SET_ABSBIT },
		{ EV_MSC, MSC_MAX, UI_SET_MSCBIT },
	};
	for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
		unsigned char bits[KEY_MAX / 8 + 1] = { 0 };
		if (!BIT(types, kinds[k].type))
			continue;
		ioctl(fd, UI_SET_EVBIT, kinds[k].type);
		ioctl(src, EVIOCGBIT(kinds[k].type, sizeof(bits)), bits);
		for (int c = 0; c <= kinds[k].max; c++) {
			if (!BIT(bits, c))
				continue;
			ioctl(fd, kinds[k].set, c);
			if (kinds[k].type == EV_ABS) {
				struct uinput_abs_setup a = { .code = c };
				ioctl(src, EVIOCGABS(c), &a.absinfo);
				ioctl(fd, UI_ABS_SETUP, &a);
			}
		}
	}
	struct uinput_setup us = { .id = { .bustype = BUS_VIRTUAL, .vendor = 0x1d6b, .product = 0xa2 } };
	snprintf(us.name, sizeof(us.name), "aztouch (%s)", name);
	if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void emit(int out, const struct frame *f)
{
	if (f->n && write(out, f->ev, sizeof(f->ev[0]) * f->n) < 0)
		perror("aztouch: write");
}

// Reads one report (up to SYN_REPORT) into f, starting from position x, y.
// Returns 1 for a report, 0 on timeout, -1 on error.
static int read_frame(int fd, struct frame *f, int x, int y, int timeout_ms)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	f->n = 0;
	f->x = x;
	f->y = y;
	f->moved = f->edge = 0;
	for (;;) {
		int r = poll(&p, 1, f->n ? -1 : timeout_ms);
		if (r <= 0)
			return r < 0 && errno != EINTR ? -1 : 0;
		struct input_event e;
		if (read(fd, &e, sizeof(e)) != sizeof(e))
			return -1;
		if (f->n < MAXEV)
			f->ev[f->n++] = e;
		if (e.type == EV_ABS && (e.code == ABS_X || e.code == ABS_MT_POSITION_X) && e.value != f->x)
			f->x = e.value, f->moved = 1;
		else if (e.type == EV_ABS && (e.code == ABS_Y || e.code == ABS_MT_POSITION_Y) && e.value != f->y)
			f->y = e.value, f->moved = 1;
		else if ((e.type == EV_KEY && e.code == BTN_TOUCH) ||
			 (e.type == EV_ABS && (e.code == ABS_MT_TRACKING_ID || e.code == ABS_MT_SLOT)))
			f->edge = 1;
		if (e.type == EV_SYN && e.code == SYN_REPORT)
			return 1;
	}
}

static int near(int a, int b) { return abs(a - b) <= BACK; }

// Lifts every contact on the output, for when the input goes away mid-touch.
static void release_all(int out)
{
	struct frame f = { .n = 0 };
	for (int slot = 0; slot < 10; slot++) {	// slots the device lacks are ignored
		f.ev[f.n++] = (struct input_event){ .type = EV_ABS, .code = ABS_MT_SLOT, .value = slot };
		f.ev[f.n++] = (struct input_event){ .type = EV_ABS, .code = ABS_MT_TRACKING_ID, .value = -1 };
	}
	f.ev[f.n++] = (struct input_event){ .type = EV_KEY, .code = BTN_TOUCH, .value = 0 };
	f.ev[f.n++] = (struct input_event){ .type = EV_SYN, .code = SYN_REPORT };
	emit(out, &f);
}

int main(int argc, char **argv)
{
	char name[200] = "touchscreen";
	const char *path = argc > 1 ? argv[1] : NULL;
	int in = open_touchscreen(path, name, sizeof(name));
	if (in < 0) {
		fprintf(stderr, "aztouch: no touchscreen found\n");
		return 1;
	}
	int out = clone_device(in, name);
	if (out < 0 || ioctl(in, EVIOCGRAB, 1) < 0) {
		perror("aztouch: uinput/grab");
		return 1;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	fprintf(stderr, "aztouch: filtering \"%s\"\n", name);

	struct frame held, f;
	int has_held = 0, x = -1, y = -1;	// position last passed on
	unsigned long dropped = 0;

	while (!stop) {
		int r = read_frame(in, &f, has_held ? held.x : x, has_held ? held.y : y,
				   has_held ? HOLD_MS : -1);
		if (r < 0) {
			release_all(out);
			has_held = 0;
			x = y = -1;
			close(in);
			fprintf(stderr, "aztouch: touchscreen lost, waiting for it\n");
			while (!stop && (in = open_touchscreen(path, name, sizeof(name))) < 0)
				usleep(200000);
			if (in >= 0 && ioctl(in, EVIOCGRAB, 1) < 0)
				perror("aztouch: grab");
			if (in >= 0)
				fprintf(stderr, "aztouch: touchscreen back\n");
			continue;
		}
		if (r == 0) {			// nothing followed: it was real
			if (!has_held)
				continue;
			emit(out, &held);
			x = held.x, y = held.y;
			has_held = 0;
			continue;
		}
		if (has_held) {
			has_held = 0;
			if (!f.edge && near(f.x, x) && near(f.y, y)) {
				// straight back: the held report was a glitch
				dropped++;
				if ((dropped & (dropped - 1)) == 0)	// log 1, 2, 4, 8, ...
					fprintf(stderr, "aztouch: %lu glitches dropped\n", dropped);
				emit(out, &f);
				x = f.x, y = f.y;
				continue;
			}
			emit(out, &held);
			x = held.x, y = held.y;
		}
		if (f.moved && !f.edge) {
			held = f;
			has_held = 1;
		} else {
			emit(out, &f);
			x = f.x, y = f.y;
		}
	}
	if (in >= 0)
		ioctl(in, EVIOCGRAB, 0);
	release_all(out);
	ioctl(out, UI_DEV_DESTROY);
	return 0;
}
