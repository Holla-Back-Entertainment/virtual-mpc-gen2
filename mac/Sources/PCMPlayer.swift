// PCMPlayer: the virtual MPC's audio on the Mac.
//
// The bridge in the VM streams S16LE interleaved 44.1 kHz to QEMU's WebSocket chardev
// (ws://127.0.0.1:<port>, binary frames, ~256 frames per message). Only one client can hold
// that socket. PlayerCore is a native port of the AudioWorklet player in panel/gen2.html,
// extended to N channels: a ring buffer that starts at 1024 frames queued, resamples by up to
// ±0.3% to hold that level (guest and sound-card clocks drift), fades the last sample out on an
// underrun and rebuffers to a target 256 frames larger (max 4096), creeps back after 10 s
// without one, and cross-fades into the next sound after any gap or skip.
//
// Two modes, picked from the device list (re-checked on every kAudioHardwarePropertyDevices change):
//  - stereo (no "Virtual MPC XL" driver, or the Live III): POST /audio width 2; MPC Main 1/2 plays
//    on the chosen speaker device. Same as before the driver existed.
//  - driver (XL + driver present): POST /audio width 8 / in_width 4 / in "mic". MPC Out 1-8 go
//    to the driver's output 9-16 (Live reads them on its input 1-8). The driver's input 9-16
//    ("Computer", what Live plays to it): 1/2 -> the speaker monitor, 3/4 + 5/6 -> MPC Input
//    1,2 + 3,4, sent back on the socket. The speaker monitor is Main 1/2 ("Hear MPC directly")
//    + Computer 1/2, through a second 2-ch PlayerCore so the speaker's clock can drift freely.
//    Whatever widths the VM honours (its POST reply) are used, so an older VM gives stereo in 8ch.
//
// Units are CoreAudio AUHAL (kAudioUnitSubType_HALOutput). Render threads only try-lock the
// stream core; the driver's IO thread does short plain locks on the monitor core and TX ring.

import AudioToolbox
import CoreAudio
import Foundation
import os

// MARK: - Jitter buffer (N channels, interleaved)

final class PlayerCore: @unchecked Sendable {
    static let sampleRate = 44100.0
    let size = 65536                                   // frames
    let ch: Int
    let buf: UnsafeMutablePointer<Float>               // interleaved, ch per frame
    private let last: UnsafeMutablePointer<Float>, from: UnsafeMutablePointer<Float>
    private let lock: os_unfair_lock_t = {             // stable address (a stored os_unfair_lock is not)
        let l = os_unfair_lock_t.allocate(capacity: 1); l.initialize(to: os_unfair_lock()); return l
    }()

    // all below guarded by `lock`
    private var w = 0, r = 0
    private var frac = 0.0
    private(set) var target = 1024
    private var ema = 1024.0
    private var primed = false
    private var calm = 0
    private var xf = 128
    private(set) var under = 0, skip = 0

    // render-thread only
    var volume: Float = 1          // written from any thread; a torn float read is impossible on arm64/x86_64
    private var gain: Float = 1    // smoothed volume

    init(channels: Int) {
        ch = max(1, channels)
        buf = .allocate(capacity: size * ch); buf.initialize(repeating: 0, count: size * ch)
        last = .allocate(capacity: ch); last.initialize(repeating: 0, count: ch)
        from = .allocate(capacity: ch); from.initialize(repeating: 0, count: ch)
    }
    deinit { buf.deallocate(); last.deallocate(); from.deallocate(); lock.deallocate() }

    func snapshot() -> (under: Int, skip: Int, targetFrames: Int) {
        os_unfair_lock_lock(lock); defer { os_unfair_lock_unlock(lock) }
        return (under, skip, target)
    }

    /// Drop everything queued (new connection): rebuffer from scratch, keep stats.
    func reset() {
        os_unfair_lock_lock(lock); defer { os_unfair_lock_unlock(lock) }
        r = w; frac = 0; primed = false
    }

    /// Socket thread: append whole S16LE interleaved frames of `ch` channels.
    func push(_ p: UnsafeRawBufferPointer) {
        let n = p.count / (2 * ch)
        guard n > 0 else { return }
        os_unfair_lock_lock(lock); defer { os_unfair_lock_unlock(lock) }
        for i in 0..<n {
            let k = ((w + i) % size) * ch
            for c in 0..<ch {
                buf[k + c] = Float(Int16(littleEndian: p.loadUnaligned(fromByteOffset: (i * ch + c) * 2, as: Int16.self))) / 32768
            }
        }
        advance(n)
    }

