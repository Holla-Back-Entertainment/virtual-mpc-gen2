#!/usr/bin/env python3
"""MIDI between the lab VM and Windows apps (a DAW), the Windows stand-in for the Mac app's CoreMIDI ports.

Windows cannot make virtual MIDI ports without a driver, so this joins the lab's raw-MIDI TCP
ports to loopback ports you create once with loopMIDI (or Windows MIDI Services' loopback
endpoints). A loopback port passes what one app sends to it on to the app that reads it, so each
direction gets its own port:

    python windows/midi_bridge.py [--model xl|live3]   # run while the VM runs; Ctrl-C stops it
    python windows/midi_bridge.py --list               # the port names to create, and what exists
    python windows/midi_bridge.py --selftest           # parser and TCP plumbing, any OS

Your DAW sends to the "to MPC" ports and receives from the "from MPC" ports. Standard library
only; WinMM through ctypes.
"""
import argparse
import os
import queue
import socket
import sys
import threading
import time

LAB = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, LAB)
import lab  # noqa: E402  (ports and models only; importing it has no side effects)

SHORT = {"xl": "acvc", "live3": "acvg", "acvc": "acvc", "acvg": "acvg"}
NAMES = {"acvc": "Virtual MPC XL", "acvg": "Virtual MPC Live III"}
# (lab port key, direction, port name after the model name). "to" = DAW -> MPC, "from" = MPC -> DAW.
# External MIDI is one port both ways on the device; a loopback port carries one direction, so it
# is two here. WinMM names hold 31 characters: "Virtual MPC Live III MIDI Out A" is exactly that.
ROUTES = {
    "acvc": [("ext_midi", "to", ""), ("ext_midi", "from", " Ext Out"),
             ("midi_in1", "to", " MIDI In 1"), ("midi_in2", "to", " MIDI In 2"),
             ("midi_outa", "from", " MIDI Out A"), ("midi_outb", "from", " MIDI Out B"),
             ("midi_outc", "from", " MIDI Out C"), ("midi_outd", "from", " MIDI Out D")],
    "acvg": [("ext_midi", "to", ""), ("ext_midi", "from", " Ext Out"),
             ("midi_in1", "to", " MIDI In 1"), ("midi_in2", "to", " MIDI In 2"),
             ("midi_outa", "from", " MIDI Out A"), ("midi_outb", "from", " MIDI Out B")],
}
MAXPNAMELEN = 32


def routes(model, offset=None):
    """[(tcp port, direction, Windows port name)] for one model."""
    ports = lab.model_ports(model, offset)
    return [(ports[k], d, (NAMES[model] + suffix)[:MAXPNAMELEN - 1]) for k, d, suffix in ROUTES[model]]


# ----------------------------------------------------------------------------- MIDI byte stream
def msg_len(status):
    """Length of a message starting with `status` (0 for SysEx, which ends with F7)."""
    if status < 0xF0:
        return 2 if 0xC0 <= status <= 0xDF else 3
    return {0xF0: 0, 0xF1: 2, 0xF2: 3, 0xF3: 2}.get(status, 1)


class Parser:
    """Raw MIDI bytes -> whole messages (bytes). Handles running status, real-time bytes inside
    other messages, and SysEx (returned F0 ... F7 in one piece, up to `max_sysex` bytes)."""

    def __init__(self, max_sysex=65536):
        self.status, self.data, self.sysex, self.max_sysex = 0, bytearray(), None, max_sysex

    def feed(self, chunk):
        out = []
        for b in chunk:
            if b >= 0xF8:                         # real time: passes through anywhere
                out.append(bytes([b]))
            elif self.sysex is not None:
                if b == 0xF7:
                    self.sysex.append(b)
                    out.append(bytes(self.sysex))
                    self.sysex = None
                elif b >= 0x80:                   # SysEx cut short by a new status: drop it
                    self.sysex = None
                    self._status(b, out)
                elif len(self.sysex) < self.max_sysex:
                    self.sysex.append(b)
            elif b >= 0x80:
                self._status(b, out)
            elif self.status:
                self.data.append(b)
                if 1 + len(self.data) == msg_len(self.status):
                    out.append(bytes([self.status]) + bytes(self.data))
                    self.data = bytearray()
                    if self.status >= 0xF0:       # system common: no running status
                        self.status = 0
        return out

    def _status(self, b, out):
        self.data = bytearray()
        if b == 0xF0:
            self.sysex, self.status = bytearray([b]), 0
        elif msg_len(b) == 1:                     # F6 tune request (F4/F5 undefined)
            out.append(bytes([b]))
            self.status = 0
        else:
            self.status = b


