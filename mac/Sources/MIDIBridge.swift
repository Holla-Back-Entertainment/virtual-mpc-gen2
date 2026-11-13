import Foundation
import CoreMIDI
import Darwin

/// Bridges one CoreMIDI port pair to one raw-MIDI TCP socket of the virtual MPC (127.0.0.1:<port>):
/// a virtual destination (Mac -> MPC) and/or a virtual source (MPC -> Mac), both named `name`.
/// The External MIDI port has both; the XL's MIDI In 1-2 only a destination, Out A-D only a source.
final class MIDIBridge: @unchecked Sendable {

    // MARK: Pure stream parser

    struct ParserState {
        var runningStatus: UInt8 = 0
        var buf: [UInt8] = []
        var need = 0            // total bytes expected for the in-flight channel/common message
        var inSysex = false
        init() {}
    }

    /// Number of data bytes for a channel status, or -1 if not a channel status.
    private static func dataLen(channelStatus s: UInt8) -> Int {
        switch s & 0xF0 {
        case 0x80, 0x90, 0xA0, 0xB0, 0xE0: return 2
        case 0xC0, 0xD0: return 1
        default: return -1
        }
    }

    /// Feeds bytes into the parser; returns complete messages (each a full MIDI 1.0 message,
    /// running status expanded, SysEx as one F0..F7 message, realtime as single bytes).
    static func parse(_ bytes: [UInt8], state st: inout ParserState) -> [[UInt8]] {
        var out: [[UInt8]] = []
        for b in bytes {
            if b >= 0xF8 { out.append([b]); continue }          // realtime: never disturbs state
            if b & 0x80 != 0 {                                  // status byte
                if b == 0xF7 {
                    if st.inSysex { st.buf.append(b); out.append(st.buf) }
                    st.inSysex = false; st.buf = []; st.need = 0
                    continue
                }
                st.inSysex = false; st.buf = []; st.need = 0    // any new status aborts a partial message
                if b == 0xF0 {
                    st.inSysex = true; st.buf = [b]; st.runningStatus = 0
                } else if b >= 0xF1 {
                    st.runningStatus = 0
                    switch b {
                    case 0xF6: out.append([b])
                    case 0xF1, 0xF3: st.buf = [b]; st.need = 2
                    case 0xF2: st.buf = [b]; st.need = 3
                    default: break                              // F4/F5 undefined
                    }
                } else {
                    st.runningStatus = b
                    st.buf = [b]; st.need = 1 + dataLen(channelStatus: b)
                }
                continue
            }
            // data byte
            if st.inSysex { st.buf.append(b); continue }
            if st.buf.isEmpty {
                guard st.runningStatus != 0 else { continue }   // stray data, drop
                st.buf = [st.runningStatus]
                st.need = 1 + dataLen(channelStatus: st.runningStatus)
            }
            st.buf.append(b)
            if st.buf.count == st.need {
                out.append(st.buf); st.buf = []; st.need = 0
            }
        }
        return out
    }

    // MARK: State

    private let name: String
    private let port: UInt16
    private let toMPC: Bool, fromMPC: Bool
    private var client = MIDIClientRef()
    private var destination = MIDIEndpointRef()
    private var source = MIDIEndpointRef()
    private let writeQueue = DispatchQueue(label: "MIDIBridge.write")
    private let lock = NSLock()
    private var fd: Int32 = -1
    private var running = false
    private var thread: Thread?

    init(name: String, port: UInt16, toMPC: Bool = true, fromMPC: Bool = true) {
        self.name = name
        self.port = port
        self.toMPC = toMPC
        self.fromMPC = fromMPC
    }

    var connected: Bool {
        lock.lock(); defer { lock.unlock() }
        return fd >= 0
    }

    // MARK: Lifecycle

    func start() throws {
        lock.lock()
        if running { lock.unlock(); return }
        lock.unlock()

        var st = MIDIClientCreateWithBlock(name as CFString, &client) { _ in }
        guard st == noErr else { throw NSError(domain: NSOSStatusErrorDomain, code: Int(st)) }

        if toMPC {
            st = MIDIDestinationCreateWithBlock(client, name as CFString, &destination) { [weak self] list, _ in
                self?.handleIncoming(list)
            }
            guard st == noErr else { MIDIClientDispose(client); throw NSError(domain: NSOSStatusErrorDomain, code: Int(st)) }
        }
        if fromMPC {
            st = MIDISourceCreate(client, name as CFString, &source)
            guard st == noErr else {
                if toMPC { MIDIEndpointDispose(destination) }
                MIDIClientDispose(client)
                throw NSError(domain: NSOSStatusErrorDomain, code: Int(st))
            }
        }

        lock.lock(); running = true; lock.unlock()
        let t = Thread { [weak self] in self?.socketLoop() }
        t.name = "MIDIBridge.socket"
        thread = t
        t.start()
    }

    func stop() {
        lock.lock()
        let was = running
        running = false
        if fd >= 0 { shutdown(fd, SHUT_RDWR) }   // unblocks read; loop closes fd
        lock.unlock()
        guard was else { return }
        // Wait briefly for the socket thread to close the fd.
        for _ in 0..<50 where connected { usleep(20_000) }
        if fromMPC { MIDIEndpointDispose(source) }
        if toMPC { MIDIEndpointDispose(destination) }
        MIDIClientDispose(client)
    }

    // MARK: CoreMIDI -> TCP

