"""Triggers frame captures in the running game (started by launch_rdc.ps1) and waits until they are written.

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_trigger.py) with:
  RDC_OUT           log file (one line per event; the capture paths are listed there)
  RDC_FRAMES        optional: consecutive frames to capture (default 1)
  RDC_DELAY         optional: seconds to wait before triggering (default 0)
  RDC_INPUT_DELAYS  optional: input-triggered captures, for moments too short to time by hand (a HUD animation
                    after closing a menu). ','-separated seconds, one capture each, in turn: M5 (End) arms it, the
                    next gamepad button (XInput) or Esc, Backspace, Enter or Space press starts the delay, and the
                    capture is triggered when the delay runs out. RDC_FRAMES and RDC_DELAY are not used then.

A newly connected client is first sent the session's earlier captures, thumbnails included, and the game drops a
client that leaves them unread for a few seconds ("Timeout in send" in RenderDoc's log). So the script keeps reading
while it waits, tells the earlier captures from new ones by their ids, and in the input mode reconnects and repeats
the capture if the connection drops anyway.
"""
import ctypes
import os
import time
import traceback

import renderdoc as rd

LOG = open(os.environ["RDC_OUT"], "w", encoding="utf-8")

VK_END = 0x23
TRIGGER_KEYS = (0x1B, 0x08, 0x0D, 0x20)  # Esc, Backspace, Enter, Space
KNOWN = set()  # ids of the captures the game reported so far


class XInputGamepad(ctypes.Structure):
    _fields_ = [("buttons", ctypes.c_ushort), ("left_trigger", ctypes.c_ubyte), ("right_trigger", ctypes.c_ubyte),
                ("thumbs", ctypes.c_short * 4)]


class XInputState(ctypes.Structure):
    _fields_ = [("packet", ctypes.c_uint), ("gamepad", XInputGamepad)]


def log(text):
    LOG.write("%s %s\n" % (time.strftime("%H:%M:%S"), text))
    LOG.flush()


def connect():
    targets = []
    ident = rd.EnumerateRemoteTargets("", 0)
    while ident:
        targets.append(ident)
        ident = rd.EnumerateRemoteTargets("", ident)
    log("targets %s" % targets)
    if not targets:
        return None
    control = rd.CreateTargetControl("", targets[-1], "p3r-trigger", True)
    if control is None:
        log("cannot connect")
        return None
    log("connected: %s pid %d api %s" % (control.GetTarget(), control.GetPID(), control.GetAPI()))
    return control


def pump(control):
    """Reads what the game sent, without waiting (ReceiveMessage returns Noop when nothing is there). Captures it
    reports now are earlier ones. False once disconnected."""
    while True:
        msg = control.ReceiveMessage(None)
        if msg.type == rd.TargetControlMessageType.Noop:
            return True
        if msg.type == rd.TargetControlMessageType.Disconnected:
            log("disconnected")
            return False
        if msg.type == rd.TargetControlMessageType.NewCapture and msg.newCapture.captureId not in KNOWN:
            KNOWN.add(msg.newCapture.captureId)
            log("earlier capture id %d frame %d" % (msg.newCapture.captureId, msg.newCapture.frameNumber))


def wait_pumping(control, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if not pump(control):
            return False
        time.sleep(0.02)
    return True


def wait_for_capture(control, count):
    deadline = time.time() + 300
    got = 0
    while time.time() < deadline and got < count:
        msg = control.ReceiveMessage(None)
        if msg.type == rd.TargetControlMessageType.NewCapture:
            cap = msg.newCapture
            if cap.captureId in KNOWN:
                continue  # an earlier capture
            KNOWN.add(cap.captureId)
            log("capture id %d frame %d bytes %d path %s" % (cap.captureId, cap.frameNumber, cap.byteSize, cap.path))
            got += 1
        elif msg.type == rd.TargetControlMessageType.Disconnected:
            log("disconnected")
            return got, False
        elif msg.type == rd.TargetControlMessageType.Noop:
            time.sleep(0.1)
        else:
            log("message %s" % msg.type)
    return got, True


def input_captures(control, delays):
    user32 = ctypes.windll.user32
    xinput = ctypes.windll.xinput1_4
    ctypes.windll.winmm.timeBeginPeriod(1)

    def pads():
        state = XInputState()
        buttons = {}
        for user in range(4):
            if xinput.XInputGetState(user, ctypes.byref(state)) == 0:
                g = state.gamepad
                buttons[user] = g.buttons | (0x10000 if g.left_trigger > 128 else 0) | (
                    0x20000 if g.right_trigger > 128 else 0)
        return buttons

    def keys():
        return {vk for vk in TRIGGER_KEYS if user32.GetAsyncKeyState(vk) & 0x8000}

    def end_down():
        return bool(user32.GetAsyncKeyState(VK_END) & 0x8000)

    log("gamepads connected: %s" % sorted(pads()))
    got = 0
    index = 0
    while index < len(delays) and control is not None:
        delay = delays[index]
        log("capture %d of %d (%.2f s after the input): waiting for M5 (End)" % (index + 1, len(delays), delay))
        ok = True
        while ok and end_down():
            ok = pump(control)
            time.sleep(0.005)
        while ok and not end_down():
            ok = pump(control)
            time.sleep(0.005)
        if ok:
            log("armed; waiting for a gamepad button or Esc, Backspace, Enter or Space")
            before_pads, before_keys = pads(), keys()
            last_pump = time.perf_counter()
            while True:
                now_pads, now_keys = pads(), keys()
                new_pads = {u: b & ~before_pads.get(u, 0) for u, b in now_pads.items() if b & ~before_pads.get(u, 0)}
                new_keys = now_keys - before_keys
                if new_pads or new_keys:
                    start = time.perf_counter()
                    break
                before_pads, before_keys = now_pads, now_keys
                if time.perf_counter() - last_pump > 0.05:
                    ok = pump(control)
                    if not ok:
                        break
                    last_pump = time.perf_counter()
                time.sleep(0.001)
        if ok:
            log("input %s %s; triggering in %.2f s" % (
                {u: hex(b) for u, b in new_pads.items()}, sorted(hex(k) for k in new_keys), delay))
            while time.perf_counter() - start < delay:
                time.sleep(0.001)
            control.TriggerCapture(1)
            log("triggered %.3f s after the input" % (time.perf_counter() - start))
            count, ok = wait_for_capture(control, 1)
            got += count
            if count:
                index += 1
        if not ok:
            control.Shutdown()
            log("reconnecting; capture %d of %d again" % (index + 1, len(delays)))
            control = connect()
    if control is not None:
        control.Shutdown()
    return got


def main():
    control = connect()
    if control is None:
        return
    input_delays = [float(d) for d in os.environ.get("RDC_INPUT_DELAYS", "").split(",") if d.strip()]
    if input_delays:
        got = input_captures(control, input_delays)
    else:
        if not wait_pumping(control, float(os.environ.get("RDC_DELAY", "0")) + 0.5):
            return
        frames = int(os.environ.get("RDC_FRAMES", "1"))
        control.TriggerCapture(frames)
        log("triggered %d frame(s)" % frames)
        got, _ = wait_for_capture(control, frames)
        control.Shutdown()
    log("done, %d capture(s)" % got)


try:
    main()
except Exception:
    log("FATAL " + traceback.format_exc())
finally:
    LOG.close()
    os._exit(0)
