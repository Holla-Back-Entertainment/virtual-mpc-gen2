// Offline self-checks run by `VirtualMPC --selftest` (no VM, no driver, no audio device needed).

import AudioToolbox
import CoreAudio
import Foundation

/// n buffers of `frames` floats, each filled with `value(k)`.
private func makeABL(_ n: Int, frames: Int, _ value: (Int) -> Float) -> UnsafeMutableAudioBufferListPointer {
    let abl = AudioBufferList.allocate(maximumBuffers: n)
    for k in 0..<n {
        let p = UnsafeMutablePointer<Float>.allocate(capacity: frames); p.initialize(repeating: value(k), count: frames)
        abl[k] = AudioBuffer(mNumberChannels: 1, mDataByteSize: UInt32(frames * 4), mData: p)
    }
    return abl
}
private func freeABL(_ abl: UnsafeMutableAudioBufferListPointer) {
    for b in abl { b.mData?.deallocate() }
    free(abl.unsafeMutablePointer)
}

/// S16 test signal: channel c carries c*1000 + frame index % 900.
private func testPCM(_ ch: Int, _ frames: Int) -> [Int16] {
    var pcm = [Int16](repeating: 0, count: frames * ch)
    for i in 0..<frames { for c in 0..<ch { pcm[i * ch + c] = Int16(c * 1000 + i % 900) } }
    return pcm
}

/// Is v (a float sample) channel c's test signal?
private func isChannel(_ v: Float, _ c: Int) -> Bool {
    let d = v * 32768 - Float(c * 1000)
    return d >= -1 && d <= 901
}

extension PCMPlayer {
    static func selfTest() {
        // 8-ch jitter buffer: channels must never mix.
        let core = PlayerCore(channels: 8)
        let pcm8 = testPCM(8, 3000)
        pcm8.withUnsafeBytes { core.push($0) }
        let out = UnsafeMutablePointer<Float>.allocate(capacity: 512 * 32); defer { out.deallocate() }
        core.render(out, 256)                          // primes + 128-frame cross-fade in from silence
        for _ in 0..<4 {
            core.render(out, 256)
            for i in 0..<256 { for c in 0..<8 { precondition(isChannel(out[i * 8 + c], c), "channel \(c) frame \(i) leaked") } }
        }
        // drain to underrun: counted, target grows by 256, output fades instead of stepping
        for _ in 0..<20 { core.render(out, 512) }
        let s = core.snapshot()
        precondition(s.under == 1 && s.targetFrames == 1280, "underrun handling: \(s)")
        precondition(abs(out[511 * 8 + 7]) < abs(Float(7000) / 32768), "no fade on underrun")
        // stereo S16 framing still works (width 2 = the stereo fallback)
        let st = PlayerCore(channels: 2)
        [Int16](repeating: 16384, count: 2048 * 2).withUnsafeBytes { st.push($0) }
        st.render(out, 256); st.render(out, 256)
        precondition(abs(out[0] - 0.5) < 1e-4 && abs(out[1] - 0.5) < 1e-4, "stereo framing")

        // TX ring: interleaved 30-ch source, channels 3..30 -> S16 channels 1..28.
        let src = UnsafeMutablePointer<Float>.allocate(capacity: 4 * 30); defer { src.deallocate() }
        for i in 0..<4 { for k in 0..<30 { src[i * 30 + k] = Float(k) / 100 + Float(i) / 1000 } }
        let tx = TxRing(channels: 28)
        tx.push(src, stride: 30, first: 2, 4)
        let d = tx.drain()
        precondition(d.count == 4 * 28 * 2 && tx.drain().isEmpty, "tx size")
        d.withUnsafeBytes { raw in
            for i in 0..<4 { for c in 0..<28 {
                let got = Int16(littleEndian: raw.loadUnaligned(fromByteOffset: (i * 28 + c) * 2, as: Int16.self))
                let want = Int16((Float(2 + c) / 100 + Float(i) / 1000) * 32768)
                precondition(abs(Int(got) - Int(want)) <= 1, "tx frame \(i) ch \(c): \(got) != \(want)")
            } }
        }

        rateConverterTest()
        for map in [AudioMap.xl, AudioMap.live3] { driverMapTest(map) }
        print("PCMPlayer.selfTest OK")
    }

    /// 48 kHz -> 44.1 kHz: frame count, DC level and a ramp's slope survive, across block joins.
    private static func rateConverterTest() {
        let id = RateConverter(channels: 2, from: 44100, to: 44100)
        precondition(id.isIdentity, "44.1 -> 44.1 is a copy")
        let conv = RateConverter(channels: 2, from: 48000, to: 44100)
        let block = 480, inp = UnsafeMutablePointer<Float>.allocate(capacity: block * 2)
        let o = UnsafeMutablePointer<Float>.allocate(capacity: 1024 * 2)
        defer { inp.deallocate(); o.deallocate() }
        var total = 0, x = 0, prevR: Float = -1
        for _ in 0..<10 {
            for i in 0..<block { inp[2 * i] = 0.5; inp[2 * i + 1] = Float(x + i) * 1e-4 }
            x += block
            let m = conv.process(inp, block, o, maxOut: 1024)
            for i in 0..<m {
                precondition(abs(o[2 * i] - 0.5) < 1e-6, "rate converter DC")
                if total + i > 0 {   // ramp: each output step = 48000/44100 input frames
                    precondition(abs((o[2 * i + 1] - prevR) - Float(48000.0 / 44100.0 * 1e-4)) < 1e-6, "rate converter ramp at \(total + i)")
                }
                prevR = o[2 * i + 1]
            }
            total += m
        }
        precondition(abs(total - 4410) <= 1, "rate converter frames: \(total) != 4410")
    }

