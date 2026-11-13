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
//  - stereo (the "Virtual MPC" driver is not installed, or only the old 16-ch one is): POST /audio
//    width 2; MPC Main 1/2 plays on the chosen speaker device.
//  - driver (the model's hidden "... Engine" device is present): POST /audio width 32 / in_width 28
//    / in "mic". AudioMap (AudioRouting.swift) has the per-model channel maps: VM playback k ->
//    Engine output k (= Live's input k); Engine input j+2 (Live's output) -> VM capture j. Engine
//    inputs 1/2 are Live's "Monitor" outs. The speaker plays MPC Main 1/2 ("Hear MPC Directly") +
//    Monitor 1/2, each through its own 2-ch PlayerCore so the speaker's clock can drift freely.
//    The Engine follows Live's rate (44.1 or 48 kHz): AUHAL converts the output side, RateConverter
//    the input side. Whatever widths the VM honours (its POST reply) are used.
//
// Units are CoreAudio AUHAL (kAudioUnitSubType_HALOutput). Render threads only try-lock the
// stream core; the driver's IO thread does short plain locks on the monitor cores and TX ring.

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

// MARK: - Per-mode IO state (refCon of the render callbacks)

final class Engine: @unchecked Sendable {
    static let maxFrames = 8192
    let width: Int                     // VM -> Mac channels
    let devOut: Int, devIn: Int        // Engine device channels in use (0 = stereo mode)
    let stream: PlayerCore
    let monMain: PlayerCore?           // driver mode: MPC Main 1/2 -> speaker (when hearDirect)
    let monLive: PlayerCore?           // driver mode: Live's Monitor 1/2 -> speaker
    let tx: TxRing?                    // driver mode: Engine inputs 3.. -> VM capture 1..
    let conv: RateConverter?           // driver mode: Engine input rate -> 44.1 kHz
    var hearDirect = true              // written from any thread
    var driverUnit: AudioUnit?
    let scratch: UnsafeMutablePointer<Float>                         // stream render (driver or speaker thread)
    let mix: UnsafeMutablePointer<Float>, mon: UnsafeMutablePointer<Float>   // driver IO thread
    let spkA: UnsafeMutablePointer<Float>, spkB: UnsafeMutablePointer<Float> // speaker thread
    let inABL: UnsafeMutableAudioBufferListPointer                   // the Engine's inputs, device rate
    let inInter: UnsafeMutablePointer<Float>, inConv: UnsafeMutablePointer<Float>

    var driver: Bool { devOut > 0 }

    init(width: Int, sendWidth: Int, devOut: Int = 0, devIn: Int = 0, inRate: Double = PlayerCore.sampleRate) {
        self.width = width
        let drv = devOut > 0 && devIn >= 2
        self.devOut = drv ? devOut : 0
        self.devIn = drv ? devIn : 0
        stream = PlayerCore(channels: width)
        monMain = drv ? PlayerCore(channels: 2) : nil
        monLive = drv ? PlayerCore(channels: 2) : nil
        tx = drv && sendWidth > 0 ? TxRing(channels: sendWidth) : nil
        conv = drv ? RateConverter(channels: devIn, from: inRate, to: PlayerCore.sampleRate) : nil
        let m = Self.maxFrames, ins = max(1, self.devIn)
        scratch = .allocate(capacity: m * width)
        mix = .allocate(capacity: m * 2); mon = .allocate(capacity: m * 2)
        spkA = .allocate(capacity: m * 2); spkB = .allocate(capacity: m * 2)
        inInter = .allocate(capacity: m * ins); inConv = .allocate(capacity: m * ins)
        inABL = AudioBufferList.allocate(maximumBuffers: ins)
        for k in 0..<ins {
            let p = UnsafeMutablePointer<Float>.allocate(capacity: m); p.initialize(repeating: 0, count: m)
            inABL[k] = AudioBuffer(mNumberChannels: 1, mDataByteSize: UInt32(m * 4), mData: p)
        }
    }
    deinit {
        scratch.deallocate(); mix.deallocate(); mon.deallocate(); spkA.deallocate(); spkB.deallocate()
        inInter.deallocate(); inConv.deallocate()
        for b in inABL { b.mData?.deallocate() }
        free(inABL.unsafeMutablePointer)
    }

