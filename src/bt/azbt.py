#!/usr/bin/env python3
"""azbt: the Pi's Bluetooth controller as the serial port the player's own stack expects.

The player (EP147) carries its own Bluetooth host stack (libanwbt) and speaks HCI over a serial
port, /dev/ttyS0, to the chip. The Pi has no such port: its chip belongs to the kernel's
Bluetooth (hci0). This takes hci0 for itself in "user channel" mode (raw HCI, no BlueZ), makes a
pseudo terminal, and carries the H4 packets (type byte + packet) both ways. run-probe.sh binds
the pty's slave into the player's sandbox as /dev/ttyS0.

The player's init was written for another chip (a vendor library: NXP/Marvell or Realtek). Vendor
commands (OGF 0x3F, e.g. 0xFC09, the baud rate) are answered here with a successful Command
Complete and never reach the controller, which would refuse them. Everything is logged (not the
ACL audio, only its volume) to find what else the init needs.

Run as root (needs the HCI user channel):  azbt.py [HCI_INDEX]
Writes the slave's path to $STATE/ttyBT.path (STATE default /opt/xdjaz/state).
"""
import ctypes
import os
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import termios
import time
import tty

STATE = os.environ.get("STATE", "/opt/xdjaz/state")
AF_BLUETOOTH, BTPROTO_HCI, HCI_CHANNEL_USER = 31, 1, 1
T_CMD, T_ACL, T_SCO, T_EVT, T_ISO = 1, 2, 3, 4, 5
OGF_VENDOR = 0x3F


def log(*a):
	print("%.3f" % time.time(), *a, flush=True)


def open_hci(dev):
	"""hci<dev> in user channel mode: the kernel's Bluetooth lets go of it first."""
	subprocess.run(["rfkill", "unblock", "bluetooth"], check=False)
	subprocess.run(["systemctl", "stop", "bluetooth"], check=False)
	subprocess.run(["hciconfig", "hci%d" % dev, "down"], check=False)
	s = socket.socket(AF_BLUETOOTH, socket.SOCK_RAW | socket.SOCK_CLOEXEC, BTPROTO_HCI)
	addr = struct.pack("<HHH", AF_BLUETOOTH, dev, HCI_CHANNEL_USER)
	libc = ctypes.CDLL("libc.so.6", use_errno=True)
	if libc.bind(s.fileno(), addr, len(addr)) < 0:
		e = ctypes.get_errno()
		raise OSError(e, "hci%d user channel: %s (is it down, unblocked, free of bluetoothd?)" % (dev, os.strerror(e)))
	return s


class H4:
	"""Cuts an H4 byte stream (the pty's) into whole packets."""

	HDR = {T_CMD: 3, T_ACL: 4, T_SCO: 3, T_ISO: 4}

	def __init__(self):
		self.buf = b""

	def feed(self, data):
		self.buf += data
		out = []
		while self.buf:
			t = self.buf[0]
			if t not in self.HDR:
				log("pty: not an H4 type byte: %02x, dropped" % t)
				self.buf = self.buf[1:]
				continue
			h = self.HDR[t]
			if len(self.buf) < 1 + h:
				break
			hdr = self.buf[1:1 + h]
			if t == T_CMD:
				n = hdr[2]
			elif t == T_ACL:
				n = hdr[2] | hdr[3] << 8
			elif t == T_SCO:
				n = hdr[2]
			else:
				n = (hdr[2] | hdr[3] << 8) & 0x3FFF
			if len(self.buf) < 1 + h + n:
				break
			out.append(self.buf[:1 + h + n])
			self.buf = self.buf[1 + h + n:]
		return out


def bridge(hci, pty_master, stop=lambda: False):
	parser = H4()
	acl = {"to_ctl": 0, "to_player": 0}
	last = time.time()
	while not stop():
		r, _, _ = select.select([hci, pty_master], [], [], 1.0)
		if hci in r:
			pkt = hci.recv(2048)
			if pkt[0] == T_ACL:
				acl["to_player"] += len(pkt)
			else:
				log("ctl->player", pkt[:24].hex(), "(%d bytes)" % len(pkt))
			os.write(pty_master, pkt)
		if pty_master in r:
			try:
				data = os.read(pty_master, 4096)
			except OSError:
				data = b""
			for pkt in parser.feed(data):
				if pkt[0] == T_CMD:
					op = pkt[1] | pkt[2] << 8
					if op >> 10 == OGF_VENDOR:	# another chip's command: say it worked
						log("player->ctl VENDOR %04x answered here:" % op, pkt[:24].hex())
						os.write(pty_master, bytes([T_EVT, 0x0E, 4, 1, pkt[1], pkt[2], 0]))
						continue
					log("player->ctl", pkt[:24].hex(), "(%d bytes)" % len(pkt))
				elif pkt[0] == T_ACL:
					acl["to_ctl"] += len(pkt)
				else:
					log("player->ctl", pkt[:24].hex(), "(%d bytes)" % len(pkt))
				hci.send(pkt)
		if time.time() - last > 10 and (acl["to_ctl"] or acl["to_player"]):
			log("ACL bytes in the last 10 s: to controller %d, to player %d" % (acl["to_ctl"], acl["to_player"]))
			acl["to_ctl"] = acl["to_player"] = 0
			last = time.time()


def main():
	dev = int(sys.argv[1]) if len(sys.argv) > 1 else 0
	hci = open_hci(dev)
	master, slave = pty.openpty()
	tty.setraw(slave)
	os.chmod(os.ttyname(slave), 0o666)	# the player's sandbox user is not root here
	path = os.ttyname(slave)
	os.makedirs(STATE, exist_ok=True)
	with open(STATE + "/ttyBT.path", "w") as f:
		f.write(path + "\n")
	log("azbt: hci%d <-> %s (the player's /dev/ttyS0)" % (dev, path))
	signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
	try:
		bridge(hci, master)
	finally:
		try:
			os.remove(STATE + "/ttyBT.path")
		except OSError:
			pass


if __name__ == "__main__":
	main()
