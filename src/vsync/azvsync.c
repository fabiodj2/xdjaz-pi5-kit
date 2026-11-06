// azvsync: the panel's vblank times, in the form the AZ player reads them.
//
// On the deck the display driver publishes the time of the last vsync in
// /sys/module/rockchipdrm/parameters/vsync_time: CLOCK_MONOTONIC nanoseconds,
// first line. EP147 (device_adapter::VsyncTime) reads it to time its repaints
// against the panel; without it the player draws blind and the moving
// waveform judders and splits. azvsync waits for each vblank of the CRTC that
// drives the connected output and rewrites that number in a file, which the
// launcher binds into the player's view at the path above.
//
// -o shifts the reported time by that many microseconds into the frame. The
// player starts its repaint just after the reported vsync; with no compositor
// that is when the scanout runs over the waveforms in the upper half, and a
// slow repaint (e.g. after a zoom change) tears there. Reported as the vsync
// one period earlier plus the offset, so the time is never in the future.
//
// -w writes the time that many microseconds after the vblank instead of at
// once (the time written is the same). The player repaints when the file
// changes, whatever it says: at 60 frames a second (AZ-MODS) -o no longer
// moves its repaint, -w does.
//
//   azvsync [-d /dev/dri/card1] [-f file] [-o offset_us] [-w delay_us]
//
// Build: gcc -O2 -Wall -o azvsync azvsync.c $(pkg-config --cflags --libs libdrm)

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static volatile sig_atomic_t stop;

static void on_signal(int sig) { (void)sig; stop = 1; }

// Index of the CRTC driving the first connected connector, or -1.
static int active_crtc_index(int fd)
{
	drmModeRes *res = drmModeGetResources(fd);
	int index = -1;
	if (!res)
		return -1;
	for (int i = 0; i < res->count_connectors && index < 0; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (c && c->connection == DRM_MODE_CONNECTED && c->encoder_id) {
			drmModeEncoder *e = drmModeGetEncoder(fd, c->encoder_id);
			for (int k = 0; e && k < res->count_crtcs; k++)
				if (res->crtcs[k] == e->crtc_id)
					index = k;
			drmModeFreeEncoder(e);
		}
		drmModeFreeConnector(c);
	}
	drmModeFreeResources(res);
	return index;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/dri/card1", *path = "/dev/shm/az-vsync_time";
	long long offset_ns = 0, delay_ns = 0;
	int opt;
	while ((opt = getopt(argc, argv, "d:f:o:w:")) != -1) {
		if (opt == 'd') dev = optarg;
		else if (opt == 'f') path = optarg;
		else if (opt == 'o') offset_ns = atoll(optarg) * 1000;
		else if (opt == 'w') delay_ns = atoll(optarg) * 1000;
		else { fprintf(stderr, "usage: azvsync [-d drm-device] [-f file] [-o offset_us] [-w delay_us]\n"); return 2; }
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	int fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(dev);
		return 1;
	}
	// Kept open and overwritten in place with a fixed-width number: the file
	// is bind-mounted (a rename would detach it), and truncating could let
	// the player read it empty.
	int out = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	if (out < 0 || ftruncate(out, 0) < 0) {
		perror(path);
		return 1;
	}
	int crtc = -1;
	unsigned long errors = 0;
	long long last = 0, period = 0;		// ns
	while (!stop) {
		if (crtc < 0 && (crtc = active_crtc_index(fd)) < 0) {
			sleep(1);	// no output yet (X still starting)
			continue;
		}
		drmVBlank vb = { .request = {
			.type = DRM_VBLANK_RELATIVE |
				((crtc << DRM_VBLANK_HIGH_CRTC_SHIFT) & DRM_VBLANK_HIGH_CRTC_MASK),
			.sequence = 1,
		} };
		if (drmWaitVBlank(fd, &vb) < 0) {
			if (errno == EINTR)
				continue;
			errors++;
			if ((errors & (errors - 1)) == 0)	// log 1, 2, 4, ...
				fprintf(stderr, "azvsync: wait vblank: %s (%lu)\n", strerror(errno), errors);
			crtc = -1;		// the output may have changed
			usleep(100000);
			continue;
		}
		long long t = vb.reply.tval_sec * 1000000000LL + vb.reply.tval_usec * 1000LL;
		if (last && t - last > 5000000 && t - last < 50000000)	// one frame
			period = period ? (7 * period + (t - last)) / 8 : t - last;
		last = t;
		if (delay_ns > 0 && delay_ns < 50000000) {
			struct timespec d = { delay_ns / 1000000000, delay_ns % 1000000000 };
			nanosleep(&d, NULL);
		}
		if (offset_ns && period)
			t += offset_ns % period - period;
		char line[32];
		int n = snprintf(line, sizeof(line), "%020lld\n", t);
		if (pwrite(out, line, n, 0) != n)
			perror(path);
	}
	return 0;
}
