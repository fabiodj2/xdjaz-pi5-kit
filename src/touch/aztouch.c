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
// Snap zones: a touch that comes down inside a zone is moved, for as long as
// it lasts, to the zone's target (the centre of a small button). The AZ's
// top bar has small icons at the panel's edge (the REC timer that opens
// MIXER SETTINGS, the gear / info) and fingers land around them: measured
// 2026-11-06, taps on the gear spread over x 959..1018 for an icon at
// 951..981, the player's window ending at 992.
//
// Button zones: a touch that comes down inside one never reaches X. While it
// lasts, the zone's bit in a UCOM frame is set through azbridge's FIFO
// ("b DEV BYTE BIT 1", then "... 0"): the on-screen buttons of
// strip/azstrip.py, which draws them and lights the one held (told on its
// FIFO as "N 1" / "N 0", N the zone's number). A zone with dev -1 sends "h"
// instead (back to the waveform screen).
//
//   aztouch [-S WxH] [-r] [-z x0,y0,x1,y1,tx,ty]... [-f fifo] [-p fifo]
//           [-b x0,y0,x1,y1,dev,byte,bit]... [-c zones.conf] [-d] [/dev/input/eventN]
//     -S  the screen in pixels (zones are in screen pixels), default 1024x600
//     -r  the touchscreen reports 180 degrees off (xorg's CalibrationMatrix
//         turns it back), so zones are turned too
//     -z  a zone, up to 8
//     -f  azbridge's FIFO (its -i), for the buttons
//     -p  the button overlay's FIFO (optional)
//     -b  a button zone
//     -c  BEAT FX zones from a file (B: a command, X: the X-PAD), see load_zones
//     -d  log where each touch lands, in screen pixels (for measuring zones)
//   default device: the first direct-touch one (a fixed path is not found
//   again after a drop)
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

static struct zone { int x0, y0, x1, y1, tx, ty; } zones[8];
static int nzones, scr_w = 1024, scr_h = 600, turned;
static struct input_absinfo ax, ay;	// the device's X and Y axes
static int snapping, snap_x, snap_y;	// device units

// kind 0: a bit of a UCOM frame (azstrip's buttons); 1: a BEAT FX command sent as
// "f CMD" on the touch down; 2: the X-PAD, "x 1 POS" while the finger is down and
// moves, "x 0" on the lift (horizontal: left 0 .. right 255; vertical: bottom 0 ..
// top 255).
enum { B_BIT, B_CMD, B_XPAD };
#define MAXBUTTONS	32
static struct button { int x0, y0, x1, y1, dev, byte, bit, kind, vertical; char cmd[16]; } buttons[MAXBUTTONS];
static int xpad_last = -1;
static int debug_touch;
static int nbuttons, held_button = -1;	// the button the finger is on
static const char *bridge_fifo, *strip_fifo;

// One line to a FIFO, if someone reads it (a missing reader is no error).
static void fifo_line(const char *path, const char *line)
{
	if (!path)
		return;
	int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	if (write(fd, line, strlen(line)) < 0)
		perror("aztouch: fifo");
	close(fd);
}

// The X-PAD's position for a finger at screen x, y; sent when it changes.
static void xpad_update(const struct button *b, int sx, int sy)
{
	int pos;
	if (b->vertical)
		pos = (b->y1 - sy) * 255 / (b->y1 - b->y0 > 0 ? b->y1 - b->y0 : 1);
	else
		pos = (sx - b->x0) * 255 / (b->x1 - b->x0 > 0 ? b->x1 - b->x0 : 1);
	pos = pos < 0 ? 0 : pos > 255 ? 255 : pos;
	if (pos == xpad_last)
		return;
	xpad_last = pos;
	char line[32];
	snprintf(line, sizeof(line), "x 1 %d\n", pos);
	fifo_line(bridge_fifo, line);
}

