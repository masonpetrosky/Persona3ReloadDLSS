"""A/B/A screenshots and frame rates in Persona 3 Reload: DLSS, the game's TAA (F9), DLSS again (F9).

  python tools/ab_screenshots.py [--settle 3] [--fps 6] [--out DIR] [--dry-run]

For comparison images: stand still in game with the camera at rest, then run it. DLSS runs in the ini's Mode, so with
an upscaling mode the middle state is the game's TAA at native resolution. It brings the game's window to the front,
checks before every key press that the game is still in front (and stops otherwise, so no key ever lands in another
program), holds each key for a few frames (the add-on polls F9 once per frame; ReShade's screenshot key, Page Down
here, needs the game focused), waits --settle seconds after each switch for the TAA or DLSS history to settle, then
records the frame rate for --fps seconds with PresentMon (Documents\\Tools\\PresentMon, the whole run in one trace,
split by QPC time) and only then takes the screenshot (saving a 16-bit PNG takes about 2 s and would hitch the
measurement). The first and last states should match except for things that move, which tells what in the middle
one comes from the switch. If it stops after switching DLSS off, it switches DLSS back on once the game is in front.
Copies the screenshots, presentmon.csv and timeline.json to --out (default captures/shots/<date time>) and prints
each state's frame rate. --dry-run only looks for the window.
"""
import argparse
import csv
import ctypes
import ctypes.wintypes as wt
import json
import os
import shutil
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIN64 = r"C:\Program Files (x86)\Steam\steamapps\common\P3R\P3R\Binaries\Win64"
PRESENTMON = os.path.join(ROOT, "..", "..", "Tools", "PresentMon", "2.5.1", "PresentMon-2.5.1-x64.exe")
SESSION = "P3RDLSSShots"
VK_F9, VK_NEXT = 0x78, 0x22  # F9 (the add-on's toggle), Page Down (ReShade's screenshot key in ReShade.ini)
INPUT_KEYBOARD, KEYEVENTF_EXTENDEDKEY, KEYEVENTF_KEYUP = 1, 0x0001, 0x0002

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
ULONG_PTR = ctypes.c_size_t


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ULONG_PTR)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ULONG_PTR)]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", wt.DWORD), ("wParamL", wt.WORD), ("wParamH", wt.WORD)]


class _INPUTUNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", wt.DWORD), ("u", _INPUTUNION)]


assert ctypes.sizeof(INPUT) == (40 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28)
user32.SendInput.argtypes = [wt.UINT, ctypes.POINTER(INPUT), ctypes.c_int]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetForegroundWindow.restype = wt.HWND
user32.SetForegroundWindow.argtypes = [wt.HWND]
user32.BringWindowToTop.argtypes = [wt.HWND]
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]


def qpc():
    value = ctypes.c_int64()
    kernel32.QueryPerformanceCounter(ctypes.byref(value))
    return value.value


def qpc_frequency():
    value = ctypes.c_int64()
    kernel32.QueryPerformanceFrequency(ctypes.byref(value))
    return value.value


def process_name(hwnd):
    pid = wt.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    handle = kernel32.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return ""
    try:
        size = wt.DWORD(1024)
        buf = ctypes.create_unicode_buffer(size.value)
        if not kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
            return ""
        return os.path.basename(buf.value)
    finally:
        kernel32.CloseHandle(handle)


def find_game_window():
    """The largest visible top-level window of P3R.exe."""
    found = []
    proc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def visit(hwnd, _):
        if user32.IsWindowVisible(hwnd) and process_name(hwnd).lower() == "p3r.exe":
            rect = wt.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(rect))
            found.append(((rect.right - rect.left) * (rect.bottom - rect.top), hwnd))
        return True

    user32.EnumWindows(proc(visit), 0)
    return max(found)[1] if found else None


def in_front(hwnd):
    return user32.GetForegroundWindow() == hwnd


def bring_to_front(hwnd):
    if in_front(hwnd):
        return True
    # Windows lets a process take the foreground only in some cases; attaching to the foreground thread's input is one.
    foreground = user32.GetForegroundWindow()
    other = user32.GetWindowThreadProcessId(foreground, None) if foreground else 0
    me = kernel32.GetCurrentThreadId()
    if other and other != me:
        user32.AttachThreadInput(me, other, True)
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    if other and other != me:
        user32.AttachThreadInput(me, other, False)
    time.sleep(0.5)
    return in_front(hwnd)


def press(hwnd, vk, extended=False, hold=0.15):
    if not in_front(hwnd):
        raise RuntimeError("the game is no longer in front; stopped before pressing a key")
    scan = user32.MapVirtualKeyW(vk, 0)
    flags = KEYEVENTF_EXTENDEDKEY if extended else 0
    for up in (False, True):
        event = INPUT(type=INPUT_KEYBOARD)
        event.ki = KEYBDINPUT(vk, scan, flags | (KEYEVENTF_KEYUP if up else 0), 0, 0)
        if user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(INPUT)) != 1:
            raise RuntimeError("SendInput failed (%d)" % ctypes.get_last_error())
        if not up:
            time.sleep(hold)


def screenshots():
    return {f: os.path.getmtime(os.path.join(WIN64, f)) for f in os.listdir(WIN64) if f.lower().endswith(".png")}


