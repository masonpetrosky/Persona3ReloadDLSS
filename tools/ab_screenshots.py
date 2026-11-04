"""Takes A/B/A ReShade screenshots in Persona 3 Reload: DLSS, the game's TAA (F9), DLSS again (F9).

  python tools/ab_screenshots.py [--settle 3] [--dry-run]

For comparison images: stand still in game with the camera at rest, then run it. It brings the game's window to the
front, checks before every key press that the game is still in front (and stops otherwise, so no key ever lands in
another program), holds each key for a few frames (the add-on polls F9 once per frame; ReShade's screenshot key,
Page Down here, needs the game focused), and waits --settle seconds after each switch for the TAA or DLSS history to
settle. The first and last shots should match except for things that move, which tells what in the middle shot comes
from the switch. Prints the new screenshots and the add-on's "DLSS on/off" log lines. --dry-run only finds the window.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

WIN64 = r"C:\Program Files (x86)\Steam\steamapps\common\P3R\P3R\Binaries\Win64"
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
        raise SystemExit("The game is no longer in front; stopped before pressing a key.")
    scan = user32.MapVirtualKeyW(vk, 0)
    flags = KEYEVENTF_EXTENDEDKEY if extended else 0
    for up in (False, True):
        event = INPUT(type=INPUT_KEYBOARD)
        event.ki = KEYBDINPUT(vk, scan, flags | (KEYEVENTF_KEYUP if up else 0), 0, 0)
        if user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(INPUT)) != 1:
            raise SystemExit("SendInput failed (%d)." % ctypes.get_last_error())
        if not up:
            time.sleep(hold)


def screenshots():
    return {f: os.path.getmtime(os.path.join(WIN64, f)) for f in os.listdir(WIN64) if f.lower().endswith(".png")}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--settle", type=float, default=3.0)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    hwnd = find_game_window()
    if hwnd is None:
        raise SystemExit("No Persona 3 Reload window (P3R.exe) found.")
    print("Game window found, in front: %s" % in_front(hwnd))
    if args.dry_run:
        return
    if not bring_to_front(hwnd):
        raise SystemExit("Could not bring the game to the front; nothing was pressed.")
    log = os.path.join(WIN64, "ReShade.log")
    log_start = os.path.getsize(log)
    before = screenshots()
    time.sleep(args.settle)  # the game may have paused while it was behind another window

    labels = ["1 DLSS", "2 game TAA", "3 DLSS again"]
    press(hwnd, VK_NEXT, extended=True)
    print("shot", labels[0])
    time.sleep(1.5)
    for label in labels[1:]:
        press(hwnd, VK_F9)
        time.sleep(args.settle)
        press(hwnd, VK_NEXT, extended=True)
        print("shot", label)
        time.sleep(1.5)
    time.sleep(1.0)

    new = sorted((t, f) for f, t in screenshots().items() if f not in before or t > before[f])
    print("New screenshots:")
    for _, f in new:
        print("  " + f)
    with open(log, "rb") as handle:
        handle.seek(log_start)
        for line in handle.read().decode("utf-8", "replace").splitlines():
            if "DLSS on" in line or "DLSS off" in line or "Screenshot" in line or "screenshot" in line:
                print("  log: " + line.strip()[-110:])
    if len(new) != 3:
        sys.exit("Expected 3 new screenshots, found %d." % len(new))


main()