    /// Any thread: append n interleaved float frames.
    func pushFloat(_ p: UnsafePointer<Float>, _ n: Int) {
        guard n > 0 else { return }
        os_unfair_lock_lock(lock); defer { os_unfair_lock_unlock(lock) }
        for i in 0..<n {
            let k = ((w + i) % size) * ch
            for c in 0..<ch { buf[k + c] = p[i * ch + c] }
        }
        advance(n)
    }

    private func advance(_ n: Int) {
        w += n
        if w - r > size / 2 { r = w - target; frac = 0; cut(count: &skip) }
    }

    private func cut() { xf = 0; from.update(from: last, count: ch) }
    private func cut(count: inout Int) { cut(); count += 1 }

    private func hold(_ out: UnsafeMutablePointer<Float>, from i0: Int, to n: Int) {
        for i in i0..<max(i0, n) { for c in 0..<ch { last[c] *= 0.995; out[i * ch + c] = last[c] } }
    }

    /// Render thread: n interleaved frames into out. Never blocks.
    func render(_ out: UnsafeMutablePointer<Float>, _ n: Int) {
        guard os_unfair_lock_trylock(lock) else {
            // writer is mid-copy: hold the last sample rather than wait (rare, ~µs window)
            for i in 0..<n { for c in 0..<ch { out[i * ch + c] = last[c] * gain } }
            return
        }
        renderLocked(out, n)
        os_unfair_lock_unlock(lock)
        // volume, smoothed per buffer to avoid zipper noise
        let v = max(0, min(1, volume))
        let g0 = gain, step = (v - g0) / Float(max(n, 1))
        for i in 0..<n { let g = g0 + step * Float(i + 1); for c in 0..<ch { out[i * ch + c] *= g } }
        gain = v
    }

    private func renderLocked(_ out: UnsafeMutablePointer<Float>, _ n: Int) {
        let fill = w - r
        if !primed {
            if fill < target { hold(out, from: 0, to: n); return }
            primed = true; ema = Double(fill); cut()
        }
        if fill > 4 * target { r = w - target; ema = Double(target); cut(count: &skip) }
        // the worklet runs on 128-frame quanta: scale its per-quantum constants to n frames
        let alpha = 1 - pow(0.99, Double(n) / 128)
        ema += alpha * (Double(w - r) - ema)
        let ratio = 1 + max(-0.003, min(0.003, 0.003 * (ema - Double(target)) / Double(target)))
        calm += n
        if calm > Int(PlayerCore.sampleRate) * 10 && target > 1024 { target = max(1024, target - 32); calm = 0 }
        for i in 0..<n {
            if w - r < 2 {                     // underrun: fade out, then rebuffer to a larger target
                hold(out, from: i, to: n)
                primed = false; calm = 0; target = min(4096, target + 256)
                cut(count: &under)
                return
            }
            let a = (r % size) * ch, b = ((r + 1) % size) * ch, t = Float(frac)
            var g: Float = -1
            if xf < 128 { xf += 1; g = Float(xf) / 128 }
            for c in 0..<ch {
                var v = buf[a + c] + (buf[b + c] - buf[a + c]) * t
                if g >= 0 { v = from[c] + (v - from[c]) * g }
                out[i * ch + c] = v; last[c] = v
            }
            frac += ratio
            let s = Int(frac)
            r += s; frac -= Double(s)
        }
    }
}

// MARK: - Mac -> VM ring (S16 interleaved), filled by the driver's IO thread, drained by the socket queue

final class TxRing: @unchecked Sendable {
    let ch: Int, size = 16384
    private let buf: UnsafeMutablePointer<Int16>
    private var w = 0, r = 0
    private let lock: os_unfair_lock_t = {
        let l = os_unfair_lock_t.allocate(capacity: 1); l.initialize(to: os_unfair_lock()); return l
    }()

    init(channels: Int) { ch = channels; buf = .allocate(capacity: size * ch); buf.initialize(repeating: 0, count: size * ch) }
    deinit { buf.deallocate(); lock.deallocate() }

    /// n frames from non-interleaved buffers abl[first ..< first+ch]. Oldest frames drop on overflow.
    func push(_ abl: UnsafeMutableAudioBufferListPointer, first: Int, _ n: Int) {
        os_unfair_lock_lock(lock); defer { os_unfair_lock_unlock(lock) }
        for c in 0..<ch {
            guard first + c < abl.count, let p = abl[first + c].mData?.assumingMemoryBound(to: Float.self) else { continue }
            for i in 0..<n { buf[((w + i) % size) * ch + c] = Int16(max(-32768, min(32767, (p[i] * 32768).rounded()))) }
        }
        w += n
        if w - r > size { r = w - size }
    }