    /// One model's driver path: VM playback k -> Engine out k, Engine in j+2 -> VM capture j,
    /// speaker = Main 1/2 + Monitor 1/2.
    private static func driverMapTest(_ map: AudioMap) {
        let e = Engine(width: AudioMap.requestWidth, sendWidth: AudioMap.requestInWidth,
                       devOut: map.engineOuts, devIn: map.engineIns, inRate: 44100)
        precondition(e.driver && e.devOut == map.engineOuts && e.devIn == map.engineIns, "\(map.device) engine size")
        let pcm = testPCM(32, 3000)
        pcm.withUnsafeBytes { e.stream.push($0) }
        // Engine output buffers: give it 32 even where the device has fewer, to see 31/32 stay silent.
        let dev = makeABL(32, frames: 256) { _ in 9 }; defer { freeABL(dev) }
        e.renderDriver(dev, 256); e.renderDriver(dev, 256)
        for k in 0..<32 {
            let v = dev[k].mData!.assumingMemoryBound(to: Float.self)[200]
            if k < map.engineOuts { precondition(isChannel(v, k), "\(map.device) out \(k + 1) is not VM channel \(k + 1)") }
            else { precondition(v == 0, "\(map.device) out \(k + 1) not silent") }
        }
        // Engine inputs: channel k carries (k+1)/100; Monitor 1/2 = 0.01/0.02.
        for k in 0..<e.inABL.count { e.inABL[k].mData!.assumingMemoryBound(to: Float.self).update(repeating: Float(k + 1) / 100, count: 256) }
        e.captured(256)
        let d = e.tx!.drain()
        precondition(d.count == 256 * 28 * 2, "\(map.device) capture frames")
        d.withUnsafeBytes { raw in
            for j in 0..<28 {
                let got = Int16(littleEndian: raw.loadUnaligned(fromByteOffset: (100 * 28 + j) * 2, as: Int16.self))
                let want = j < map.captureChannels ? Int16((Float(j + 3) / 100 * 32768).rounded()) : 0
                precondition(abs(Int(got) - Int(want)) <= 1, "\(map.device) capture \(j + 1): \(got) != \(want)")
            }
        }
        // Speaker: fill both monitors past their 1024-frame target, then Main 1 + Monitor 1.
        for _ in 0..<5 { e.renderDriver(dev, 256); e.captured(256) }
        let l = UnsafeMutablePointer<Float>.allocate(capacity: 256), r = UnsafeMutablePointer<Float>.allocate(capacity: 256)
        defer { l.deallocate(); r.deallocate() }
        e.renderSpeaker(l, r, 256); e.renderSpeaker(l, r, 256)
        precondition(isChannel(l[200] - 0.01, 0), "\(map.device) speaker L = Main 1 + Monitor 1: \(l[200])")
        precondition(isChannel(r[200] - 0.02, 1), "\(map.device) speaker R = Main 2 + Monitor 2: \(r[200])")
        e.hearDirect = false
        for _ in 0..<8 { e.renderDriver(dev, 256); e.captured(256); e.renderSpeaker(l, r, 256) }
        precondition(abs(l[200] - 0.01) < 1e-4, "\(map.device) Hear MPC Directly off: speaker = Monitor only, got \(l[200])")
    }
}

extension Model {
    /// Per-model device UIDs, channel counts and CoreMIDI ports.
    static func selfTest() {
        let xl = all.first { $0.id == "acvc" }!, l3 = all.first { $0.id == "acvg" }!
        precondition(xl.audio.userUID == "VirtualMPCXL_UID", "XL keeps its UID (Live's saved setting)")
        precondition(xl.audio.engineOuts == 32 && xl.audio.engineIns == 30 && xl.audio.captureChannels == 28, "XL map")
        precondition(l3.audio.engineOuts == 30 && l3.audio.engineIns == 28 && l3.audio.captureChannels == 26, "Live III map")
        precondition(AudioMap.allUIDs.count == 4, "four driver UIDs")
        let xlPorts = xl.midiPorts.map { "\($0.name)=\($0.port)\($0.toMPC ? ">" : "")\($0.fromMPC ? "<" : "")" }
        precondition(xlPorts == ["Virtual MPC XL=5652><", "Virtual MPC XL MIDI In 1=5751>", "Virtual MPC XL MIDI In 2=5752>",
                                 "Virtual MPC XL MIDI Out A=5753<", "Virtual MPC XL MIDI Out B=5754<",
                                 "Virtual MPC XL MIDI Out C=5755<", "Virtual MPC XL MIDI Out D=5756<"], "XL MIDI: \(xlPorts)")
        let l3Ports = l3.midiPorts.map { "\($0.name)=\($0.port)\($0.toMPC ? ">" : "")\($0.fromMPC ? "<" : "")" }
        precondition(l3Ports == ["Virtual MPC Live III=5662><", "Virtual MPC Live III MIDI In 1=5761>", "Virtual MPC Live III MIDI In 2=5762>",
                                 "Virtual MPC Live III MIDI Out A=5763<", "Virtual MPC Live III MIDI Out B=5764<"], "Live III MIDI: \(l3Ports)")
        print("Model.selfTest OK")
    }
}
