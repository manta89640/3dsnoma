#!/usr/bin/env python3
"""The kept flow, end to end on the host, by touch alone: the opening skipped with a tap, the room's
bottom-screen buttons to the options (shown whole on the bottom screen) and back by their Home tab,
to the Sound Player and back, to Wuhu Island and back through its menu and question, then the
app closed from the room as the 3DS's HOME Menu closes it. The run is scripted (NOMA_INPUT, on the
game's clock) and its log is checked for each step's line, in order.

    tools/flow_check.py [--host build-host/noma-host] [--data runs/data] [--timeout 900] [--keep LOG]
                        [--hooks] [--env K=V ...]

Exit status 0 when every step was seen (and the layout draw check, when it ran, found nothing). The
touches are in bottom-screen pixels (0..319, 0..239); the places are those of docs/two_screens.md's
regions (the room's panel and the native scenes in BOTTOM, the room's other screens in FULL).

The run draws nothing (NOMA_NO_RENDER=1). --hooks installs the host's renderer without letting it
rasterise a frame (NOMA_DUMP_FRAMES=100000 NOMA_DUMP_FROM=100000000), so that what hangs on its
hooks runs, at the game's speed: the GX decoder's checks. --env K=V (repeatable) sets an environment
variable for the run, after the script's own. A last step checks the layout draw check
(NOMA_CHECK_LYT_DRAW, engine/lyt/draw_check.cpp) when it is on: it fails when the log shows a
difference, or no summary of the check at all, e.g.

    tools/flow_check.py --hooks --env NOMA_CHECK_LYT_DRAW=1

(NOMA_CHECK_LYT_DRAW=1 or pixels implies --hooks: the check sees the GX draws through the renderer.
The pixel check compares only the frames the renderer draws, none with --hooks alone: --env
NOMA_DUMP_FROM=0 --env NOMA_RENDER_ALL=1 draws them all, several times slower than the game.)
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
    (27, 120, 217, "the room's サウンド"),
    (36, 20, 225, "the Sound Player's Home tab"),
    (44, 200, 217, "the room's 出発"),
    (80, 20, 223, "the island's Home tab: its menu"),
    (85, 160, 51, "部屋にもどる"),
    (90, 233, 218, "はい"),
]
# then the app closed from the room as the 3DS's HOME Menu closes it (the Wii's power button: the
# room's way back to the Wii Menu isn't on the 3DS)
CLOSE_AT = 110

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
    ("the app closed from the room", r"stm: the power button"),
    ("the program quit", r"quit: "),
]

# NOMA_CHECK_LYT_DRAW's reports of a difference: the state check's ("<field>: layout ...: GX ...,
# native ...", the first 40 and then each field's first; markers out of order: "order: GX ..., native
# ..."), the pixel check's layout draws that differ (the first 400) and what came out of order (the
# first 40); and the two checks' summaries every 10 s, with what they counted
LYT_MISMATCH = re.compile(r"lyt draw check: [a-z0-9 ]+: (layout [0-9A-F]{8} .*|GX .*), native |"
                          r"lyt pixels: (layout [0-9A-F]{8} |order: )")
LYT_STATE_SUMMARY = re.compile(r"lyt draw check: \d+ layout draws, .*: (\d+) mismatches")
LYT_PIXEL_SUMMARY = re.compile(r"lyt pixels: \d+ layout draws: \d+ compared, (\d+) differ, .*, (\d+) out of order")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.path.join(root, "build-host", "noma-host"))
    ap.add_argument("--data", default=os.path.join(root, "runs", "data"))
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--keep", help="write the run's log here")
    ap.add_argument("--hooks", action="store_true",
                    help="install the renderer (its hooks run) but rasterise nothing, instead of NOMA_NO_RENDER=1")
    ap.add_argument("--env", action="append", default=[], metavar="K=V",
                    help="an environment variable for the run (repeatable)")
    a = ap.parse_args()

    extra = {}
    for kv in a.env:
        k, eq, v = kv.partition("=")
        if not k or not eq:
            ap.error(f"--env {kv}: not K=V")
        extra[k] = v
    script = ",".join(f"{t}:T({x};{y})" for t, x, y, _ in TOUCHES) + f",{CLOSE_AT}:OFF"
    env = dict(os.environ, NOMA_INPUT=script)
    for k in ("NOMA_NO_RENDER", "NOMA_TEST_CARD", "NOMA_DUMP_FRAMES", "NOMA_DUMP_FROM", "NOMA_DUMP_3DS",
              "NOMA_RENDER_ALL"):
        env.pop(k, None)
    check = extra.get("NOMA_CHECK_LYT_DRAW", env.get("NOMA_CHECK_LYT_DRAW"))
    checked = check in ("1", "pixels")
    if checked and not a.hooks:
        print(f"note     NOMA_CHECK_LYT_DRAW={check} sees the GX draws through the renderer: --hooks")
        a.hooks = True
    if a.hooks:
        env.update(NOMA_DUMP_FRAMES="100000", NOMA_DUMP_FROM="100000000")
    else:
        env["NOMA_NO_RENDER"] = "1"
    env.update(extra)
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
    # the layout draw check, when it ran: no difference reported, none counted in its last summaries,
    # and a summary at all when it was asked for (it writes one at its first layout draw after 10 s)
    mismatches = [l for l in lines if LYT_MISMATCH.search(l)]
    summaries = []
    for rx in (LYT_STATE_SUMMARY, LYT_PIXEL_SUMMARY):
        found = [(l, rx.search(l)) for l in lines if rx.search(l)]
        if found:
            summaries.append(found[-1])
    counted = any(int(n) for _, m in summaries for n in m.groups())
    if mismatches or counted:
        print(f"MISMATCH lyt draw check: {len(mismatches)} lines of differences"
              + (f", the first: {mismatches[0]}" if mismatches else ""))
        for l, _ in summaries:
            print(f"         the last summary: {l}")
        ok = False
    elif checked and not summaries:
        print(f"MISSING  lyt draw check: NOMA_CHECK_LYT_DRAW={check} was asked for, the log has no summary of it")
        ok = False
    else:
        for l, _ in summaries:
            print(f"ok       lyt draw check: {l.split('] ', 1)[-1]}")
    print("flow: " + ("all steps seen" if ok else "FAILED") + f" (log: {log_path})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