    private func handleIncoming(_ list: UnsafePointer<MIDIPacketList>) {
        // Copy bytes out on the CoreMIDI thread; do the socket write on our queue.
        var data = [UInt8]()
        // Walk the real list in place: copying a MIDIPacket to the stack and stepping from the
        // copy reads the wrong memory from packet 2 on (and past 256 bytes of data).
        let dataOffset = MemoryLayout<MIDIPacket>.offset(of: \MIDIPacket.data)!
        for p in list.unsafeSequence() {
            let base = UnsafeRawPointer(p) + dataOffset
            data.append(contentsOf: UnsafeRawBufferPointer(start: base, count: Int(p.pointee.length)))
        }
        guard !data.isEmpty, connected else { return }   // drop if not connected
        writeQueue.async { [weak self] in self?.writeAll(data) }
    }

    private func writeAll(_ data: [UInt8]) {
        // Runs on writeQueue; the socket is only closed on writeQueue too, so `s` cannot be
        // closed (and its number reused) while this sends.
        lock.lock(); let s = fd; lock.unlock()
        guard s >= 0 else { return }
        var sent = 0
        data.withUnsafeBytes { raw in
            while sent < data.count {
                let n = send(s, raw.baseAddress! + sent, data.count - sent, 0)
                if n <= 0 {
                    if n < 0 && errno == EINTR { continue }
                    shutdown(s, SHUT_RDWR)   // read loop notices and reconnects
                    return
                }
                sent += n
            }
        }
    }

    // MARK: TCP -> CoreMIDI

    private func isRunning() -> Bool { lock.lock(); defer { lock.unlock() }; return running }

    private func socketLoop() {
        while isRunning() {
            let s = socket(AF_INET, SOCK_STREAM, 0)
            if s >= 0 {
                var one: Int32 = 1
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))
                setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
                var tv = timeval(tv_sec: 1, tv_usec: 0)
                setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
                var addr = sockaddr_in()
                addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
                addr.sin_family = sa_family_t(AF_INET)
                addr.sin_port = port.bigEndian
                addr.sin_addr.s_addr = inet_addr("127.0.0.1")
                let rc = withUnsafePointer(to: &addr) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                        connect(s, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                    }
                }
                if rc == 0 {
                    lock.lock(); fd = s; let keep = running; lock.unlock()
                    if !keep { shutdown(s, SHUT_RDWR) }
                    readLoop(s)
                    lock.lock(); fd = -1; lock.unlock()
                }
                writeQueue.sync { _ = close(s) }
            }
            if isRunning() { sleep(1) }
        }
    }

    private func readLoop(_ s: Int32) {
        var state = ParserState()
        var buf = [UInt8](repeating: 0, count: 4096)
        while true {
            let n = buf.withUnsafeMutableBytes { recv(s, $0.baseAddress, $0.count, 0) }
            if n == 0 { return }
            if n < 0 { if errno == EINTR { continue }; return }
            guard fromMPC else { continue }     // a Mac -> MPC port: nothing should come back
            for msg in Self.parse(Array(buf[0..<n]), state: &state) { emit(msg) }
        }
    }

    private func emit(_ msg: [UInt8]) {
        let size = msg.count + 64
        let raw = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 8)
        defer { raw.deallocate() }
        let list = raw.bindMemory(to: MIDIPacketList.self, capacity: 1)
        let p = MIDIPacketListInit(list)
        _ = msg.withUnsafeBufferPointer {
            MIDIPacketListAdd(list, size, p, 0, msg.count, $0.baseAddress!)
        }
        MIDIReceived(source, list)
    }

    // MARK: Self-check

    static func selfTest() {
        func run(_ chunks: [[UInt8]]) -> [[UInt8]] {
            var st = ParserState(); var out: [[UInt8]] = []
            for c in chunks { out += parse(c, state: &st) }
            return out
        }
        // running status, split across reads
        precondition(run([[0x90, 36, 110, 38], [100, 36, 0]]) ==
               [[0x90, 36, 110], [0x90, 38, 100], [0x90, 36, 0]])
        // two-byte-less messages (program change) + running status
        precondition(run([[0xC1, 5, 6]]) == [[0xC1, 5], [0xC1, 6]])
        // SysEx, split across reads
        precondition(run([[0xF0, 1, 2], [3, 0xF7]]) == [[0xF0, 1, 2, 3, 0xF7]])
        // realtime F8 interleaved inside sysex and inside a channel message
        precondition(run([[0xF0, 1, 0xF8, 2, 0xF7, 0x90, 60, 0xF8, 100]]) ==
               [[0xF8], [0xF0, 1, 2, 0xF7], [0xF8], [0x90, 60, 100]])
        // running status survives interleaved realtime
        precondition(run([[0xB0, 7, 100, 0xF8, 10, 20]]) == [[0xB0, 7, 100], [0xF8], [0xB0, 10, 20]])
        // system common clears running status; stray data dropped
        precondition(run([[0x90, 60, 100, 0xF6, 60, 100]]) == [[0x90, 60, 100], [0xF6]])
        // song position pointer
        precondition(run([[0xF2, 1, 2]]) == [[0xF2, 1, 2]])
        // sysex aborted by a new status byte
        precondition(run([[0xF0, 1, 2, 0x90, 60, 100]]) == [[0x90, 60, 100]])
        print("MIDIBridge.selfTest OK")
    }
}
