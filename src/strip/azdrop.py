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
# 0 = never), DROP_SCALE (tile size factor, default 1).

import os
import subprocess
import sys
import tkinter as tk

from PIL import Image, ImageDraw, ImageFont

import azstrip

STATE = azstrip.STATE
BRIDGE_FIFO = azstrip.BRIDGE_FIFO
STRIP_FIFO = azstrip.STRIP_FIFO
BUTTONS = azstrip.BUTTONS
VIEWS = azstrip.VIEWS
MC = azstrip.MC

RETRACT_S = float(os.environ.get("DROP_RETRACT", "8"))
SEP = 3
BAR_FULL = 9 * 150 + 8 * SEP	# the bar at scale 1, for 9 buttons


def apply_scale(scale):
	global SCALE, TAB_W, TAB_H, TILE_W, TILE_H, ICON, FONT_PT
	SCALE = scale
	TAB_W, TAB_H = int(220 * scale), int(34 * scale)
	TILE_W, TILE_H = int(150 * scale), int(104 * scale)
	ICON = int(44 * scale)
	FONT_PT = max(10, int(15 * scale))


apply_scale(float(os.environ.get("DROP_SCALE", "1")))

TILE, LIT, GAP, INK = azstrip.TILE, azstrip.LIT, azstrip.GAP, azstrip.INK


def font(size):
	try:
		return ImageFont.truetype(azstrip.FONT, size)
	except OSError:
		return ImageFont.load_default()


def safe_icon(path, crop):
	azstrip.ICON = ICON	# load_icon scales to this
	try:
		return azstrip.load_icon(path, crop)
	except (OSError, ValueError):	# the player's rootfs not there (--preview off the Pi)
		return Image.new("L", (ICON, ICON), 0)


def render_tile(label, icon, lit):
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
	preview = "--preview" in sys.argv
	root = None if preview else tk.Tk()
	sw, sh = (1920, 1080) if preview else (root.winfo_screenwidth(), root.winfo_screenheight())
	if "DROP_SCALE" not in os.environ:	# the bar must fit the screen (a 1024 px panel)
		apply_scale(min(1.0, (sw - 16) / BAR_FULL))
	icons = [safe_icon(p, c) for (_, p, c, _, _) in BUTTONS]

	if "--preview" in sys.argv:
		n = len(BUTTONS)
		out = Image.new("RGB", (n * (TILE_W + SEP), TAB_H + TILE_H + 4), GAP)
		out.paste(render_tab(True), ((out.width - TAB_W) // 2, 0))
		for i, (b, ic) in enumerate(zip(BUTTONS, icons)):
			out.paste(render_tile(b[0], ic, i == 0), (i * (TILE_W + SEP), TAB_H + 4))
		out.save(sys.argv[sys.argv.index("--preview") + 1])
		return

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

	n = len(BUTTONS)
	bar_w = n * TILE_W + (n - 1) * SEP
	bar = tk.Toplevel(root)
	bar.overrideredirect(True)
	bar.geometry("%dx%d+%d+%d" % (bar_w, TILE_H, (sw - bar_w) // 2, TAB_H))
	bar.configure(bg="black")
	bar_canvas = tk.Canvas(bar, width=bar_w, height=TILE_H, bg="black", highlightthickness=0)
	bar_canvas.pack()
	bar.withdraw()

	tiles = []
	for i, (b, ic) in enumerate(zip(BUTTONS, icons)):
		imgs = []
		for lit in (0, 1):
			f = "%s/drop_%d_%d.png" % (azstrip.CACHE, i, lit)
			render_tile(b[0], ic, lit).save(f)
			imgs.append(tk.PhotoImage(file=f))
		item = bar_canvas.create_image(i * (TILE_W + SEP), 0, image=imgs[0], anchor="nw")
		tiles.append((item, imgs[0], imgs[1]))

	state = {"open": False, "held": -1, "timer": None, "mask": -1}

	def show():
		for k, (item, normal, lit) in enumerate(tiles):
			label = BUTTONS[k][0]
			v = VIEWS.index(label) if label in VIEWS else -1
			on = v >= 0 and state["mask"] > 0 and state["mask"] >> v & 1
			bar_canvas.itemconfigure(item, image=lit if state["held"] == k or on else normal)

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
		state["open"] = v
		tab_canvas.itemconfigure(tab_item, image=tab_imgs[1 if v else 0])
		if v:
			bar.deiconify()
			bar.lift()
		else:
			bar.withdraw()
		arm()
		give_focus_back()

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