static void press(int b, int down)
{
	char line[64];
	if (buttons[b].kind == B_CMD) {
		if (down) {
			snprintf(line, sizeof(line), "f %s\n", buttons[b].cmd);
			fifo_line(bridge_fifo, line);
		}
		return;
	}
	if (buttons[b].kind == B_XPAD) {
		if (!down) {
			xpad_last = -1;
			fifo_line(bridge_fifo, "x 0\n");
		}
		return;
	}
	if (buttons[b].dev < 0) {	// HOME: azbridge's "h", on the press
		if (down)
			fifo_line(bridge_fifo, "h\n");
	} else {
		snprintf(line, sizeof(line), "b %x %x %x %d\n", buttons[b].dev, buttons[b].byte,
			 buttons[b].bit, down);
		fifo_line(bridge_fifo, line);
	}
	snprintf(line, sizeof(line), "%d %d\n", b, down);
	fifo_line(strip_fifo, line);
}

// device <-> screen pixels
static int to_screen(int v, const struct input_absinfo *a, int size)
{
	int s = (int)((long long)(v - a->minimum) * size / (a->maximum - a->minimum + 1));
	return turned ? size - 1 - s : s;
}

static int to_device(int s, const struct input_absinfo *a, int size)
{
	if (turned)
		s = size - 1 - s;
	return a->minimum + (int)(((long long)s * 2 + 1) * (a->maximum - a->minimum + 1) / (2 * size));
}

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