def pack_short(msg):
    """Message bytes -> the DWORD midiOutShortMsg takes."""
    v = 0
    for i, b in enumerate(msg[:3]):
        v |= b << (8 * i)
    return v


def unpack_short(dw):
    """MIM_DATA dwParam1 -> message bytes."""
    status = dw & 0xFF
    n = msg_len(status) or 1
    return bytes((dw >> (8 * i)) & 0xFF for i in range(n))


# ----------------------------------------------------------------------------- WinMM
class WinMM:
    """The few WinMM calls the bridge needs. Windows only."""
    CALLBACK_FUNCTION, MIM_DATA, MIM_LONGDATA, MHDR_DONE = 0x30000, 0x3C3, 0x3C4, 0x1

    def __init__(self):
        import ctypes
        from ctypes import wintypes as w
        self.c, self.dll = ctypes, ctypes.WinDLL("winmm")

        class INCAPS(ctypes.Structure):
            _fields_ = [("wMid", w.WORD), ("wPid", w.WORD), ("vDriverVersion", w.UINT),
                        ("szPname", w.WCHAR * MAXPNAMELEN), ("dwSupport", w.DWORD)]

        class OUTCAPS(ctypes.Structure):
            _fields_ = [("wMid", w.WORD), ("wPid", w.WORD), ("vDriverVersion", w.UINT),
                        ("szPname", w.WCHAR * MAXPNAMELEN), ("wTechnology", w.WORD), ("wVoices", w.WORD),
                        ("wNotes", w.WORD), ("wChannelMask", w.WORD), ("dwSupport", w.DWORD)]

        class MIDIHDR(ctypes.Structure):
            pass
        MIDIHDR._fields_ = [("lpData", ctypes.c_void_p), ("dwBufferLength", w.DWORD), ("dwBytesRecorded", w.DWORD),
                            ("dwUser", ctypes.c_size_t), ("dwFlags", w.DWORD), ("lpNext", ctypes.POINTER(MIDIHDR)),
                            ("reserved", ctypes.c_size_t), ("dwOffset", w.DWORD), ("dwReserved", ctypes.c_size_t * 8)]
        self.INCAPS, self.OUTCAPS, self.MIDIHDR = INCAPS, OUTCAPS, MIDIHDR
        self.PROC = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, w.UINT, ctypes.c_size_t, ctypes.c_size_t, ctypes.c_size_t)
        d = self.dll
        d.midiInGetDevCapsW.argtypes = [ctypes.c_size_t, ctypes.c_void_p, w.UINT]
        d.midiOutGetDevCapsW.argtypes = [ctypes.c_size_t, ctypes.c_void_p, w.UINT]
        d.midiInOpen.argtypes = [ctypes.POINTER(ctypes.c_void_p), w.UINT, ctypes.c_void_p, ctypes.c_size_t, w.DWORD]
        d.midiOutOpen.argtypes = [ctypes.POINTER(ctypes.c_void_p), w.UINT, ctypes.c_size_t, ctypes.c_size_t, w.DWORD]
        for name in ("midiInStart", "midiInStop", "midiInReset", "midiInClose", "midiOutReset", "midiOutClose"):
            getattr(d, name).argtypes = [ctypes.c_void_p]
        d.midiOutShortMsg.argtypes = [ctypes.c_void_p, w.DWORD]
        for name in ("midiInPrepareHeader", "midiInUnprepareHeader", "midiInAddBuffer",
                     "midiOutPrepareHeader", "midiOutUnprepareHeader", "midiOutLongMsg"):
            getattr(d, name).argtypes = [ctypes.c_void_p, ctypes.c_void_p, w.UINT]

    def inputs(self):
        caps = self.INCAPS()
        return [caps.szPname if self.dll.midiInGetDevCapsW(i, self.c.byref(caps), self.c.sizeof(caps)) == 0 else ""
                for i in range(self.dll.midiInGetNumDevs())]

    def outputs(self):
        caps = self.OUTCAPS()
        return [caps.szPname if self.dll.midiOutGetDevCapsW(i, self.c.byref(caps), self.c.sizeof(caps)) == 0 else ""
                for i in range(self.dll.midiOutGetNumDevs())]

    def check(self, rc, what):
        if rc:
            raise OSError(f"{what} failed (MMRESULT {rc}; is another app holding the port?)")