    func drain() -> Data {
        os_unfair_lock_lock(lock); defer { os_unfair_lock_unlock(lock) }
        var d = Data(capacity: (w - r) * ch * 2)
        while r < w {
            let k = (r % size) * ch
            for c in 0..<ch { var v = buf[k + c].littleEndian; withUnsafeBytes(of: &v) { d.append(contentsOf: $0) } }
            r += 1
        }
        return d
    }
}

// MARK: - Per-mode IO state (refCon of the render callbacks)

private final class Engine: @unchecked Sendable {
    static let maxFrames = 8192
    let width: Int                     // VM -> Mac channels
    let stream: PlayerCore
    let monitor: PlayerCore?           // driver mode: Main 1/2 + Computer 1/2 -> speaker
    let tx: TxRing?                    // driver mode: Computer 3-6 -> VM (in_width channels)
    var hearDirect = true              // written from any thread
    var driverUnit: AudioUnit?
    var inFrames = 0                   // driver IO thread only
    let scratch: UnsafeMutablePointer<Float>, monScratch: UnsafeMutablePointer<Float>, mix: UnsafeMutablePointer<Float>
    let inABL: UnsafeMutableAudioBufferListPointer   // the driver's 16 inputs

    init(width: Int, sendWidth: Int, driver: Bool) {
        self.width = width
        stream = PlayerCore(channels: width)
        monitor = driver ? PlayerCore(channels: 2) : nil
        tx = driver && sendWidth > 0 ? TxRing(channels: sendWidth) : nil
        scratch = .allocate(capacity: Self.maxFrames * width)
        monScratch = .allocate(capacity: Self.maxFrames * 2)
        mix = .allocate(capacity: Self.maxFrames * 2)
        inABL = AudioBufferList.allocate(maximumBuffers: 16)
        for k in 0..<16 {
            let p = UnsafeMutablePointer<Float>.allocate(capacity: Self.maxFrames); p.initialize(repeating: 0, count: Self.maxFrames)
            inABL[k] = AudioBuffer(mNumberChannels: 1, mDataByteSize: UInt32(Self.maxFrames * 4), mData: p)
        }
    }
    deinit {
        scratch.deallocate(); monScratch.deallocate(); mix.deallocate()
        for b in inABL { b.mData?.deallocate() }
        free(inABL.unsafeMutablePointer)
    }

    /// Driver output: MPC Out 1..width -> device channels 9.., zeros elsewhere; feed the monitor.
    func renderDriver(_ abl: UnsafeMutableAudioBufferListPointer, _ n: Int) {
        stream.render(scratch, n)
        for k in 0..<abl.count {
            guard let p = abl[k].mData?.assumingMemoryBound(to: Float.self) else { continue }
            let c = k - 8
            if c >= 0 && c < width { for i in 0..<n { p[i] = scratch[i * width + c] } }
            else { p.update(repeating: 0, count: n) }
        }
        guard let monitor else { return }
        let hear: Float = hearDirect ? 1 : 0, rc = width > 1 ? 1 : 0
        let have = inFrames >= n
        let cl = have ? inABL[8].mData?.assumingMemoryBound(to: Float.self) : nil
        let cr = have ? inABL[9].mData?.assumingMemoryBound(to: Float.self) : nil
        for i in 0..<n {
            mix[2 * i] = hear * scratch[i * width] + (cl?[i] ?? 0)
            mix[2 * i + 1] = hear * scratch[i * width + rc] + (cr?[i] ?? 0)
        }
        inFrames = 0
        monitor.pushFloat(mix, n)
    }

    /// Speaker output: stereo mode plays the stream's 1/2, driver mode plays the monitor.
    func renderSpeaker(_ l: UnsafeMutablePointer<Float>, _ r: UnsafeMutablePointer<Float>, _ n: Int) {
        let core = monitor ?? stream, src = monitor == nil ? scratch : monScratch   // own scratch per IO thread
        core.render(src, n)
        let w = core.ch, rc = w > 1 ? 1 : 0
        for i in 0..<n { l[i] = src[i * w]; r[i] = src[i * w + rc] }
    }
}

// MARK: - CoreAudio callbacks (C functions, no captures)

private func speakerRender(_ refCon: UnsafeMutableRawPointer, _ flags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
                           _ ts: UnsafePointer<AudioTimeStamp>, _ bus: UInt32, _ frames: UInt32,
                           _ ioData: UnsafeMutablePointer<AudioBufferList>?) -> OSStatus {
    let e = Unmanaged<Engine>.fromOpaque(refCon).takeUnretainedValue()
    guard let abl = UnsafeMutableAudioBufferListPointer(ioData), abl.count >= 1,
          let l = abl[0].mData?.assumingMemoryBound(to: Float.self) else { return noErr }
    let r = abl.count > 1 ? (abl[1].mData?.assumingMemoryBound(to: Float.self) ?? l) : l
    e.renderSpeaker(l, r, min(Int(frames), Engine.maxFrames))
    return noErr
}

