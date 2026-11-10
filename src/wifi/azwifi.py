#!/usr/bin/env python3
"""azwifi: the player's own Wi-Fi screen drives the host's Wi-Fi.

The stock player (EP147) runs its Wi-Fi commands on "mlan0" through /bin/sh
(wpa_supplicant, wpa_cli, iw, ifconfig). azshim sends each of them here, over
a unix socket inside the player's /tmp, and exits with the code we answer.
We carry them out on the host's Wi-Fi interface and write what the player
reads back: the first line of /tmp/shelloutput/<id>, the iw-format
/tmp/scan.log, and CONNECTED / DISCONNECTED in /tmp/WpaCtrl/wpa_event.

The player connects by writing network={...} blocks to /tmp/wpa.conf, then
"wpa_cli reconfigure" (we read the file then) and "wpa_cli reconnect"; it
counts the connect as done once "ifconfig mlan0" shows an IPv4 address.

Interface: AZWIFI_IFACE, else the first Wi-Fi device NetworkManager lists,
else the first /sys/class/net/*/wireless.
Backend: AZWIFI_BACKEND=nm|wpa, else NetworkManager (nmcli) when it runs and
manages the interface, else the host's wpa_supplicant (wpa_cli, its control
socket in /run/wpa_supplicant; DHCP is then up to the host, e.g. dhcpcd or
systemd-networkd).
Networks the player adds are marked as ours (NM profiles "azwifi-<ssid>",
wpa_supplicant networks with id_str "azwifi"); the host's own networks are
never changed or removed.

Run as root:  azwifi.py [STATE]   (default /opt/xdjaz/state)
"""
import glob
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

STATE = sys.argv[1] if len(sys.argv) > 1 else "/opt/xdjaz/state"
TMP = os.path.join(STATE, "tmp")          # the player's /tmp
SOCK = os.path.join(TMP, "azwifi.sock")
PREFIX = "azwifi-"                        # NM profile names
IDSTR = "azwifi"                          # wpa_supplicant id_str
ENV = dict(os.environ, LC_ALL="C", LANG="C")

lock = threading.Lock()
nets = []            # last /tmp/wpa.conf: [{ssid, psk, priority, hidden}]
listed = None        # ssids of the player's last saved list (None: none yet)
manual = False       # a disconnect came first: the user picked a network
events = False       # the player listens for wpa_event (wpa_cli -a)
gen = 0              # bumped on every disconnect/reconnect; stale connects stop


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def run(*args, timeout=40):
    try:
        r = subprocess.run(list(args), env=ENV, capture_output=True,
                           text=True, timeout=timeout)
        return r.returncode, r.stdout
    except (subprocess.TimeoutExpired, OSError):
        return 124, ""


def nmcli(*args, timeout=40):
    return run("nmcli", *args, timeout=timeout)


def split_t(line):
    """Split an nmcli -t line on unescaped ':'."""
    out, cur, esc = [], "", False
    for ch in line:
        if esc:
            cur += ch
            esc = False
        elif ch == "\\":
            esc = True
        elif ch == ":":
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return out


def host_path(p):
    """Sandbox /tmp/... -> host path; anything else is refused."""
    if not p.startswith("/tmp/") or ".." in p:
        return None
    return os.path.join(TMP, p[5:])


def write_file(p, text):
    hp = host_path(p)
    if not hp:
        return
    os.makedirs(os.path.dirname(hp), exist_ok=True)
    with open(hp, "w") as f:
        f.write(text)


def pct_dbm(pct):
    return int(pct or 0) // 2 - 100


def ipv4(iface):
    rc, out = run("ip", "-4", "-o", "addr", "show", "dev", iface, timeout=5)
    m = re.search(r"inet (\d+\.\d+\.\d+\.\d+)", out)
    return m.group(1) if m else ""


# ---- interface and backend -----------------------------------------------------

def nm_running():
    if not shutil.which("nmcli"):
        return False
    rc, out = nmcli("-t", "-f", "RUNNING", "general", timeout=5)
    return out.strip() == "running"