    /// Engine output: VM channel k -> device channel k (Live's input k), zeros past the map; feed
    /// the Main 1/2 monitor.
    func renderDriver(_ abl: UnsafeMutableAudioBufferListPointer, _ n: Int) {
        stream.render(scratch, n)
        for k in 0..<abl.count {
            guard let p = abl[k].mData?.assumingMemoryBound(to: Float.self) else { continue }
            if k < width && k < devOut { for i in 0..<n { p[i] = scratch[i * width + k] } }
            else { p.update(repeating: 0, count: n) }
        }
        guard let monMain else { return }
        let hear: Float = hearDirect ? 1 : 0, rc = width > 1 ? 1 : 0
        for i in 0..<n {
            mix[2 * i] = hear * scratch[i * width]
            mix[2 * i + 1] = hear * scratch[i * width + rc]
        }
        monMain.pushFloat(mix, n)
    }

    /// Engine input (inABL holds n device-rate frames): to 44.1 kHz, Monitor 1/2 -> speaker,
    /// inputs 3.. -> VM capture 1.. (silence past what this model has).
    func captured(_ n: Int) {
        guard let conv, n > 0 else { return }
        let ch = devIn
        for c in 0..<ch {
            guard c < inABL.count, let p = inABL[c].mData?.assumingMemoryBound(to: Float.self) else {
                for i in 0..<n { inInter[i * ch + c] = 0 }; continue
            }
            for i in 0..<n { inInter[i * ch + c] = p[i] }
        }
        let m = conv.process(inInter, n, inConv, maxOut: Self.maxFrames)
        for i in 0..<m { mon[2 * i] = inConv[i * ch]; mon[2 * i + 1] = inConv[i * ch + 1] }
        monLive?.pushFloat(mon, m)
        tx?.push(inConv, stride: ch, first: 2, m)
    }

    /// Speaker output: stereo mode plays the stream's 1/2, driver mode Main 1/2 + Monitor 1/2.
    func renderSpeaker(_ l: UnsafeMutablePointer<Float>, _ r: UnsafeMutablePointer<Float>, _ n: Int) {
        if let monMain, let monLive {
            monMain.render(spkA, n); monLive.render(spkB, n)
            for i in 0..<n { l[i] = spkA[2 * i] + spkB[2 * i]; r[i] = spkA[2 * i + 1] + spkB[2 * i + 1] }
            return
        }
        stream.render(scratch, n)
        let w = width, rc = w > 1 ? 1 : 0
        for i in 0..<n { l[i] = scratch[i * w]; r[i] = scratch[i * w + rc] }
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
    guard AudioUnitRender(u, flags, ts, 1, UInt32(n), e.inABL.unsafeMutablePointer) == noErr else { return noErr }
    e.captured(n)
    return noErr
}

// MARK: - Public player

final class PCMPlayer: @unchecked Sendable {
    private let port: UInt16, panel: Int, map: AudioMap?
    private let q = DispatchQueue(label: "mpclab.pcmplayer")   // guards everything below
    private var running = false
    private var engine: Engine?
    private var speaker: AudioUnit?, driverUnit: AudioUnit?
    private var driverID: AudioDeviceID?                       // the model's Engine device; non-nil = driver mode
    private var legacy = false                                 // only the old 16-ch driver is installed
    private var widths = (out: 2, in: 0)                       // what the VM honours
    private var modeLine = ""                                  // last logged mode
    private var _deviceUID: String?
    private var _hearDirect = true
    private var _volume: Float = 1
    private var generation = 0                                 // bumps on every (re)connect / stop
    private var session: URLSession?
    private var task: URLSessionWebSocketTask?
    private var rest = Data()                                  // partial frame carried between messages
    private var listener: AudioObjectPropertyListenerBlock?
    private var rateListener: (AudioDeviceID, AudioObjectPropertyListenerBlock)?
    private let log = Logger(subsystem: "mpclab", category: "PCMPlayer")

