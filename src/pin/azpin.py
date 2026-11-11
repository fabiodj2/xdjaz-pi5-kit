#!/usr/bin/env python3
# azpin: give the player's UI thread a core of its own.
#
# EP147 paints its whole window (waveforms, browser, text) in software on one
# thread, its main (JUCE message) thread. Other threads preempting it make a
# frame miss its vsync: at 1.5 GHz the waveforms lost 215 frames a minute to
# that. With the UI thread alone on one core and at nice -10, 2 (as at
# 2.4 GHz). Measured in perf/2026-11-10-1g5-*.
#
# Every PERIOD seconds: the UI thread (the main thread of the EP147 process
# whose parent is not EP147; its forks are briefly named EP147 too) goes to
# CPU UI and gets nice NICE (again whenever the player moved it back); every
# other user thread on the system that may
# run on UI is moved to the rest of the cores. Threads the UI thread creates
# inherit its mask; the next pass moves them. A new player (sleep/wake) is
# found the same way. No forks: sched_setaffinity on thread ids.
# On SIGTERM/SIGINT the threads it moved get all cores back.
#
#   azpin.py [-c CPU] [-n NICE] [-p PERIOD]     (root)
#
# run-probe.sh starts it (AZ_PIN=0: not started).

import os
import signal
import sys
import time

UI, NICE, PERIOD = 3, -10, 2.0
args = sys.argv[1:]
while args:
    o = args.pop(0)
    if o == "-c": UI = int(args.pop(0))
    elif o == "-n": NICE = int(args.pop(0))
    elif o == "-p": PERIOD = float(args.pop(0))
    else: sys.exit("usage: azpin.py [-c CPU] [-n NICE] [-p PERIOD]")

ALL = set(range(os.cpu_count() or 1))
REST = ALL - {UI}
if UI not in ALL or not REST:
    sys.exit(f"azpin: cpu {UI} unusable with {len(ALL)} cpus")

moved = set()           # thread ids whose mask we narrowed
ui_tid = None


def comm(pid):
    try:
        with open(f"/proc/{pid}/comm") as f: return f.read().strip()
    except OSError: return ""


def ppid(pid):
    try:
        with open(f"/proc/{pid}/stat") as f: return int(f.read().rsplit(")", 1)[1].split()[1])
    except (OSError, ValueError, IndexError): return 0


def is_kernel(pid):
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f: return not f.read(1)
    except OSError: return True


def find_player():
    for d in os.listdir("/proc"):
        if d.isdigit() and comm(d) == "EP147" and comm(ppid(d)) != "EP147":
            return int(d)
    return None


def one_pass():
    global ui_tid
    p = find_player()
    # The player sets its threads' masks itself (pthread_setaffinity_np, its
    # threads end up on cpu 0,1), the UI thread included, some time after
    # start: checked on every pass, not once.
    try:
        if p and (p != ui_tid or os.sched_getaffinity(p) != {UI}):
            os.sched_setaffinity(p, {UI})
            os.setpriority(os.PRIO_PROCESS, p, NICE)  # per thread on Linux
            print(f"azpin: EP147 UI thread {p} on cpu {UI}, nice {NICE}"
                  + ("" if p != ui_tid else " (again)"), flush=True)
            ui_tid = p
    except OSError as e:
        print(f"azpin: {p}: {e}", flush=True)
    me = os.getpid()
    for d in os.listdir("/proc"):
        if not d.isdigit() or int(d) == me or is_kernel(d):
            continue
        try: tids = os.listdir(f"/proc/{d}/task")
        except OSError: continue
        for t in tids:
            tid = int(t)
            if tid == ui_tid:
                continue
            try:
                if UI in os.sched_getaffinity(tid):
                    os.sched_setaffinity(tid, os.sched_getaffinity(tid) & REST or REST)
                    moved.add(tid)
            except OSError:
                moved.discard(tid)


def restore(*_):
    for tid in moved | ({ui_tid} if ui_tid else set()):
        try: os.sched_setaffinity(tid, ALL)
        except OSError: pass
    sys.exit(0)


signal.signal(signal.SIGTERM, restore)
signal.signal(signal.SIGINT, restore)
while True:
    one_pass()
    time.sleep(PERIOD)