def find_iface():
    if os.environ.get("AZWIFI_IFACE"):
        return os.environ["AZWIFI_IFACE"]
    if nm_running():
        rc, out = nmcli("-t", "-f", "DEVICE,TYPE", "dev", timeout=5)
        for line in out.splitlines():
            f = split_t(line)
            if len(f) >= 2 and f[1] == "wifi":
                return f[0]
    for d in sorted(glob.glob("/sys/class/net/*/wireless")):
        return d.split("/")[4]
    return ""


class NM:
    """NetworkManager through nmcli."""
    name = "nm"

    def __init__(self, iface):
        self.iface = iface

    @staticmethod
    def usable(iface):
        if not nm_running():
            return False
        rc, out = nmcli("-t", "-f", "DEVICE,STATE", "dev", timeout=5)
        for line in out.splitlines():
            f = split_t(line)
            if len(f) >= 2 and f[0] == iface:
                return f[1] != "unmanaged"
        return False

    def state(self):
        rc, out = nmcli("-t", "-f", "GENERAL.STATE", "dev", "show",
                        self.iface, timeout=10)
        st = out.partition(":")[2]
        up = st[:3] == "100"
        return up, (ipv4(self.iface) if up else "")

    def ap(self):
        rc, out = nmcli("-t", "-f", "ACTIVE,BSSID,SSID,FREQ,SIGNAL", "dev",
                        "wifi", "list", "ifname", self.iface, "--rescan", "no",
                        timeout=10)
        for line in out.splitlines():
            f = split_t(line)
            if len(f) >= 5 and f[0] == "yes":
                return {"bssid": f[1].lower(), "ssid": f[2],
                        "freq": int(re.sub(r"\D", "", f[3]) or 0),
                        "signal": pct_dbm(f[4])}
        return None

    def scan(self):
        rc, out = nmcli("-t", "-f", "BSSID,SSID,FREQ,SIGNAL,SECURITY", "dev",
                        "wifi", "list", "ifname", self.iface, "--rescan", "yes",
                        timeout=30)
        if rc:
            return None
        res = []
        for line in out.splitlines():
            f = split_t(line)
            if len(f) < 5:
                continue
            sec = f[4]
            res.append({"bssid": f[0].lower(), "ssid": f[1],
                        "freq": int(re.sub(r"\D", "", f[2]) or 0),
                        "signal": pct_dbm(f[3]), "wpa1": "WPA1" in sec,
                        "psk": "WPA2" in sec, "sae": "WPA3" in sec,
                        "priv": bool(sec and sec != "--")})
        return res

    def rescan(self):
        nmcli("dev", "wifi", "rescan", "ifname", self.iface)

    def disconnect(self):
        nmcli("dev", "disconnect", self.iface, timeout=15)

    def radio_on(self):
        nmcli("radio", "wifi", "on", timeout=10)

    def ours(self):
        """{ssid: profile name} of the profiles we made."""
        rc, out = nmcli("-t", "-f", "NAME,TYPE", "con", "show", timeout=10)
        res = {}
        for line in out.splitlines():
            f = split_t(line)
            if len(f) >= 2 and f[0].startswith(PREFIX) and "wireless" in f[1]:
                res[f[0][len(PREFIX):]] = f[0]
        return res

    def forget(self, ssid, ref):
        nmcli("con", "delete", ref, timeout=15)

    def security(self, ssid):
        """The SECURITY column NM's last scan has for ssid ("" if unseen)."""
        rc, out = nmcli("-t", "-f", "SSID,SECURITY", "dev", "wifi", "list",
                        "ifname", self.iface, "--rescan", "no", timeout=10)
        for line in out.splitlines():
            f = split_t(line)
            if len(f) >= 2 and f[0] == ssid:
                return f[1]
        return ""

    def connect(self, n):
        name = PREFIX + n["ssid"]
        if n["ssid"] in self.ours():
            nmcli("con", "delete", name, timeout=15)
        # The security comes from the scan, not from the player's authType
        # guess (on AUTO it tries WPA3, WPA2/3, WPA2, ... in turn). A WPA2/WPA3
        # network is joined as WPA2-PSK: SAE fails on some Wi-Fi chips
        # (brcmfmac of the Raspberry Pi among them), PSK works everywhere.
        sec = self.security(n["ssid"])
        if not n["psk"]:
            km = []
        elif "WPA2" in sec or "WPA1" in sec:
            km = ["wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.psk", n["psk"]]
        elif "WPA3" in sec:
            km = ["wifi-sec.key-mgmt", "sae", "wifi-sec.psk", n["psk"]]
        elif "WEP" in sec:
            km = ["wifi-sec.key-mgmt", "none", "wifi-sec.wep-key0", n["psk"],
                  "wifi-sec.wep-key-type", "key"]
        else:       # not in the scan (hidden): the most common kind
            km = ["wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.psk", n["psk"]]
        rc, out = nmcli("con", "add", "type", "wifi", "ifname", self.iface,
                        "con-name", name, "ssid", n["ssid"],
                        "connection.autoconnect", "yes",
                        "connection.autoconnect-priority", str(n["priority"]),
                        "802-11-wireless.hidden", "yes" if n["hidden"] else "no",
                        *km, timeout=15)
        if rc:
            log("profile not made for", n["ssid"])
            return False
        rc, out = nmcli("--wait", "25", "con", "up", name, "ifname", self.iface,
                        timeout=35)
        return rc == 0

    def drop(self, n):
        nmcli("con", "delete", PREFIX + n["ssid"], timeout=15)

    def host_networks(self):
        nmcli("--wait", "25", "dev", "connect", self.iface, timeout=35)