static void emit(int out, struct frame *f)
{
	int down = 0, up = 0;
	for (int i = 0; i < f->n; i++)
		if (f->ev[i].type == EV_KEY && f->ev[i].code == BTN_TOUCH)
			*(f->ev[i].value ? &down : &up) = 1;
	int axes = ax.maximum > ax.minimum && ay.maximum > ay.minimum;
	if (down && debug_touch && axes)
		fprintf(stderr, "aztouch: touch at %d,%d\n", to_screen(f->x, &ax, scr_w),
			to_screen(f->y, &ay, scr_h));
	if (down && nbuttons && axes && held_button < 0) {
		int sx = to_screen(f->x, &ax, scr_w), sy = to_screen(f->y, &ay, scr_h);
		for (int b = 0; b < nbuttons; b++)
			if (sx >= buttons[b].x0 && sx <= buttons[b].x1 &&
			    sy >= buttons[b].y0 && sy <= buttons[b].y1) {
				held_button = b;
				press(b, 1);
				if (buttons[b].kind == B_XPAD)
					xpad_update(&buttons[b], sx, sy);
				break;
			}
	}
	if (held_button >= 0) {		// the whole touch is the button's
		if (!up && axes && buttons[held_button].kind == B_XPAD)
			xpad_update(&buttons[held_button], to_screen(f->x, &ax, scr_w),
				    to_screen(f->y, &ay, scr_h));
		if (up) {
			press(held_button, 0);
			held_button = -1;
		}
		return;
	}
	if (down && nzones && axes) {
		int sx = to_screen(f->x, &ax, scr_w), sy = to_screen(f->y, &ay, scr_h);
		snapping = 0;
		for (int z = 0; z < nzones && !snapping; z++)
			if (sx >= zones[z].x0 && sx <= zones[z].x1 && sy >= zones[z].y0 && sy <= zones[z].y1) {
				snapping = 1;
				snap_x = to_device(zones[z].tx, &ax, scr_w);
				snap_y = to_device(zones[z].ty, &ay, scr_h);
			}
	}
	if (snapping)
		for (int i = 0; i < f->n; i++) {
			struct input_event *e = &f->ev[i];
			if (e->type == EV_ABS && (e->code == ABS_X || e->code == ABS_MT_POSITION_X))
				e->value = snap_x;
			else if (e->type == EV_ABS && (e->code == ABS_Y || e->code == ABS_MT_POSITION_Y))
				e->value = snap_y;
		}
	if (up)
		snapping = 0;
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

// Zones from a text file (-c), one per line, "#" starts a comment:
//   B x0 y0 x1 y1 CMD      a BEAT FX command: on, next, prev, beat+, beat-, type=N
//   X x0 y0 x1 y1 [v]      the X-PAD (v: vertical, bottom 0 .. top 255)
// Coordinates are screen pixels.
static void load_zones(const char *path)
{
	FILE *fp = fopen(path, "r");
	if (!fp) {
		fprintf(stderr, "aztouch: %s: %s\n", path, strerror(errno));
		return;
	}
	char line[200];
	int n = 0;
	while (fgets(line, sizeof(line), fp)) {
		char *hash = strchr(line, '#');
		if (hash)
			*hash = 0;
		char kind, word[40] = "";
		struct button b = { .dev = -2 };
		int got = sscanf(line, " %c %d %d %d %d %39s", &kind, &b.x0, &b.y0, &b.x1, &b.y1, word);
		if (got < 5 || nbuttons >= MAXBUTTONS)
			continue;
		if (kind == 'B' && got == 6) {
			b.kind = B_CMD;
			snprintf(b.cmd, sizeof(b.cmd), "%s", word);
			char *eq = strchr(b.cmd, '=');	// type=N -> "type N"
			if (eq)
				*eq = ' ';
		} else if (kind == 'X') {
			b.kind = B_XPAD;
			b.vertical = word[0] == 'v';
		} else {
			fprintf(stderr, "aztouch: %s: ignored: %s", path, line);
			continue;
		}
		buttons[nbuttons++] = b;
		n++;
	}
	fclose(fp);
	fprintf(stderr, "aztouch: %d BEAT FX zones from %s\n", n, path);
}

int main(int argc, char **argv)
{
	char name[200] = "touchscreen";
	int opt;
	while ((opt = getopt(argc, argv, "S:rz:f:p:b:c:d")) != -1) {
		struct zone *z = &zones[nzones];
		struct button *b = &buttons[nbuttons];
		if (opt == 'f' || opt == 'p') {
			*(opt == 'f' ? &bridge_fifo : &strip_fifo) = optarg;
			continue;
		}
		if (opt == 'b' && nbuttons < MAXBUTTONS && sscanf(optarg, "%d,%d,%d,%d,%d,%d,%d", &b->x0, &b->y0,
							  &b->x1, &b->y1, &b->dev, &b->byte, &b->bit) == 7) {
			nbuttons++;
			continue;
		}
		if (opt == 'c') {
			load_zones(optarg);
			continue;
		}
		if (opt == 'd') {
			debug_touch = 1;
			continue;
		}
		if (opt == 'S' && sscanf(optarg, "%dx%d", &scr_w, &scr_h) == 2)
			continue;
		if (opt == 'r') {
			turned = 1;
			continue;
		}
		if (opt == 'z' && nzones < 8 && sscanf(optarg, "%d,%d,%d,%d,%d,%d", &z->x0, &z->y0,
							&z->x1, &z->y1, &z->tx, &z->ty) == 6) {
			nzones++;
			continue;
		}
		fprintf(stderr, "usage: aztouch [-S WxH] [-r] [-z x0,y0,x1,y1,tx,ty]... [-f fifo] [-p fifo]\n"
				"               [-b x0,y0,x1,y1,dev,byte,bit]... [-c zones.conf] [-d] [device]\n");
		return 2;
	}
	const char *path = optind < argc ? argv[optind] : NULL;
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
	ioctl(in, EVIOCGABS(ABS_X), &ax);
	ioctl(in, EVIOCGABS(ABS_Y), &ay);
	fprintf(stderr, "aztouch: filtering \"%s\" (%d..%d x %d..%d), %d snap zones, %d buttons\n", name,
		ax.minimum, ax.maximum, ay.minimum, ay.maximum, nzones, nbuttons);

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
