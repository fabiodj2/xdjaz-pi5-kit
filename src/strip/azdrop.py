#!/usr/bin/env python3
# azdrop: the XDJ-AZ's browse buttons (HOME, SOURCE, BROWSE, TAG LIST,
# PLAYLIST, SEARCH, SHORTCUT, MENU, BACK) as a drop-down bar at the top of the
# screen, for a touch screen of any size.
#
# A small tab sits at the top edge, centred. A tap opens a bar of buttons under
# it, over the player; a tap on the tab again, or RETRACT_S seconds without a
# touch, puts it away. Like azstrip.py, it presses the AZ's buttons by setting
# their bit in the mixer frame through azbridge's FIFO ("b DEV BYTE BIT VALUE",
# "h" for HOME) while the finger is down, and lights the button of the open
# view from azbridge's "L mask" on its own FIFO (azbridge -L).
#
# Unlike azstrip.py it takes the touches itself (Tk gets the pointer events X
# makes from the touch screen), so it needs no zones in aztouch and works at
# any resolution. The player's window keeps the keyboard focus (the search
# bar's keyboard needs it): after a touch the focus is handed back to it.
#
#   azdrop.py                 the overlay (needs DISPLAY)
#   azdrop.py --preview OUT   the bar as a picture (no X needed)
#
# Environment: STATE (default /opt/xdjaz/state), DROP_RETRACT (seconds, 8;
# 0 = never), DROP_SCALE (tile size factor, default 1), DROP_BAR (a height in pixels: a fixed bar
# of that height across the top of the screen instead of the tab, no drop-down; run-probe.sh
# AZ_TOPBAR puts the player's window under it).

import os
import subprocess
import sys
import tkinter as tk

from PIL import Image, ImageDraw, ImageFilter, ImageFont

import azstrip

STATE = azstrip.STATE
BRIDGE_FIFO = azstrip.BRIDGE_FIFO
STRIP_FIFO = azstrip.STRIP_FIFO
BUTTONS = azstrip.BUTTONS
VIEWS = azstrip.VIEWS
MC = azstrip.MC

RETRACT_S = float(os.environ.get("DROP_RETRACT", "8"))
# DROP_STYLE=keys (default): dark bevelled keys with the label only, as the buttons above the
# AZ's screen (white, blue when lit). DROP_STYLE=tiles: the flat tiles with an icon.
STYLE = os.environ.get("DROP_STYLE", "keys")
BAR_H = int(os.environ.get("DROP_BAR", "0"))
FIXED = BAR_H > 0
SEP = 0 if STYLE == "keys" else 3
BAR_FULL = 9 * 150 + 8 * SEP	# the bar at scale 1, for 9 buttons


def apply_scale(scale):
	global SCALE, TAB_W, TAB_H, TILE_W, TILE_H, ICON, FONT_PT
	SCALE = scale
	TAB_W, TAB_H = int(220 * scale), int(34 * scale)
	TILE_W, TILE_H = int(150 * scale), int(104 * scale)
	if STYLE == "keys":
		TILE_H = max(48, int(62 * scale))
	ICON = int(44 * scale)
	FONT_PT = max(10, int(15 * scale))


apply_scale(float(os.environ.get("DROP_SCALE", "1")))

TILE, LIT, GAP, INK = azstrip.TILE, azstrip.LIT, azstrip.GAP, azstrip.INK


FONTS = [azstrip.FONT, "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
	 "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"]


def font(size):
	for f in FONTS:
		try:
			return ImageFont.truetype(f, size)
		except OSError:
			pass
	return ImageFont.load_default()


def safe_icon(path, crop):
	azstrip.ICON = ICON	# load_icon scales to this
	try:
		return azstrip.load_icon(path, crop)
	except (OSError, ValueError):	# the player's rootfs not there (--preview off the Pi)
		return Image.new("L", (ICON, ICON), 0)


# The look of the buttons above the AZ's screen (measured off a photo of the unit): a dark
# plate, keys with a light top edge and a dark outline, the label white or lit blue.
PLATE = (0x12, 0x12, 0x12)
KEY_EDGE = (0x05, 0x05, 0x05)
KEY_TOP, KEY_BOT = (0x44, 0x44, 0x44), (0x24, 0x24, 0x24)
KEY_DN_TOP, KEY_DN_BOT = (0x1c, 0x1c, 0x1c), (0x34, 0x34, 0x34)	# pressed: lit from below
HIGHLIGHT = (0x6a, 0x6a, 0x6a)
WHITE = (0xf2, 0xf2, 0xf2)
BLUE = (0x3d, 0x8b, 0xff)
SS = 3					# supersampling, for smooth corners