class WPA:
    """The host's own wpa_supplicant through wpa_cli."""
    name = "wpa"

    def __init__(self, iface):
        self.iface = iface

    def cli(self, *args, timeout=15):
        return run("wpa_cli", "-i", self.iface, *args, timeout=timeout)

    @staticmethod
    def usable(iface):
        if not shutil.which("wpa_cli"):
            return False
        rc, out = run("wpa_cli", "-i", iface, "ping", timeout=5)
        return rc == 0 and "PONG" in out

    def status(self):
        rc, out = self.cli("status")
        return dict(l.split("=", 1) for l in out.splitlines() if "=" in l)

    def state(self):
        up = self.status().get("wpa_state") == "COMPLETED"
        ip = ipv4(self.iface) if up else ""
        # the player waits for an address; without one it is not connected yet
        return up and bool(ip), ip

    def ap(self):
        s = self.status()
        if s.get("wpa_state") != "COMPLETED":
            return None
        rc, out = self.cli("signal_poll")
        m = re.search(r"RSSI=(-?\d+)", out)
        return {"bssid": s.get("bssid", "").lower(), "ssid": s.get("ssid", ""),
                "freq": int(s.get("freq", 0) or 0),
                "signal": int(m.group(1)) if m else -100}

    def scan(self):
        rc, out = self.cli("scan")
        if rc or "FAIL" in out and "BUSY" not in out:
            return None
        time.sleep(4)
        rc, out = self.cli("scan_results")
        if rc:
            return None
        res = []
        for line in out.splitlines()[1:]:      # header: bssid / frequency / ...
            f = line.split("\t")
            if len(f) < 5:
                continue
            fl = f[3]
            res.append({"bssid": f[0].lower(), "ssid": wpa_unescape(f[4]),
                        "freq": int(f[1] or 0), "signal": int(f[2] or -100),
                        "wpa1": "[WPA-" in fl, "psk": "PSK" in fl,
                        "sae": "SAE" in fl,
                        "priv": any(k in fl for k in ("WPA", "RSN", "WEP", "SAE"))})
        return res

    def rescan(self):
        self.cli("scan")

    def disconnect(self):
        self.cli("disconnect")

    def radio_on(self):
        pass

    def networks(self):
        """[(id, ssid)] of all configured networks."""
        rc, out = self.cli("list_networks")
        res = []
        for line in out.splitlines()[1:]:
            f = line.split("\t")
            if len(f) >= 2 and f[0].isdigit():
                res.append((f[0], f[1]))
        return res

    def ours(self):
        res = {}
        for nid, ssid in self.networks():
            rc, out = self.cli("get_network", nid, "id_str")
            if out.strip().strip('"') == IDSTR:
                res[ssid] = nid
        return res

    def forget(self, ssid, ref):
        self.cli("remove_network", ref)
        self.cli("save_config")

    def connect(self, n):
        for ssid, nid in self.ours().items():
            if ssid == n["ssid"]:
                self.cli("remove_network", nid)
        sec = next((a for a in (self.scan_cache or [])
                    if a["ssid"] == n["ssid"]), None)
        rc, out = self.cli("add_network")
        nid = out.strip().splitlines()[-1] if out.strip() else ""
        if not nid.isdigit():
            return False
        q = lambda v: '"%s"' % v
        sets = [("ssid", q(n["ssid"])), ("id_str", q(IDSTR)),
                ("priority", str(n["priority"]))]
        if n["hidden"]:
            sets.append(("scan_ssid", "1"))
        pw = n["psk"]
        if not pw:
            sets.append(("key_mgmt", "NONE"))
        elif sec and sec["sae"] and not sec["psk"]:
            sets += [("key_mgmt", "SAE"), ("sae_password", q(pw)),
                     ("ieee80211w", "2")]
        elif sec and not (sec["psk"] or sec["sae"] or sec["wpa1"]) and sec["priv"]:
            sets += [("key_mgmt", "NONE"), ("wep_key0", q(pw))]
        else:       # WPA2/WPA3 joined as WPA2-PSK (see NM.connect)
            sets += [("key_mgmt", "WPA-PSK"), ("psk", q(pw))]
        for k, v in sets:
            self.cli("set_network", nid, k, v)
        self.cli("enable_network", nid)
        self.cli("select_network", nid)
        for _ in range(25):
            time.sleep(1)
            if self.state()[0]:
                self.cli("enable_network", "all")   # select_network disabled them
                self.cli("save_config")
                return True
        self.cli("remove_network", nid)
        self.cli("enable_network", "all")
        return False

    def drop(self, n):
        pass        # connect() already removed the failed network

    def host_networks(self):
        self.cli("enable_network", "all")
        self.cli("reconnect")

    scan_cache = None