def frame_stats(csv_path, segments):
    """Per segment: frames, average fps, median frame time, median GPU busy, from the busiest swap chain."""
    chains = {}
    with open(csv_path, newline="", encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            if r.get("Application", "").lower() == "p3r.exe":
                chains.setdefault(r["SwapChainAddress"], []).append(r)
    rows = max(chains.values(), key=len) if chains else []
    if rows and "CPUStartQPC" not in rows[0]:
        print("presentmon.csv has no CPUStartQPC column (was --qpc_time ignored?); no frame rates.")
        return []

    def num(v):
        try:
            return float(v)
        except (TypeError, ValueError):
            return None
    out = []
    for s in segments:
        inside = [r for r in rows if s["start"] <= int(r["CPUStartQPC"]) <= s["end"]]
        ft = [num(r["MsBetweenPresents"]) for r in inside]
        ft = [v for v in ft if v and v > 0]
        gpu = [v for v in (num(r["MsGPUBusy"]) for r in inside) if v is not None]
        if not ft:
            out.append((s["label"], 0, 0.0, 0.0, 0.0))
            continue
        out.append((s["label"], len(ft), 1000.0 / statistics.mean(ft), statistics.median(ft),
                    statistics.median(gpu) if gpu else float("nan")))
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--settle", type=float, default=3.0)
    parser.add_argument("--fps", type=float, default=6.0, help="seconds of frame rate per state (0: none)")
    parser.add_argument("--out", default=os.path.join(ROOT, "captures", "shots", time.strftime("%Y-%m-%d %H%M%S")))
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    hwnd = find_game_window()
    if hwnd is None:
        raise SystemExit("No Persona 3 Reload window (P3R.exe) found.")
    print("Game window found, in front: %s" % in_front(hwnd))
    if args.fps > 0 and not os.path.exists(PRESENTMON):
        raise SystemExit("PresentMon not found: " + PRESENTMON)
    if args.dry_run:
        return
    if not bring_to_front(hwnd):
        raise SystemExit("Could not bring the game to the front; nothing was pressed.")
    os.makedirs(args.out, exist_ok=True)
    log = os.path.join(WIN64, "ReShade.log")
    log_start = os.path.getsize(log)
    before = screenshots()
    timeline = {"qpc_frequency": qpc_frequency(), "segments": [], "shots": []}
    pm = None
    if args.fps > 0:
        pm = subprocess.Popen([PRESENTMON, "--process_name", "P3R.exe", "--output_file",
                               os.path.join(args.out, "presentmon.csv"), "--qpc_time", "--no_console_stats",
                               "--session_name", SESSION, "--stop_existing_session"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    toggles = 0
    try:
        time.sleep(args.settle)  # the game may have paused while it was behind another window; PresentMon starts
        for i, label in enumerate(["1 DLSS", "2 game TAA", "3 DLSS again"]):
            if i > 0:
                press(hwnd, VK_F9)
                toggles += 1
                time.sleep(args.settle)
            if args.fps > 0:
                start = qpc()
                time.sleep(args.fps)
                timeline["segments"].append({"label": label, "start": start, "end": qpc()})
            press(hwnd, VK_NEXT, extended=True)
            timeline["shots"].append({"label": label, "qpc": qpc()})
            print("shot", label, flush=True)
            time.sleep(2.5)
    except RuntimeError as error:
        print("Stopped: %s." % error)
    finally:
        if toggles % 2:
            # DLSS was left off: switch it back on once the game is in front again (up to 10 minutes).
            for _ in range(1200):
                if in_front(hwnd):
                    press(hwnd, VK_F9)
                    print("DLSS switched back on.")
                    break
                time.sleep(0.5)
            else:
                print("WARNING: DLSS is still off; press F9 in the game.")
        if pm is not None:
            subprocess.run([PRESENTMON, "--terminate_existing_session", "--session_name", SESSION],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                pm.wait(timeout=15)
            except subprocess.TimeoutExpired:
                pm.kill()
        with open(os.path.join(args.out, "timeline.json"), "w") as f:
            json.dump(timeline, f, indent=1)

    time.sleep(1.0)
    new = sorted((t, f) for f, t in screenshots().items() if f not in before or t > before[f])
    print("New screenshots (copied to %s):" % args.out)
    for _, f in new:
        shutil.copy2(os.path.join(WIN64, f), args.out)
        print("  " + f)
    with open(log, "rb") as handle:
        handle.seek(log_start)
        for line in handle.read().decode("utf-8", "replace").splitlines():
            if "[P3RDLSS]" in line and ("DLSS on" in line or "DLSS off" in line or "Created the DLSS" in line):
                print("  log: " + line.strip()[-120:])
    if pm is not None and os.path.exists(os.path.join(args.out, "presentmon.csv")):
        print("%-14s %7s %8s %10s %10s" % ("state", "frames", "fps", "frame ms", "GPU busy"))
        for label, n, fps, ft, gpu in frame_stats(os.path.join(args.out, "presentmon.csv"), timeline["segments"]):
            print("%-14s %7d %8.1f %10.2f %10.2f" % (label, n, fps, ft, gpu))
    if len(new) != 3:
        sys.exit("Expected 3 new screenshots, found %d." % len(new))


main()