def vgrad(w, h, top, bot):
	g = Image.new("RGB", (1, h))
	px = g.load()
	for y in range(h):
		t = y / max(1, h - 1)
		px[0, y] = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
	return g.resize((w, h))


def draw_label(im, label, size, ink, glow):
	"""The label centred on im, with a soft glow when lit."""
	w, h = im.size
	f = font(size)
	d = ImageDraw.Draw(im)
	track = max(1, size // 14)			# a little air between the letters
	widths = [d.textlength(c, font=f) for c in label]
	tw = sum(widths) + track * (len(label) - 1)
	asc, desc = f.getmetrics()
	x, y = (w - tw) / 2, (h - (asc + desc)) / 2
	layer = Image.new("L", im.size, 0)
	ld = ImageDraw.Draw(layer)
	cx = x
	for c, cw in zip(label, widths):
		ld.text((cx, y), c, font=f, fill=255)
		cx += cw + track
	if glow:
		g = layer.filter(ImageFilter.GaussianBlur(size * 0.22))
		im.paste(Image.new("RGB", im.size, ink), (0, 0), g.point(lambda v: min(255, int(v * 1.6))))
	im.paste(Image.new("RGB", im.size, ink), (0, 0), layer)


def render_key(label, lit, pressed):
	w, h = TILE_W * SS, TILE_H * SS
	m = max(2, int(3 * SCALE)) * SS
	r = min((h - 2 * m) // 3, 11 * SS)
	im = Image.new("RGB", (w, h), PLATE)
	d = ImageDraw.Draw(im)
	d.rounded_rectangle((m, m, w - m - 1, h - m - 1), r, fill=KEY_EDGE)
	x0, y0, x1, y1 = m + SS, m + SS, w - m - 1 - SS, h - m - 1 - SS
	body = vgrad(x1 - x0 + 1, y1 - y0 + 1, *((KEY_DN_TOP, KEY_DN_BOT) if pressed else (KEY_TOP, KEY_BOT)))
	mask = Image.new("L", body.size, 0)
	ImageDraw.Draw(mask).rounded_rectangle((0, 0, body.width - 1, body.height - 1), r - SS, fill=255)
	im.paste(body, (x0, y0), mask)
	if not pressed:
		d.line((x0 + r, y0 + SS // 2, x1 - r, y0 + SS // 2), fill=HIGHLIGHT, width=SS)
	on = lit or pressed
	draw_label(im, label, int(h * 0.30), BLUE if on else WHITE, on)
	return im.resize((TILE_W, TILE_H), Image.LANCZOS)


def render_tile(label, icon, lit, pressed=False):
	if STYLE == "keys":
		return render_key(label, lit, pressed)
	return render_tile_flat(label, icon, lit or pressed)


def render_tile_flat(label, icon, lit):
	im = Image.new("RGB", (TILE_W, TILE_H), LIT if lit else TILE)
	d = ImageDraw.Draw(im)
	f = font(FONT_PT)
	tw = d.textlength(label, font=f)
	block = icon.height + 8 + FONT_PT
	iy = max(4, (TILE_H - block) // 2)
	im.paste(Image.new("RGB", icon.size, INK), ((TILE_W - icon.width) // 2, iy), icon)
	d.text(((TILE_W - tw) / 2, iy + icon.height + 8), label, font=f, fill=INK)
	return im


def render_tab(open_):
	if STYLE == "keys":
		w, h = TAB_W * SS, TAB_H * SS
		im = Image.new("RGB", (w, h), PLATE)
		d = ImageDraw.Draw(im)
		m, r = SS, min(h // 3, 9 * SS)
		d.rounded_rectangle((m, -2 * r, w - m - 1, h - m - 1), r, fill=KEY_EDGE)
		body = vgrad(w - 2 * m - 2 * SS, h - m - 2 * SS, KEY_TOP, KEY_BOT)
		mask = Image.new("L", body.size, 0)
		ImageDraw.Draw(mask).rounded_rectangle((0, -2 * r, body.width - 1, body.height - 1), r - SS, fill=255)
		im.paste(body, (m + SS, SS), mask)
		f = font(int(h * 0.42))
		tw = d.textlength("AZ", font=f)
		tri = h // 6
		x0 = (w - (tw + tri * 2 + 3 * tri)) / 2
		asc, desc = f.getmetrics()
		d.text((x0, (h - asc - desc) / 2), "AZ", font=f, fill=WHITE)
		ax, cy = x0 + tw + 3 * tri, h / 2
		pts = ([(ax, cy + tri), (ax + 2 * tri, cy + tri), (ax + tri, cy - tri)] if open_
		       else [(ax, cy - tri), (ax + 2 * tri, cy - tri), (ax + tri, cy + tri)])
		d.polygon(pts, fill=BLUE if open_ else WHITE)
		return im.resize((TAB_W, TAB_H), Image.LANCZOS)
	im = Image.new("RGB", (TAB_W, TAB_H), TILE)
	d = ImageDraw.Draw(im)
	f = font(max(10, TAB_H // 2))
	tw = d.textlength("AZ", font=f)
	tri = TAB_H // 3			# the arrow is drawn: the player's font has no glyph for it
	gap = tri
	x0 = (TAB_W - (tw + gap + 2 * tri)) / 2
	d.text((x0, (TAB_H - tri * 2) // 2 - 2), "AZ", font=f, fill=INK)
	ax, cy = x0 + tw + gap, TAB_H // 2
	if open_:	# up
		d.polygon([(ax, cy + tri // 2), (ax + 2 * tri, cy + tri // 2), (ax + tri, cy - tri // 2)], fill=INK)
	else:		# down
		d.polygon([(ax, cy - tri // 2), (ax + 2 * tri, cy - tri // 2), (ax + tri, cy + tri // 2)], fill=INK)
	return im


def send(line):
	"""One line to azbridge's FIFO, if it is there and someone reads it."""
	try:
		fd = os.open(BRIDGE_FIFO, os.O_WRONLY | os.O_NONBLOCK)
	except OSError:
		return
	try:
		os.write(fd, line.encode())
	except OSError:
		pass
	finally:
		os.close(fd)


def press(n, down):
	_, _, _, byte, bit = BUTTONS[n]
	if byte < 0:		# HOME: azbridge's "h", on the press
		if down:
			send("h\n")
	else:
		send("b %x %x %x %d\n" % (MC, byte, bit, down))


def give_focus_back():
	"""The player's window keeps the keyboard (the search bar needs it)."""
	subprocess.Popen(["sh", "-c", "xdotool search --name '^EP147$' | head -1 | xargs -r xdotool windowfocus"],
			 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
	global TILE_W, TILE_H
	preview = "--preview" in sys.argv
	root = None if preview else tk.Tk()
	sw, sh = (1024 if FIXED else 1920, 640 if FIXED else 1080) if preview else (root.winfo_screenwidth(), root.winfo_screenheight())
	if FIXED:				# the keys share the width, the bar's height is the keys'
		apply_scale(BAR_H / 62.0)
		TILE_W, TILE_H = sw // len(BUTTONS), BAR_H
	elif "DROP_SCALE" not in os.environ:	# the bar must fit the screen (a 1024 px panel)
		apply_scale(min(1.0, (sw - 16) / BAR_FULL))
	icons = [safe_icon(p, c) for (_, p, c, _, _) in BUTTONS]

	if "--preview" in sys.argv:
		n = len(BUTTONS)
		top = 0 if FIXED else TAB_H + 4
		out = Image.new("RGB", (n * (TILE_W + SEP), top + TILE_H), PLATE if STYLE == "keys" else GAP)
		if not FIXED:
			out.paste(render_tab(True), ((out.width - TAB_W) // 2, 0))
		lit_demo = {"BROWSE", "TAG LIST", "PLAYLIST", "SEARCH"}	# as in a photo of the unit
		for i, (b, ic) in enumerate(zip(BUTTONS, icons)):
			out.paste(render_tile(b[0], ic, b[0] in lit_demo, b[0] == "HOME"),
				  (i * (TILE_W + SEP), top))
		out.save(sys.argv[sys.argv.index("--preview") + 1])
		return

	n = len(BUTTONS)
	bar_w = n * TILE_W + (n - 1) * SEP
	plate = "#%02x%02x%02x" % PLATE if STYLE == "keys" else "black"
	if FIXED:				# the bar itself, always there, at the top edge
		root.overrideredirect(True)
		root.geometry("%dx%d+%d+0" % (bar_w, TILE_H, (sw - bar_w) // 2))
		root.configure(bg=plate)
		bar_canvas = tk.Canvas(root, width=bar_w, height=TILE_H, bg=plate, highlightthickness=0)
		bar_canvas.pack()
		bar = root
	else:
		root.overrideredirect(True)
		root.geometry("%dx%d+%d+0" % (TAB_W, TAB_H, (sw - TAB_W) // 2))
		root.configure(bg="black")
		tab_canvas = tk.Canvas(root, width=TAB_W, height=TAB_H, bg="black", highlightthickness=0)
		tab_canvas.pack()
		tab_imgs = []
		for open_ in (0, 1):
			f = "%s/tab_%d.png" % (azstrip.CACHE, open_)
			os.makedirs(azstrip.CACHE, exist_ok=True)
			render_tab(open_).save(f)
			tab_imgs.append(tk.PhotoImage(file=f))
		tab_item = tab_canvas.create_image(0, 0, image=tab_imgs[0], anchor="nw")

		bar = tk.Toplevel(root)
		bar.overrideredirect(True)
		bar.geometry("%dx%d+%d+%d" % (bar_w, TILE_H, (sw - bar_w) // 2, TAB_H))
		bar.configure(bg=plate)
		bar_canvas = tk.Canvas(bar, width=bar_w, height=TILE_H, bg=plate, highlightthickness=0)
		bar_canvas.pack()
		bar.withdraw()

	tiles = []
	for i, (b, ic) in enumerate(zip(BUTTONS, icons)):
		imgs = []
		for st in (0, 1, 2):			# idle, lit (its view is open), pressed
			f = "%s/drop_%d_%d.png" % (azstrip.CACHE, i, st)
			render_tile(b[0], ic, st == 1, st == 2).save(f)
			imgs.append(tk.PhotoImage(file=f))
		item = bar_canvas.create_image(i * (TILE_W + SEP), 0, image=imgs[0], anchor="nw")
		tiles.append((item, imgs[0], imgs[1], imgs[2]))

	state = {"open": False, "held": -1, "timer": None, "mask": -1}

	def show():
		for k, (item, normal, lit, down) in enumerate(tiles):
			label = BUTTONS[k][0]
			v = VIEWS.index(label) if label in VIEWS else -1
			on = v >= 0 and state["mask"] > 0 and state["mask"] >> v & 1
			bar_canvas.itemconfigure(item, image=down if state["held"] == k else lit if on else normal)

	def retract():
		state["timer"] = None
		if state["held"] < 0:
			set_open(False)

	def arm():
		if state["timer"]:
			root.after_cancel(state["timer"])
			state["timer"] = None
		if state["open"] and RETRACT_S > 0:
			state["timer"] = root.after(int(RETRACT_S * 1000), retract)

	def set_open(v):
		if FIXED:
			return
		state["open"] = v
		tab_canvas.itemconfigure(tab_item, image=tab_imgs[1 if v else 0])
		if v:
			bar.deiconify()
			bar.lift()
		else:
			bar.withdraw()
		arm()
		give_focus_back()

	if not FIXED:
		tab_canvas.bind("<ButtonPress-1>", lambda e: set_open(not state["open"]))

	def tile_at(x):
		k = x // (TILE_W + SEP)
		return k if 0 <= k < n and x % (TILE_W + SEP) < TILE_W else -1

	def on_down(e):
		k = tile_at(e.x)
		if k >= 0:
			state["held"] = k
			press(k, 1)
			show()
			arm()

	def on_up(e):
		k = state["held"]
		if k >= 0:
			state["held"] = -1
			press(k, 0)
			show()
			arm()
			give_focus_back()

	bar_canvas.bind("<ButtonPress-1>", on_down)
	bar_canvas.bind("<ButtonRelease-1>", on_up)

	# azbridge: "L mask", the open views (to light their buttons)
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
			if line.startswith("L "):
				try:
					state["mask"] = int(line[2:])
				except ValueError:
					pass
		show()

	root.tk.createfilehandler(fd, tk.READABLE, on_fifo)

	def keep_on_top():
		root.lift()
		if state["open"]:
			bar.lift()
		root.after(2000, keep_on_top)
	keep_on_top()
	root.mainloop()


if __name__ == "__main__":
	main()