def wpa_unescape(s):
    """wpa_cli prints non-printable SSID bytes as \\xNN, \\" and \\\\."""
    out, i = bytearray(), 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            if s[i + 1] == "x" and i + 3 < len(s):
                out.append(int(s[i + 2:i + 4], 16))
                i += 4
                continue
            out += s[i + 1].encode()
            i += 2
            continue
        out += s[i].encode()
        i += 1
    return out.decode(errors="replace")


def pick_backend(iface):
    want = os.environ.get("AZWIFI_BACKEND", "")
    for cls in (NM, WPA):
        if want == cls.name or (not want and cls.usable(iface)):
            return cls(iface)
    return None


IFACE = ""
B = None


_cache = {}


def cached(fn):
    """fn's result for up to 1 s (the player polls the signal every second)."""
    def wrap(fresh=False):
        t, v = _cache.get(fn, (0, None))
        if fresh or time.monotonic() - t > 1.0:
            v = fn()
            _cache[fn] = (time.monotonic(), v)
        return v
    return wrap


@cached
def dev_state():
    """(connected, ipv4)."""
    return B.state() if B else (False, "")


@cached
def active_ap():
    return B.ap() if B else None


def mac():
    try:
        with open(f"/sys/class/net/{IFACE}/address") as f:
            return f.read().strip()
    except OSError:
        return "00:00:00:00:00:00"


# ---- scan --------------------------------------------------------------------

def iw_ssid(ssid):
    return "".join(chr(b) if 0x20 <= b < 0x7f and b != 0x5c else "\\x%02x" % b
                   for b in ssid.encode())


def scan_log(freqs):
    """The scan in `iw dev ... scan` format (what the player's parser reads)."""
    aps = B.scan() if B else None
    if aps is None:
        return None
    if isinstance(B, WPA):
        B.scan_cache = aps
    lines = []
    for a in aps:
        if not a["ssid"] or (freqs and a["freq"] not in freqs):
            continue
        lines += [f"BSS {a['bssid']}(on mlan0)",
                  f"\tfreq: {a['freq']}",
                  "\tcapability: ESS%s ShortSlotTime (0x%04x)"
                  % (" Privacy" if a["priv"] else "",
                     0x0411 if a["priv"] else 0x0401),
                  f"\tsignal: {a['signal']}.00 dBm",
                  "\tlast seen: 0 ms ago",
                  f"\tSSID: {iw_ssid(a['ssid'])}"]
        if a["wpa1"]:
            lines += ["\tWPA:\t * Version: 1",
                      "\t\t * Authentication suites: PSK"]
        if a["psk"] or a["sae"]:
            suites = ("PSK " if a["psk"] else "") + ("SAE" if a["sae"] else "")
            lines += ["\tRSN:\t * Version: 1",
                      "\t\t * Authentication suites: " + suites.strip()]
    return "".join(l + "\n" for l in lines)


