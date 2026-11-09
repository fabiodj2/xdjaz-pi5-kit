#!/usr/bin/env python3
# azstrip: the XDJ-AZ's browse buttons (the ones above its screen) as a column
# of on-screen buttons in the panel's spare strip on the right.
#
# With PANEL=juce the player draws 961x600 on the 1024x600 panel. run-probe.sh
# (STRIP=1) puts its window at x 0, which leaves x 961..1023 free; this draws
# the buttons there, in the look of the browser's own category column (the
# player's icons and font, read from its rootfs at start, nothing copied).
#
# The open view's button is lit (HOME only while held), however it was
# opened: azbridge reads the browse-area LEDs from the player's mixer TX and
# tells us on our FIFO ("L mask", see azbridge.c "views").
#
# The touches never reach X: aztouch catches a touch that comes down on a
# button (its -b zones, printed by --aztouch-args from the table below), sets
# the button's bit in the mixer frame through azbridge's FIFO ("b DEV BYTE BIT
# VALUE") while the finger is down, and tells us on our FIFO ("N 1" / "N 0")
# so the button lights while pressed. The player's window keeps the focus
# (the search bar's keyboard needs it).
#
#   azstrip.py [-x STRIP_X]       the overlay (needs DISPLAY)
#   azstrip.py --aztouch-args     aztouch's options for the buttons
#   azstrip.py --preview OUT.png  the strip as a picture (no X needed)
#
# Bits: UCOM-PROTOCOL.md, "Browse block" (mixer frame, DEV 2).

import os
import sys
import tkinter as tk

from PIL import Image, ImageDraw, ImageFont

ROOTFS = os.environ.get("AZ_ROOTFS", "/opt/xdjaz/rootfs")
RES = ROOTFS + "/home/root/pdj"
FONT = ROOTFS + "/usr/share/fonts/ttf/PioneerDJ-UCGothicLatin-DB.ttf"
STATE = os.environ.get("STATE", "/opt/xdjaz/state")
BRIDGE_FIFO = STATE + "/azbridge.in"
STRIP_FIFO = STATE + "/azstrip.in"
CACHE = STATE + "/tmp/azstrip"

SCR_W, SCR_H = 1024, 600
STRIP_X = int(os.environ.get("STRIP_X", "961"))	# the player's window width
MC = 2						# azbridge's mixer frame

# label, icon (in RES, or "wave"/"sliders": drawn), crop box of the icon or
# None, mixer byte, bit (byte -1: HOME, azbridge's "h": back to the waveform
# screen by pressing the open view's button). DISCOVER and VIEW are the GRV6's
# names for SOURCE and BROWSE.
BUTTONS = [
	("HOME", "wave", None, -1, 0),
	("SOURCE", "DynamicLoad/DeviceIcon/img_Source_USB.png", (0, 0, 90, 60), 32, 7),
	("BROWSE", "DynamicLoad/Browse/StatusAreaCategoryIcon/img_#closeFolder_trackStatusList2.png", None, 32, 6),
	("TAG LIST", "Projucer/Waveform/PlayerWaveform/2Deck/Track/img_2Deck_tagIcon.png", None, 32, 5),
	("PLAYLIST", "DynamicLoad/Browse/StatusAreaCategoryIcon/img_icn_#7_playlist_list.png", None, 32, 4),
	("SEARCH", "DynamicLoad/Browse/SEARCH/img_searchSc1.png", None, 32, 3),
	("SHORTCUT", "sliders", None, 33, 4),
	("MENU", "Projucer/StatusArea/OptionButton/img_utilityBtn_off.png", None, 32, 2),
	("BACK", "Projucer/StatusArea/Title/img_backBtn_normal.png", (9, 9, 41, 41), 33, 7),
]

# The buttons whose view has an LED, in azbridge's order ("views"): bit i of
# its "L mask" lights VIEWS[i]. SHORTCUT has none.
VIEWS = ["SOURCE", "BROWSE", "TAG LIST", "PLAYLIST", "SEARCH", "MENU"]

# colours sampled from the player's screen (the category column's blue)
TILE = (0x32, 0x32, 0x32)	# as the BEAT FX panel's title bar
LIT = (0x00, 0x7d, 0xe1)
GAP = (0, 0, 0)
INK = (0xe8, 0xe8, 0xe8)

MARGIN = 2	# black between the player and the column
SEP = 2		# black between tiles
ICON = 30	# icon box


def tile_rects(x0):
	"""Each button's tile (x0, y0, x1, y1), screen pixels, inclusive."""
	n = len(BUTTONS)
	h = (SCR_H - SEP * (n - 1)) // n
	top = (SCR_H - h * n - SEP * (n - 1)) // 2
	return [(x0 + MARGIN, top + i * (h + SEP), SCR_W - 1, top + i * (h + SEP) + h - 1)
		for i in range(n)]


