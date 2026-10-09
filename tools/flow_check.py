#!/usr/bin/env python3
"""The kept flow, end to end on the host, by touch alone: the opening skipped with a tap, the room's
bottom-screen buttons to the options (shown whole on the bottom screen) and back by their Home tab,
to the Sound Player and back, to Wuhu Island and back through its menu and question, then the
room's way back and its question's はい, which ends the app. The run is scripted (NOMA_INPUT, on the
game's clock) and its log is checked for each step's line, in order.

    tools/flow_check.py [--host build-host/noma-host] [--data runs/data] [--timeout 900] [--keep LOG]

Exit status 0 when every step was seen. The touches are in bottom-screen pixels (0..319, 0..239);
the places are those of docs/two_screens.md's regions (the room's panel and the native scenes in
BOTTOM, the room's other screens in FULL).
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

# (game seconds, touch x, touch y, what it is)
TOUCHES = [
    (4, 160, 120, "a tap during the opening: skipped"),
    (12, 224, 119, "the room's 設定"),
    (19, 18, 225, "the options' Home tab"),
    (27, 173, 217, "the room's サウンド"),
    (36, 20, 225, "the Sound Player's Home tab"),
    (44, 253, 217, "the room's 出発"),
    (80, 20, 223, "the island's Home tab: its menu"),
    (85, 160, 51, "部屋にもどる"),
    (90, 233, 218, "はい"),
    (110, 20, 217, "the room's way back: its question"),
    (115, 233, 218, "はい: the app ends"),
]

# the log's lines, in this order
STEPS = [
    ("the opening skipped", r"title: the opening skipped"),
    ("the room's panel", r"room: bottom screen panel"),
    ("to the options", r"room: to scene 7"),
    ("the options on the bottom screen", r"room: away to scene 7"),
    ("the room again", r"room: bottom screen panel"),
    ("to the Sound Player", r"room: to scene 9"),
    ("the Sound Player", r"soundplayer: open"),
    ("the Sound Player left", r"soundplayer: closed"),
    ("the room again", r"room: bottom screen panel"),
    ("to the island", r"room: to scene 10"),
    ("the island", r"wuhu: open"),
    ("the island left", r"wuhu: closed"),
    ("the room again", r"room: bottom screen panel"),
    ("the app ended from the room", r"room: quit"),
    ("the program quit", r"quit: "),
]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.path.join(root, "build-host", "noma-host"))
    ap.add_argument("--data", default=os.path.join(root, "runs", "data"))
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--keep", help="write the run's log here")
    a = ap.parse_args()

    script = ",".join(f"{t}:T({x};{y})" for t, x, y, _ in TOUCHES)
    env = dict(os.environ, NOMA_NO_RENDER="1", NOMA_INPUT=script)
    for k in ("NOMA_TEST_CARD", "NOMA_DUMP_FRAMES", "NOMA_DUMP_FROM", "NOMA_DUMP_3DS"):
        env.pop(k, None)
    with tempfile.NamedTemporaryFile("w+", suffix=".log", delete=False) as f:
        log_path = a.keep or f.name
    with open(log_path, "w") as out:
        try:
            subprocess.run([a.host, "--data", a.data], env=env, stdout=out, stderr=subprocess.STDOUT,
                           timeout=a.timeout)
        except subprocess.TimeoutExpired:
            print(f"the run took longer than {a.timeout} s")
    lines = open(log_path, errors="replace").read().splitlines()

    at, ok = 0, True
    for name, pattern in STEPS:
        rx = re.compile(pattern)
        while at < len(lines) and not rx.search(lines[at]):
            at += 1
        if at == len(lines):
            print(f"MISSING  {name} ({pattern})")
            ok = False
            break
        stamp = lines[at].split("]")[0].lstrip("[ ")
        print(f"ok       {name}  [{stamp}]")
        at += 1
    fatal = [l for l in lines if "fatal" in l.lower()]
    if fatal:
        print("FATAL    " + fatal[0])
        ok = False
    print("flow: " + ("all steps seen" if ok else "FAILED") + f" (log: {log_path})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