# ---- connect -----------------------------------------------------------------

def parse_wpa_conf(text):
    """network={...} blocks of the player's /tmp/wpa.conf."""
    out = []
    for blk in re.findall(r"network=\{(.*?)\n\}", text, re.S):
        n = {"ssid": "", "psk": "", "priority": 0, "hidden": False}
        for line in blk.splitlines():
            k, _, v = line.strip().partition("=")
            if v.startswith('"') and v.endswith('"') and len(v) >= 2:
                v = v[1:-1]     # unescaped: everything up to the last quote
            if k == "ssid":
                n["ssid"] = v
            elif k in ("psk", "sae_password", "wep_key0") and not n["psk"]:
                n["psk"] = v
            elif k == "priority":
                n["priority"] = int(v or 0)
            elif k == "scan_ssid":
                n["hidden"] = v == "1"
        if n["ssid"]:
            out.append(n)
    return out


def reconnect(my_gen, was_manual, todo):
    if not was_manual and dev_state(True)[0]:
        log("reconnect: already connected, left as is")
        return
    for n in sorted(todo, key=lambda n: -n["priority"]):
        if my_gen != gen:
            return
        if B.connect(n):
            log("connected", n["ssid"])
            return
        log("connect failed", n["ssid"])
        if my_gen == gen:       # not superseded: drop the half-made profile
            B.drop(n)
    # none of the player's networks: back to the host's own (e.g. Wi-Fi
    # switched off and on again on the player)
    if my_gen == gen and not dev_state(True)[0]:
        log("reconnect: host networks")
        B.host_networks()


def forget_missing(todo, before):
    """Forget the networks the player listed before and no longer lists (its
    SAVED NETWORKS -> delete). The first list after the player starts only
    sets `before`: that list can be empty (the player keeps it in an encrypted
    settings file it may fail to read here) and must not wipe what the host
    remembers."""
    keep = {n["ssid"] for n in todo}
    for ssid, ref in B.ours().items():
        if ssid in before and ssid not in keep:
            log("forget", ssid)
            B.forget(ssid, ref)


# ---- commands ----------------------------------------------------------------