class WinMMOut:
    """One MIDI output port (MPC -> DAW)."""

    def __init__(self, mm, index):
        self.mm, self.h = mm, mm.c.c_void_p()
        mm.check(mm.dll.midiOutOpen(mm.c.byref(self.h), index, 0, 0, 0), "midiOutOpen")

    def send(self, msg):
        mm = self.mm
        if msg[0] != 0xF0:
            mm.check(mm.dll.midiOutShortMsg(self.h, pack_short(msg)), "midiOutShortMsg")
            return
        buf = mm.c.create_string_buffer(bytes(msg), len(msg))
        hdr = mm.MIDIHDR(lpData=mm.c.cast(buf, mm.c.c_void_p), dwBufferLength=len(msg))
        size = mm.c.sizeof(hdr)
        mm.check(mm.dll.midiOutPrepareHeader(self.h, mm.c.byref(hdr), size), "midiOutPrepareHeader")
        try:
            mm.check(mm.dll.midiOutLongMsg(self.h, mm.c.byref(hdr), size), "midiOutLongMsg")
            deadline = time.monotonic() + 2
            while not hdr.dwFlags & mm.MHDR_DONE and time.monotonic() < deadline:
                time.sleep(0.001)
        finally:
            mm.dll.midiOutUnprepareHeader(self.h, mm.c.byref(hdr), size)

    def close(self):
        self.mm.dll.midiOutReset(self.h)
        self.mm.dll.midiOutClose(self.h)


class WinMMIn:
    """One MIDI input port (DAW -> MPC). Messages go to `sink(bytes)` from a worker thread: the
    WinMM callback only queues them, as WinMM asks of callbacks."""
    NBUF, BUFSIZE = 4, 4096

    def __init__(self, mm, index, sink):
        self.mm, self.h, self.sink, self.q, self.open = mm, mm.c.c_void_p(), sink, queue.Queue(), True
        self.cb = mm.PROC(self._callback)         # keep a reference: WinMM calls it until close
        mm.check(mm.dll.midiInOpen(mm.c.byref(self.h), index, mm.c.cast(self.cb, mm.c.c_void_p), 0, mm.CALLBACK_FUNCTION), "midiInOpen")
        self.bufs, self.hdrs = [], []
        for _ in range(self.NBUF):                # SysEx arrives in these buffers
            buf = mm.c.create_string_buffer(self.BUFSIZE)
            hdr = mm.MIDIHDR(lpData=mm.c.cast(buf, mm.c.c_void_p), dwBufferLength=self.BUFSIZE)
            mm.check(mm.dll.midiInPrepareHeader(self.h, mm.c.byref(hdr), mm.c.sizeof(hdr)), "midiInPrepareHeader")
            mm.check(mm.dll.midiInAddBuffer(self.h, mm.c.byref(hdr), mm.c.sizeof(hdr)), "midiInAddBuffer")
            self.bufs.append(buf)
            self.hdrs.append(hdr)
        self.worker = threading.Thread(target=self._work, daemon=True)
        self.worker.start()
        mm.check(mm.dll.midiInStart(self.h), "midiInStart")

    def _callback(self, h, msg, inst, p1, p2):
        if msg == self.mm.MIM_DATA:
            self.q.put(("data", p1))
        elif msg == self.mm.MIM_LONGDATA:
            self.q.put(("long", p1))

    def _work(self):
        mm, sysex = self.mm, bytearray()
        addr = {mm.c.addressof(h): h for h in self.hdrs}
        while True:
            kind, p = self.q.get()
            if kind == "stop":
                return
            if kind == "data":
                self.sink(unpack_short(p))
                continue
            hdr = addr.get(p)
            if hdr is None:
                continue
            sysex += mm.c.string_at(hdr.lpData, hdr.dwBytesRecorded)
            if self.open:                         # hand the buffer back for the next SysEx
                mm.dll.midiInAddBuffer(self.h, mm.c.byref(hdr), mm.c.sizeof(hdr))
            if sysex and sysex[-1] == 0xF7:       # a long message may span buffers
                self.sink(bytes(sysex))
                sysex = bytearray()

    def close(self):
        mm, self.open = self.mm, False
        mm.dll.midiInStop(self.h)
        mm.dll.midiInReset(self.h)                # returns the buffers (MIM_LONGDATA, 0 bytes)
        time.sleep(0.05)
        self.q.put(("stop", 0))
        self.worker.join(1)
        for hdr in self.hdrs:
            mm.dll.midiInUnprepareHeader(self.h, mm.c.byref(hdr), mm.c.sizeof(hdr))
        mm.dll.midiInClose(self.h)