private func driverRender(_ refCon: UnsafeMutableRawPointer, _ flags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
                          _ ts: UnsafePointer<AudioTimeStamp>, _ bus: UInt32, _ frames: UInt32,
                          _ ioData: UnsafeMutablePointer<AudioBufferList>?) -> OSStatus {
    let e = Unmanaged<Engine>.fromOpaque(refCon).takeUnretainedValue()
    guard let abl = UnsafeMutableAudioBufferListPointer(ioData) else { return noErr }
    e.renderDriver(abl, min(Int(frames), Engine.maxFrames))
    return noErr
}

private func driverInput(_ refCon: UnsafeMutableRawPointer, _ flags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
                         _ ts: UnsafePointer<AudioTimeStamp>, _ bus: UInt32, _ frames: UInt32,
                         _ ioData: UnsafeMutablePointer<AudioBufferList>?) -> OSStatus {
    let e = Unmanaged<Engine>.fromOpaque(refCon).takeUnretainedValue()
    guard let u = e.driverUnit else { return noErr }
    let n = min(Int(frames), Engine.maxFrames)
    for k in 0..<e.inABL.count { e.inABL[k].mDataByteSize = UInt32(n * 4) }
    guard AudioUnitRender(u, flags, ts, 1, UInt32(n), e.inABL.unsafeMutablePointer) == noErr else { e.inFrames = 0; return noErr }
    e.inFrames = n
    e.tx?.push(e.inABL, first: 10, n)                  // Computer 3-6 = device inputs 11-14
    return noErr
}

// MARK: - Public player

final class PCMPlayer: @unchecked Sendable {
    static let driverUID = "VirtualMPCXL_UID"
    private let port: UInt16, panel: Int, allowDriver: Bool
    private let q = DispatchQueue(label: "mpclab.pcmplayer")   // guards everything below
    private var running = false
    private var engine: Engine?
    private var speaker: AudioUnit?, driverUnit: AudioUnit?
    private var driverID: AudioDeviceID?                       // non-nil = driver mode
    private var modeLine = ""                                  // last logged mode
    private var _deviceUID: String?
    private var _hearDirect = true
    private var _volume: Float = 1
    private var generation = 0                                 // bumps on every (re)connect / stop
    private var session: URLSession?
    private var task: URLSessionWebSocketTask?
    private var rest = Data()                                  // partial frame carried between messages
    private var listener: AudioObjectPropertyListenerBlock?
    private let log = Logger(subsystem: "mpclab", category: "PCMPlayer")

    /// allowDriver: route through the "Virtual MPC XL" device when it is installed (XL only).
    init(port: UInt16, panel: Int, allowDriver: Bool) { self.port = port; self.panel = panel; self.allowDriver = allowDriver }

    /// nil = system default output (follows default-device changes). Setting it while running switches device.
    var deviceUID: String? {
        get { q.sync { _deviceUID } }
        set { q.async { [self] in _deviceUID = newValue; if running { applyDevice() } } }
    }

    /// Driver mode: mix MPC Main 1/2 into the speaker monitor.
    var hearDirect: Bool {
        get { q.sync { _hearDirect } }
        set { q.async { [self] in _hearDirect = newValue; engine?.hearDirect = newValue } }
    }

    /// 0...1, applied in the render thread with per-buffer smoothing.
    var volume: Float {
        get { q.sync { _volume } }
        set { q.async { [self] in _volume = max(0, min(1, newValue)); engine?.stream.volume = _volume } }
    }

    /// e.g. "stereo" or "Virtual MPC XL: 8 out / 4 in", for the Audio menu.
    var mode: String { q.sync { modeLine.isEmpty ? "starting" : modeLine } }

    var stats: (under: Int, skip: Int, targetFrames: Int) { q.sync { engine?.stream.snapshot() ?? (0, 0, 0) } }

    func start() {
        q.async { [self] in
            guard !running else { return }
            running = true
            installListener()
            rebuild()
        }
    }

    func stop() {
        q.sync {
            guard running else { return }
            running = false
            generation += 1
            task?.cancel(with: .goingAway, reason: nil); task = nil
            session?.invalidateAndCancel(); session = nil
            removeListener()
            stopUnits()
            engine = nil
        }
    }

    // MARK: mode

    /// Pick the mode from the device list, then (re)connect; connect() sizes the engine.
    private func rebuild() {                 // on q
        let drv = allowDriver ? Self.deviceID(forUID: Self.driverUID) : nil
        if drv != driverID { stopUnits(); engine = nil }
        let leaving = driverID != nil && drv == nil
        driverID = drv
        if let drv { Self.setRate(drv) }
        connect(leaving: leaving)
    }