def handle(cmd):
    """Returns (exit code, text for /tmp/shelloutput or None)."""
    global nets, manual, events, gen, listed
    if cmd.startswith("wpa_cli -i mlan0 status"):
        return 0, None
    if cmd.startswith("ifconfig mlan0") and "HWaddr" in cmd:
        return 0, mac()
    if not B:
        log("no Wi-Fi backend:", cmd)
        if cmd.startswith("iw dev mlan0 link"):
            return 0, "0" if "freq" in cmd else ("Not Connected."
                                                  if cmd == "iw dev mlan0 link" else "")
        return (1 if cmd.startswith("iw dev mlan0 scan") else 0), ""
    if cmd.startswith("wpa_cli -i mlan0 -B -a"):
        events = True
        if dev_state(True)[0]:
            threading.Timer(1.0, emit, ("CONNECTED",)).start()
        return 0, None
    if cmd.startswith("wpa_cli -i mlan0 disconnect"):
        with lock:
            manual = True
            gen += 1
        B.disconnect()
        return 0, "OK"
    if cmd.startswith("wpa_cli -i mlan0 reconfigure"):
        try:
            with open(os.path.join(TMP, "wpa.conf")) as f:
                todo = parse_wpa_conf(f.read())
        except OSError:
            todo = []
        with lock:
            nets = todo
            was_manual = manual
            before = listed
            if was_manual:      # the network just picked counts as listed
                listed = (listed or set()) | {n["ssid"] for n in todo}
            else:
                listed = {n["ssid"] for n in todo}
        log("reconfigure:", [n["ssid"] for n in todo],
            "manual" if was_manual else "saved")
        if not was_manual and before is not None:
            threading.Thread(target=forget_missing, args=(todo, before),
                             daemon=True).start()
        return 0, "OK"
    if cmd.startswith("wpa_cli -i mlan0 reconnect"):
        with lock:
            gen += 1
            args = (gen, manual, list(nets))
            manual = False
        threading.Thread(target=reconnect, args=args, daemon=True).start()
        return 0, "OK"
    if cmd.startswith("wpa_cli -i mlan0 scan"):
        threading.Thread(target=B.rescan, daemon=True).start()
        return 0, "OK"
    if cmd.startswith("wpa_cli"):
        return 0, "OK"
    if cmd.startswith("wpa_supplicant"):
        listed = None       # the player (re)starts its Wi-Fi
        B.radio_on()
        return 0, None
    if cmd.startswith("killall wpa_supplicant"):
        # Wi-Fi OFF on the player: the host leaves its network (the radio and
        # the host's own wpa_supplicant stay)
        with lock:
            gen += 1
        B.disconnect()
        return 0, None
    if cmd.startswith("iw dev mlan0 scan"):
        freqs = {int(x) for x in re.findall(
            r"\b(\d{4})\b", cmd.split("1>")[0].partition(" freq ")[2])}
        text = scan_log(freqs)
        if text is None:
            return 1, None
        m = re.search(r"1>\s*(\S+)", cmd)
        if m:
            write_file(m.group(1), text)
        return 0, None
    if cmd.startswith("iw dev mlan0 link"):
        ap = active_ap() if dev_state()[0] else None
        if "SSID" in cmd:
            return 0, ap["ssid"] if ap else ""
        if "grep Connected" in cmd:
            return 0, ap["bssid"] if ap else ""
        if "grep freq" in cmd:
            return 0, str(ap["freq"] if ap else 0)
        if "grep signal" in cmd:
            return 0, str(ap["signal"] if ap else -100)
        return 0, (f"Connected to {ap['bssid']} (on mlan0)" if ap
                   else "Not Connected.")
    if cmd.startswith("ifconfig mlan0"):
        return 0, dev_state()[1]
    log("unknown:", cmd)
    return 0, ""


def serve(conn):
    cmd = "?"
    try:
        data = b""
        while True:
            b = conn.recv(4096)
            if not b:
                break
            data += b
        cmd = data.decode(errors="replace").strip()
        out = None
        m = re.search(r"\s>\s*(/tmp/shelloutput/\S+)\s*$", cmd)
        if m:
            out, cmd = m.group(1), cmd[:m.start()].rstrip()
        rc, text = handle(cmd)
        if out:
            write_file(out, (text or "") + "\n")
        if not cmd.startswith("wpa_cli -i mlan0 status") and "signal" not in cmd:
            log(cmd, "->", rc, repr(text) if text is not None else "")
        conn.sendall(str(rc).encode())
    except Exception as e:      # never leave the player's child hanging
        log("error:", cmd, e)
        try:
            conn.sendall(b"1")
        except OSError:
            pass
    finally:
        conn.close()


# ---- events ------------------------------------------------------------------

def emit(word):
    if not events:
        return
    try:
        write_file("/tmp/WpaCtrl/wpa_event", word + "\n")
        log("event", word)
    except OSError as e:
        log("event failed", e)


def watch():
    last = dev_state(True)[0]
    while True:
        time.sleep(1)
        now = dev_state(True)[0]
        if now != last:
            emit("CONNECTED" if now else "DISCONNECTED")
            last = now


def main():
    global IFACE, B, events
    IFACE = find_iface()
    B = pick_backend(IFACE) if IFACE else None
    os.makedirs(TMP, exist_ok=True)
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(SOCK)
    os.chmod(SOCK, 0o666)
    s.listen(16)
    events = os.path.isdir(os.path.join(TMP, "WpaCtrl"))   # broker restarted
    threading.Thread(target=watch, daemon=True).start()
    log("azwifi on", SOCK, "->", IFACE or "(no Wi-Fi interface)",
        "via", B.name if B else "(no backend)")
    while True:
        c, _ = s.accept()
        threading.Thread(target=serve, args=(c,), daemon=True).start()


if __name__ == "__main__":
    main()