def touch_rects(x0):
	"""Touch zones: the whole strip, split between the tiles."""
	n = len(BUTTONS)
	return [(x0, i * SCR_H // n, SCR_W - 1, (i + 1) * SCR_H // n - 1) for i in range(n)]


def aztouch_args(x0):
	args = ["-f", BRIDGE_FIFO, "-p", STRIP_FIFO]
	for (x_0, y_0, x_1, y_1), (_, _, _, byte, bit) in zip(touch_rects(x0), BUTTONS):
		dev = MC if byte >= 0 else -1
		args += ["-b", "%d,%d,%d,%d,%d,%d,%d" % (x_0, y_0, x_1, y_1, dev, max(byte, 0), bit)]
	return " ".join(args)


def wave_icon():
	"""A waveform: bars, tallest in the middle."""
	heights = [6, 12, 20, 14, 26, 18, 10, 22, 12, 6]
	im = Image.new("L", (ICON, ICON), 0)
	d = ImageDraw.Draw(im)
	for i, h in enumerate(heights):
		x = 1 + i * 3
		d.rectangle((x, (ICON - h) // 2, x + 1, (ICON + h) // 2), fill=255)
	return im


def sliders_icon():
	"""Quick settings: three sliders."""
	im = Image.new("L", (ICON, ICON), 0)
	d = ImageDraw.Draw(im)
	for i, knob in enumerate((19, 8, 15)):
		y = 6 + i * 9
		d.rectangle((2, y, ICON - 3, y + 1), fill=255)
		d.rectangle((knob, y - 3, knob + 4, y + 4), fill=255)
	return im


def load_icon(path, crop):
	if path == "wave":
		return wave_icon()
	if path == "sliders":
		return sliders_icon()
	im = Image.open(RES + "/" + path).convert("RGBA")
	if crop:
		im = im.crop(crop)
	box = im.getchannel("A").point(lambda v: 255 if v > 40 else 0).getbbox()
	if box:
		im = im.crop(box)
	# keep the shape (bright, opaque pixels) and draw it white: some come
	# grey, BACK's on a dark box
	shape = Image.new("L", im.size, 0)
	shape.paste(im.convert("L"), mask=im.getchannel("A"))
	shape = shape.point(lambda v: 255 if v > 140 else v * 255 // 140)
	scale = min(ICON / im.width, ICON / im.height)
	size = (max(1, round(im.width * scale)), max(1, round(im.height * scale)))
	return shape.resize(size, Image.LANCZOS)


def render(rect, label, icon, lit):
	w, h = rect[2] - rect[0] + 1, rect[3] - rect[1] + 1
	im = Image.new("RGB", (w, h), LIT if lit else TILE)
	d = ImageDraw.Draw(im)
	try:
		font = ImageFont.truetype(FONT, 10)
	except OSError:
		font = ImageFont.load_default()
	tw = d.textlength(label, font=font)
	text_h = 12
	block = icon.height + 6 + text_h
	iy = (h - block) // 2
	im.paste(Image.new("RGB", icon.size, INK), ((w - icon.width) // 2, iy), icon)
	d.text(((w - tw) / 2, iy + icon.height + 6), label, font=font, fill=INK)
	return im


def main():
	x0 = STRIP_X
	if "-x" in sys.argv:
		x0 = int(sys.argv[sys.argv.index("-x") + 1])
	if "--aztouch-args" in sys.argv:
		print(aztouch_args(x0))
		return
	if "--preview" in sys.argv:	# the strip as a PNG, button 0 lit
		out = Image.new("RGB", (SCR_W - x0, SCR_H), GAP)
		for i, (rect, (label, path, crop, _, _)) in enumerate(zip(tile_rects(x0), BUTTONS)):
			out.paste(render(rect, label, load_icon(path, crop), i == 0), (rect[0] - x0, rect[1]))
		out.save(sys.argv[sys.argv.index("--preview") + 1])
		return
	if x0 != STRIP_X:
		print("azstrip: window %d wide, aztouch has the zones for %d" % (x0, STRIP_X), file=sys.stderr)

	os.makedirs(CACHE, exist_ok=True)
	rects = tile_rects(x0)
	root = tk.Tk()
	root.overrideredirect(True)
	root.geometry("%dx%d+%d+0" % (SCR_W - x0, SCR_H, x0))
	root.configure(bg="black")
	canvas = tk.Canvas(root, width=SCR_W - x0, height=SCR_H, bg="black", highlightthickness=0)
	canvas.pack()

	tiles = []	# per button: (canvas item, normal image, lit image)
	for i, (rect, (label, path, crop, _, _)) in enumerate(zip(rects, BUTTONS)):
		icon = load_icon(path, crop)
		imgs = []
		for lit in (0, 1):
			f = "%s/%d_%d.png" % (CACHE, i, lit)
			render(rect, label, icon, lit).save(f)
			imgs.append(tk.PhotoImage(file=f))
		item = canvas.create_image(rect[0] - x0, rect[1], image=imgs[0], anchor="nw")
		tiles.append((item, imgs[0], imgs[1]))

	# aztouch tells which button is down
	if not os.path.exists(STRIP_FIFO):
		os.mkfifo(STRIP_FIFO, 0o600)
	fd = os.open(STRIP_FIFO, os.O_RDWR | os.O_NONBLOCK)
	pending = [""]

	def on_fifo(*_):
		try:
			pending[0] += os.read(fd, 512).decode(errors="replace")
		except BlockingIOError:
			return
		*lines, pending[0] = pending[0].split("\n")
		for line in lines:
			try:
				if line.startswith("L "):	# azbridge: the open views
					opened[0] = int(line[2:])
				else:				# aztouch: a button held
					n, down = map(int, line.split())
					if 0 <= n < len(tiles):
						held[n] = down
			except ValueError:
				continue
		show()

	# a tile is lit while held, or while its view is open (HOME only held)
	held = [0] * len(tiles)
	opened = [-1]		# azbridge's mask; -1 not heard yet
	def show():
		for n, (item, normal, lit) in enumerate(tiles):
			label = BUTTONS[n][0]
			v = VIEWS.index(label) if label in VIEWS else -1
			on = v >= 0 and opened[0] > 0 and opened[0] >> v & 1
			canvas.itemconfigure(item, image=lit if held[n] or on else normal)

	root.tk.createfilehandler(fd, tk.READABLE, on_fifo)

	# stay above the player's window
	def keep_on_top():
		root.lift()
		root.after(2000, keep_on_top)
	keep_on_top()
	root.mainloop()


if __name__ == "__main__":
	main()