    private func requestBody(leaving: Bool) -> [String: Any] {
        if driverID != nil { return ["width": 8, "in_width": 4, "in": "mic"] }
        return leaving ? ["width": 2, "in_width": 2, "in": "off"] : ["width": 2, "in_width": 2]
    }

    /// The VM's reply says what it honours; an older VM omits the keys (= stereo).
    private func adopt(_ reply: [String: Any]?) {   // on q
        let width = max(1, min(8, reply?["width"] as? Int ?? 2))
        let inWidth = driverID == nil ? 0 : max(0, min(4, reply?["in_width"] as? Int ?? 2))
        if let e = engine, e.width == width, (e.tx?.ch ?? 0) == inWidth { return }
        stopUnits()
        let e = Engine(width: width, sendWidth: inWidth, driver: driverID != nil)
        e.hearDirect = _hearDirect; e.stream.volume = _volume
        engine = e
        startUnits()
        let line = driverID == nil ? "stereo" : "Virtual MPC XL: \(width) out / \(inWidth) in"
        if line != modeLine { modeLine = line; Self.fileLog("audio mode: \(line)") }
    }

    // MARK: socket

    private func connect(leaving: Bool = false) {   // on q
        guard running else { return }
        generation += 1
        let gen = generation
        task?.cancel(with: .goingAway, reason: nil); task = nil
        if session == nil {
            let cfg = URLSessionConfiguration.ephemeral
            cfg.timeoutIntervalForRequest = 5
            session = URLSession(configuration: cfg)
        }
        guard let session, let api = URL(string: "http://127.0.0.1:\(panel)/audio") else { return }
        var req = URLRequest(url: api); req.httpMethod = "POST"; req.timeoutInterval = 2
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try? JSONSerialization.data(withJSONObject: requestBody(leaving: leaving))
        session.dataTask(with: req) { [weak self] data, _, err in
            guard let self else { return }
            let reply = data.flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] }
            self.q.async {
                guard self.running, gen == self.generation else { return }
                if reply == nil { self.log.debug("POST /audio failed: \(err?.localizedDescription ?? "bad reply")") }
                self.adopt(reply)
                self.open(gen)
            }
        }.resume()
    }

    private func open(_ gen: Int) {          // on q
        guard let session, let url = URL(string: "ws://127.0.0.1:\(port)") else { return }
        rest.removeAll(keepingCapacity: true)
        engine?.stream.reset()
        let t = session.webSocketTask(with: url)
        t.maximumMessageSize = 4 << 20
        task = t
        t.resume()
        receive(t, gen)
    }

    private func receive(_ t: URLSessionWebSocketTask, _ gen: Int) {
        t.receive { [weak self] result in
            guard let self else { return }
            self.q.async {
                guard self.running, gen == self.generation else { return }
                switch result {
                case .success(.data(let d)):
                    self.consume(d)
                    if let tx = self.engine?.tx {          // send MPC Input 1-4 at the stream's pace
                        let out = tx.drain()
                        if !out.isEmpty { t.send(.data(out)) { _ in } }
                    }
                    self.receive(t, gen)
                case .success:                             // text frame: ignore
                    self.receive(t, gen)
                case .failure(let e):
                    self.log.debug("pcm socket :\(self.port) down: \(e.localizedDescription); retry in 1 s")
                    self.task = nil
                    self.q.asyncAfter(deadline: .now() + 1) {
                        guard self.running, gen == self.generation else { return }
                        self.connect()                     // re-POSTs: a restarted VM forgets its width
                    }
                @unknown default:
                    break
                }
            }
        }
    }

    private func consume(_ d: Data) {        // on q
        guard let e = engine else { return }
        let fb = 2 * e.width
        var b = d
        if !rest.isEmpty { b = rest + d; rest.removeAll(keepingCapacity: true) }
        let n = b.count - b.count % fb        // whole S16 frames
        if n < b.count { rest = b.subdata(in: b.startIndex + n ..< b.endIndex) }
        guard n > 0 else { return }
        b.withUnsafeBytes { raw in e.stream.push(UnsafeRawBufferPointer(rebasing: raw[0..<n])) }
    }

    // MARK: units

    private func startUnits() {              // on q, engine set
        guard let e = engine else { return }
        if let drv = driverID {
            driverUnit = makeUnit(device: drv, channels: 16, e, render: driverRender, input: driverInput)
            e.driverUnit = driverUnit
        }
        startSpeaker()
    }

    private func startSpeaker() {            // on q
        guard let e = engine else { return }
        var id = _deviceUID.flatMap(Self.deviceID(forUID:)) ?? 0
        if id == 0 {
            if let uid = _deviceUID { log.error("output device \(uid) not found; using system default") }
            id = Self.defaultOutputDevice() ?? 0
        }
        guard id != 0 else { return }
        if driverID != nil && id == driverID {        // monitor into the loopback would feed back
            Self.fileLog("speaker is Virtual MPC XL; no monitor output")
            return
        }
        speaker = makeUnit(device: id, channels: 2, e, render: speakerRender, input: nil)
    }

    private func makeUnit(device: AudioDeviceID, channels: UInt32, _ e: Engine,
                          render: AURenderCallback, input: AURenderCallback?) -> AudioUnit? {
        var desc = AudioComponentDescription(componentType: kAudioUnitType_Output,
                                             componentSubType: kAudioUnitSubType_HALOutput,
                                             componentManufacturer: kAudioUnitManufacturer_Apple,
                                             componentFlags: 0, componentFlagsMask: 0)
        guard let comp = AudioComponentFindNext(nil, &desc) else { log.error("no HAL output component"); return nil }
        var u: AudioUnit?
        guard check(AudioComponentInstanceNew(comp, &u), "AudioComponentInstanceNew"), let u else { return nil }
        let ref = Unmanaged.passUnretained(e).toOpaque()
        if input != nil {
            var one: UInt32 = 1
            _ = check(AudioUnitSetProperty(u, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1,
                                           &one, 4), "EnableIO input")
        }
        var id = device
        _ = check(AudioUnitSetProperty(u, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
                                       &id, UInt32(MemoryLayout<AudioDeviceID>.size)), "CurrentDevice")
        var fmt = AudioStreamBasicDescription(mSampleRate: PlayerCore.sampleRate,
                                              mFormatID: kAudioFormatLinearPCM,
                                              mFormatFlags: kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved,
                                              mBytesPerPacket: 4, mFramesPerPacket: 1, mBytesPerFrame: 4,
                                              mChannelsPerFrame: channels, mBitsPerChannel: 32, mReserved: 0)
        let fsz = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        _ = check(AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, fsz), "StreamFormat out")
        var cb = AURenderCallbackStruct(inputProc: render, inputProcRefCon: ref)
        _ = check(AudioUnitSetProperty(u, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                       &cb, UInt32(MemoryLayout<AURenderCallbackStruct>.size)), "SetRenderCallback")
        if let input {
            _ = check(AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &fmt, fsz), "StreamFormat in")
            var icb = AURenderCallbackStruct(inputProc: input, inputProcRefCon: ref)
            _ = check(AudioUnitSetProperty(u, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0,
                                           &icb, UInt32(MemoryLayout<AURenderCallbackStruct>.size)), "SetInputCallback")
        }
        _ = check(AudioUnitInitialize(u), "AudioUnitInitialize")
        _ = check(AudioOutputUnitStart(u), "AudioOutputUnitStart")
        return u
    }

    private static func dispose(_ u: AudioUnit?) {
        guard let u else { return }
        AudioOutputUnitStop(u); AudioUnitUninitialize(u); AudioComponentInstanceDispose(u)
    }

    private func stopUnits() {               // on q
        Self.dispose(speaker); speaker = nil
        Self.dispose(driverUnit); driverUnit = nil
        engine?.driverUnit = nil
    }

    private func applyDevice() {             // on q, running: re-point the speaker only
        Self.dispose(speaker); speaker = nil
        startSpeaker()
    }

    /// HALOutput does not follow the system default by itself, and a device that is unplugged
    /// and replugged comes back with a new ID: re-point the speaker when the default changes
    /// (deviceUID == nil) or the device list changes; switch modes when the driver comes or goes.
    private static let watched = [kAudioHardwarePropertyDefaultOutputDevice, kAudioHardwarePropertyDevices]

    private func installListener() {
        guard listener == nil else { return }
        let block: AudioObjectPropertyListenerBlock = { [weak self] _, addrs in
            guard let self else { return }
            let listChanged = addrs.pointee.mSelector == kAudioHardwarePropertyDevices
            self.q.async {
                guard self.running else { return }
                if listChanged, (self.allowDriver ? Self.deviceID(forUID: Self.driverUID) : nil) != self.driverID {
                    self.rebuild(); return
                }
                if listChanged || self._deviceUID == nil { self.applyDevice() }
            }
        }
        for sel in Self.watched {
            var addr = AudioObjectPropertyAddress(mSelector: sel, mScope: kAudioObjectPropertyScopeGlobal,
                                                  mElement: kAudioObjectPropertyElementMain)
            AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &addr, nil, block)
        }
        listener = block
    }

    private func removeListener() {
        guard let block = listener else { return }
        for sel in Self.watched {
            var addr = AudioObjectPropertyAddress(mSelector: sel, mScope: kAudioObjectPropertyScopeGlobal,
                                                  mElement: kAudioObjectPropertyElementMain)
            AudioObjectRemovePropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &addr, nil, block)
        }
        listener = nil
    }

    private func check(_ s: OSStatus, _ what: String) -> Bool {
        if s != noErr { log.error("\(what) failed: \(s)"); Self.fileLog("\(what) failed: \(s)") }
        return s == noErr
    }

    static func fileLog(_ s: String) {
        let url = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/VirtualMPC.log")
        if !FileManager.default.fileExists(atPath: url.path) { FileManager.default.createFile(atPath: url.path, contents: nil) }
        guard let fh = try? FileHandle(forWritingTo: url) else { return }
        _ = try? fh.seekToEnd(); fh.write(Data("[audio] \(s)\n".utf8)); try? fh.close()
    }

    // MARK: device enumeration

    /// CoreAudio devices with at least one output channel, minus the driver (it is not a speaker).
    static func outputDevices() -> [(uid: String, name: String)] {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
                                              mScope: kAudioObjectPropertyScopeGlobal,
                                              mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        let sys = AudioObjectID(kAudioObjectSystemObject)
        guard AudioObjectGetPropertyDataSize(sys, &addr, 0, nil, &size) == noErr, size > 0 else { return [] }
        var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
        guard AudioObjectGetPropertyData(sys, &addr, 0, nil, &size, &ids) == noErr else { return [] }
        return ids.compactMap { id in
            guard outputChannels(id) > 0,
                  let uid = stringProperty(id, kAudioDevicePropertyDeviceUID), uid != driverUID,
                  let name = stringProperty(id, kAudioObjectPropertyName) else { return nil }
            return (uid, name)
        }
    }

    private static func outputChannels(_ id: AudioDeviceID) -> Int {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreamConfiguration,
                                              mScope: kAudioObjectPropertyScopeOutput,
                                              mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &addr, 0, nil, &size) == noErr, size > 0 else { return 0 }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { raw.deallocate() }
        guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, raw) == noErr else { return 0 }
        let abl = UnsafeMutableAudioBufferListPointer(raw.assumingMemoryBound(to: AudioBufferList.self))
        return abl.reduce(0) { $0 + Int($1.mNumberChannels) }
    }

    private static func stringProperty(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector) -> String? {
        var addr = AudioObjectPropertyAddress(mSelector: sel, mScope: kAudioObjectPropertyScopeGlobal,
                                              mElement: kAudioObjectPropertyElementMain)
        var value: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &value) == noErr,
              let s = value?.takeRetainedValue() else { return nil }
        return s as String
    }

    static func deviceID(forUID uid: String) -> AudioDeviceID? {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyTranslateUIDToDevice,
                                              mScope: kAudioObjectPropertyScopeGlobal,
                                              mElement: kAudioObjectPropertyElementMain)
        var cfUID = uid as CFString
        var id: AudioDeviceID = 0
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let s = withUnsafeMutablePointer(to: &cfUID) { p in
            AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr,
                                       UInt32(MemoryLayout<CFString>.size), p, &size, &id)
        }
        return s == noErr && id != kAudioObjectUnknown ? id : nil
    }

    private static func defaultOutputDevice() -> AudioDeviceID? {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDefaultOutputDevice,
                                              mScope: kAudioObjectPropertyScopeGlobal,
                                              mElement: kAudioObjectPropertyElementMain)
        var id: AudioDeviceID = 0
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &id) == noErr,
              id != kAudioObjectUnknown else { return nil }
        return id
    }

    /// AUHAL cannot rate-convert its input side: run the driver at the stream's 44.1 kHz.
    private static func setRate(_ id: AudioDeviceID) {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyNominalSampleRate,
                                              mScope: kAudioObjectPropertyScopeGlobal,
                                              mElement: kAudioObjectPropertyElementMain)
        var rate = 0.0, size = UInt32(MemoryLayout<Float64>.size)
        guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &rate) == noErr, rate != PlayerCore.sampleRate else { return }
        rate = PlayerCore.sampleRate
        let s = AudioObjectSetPropertyData(id, &addr, 0, nil, size, &rate)
        fileLog("Virtual MPC XL rate -> 44100: \(s == noErr ? "ok" : "failed \(s)")")
    }

    // MARK: self-check (VirtualMPC --selftest)

    static func selfTest() {
        // 8-ch jitter buffer: channel c carries c*1000 + frame index; channels must never mix.
        let core = PlayerCore(channels: 8), frames = 3000
        var pcm = [Int16](repeating: 0, count: frames * 8)
        for i in 0..<frames { for c in 0..<8 { pcm[i * 8 + c] = Int16(c * 1000 + i % 900) } }
        pcm.withUnsafeBytes { core.push($0) }
        let out = UnsafeMutablePointer<Float>.allocate(capacity: 512 * 8); defer { out.deallocate() }
        core.render(out, 256)                          // primes + 128-frame cross-fade in from silence
        for _ in 0..<4 {
            core.render(out, 256)
            for i in 0..<256 { for c in 0..<8 {
                let v = out[i * 8 + c] * 32768 - Float(c * 1000)
                precondition(v >= -1 && v <= 901, "channel \(c) frame \(i) leaked: \(v)")
            } }
        }
        // drain to underrun: counted, target grows by 256, output fades instead of stepping
        for _ in 0..<20 { core.render(out, 512) }
        let s = core.snapshot()
        precondition(s.under == 1 && s.targetFrames == 1280, "underrun handling: \(s)")
        precondition(abs(out[511 * 8 + 7]) < abs(Float(7000) / 32768), "no fade on underrun")
        // stereo S16 framing still works (width 2 = the old behaviour)
        let st = PlayerCore(channels: 2)
        [Int16](repeating: 16384, count: 2048 * 2).withUnsafeBytes { st.push($0) }
        st.render(out, 256); st.render(out, 256)
        precondition(abs(out[0] - 0.5) < 1e-4 && abs(out[1] - 0.5) < 1e-4, "stereo framing")
        // TX ring: device inputs 11-14 (Computer 3-6) -> interleaved S16 MPC Input 1-4
        let abl = AudioBufferList.allocate(maximumBuffers: 16); defer { for b in abl { b.mData?.deallocate() }; free(abl.unsafeMutablePointer) }
        for k in 0..<16 {
            let p = UnsafeMutablePointer<Float>.allocate(capacity: 4)
            for i in 0..<4 { p[i] = Float(k) / 100 + Float(i) / 1000 }
            abl[k] = AudioBuffer(mNumberChannels: 1, mDataByteSize: 16, mData: p)
        }
        let tx = TxRing(channels: 4)
        tx.push(abl, first: 10, 4)
        let d = tx.drain()
        precondition(d.count == 4 * 4 * 2 && tx.drain().isEmpty, "tx size")
        d.withUnsafeBytes { raw in
            for i in 0..<4 { for c in 0..<4 {
                let got = Int16(littleEndian: raw.loadUnaligned(fromByteOffset: (i * 4 + c) * 2, as: Int16.self))
                let want = Int16((Float(10 + c) / 100 + Float(i) / 1000) * 32768)
                precondition(abs(Int(got) - Int(want)) <= 1, "tx frame \(i) ch \(c): \(got) != \(want)")
            } }
        }
        // driver render: MPC Out 1-8 land on device channels 9-16, 1-8 silent; monitor = Main + Computer 1/2
        let e = Engine(width: 8, sendWidth: 4, driver: true)
        pcm.withUnsafeBytes { e.stream.push($0) }
        let dev = AudioBufferList.allocate(maximumBuffers: 16); defer { for b in dev { b.mData?.deallocate() }; free(dev.unsafeMutablePointer) }
        for k in 0..<16 {
            let p = UnsafeMutablePointer<Float>.allocate(capacity: 256); p.initialize(repeating: 9, count: 256)
            dev[k] = AudioBuffer(mNumberChannels: 1, mDataByteSize: 1024, mData: p)
        }
        for k in 8..<10 { e.inABL[k].mData!.assumingMemoryBound(to: Float.self).update(repeating: 0.25, count: 256) }
        e.renderDriver(dev, 256); e.inFrames = 256; e.renderDriver(dev, 256)
        for k in 0..<16 {
            let p = dev[k].mData!.assumingMemoryBound(to: Float.self)
            let v = p[200] * 32768
            if k < 8 { precondition(v == 0, "device out \(k + 1) not silent") }
            else { precondition(v - Float((k - 8) * 1000) >= -1 && v - Float((k - 8) * 1000) <= 901, "device out \(k + 1) wrong: \(v)") }
        }
        let l = UnsafeMutablePointer<Float>.allocate(capacity: 4096), r = UnsafeMutablePointer<Float>.allocate(capacity: 4096)
        defer { l.deallocate(); r.deallocate() }
        for _ in 0..<4 { e.inFrames = 256; e.renderDriver(dev, 256) }     // fill the monitor past its 1024 target
        e.renderSpeaker(l, r, 256); e.renderSpeaker(l, r, 256)
        let main1 = (l[200] - 0.25) * 32768                                  // Computer 1 adds 0.25
        precondition(main1 >= -2 && main1 <= 902, "monitor L = Main 1 + Computer 1: \(main1)")
        print("PCMPlayer.selfTest OK")
    }
}