# ----------------------------------------------------------------------------- TCP side
class Link:
    """One lab TCP port (QEMU listens, one client at a time). Reconnects while the VM is down."""

    def __init__(self, port, name, log=print):
        self.port, self.name, self.log, self.sock, self.lock = port, name, log, None, threading.Lock()

    def connect(self):
        while self.sock is None:
            try:
                s = socket.create_connection((lab.HOST, self.port), timeout=2)
                s.settimeout(None)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self.sock = s
                self.log(f"{self.name}: connected to the VM (port {self.port})")
            except OSError:
                time.sleep(1)
        return self.sock

    def drop(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
            self.log(f"{self.name}: VM gone, waiting for it (port {self.port})")

    def send(self, msg):
        with self.lock:
            s = self.sock
            if s is None:
                return                            # VM not up: drop, as a cable with nothing on it
            try:
                s.sendall(msg)
            except OSError:
                self.drop()


def pump(link, out, stop):
    """Keep one lab port connected. Bytes from the VM (MPC -> DAW) are parsed into messages for
    `out`, the output port; with no output port they are dropped (a MIDI In sends nothing back).
    EOF means the VM went away: reconnect when it is back."""
    while not stop.is_set():
        s, parser = link.connect(), Parser()
        while not stop.is_set():
            try:
                chunk = s.recv(4096)
            except OSError:
                chunk = b""
            if not chunk:
                link.drop()
                break
            if out:
                for msg in parser.feed(chunk):
                    out.send(msg)


def bridge(model, backend=None, stop=None, log=print):
    """Open every route whose Windows port exists; returns (threads, opened ports, missing names).
    One TCP connection per lab port (QEMU serves one client at a time), shared by both directions
    of External MIDI."""
    mm = backend or WinMM()
    stop = stop or threading.Event()
    ins, outs = mm.inputs(), mm.outputs()
    threads, opened, missing, links = [], [], [], {}
    for port, direction, name in routes(model):
        if name not in (outs if direction == "from" else ins):
            missing.append(name)
            continue
        link, _ = links.setdefault(port, (Link(port, name, log), None))
        if direction == "from":                   # MPC -> DAW: we write to the loopback's output side
            dev = mm.open_out(outs.index(name)) if hasattr(mm, "open_out") else WinMMOut(mm, outs.index(name))
            links[port] = (link, dev)
        else:                                     # DAW -> MPC: we read the loopback's input side
            dev = mm.open_in(ins.index(name), link.send) if hasattr(mm, "open_in") else WinMMIn(mm, ins.index(name), link.send)
        opened.append(dev)
        log(f"{name}: {'MPC -> Windows' if direction == 'from' else 'Windows -> MPC'}, lab port {port}")
    for link, out in links.values():
        t = threading.Thread(target=pump, args=(link, out, stop), daemon=True)
        t.start()
        threads.append(t)
    return threads, opened, missing


# ----------------------------------------------------------------------------- self-test
def selftest():
    p = Parser()
    # running status, real time inside a message, system common, SysEx with a clock in it
    got = p.feed(bytes([0x90, 60, 100, 62, 90, 0xB0, 7, 0xF8, 64, 0xC0, 5, 6, 0xF2, 1, 2, 0xF0, 0x47, 0xF8, 1, 0xF7, 0xF6]))
    assert got == [bytes([0x90, 60, 100]), bytes([0x90, 62, 90]), bytes([0xF8]), bytes([0xB0, 7, 64]),
                   bytes([0xC0, 5]), bytes([0xC0, 6]), bytes([0xF2, 1, 2]), bytes([0xF8]),
                   bytes([0xF0, 0x47, 1, 0xF7]), bytes([0xF6])], got
    assert p.feed(bytes([3, 4])) == []            # after F6 there is no running status
    p2 = Parser()
    assert p2.feed(bytes([0x80, 60])) == [] and p2.feed(bytes([0])) == [bytes([0x80, 60, 0])]   # split across reads
    assert Parser().feed(bytes([0xF0, 1, 2, 0x90, 60, 1])) == [bytes([0x90, 60, 1])]          # cut SysEx dropped
    assert pack_short(bytes([0x90, 60, 100])) == 0x643C90 and unpack_short(0x643C90) == bytes([0x90, 60, 100])
    assert unpack_short(0x05C0) == bytes([0xC0, 5]) and unpack_short(0xF8) == bytes([0xF8])
    for m in lab.MODELS:
        names = [n for _, _, n in routes(m, 0)]
        assert len(set(names)) == len(names) and all(len(n) <= MAXPNAMELEN - 1 for n in names)
    assert routes("acvg", 0)[0] == (5662, "to", "Virtual MPC Live III") and routes("acvc", 40)[2][0] == 5791
    assert "Virtual MPC Live III MIDI Out A" in [n for _, _, n in routes("acvg", 0)]

    # plumbing: fake VM ports and a fake backend, both directions
    class FakeOut:
        def __init__(self):
            self.got = []

        def send(self, msg):
            self.got.append(msg)

    class FakeMM:
        def __init__(self, ins, outs):
            self.ins, self.outs, self.sinks, self.out_devs = ins, outs, {}, {}

        def inputs(self):
            return self.ins

        def outputs(self):
            return self.outs

        def open_out(self, i):
            self.out_devs[self.outs[i]] = FakeOut()
            return self.out_devs[self.outs[i]]

        def open_in(self, i, sink):
            self.sinks[self.ins[i]] = sink
            return object()

    os.environ["MPCLAB_PORT_OFFSET"] = "600"      # stay clear of a lab that might be running
    try:
        servers, by_name = {}, {}
        for port, _, name in routes("acvg"):
            if port not in servers:               # External MIDI: one port, both directions
                s = socket.socket()
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind((lab.HOST, port))
                s.listen(2)
                servers[port] = s
            by_name[name] = servers[port]
        out_a, in_1 = "Virtual MPC Live III MIDI Out A", "Virtual MPC Live III MIDI In 1"
        ext_to, ext_from = "Virtual MPC Live III", "Virtual MPC Live III Ext Out"
        mm = FakeMM(ins=[in_1, ext_to], outs=[out_a, ext_from])
        stop = threading.Event()
        threads, opened, missing = bridge("acvg", mm, stop, log=lambda *_: None)
        assert len(opened) == 4 and len(threads) == 3 and missing == ["Virtual MPC Live III MIDI In 2", "Virtual MPC Live III MIDI Out B"]
        vm_out, _ = by_name[out_a].accept()        # the VM's MIDI Out A: MPC plays a note
        vm_out.sendall(bytes([0x92, 36, 127, 36, 0]))
        vm_in, _ = by_name[in_1].accept()          # the VM's MIDI In 1: the DAW plays one
        vm_ext, _ = by_name[ext_to].accept()       # External MIDI: one connection, both ways
        vm_ext.sendall(bytes([0xF8, 0xFA]))
        by_name[ext_to].settimeout(0.3)
        try:
            by_name[ext_to].accept()
            raise AssertionError("a second connection to the External MIDI port")
        except socket.timeout:
            pass
        deadline = time.monotonic() + 3
        while len(mm.out_devs[out_a].got) < 2 or len(mm.out_devs[ext_from].got) < 2:
            assert time.monotonic() < deadline, "bridge did not deliver"
            time.sleep(0.02)
        assert mm.out_devs[out_a].got == [bytes([0x92, 36, 127]), bytes([0x92, 36, 0])]
        assert mm.out_devs[ext_from].got == [bytes([0xF8]), bytes([0xFA])]
        for name, sock, msg in ((in_1, vm_in, bytes([0x90, 48, 99])), (ext_to, vm_ext, bytes([0xB0, 1, 2]))):
            sock.settimeout(0.2)
            while True:                            # the link may still be connecting: resend until it arrives
                mm.sinks[name](msg)
                try:
                    assert sock.recv(16) == msg
                    break
                except socket.timeout:
                    assert time.monotonic() < deadline, f"{name}: not delivered"
        stop.set()
        for s in [vm_out, vm_in, vm_ext, *servers.values()]:
            s.close()
    finally:
        del os.environ["MPCLAB_PORT_OFFSET"]
    if sys.platform == "win32":                   # the real WinMM output path, on whatever port exists
        mm = WinMM()
        ins, outs = mm.inputs(), mm.outputs()
        print(f"WinMM: {len(ins)} inputs {ins}, {len(outs)} outputs {outs}")
        if outs:
            dev = WinMMOut(mm, 0)
            for msg in (bytes([0x90, 60, 1]), bytes([0x80, 60, 0]), bytes([0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7])):
                dev.send(msg)                     # a quiet note and a GM reset (SysEx: midiOutLongMsg)
            dev.close()
            print(f"WinMM: sent a note and a SysEx to {outs[0]!r}")
    print("midi_bridge selftest ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--model", default=os.environ.get("MODEL", "acvc"), help="xl (default) or live3")
    ap.add_argument("--list", action="store_true", help="show the ports to create and which exist")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    model = SHORT.get(a.model.lower())
    if not model:
        sys.exit(f"midi_bridge: --model {a.model}: use xl or live3")
    if sys.platform != "win32":
        sys.exit("midi_bridge: Windows only (macOS has the Virtual MPC app; see mac/README.md)")
    mm = WinMM()
    if a.list:
        ins, outs = mm.inputs(), mm.outputs()
        print(f"Create these ports in loopMIDI for the {lab.MODELS[model]['model']} (exact names):")
        for port, d, name in routes(model):
            ok = name in (outs if d == "from" else ins)
            print(f"  {'found  ' if ok else 'missing'}  {name:32s} {'MPC -> DAW' if d == 'from' else 'DAW -> MPC'}  (lab port {port})")
        print("\nMIDI ports Windows has now:\n  in:  " + ", ".join(i for i in ins if i) + "\n  out: " + ", ".join(o for o in outs if o))
        return
    stop = threading.Event()
    _, opened, missing = bridge(model, mm, stop)
    if missing:
        print("not found (create them in loopMIDI to use them): " + ", ".join(missing))
    if not opened:
        sys.exit("midi_bridge: none of the ports exist yet; `python windows/midi_bridge.py --list` shows the names")
    print("bridging; Ctrl-C stops")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        stop.set()
        for dev in opened:
            try:
                dev.close()
            except OSError:
                pass


if __name__ == "__main__":
    main()