    /// map: the model's driver devices; nil = always stereo.
    init(port: UInt16, panel: Int, map: AudioMap?) { self.port = port; self.panel = panel; self.map = map }

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

    /// e.g. "stereo" or "Virtual MPC XL: 32 out / 28 in, 44.1 kHz", for the Audio menu.
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
            removeRateListener()
            stopUnits()
            engine = nil
        }
    }

    // MARK: mode

    private func findEngine() -> AudioDeviceID? { map.flatMap { Self.deviceID(forUID: $0.engineUID) } }

    /// Pick the mode from the device list, then (re)connect; connect() sizes the engine.
    private func rebuild() {                 // on q
        let drv = findEngine()
        if drv != driverID { stopUnits(); engine = nil; removeRateListener() }
        let leaving = driverID != nil && drv == nil
        driverID = drv
        legacy = drv == nil && map != nil && Self.deviceID(forUID: AudioMap.legacyUID) != nil
        if let drv { installRateListener(drv) }
        connect(leaving: leaving)
    }

    private func requestBody(leaving: Bool) -> [String: Any] {
        if driverID != nil { return ["width": AudioMap.requestWidth, "in_width": AudioMap.requestInWidth, "in": "mic"] }
        return leaving ? ["width": 2, "in_width": 2, "in": "off"] : ["width": 2, "in_width": 2]
    }

    /// The VM's reply says what it honours; an older VM omits the keys (= stereo).
    private func adopt(_ reply: [String: Any]?) {   // on q
        let width = max(1, min(32, reply?["width"] as? Int ?? 2))
        let inWidth = driverID == nil ? 0 : max(0, min(32, reply?["in_width"] as? Int ?? 2))
        if let e = engine, e.width == width, (e.tx?.ch ?? 0) == inWidth, e.driver == (driverID != nil) { return }
        widths = (width, inWidth)
        restartEngine()
    }

    /// New Engine for the current widths, device channels and device rate; restarts the units.
    private func restartEngine() {           // on q
        stopUnits()
        var devOut = 0, devIn = 0, rate = PlayerCore.sampleRate
        if let drv = driverID, let map {
            devOut = min(map.engineOuts, Self.channels(drv, kAudioObjectPropertyScopeOutput))
            devIn = min(map.engineIns, Self.channels(drv, kAudioObjectPropertyScopeInput))
            rate = Self.nominalRate(drv) ?? rate
        }
        let e = Engine(width: widths.out, sendWidth: widths.in, devOut: devOut, devIn: devIn, inRate: rate)
        e.hearDirect = _hearDirect; e.stream.volume = _volume
        engine = e
        startUnits()
        let line: String
        if let map, driverID != nil, e.driver {
            let khz = rate == 44100 ? "44.1" : String(format: "%g", rate / 1000)
            line = "\(map.device): \(min(widths.out, e.devOut)) out / \(min(widths.in, map.captureChannels)) in, \(khz) kHz"
        } else if driverID != nil {
            line = "stereo (\(map?.device ?? "driver") Engine has no channels)"
        } else {
            line = legacy ? "stereo (old Virtual MPC XL driver: reinstall the driver)" : "stereo"
        }
        if line != modeLine { modeLine = line; Self.fileLog("audio mode: \(line)") }
    }

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
        if let drv = driverID, e.driver {
            driverUnit = makeUnit(device: drv, channels: UInt32(e.devOut), e, render: driverRender,
                                  input: driverInput, inChannels: UInt32(e.devIn), inRate: e.conv.map { $0.step * PlayerCore.sampleRate })
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
        if let uid = Self.stringProperty(id, kAudioDevicePropertyDeviceUID), AudioMap.allUIDs.contains(uid) {
            // the monitor into a Virtual MPC device would come back on its own input
            Self.fileLog("speaker is a Virtual MPC device (\(uid)); no monitor output")
            return
        }
        speaker = makeUnit(device: id, channels: 2, e, render: speakerRender, input: nil)
    }

    /// AUHAL on `device`: render callback for `channels` outputs at 44.1 kHz (AUHAL converts to the
    /// device rate); with `input`, also `inChannels` inputs at the device's own rate `inRate`
    /// (AUHAL cannot convert rate on the input side).
    private func makeUnit(device: AudioDeviceID, channels: UInt32, _ e: Engine,
                          render: AURenderCallback, input: AURenderCallback?,
                          inChannels: UInt32 = 0, inRate: Double? = nil) -> AudioUnit? {
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
        func format(_ rate: Double, _ ch: UInt32) -> AudioStreamBasicDescription {
            AudioStreamBasicDescription(mSampleRate: rate, mFormatID: kAudioFormatLinearPCM,
                                        mFormatFlags: kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved,
                                        mBytesPerPacket: 4, mFramesPerPacket: 1, mBytesPerFrame: 4,
                                        mChannelsPerFrame: ch, mBitsPerChannel: 32, mReserved: 0)
        }
        var fmt = format(PlayerCore.sampleRate, channels)
        let fsz = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        _ = check(AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, fsz), "StreamFormat out")
        var cb = AURenderCallbackStruct(inputProc: render, inputProcRefCon: ref)
        _ = check(AudioUnitSetProperty(u, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                       &cb, UInt32(MemoryLayout<AURenderCallbackStruct>.size)), "SetRenderCallback")
        if let input {
            var ifmt = format(inRate ?? PlayerCore.sampleRate, inChannels)
            _ = check(AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &ifmt, fsz), "StreamFormat in")
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

    /// Live (or anyone) changed the pair's rate: the Engine's input side must match it, rebuild.
    private func installRateListener(_ dev: AudioDeviceID) {   // on q
        removeRateListener()
        let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
            guard let self else { return }
            self.q.async {
                guard self.running, self.driverID == dev, self.engine != nil else { return }
                Self.fileLog("Engine rate now \(Self.nominalRate(dev) ?? 0) Hz; restarting audio")
                self.restartEngine()
            }
        }
        var addr = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyNominalSampleRate,
                                              mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        if AudioObjectAddPropertyListenerBlock(dev, &addr, nil, block) == noErr { rateListener = (dev, block) }
    }

    private func removeRateListener() {      // on q
        guard let (dev, block) = rateListener else { return }
        var addr = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyNominalSampleRate,
                                              mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        AudioObjectRemovePropertyListenerBlock(dev, &addr, nil, block)
        rateListener = nil
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
                if listChanged, self.findEngine() != self.driverID {
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

    /// CoreAudio devices with at least one output channel, minus the driver's (they are not speakers).
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
            guard channels(id, kAudioObjectPropertyScopeOutput) > 0,
                  let uid = stringProperty(id, kAudioDevicePropertyDeviceUID), !AudioMap.allUIDs.contains(uid),
                  let name = stringProperty(id, kAudioObjectPropertyName) else { return nil }
            return (uid, name)
        }
    }

    /// Channels of `id` in scope (input or output), from its stream configuration.
    private static func channels(_ id: AudioDeviceID, _ scope: AudioObjectPropertyScope) -> Int {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreamConfiguration,
                                              mScope: scope,
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

    private static func nominalRate(_ id: AudioDeviceID) -> Double? {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyNominalSampleRate,
                                              mScope: kAudioObjectPropertyScopeGlobal,
                                              mElement: kAudioObjectPropertyElementMain)
        var rate = 0.0, size = UInt32(MemoryLayout<Float64>.size)
        guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &rate) == noErr, rate > 0 else { return nil }
        return rate
    }
}
