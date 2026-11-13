// AudioRouting: per-model channel maps for the "Virtual MPC" driver, plus the two small DSP
// pieces the driver path needs (Mac -> VM ring, fixed-ratio rate converter).
//
// The driver gives each model a user device (Live) and a hidden Engine partner (this app):
//   Engine output n -> user input n   (the MPC's outs)
//   user output n   -> Engine input n (Live's outs: Monitor 1/2, then the MPC's ins)
// The VM's channel order matches the driver's, so both maps are offsets:
//   playback (VM -> Mac): VM channel k            -> Engine output k     (k < engineOuts)
//   capture  (Mac -> VM): Engine input j + 2      -> VM channel j        (j < engineIns - 2, else silence)

import AudioToolbox
import CoreAudio
import Foundation
import os

struct AudioMap: Sendable {
    let device: String              // what Live shows
    let userUID: String
    let engineUID: String
    let analogOuts: Int             // MPC Out 1..n
    let analogIns: Int              // MPC In 1..n
    static let usb = 24             // USB Out / USB In 1-24 on both models

    /// Engine outputs = user inputs: analog outs + USB outs.
    var engineOuts: Int { analogOuts + Self.usb }
    /// Engine inputs = user outputs: Monitor 1/2 + analog ins + USB ins.
    var engineIns: Int { 2 + analogIns + Self.usb }
    /// Capture channels this model fills (the rest of in_width is sent as silence).
    var captureChannels: Int { engineIns - 2 }

    /// What the app asks the VM for when the driver is present (the VM's widths are fixed sets).
    static let requestWidth = 32, requestInWidth = 28

    static let xl = AudioMap(device: "Virtual MPC XL", userUID: "VirtualMPCXL_UID",
                             engineUID: "VirtualMPCXL_Engine_UID", analogOuts: 8, analogIns: 4)
    static let live3 = AudioMap(device: "Virtual MPC Live III", userUID: "VirtualMPCLive3_UID",
                                engineUID: "VirtualMPCLive3_Engine_UID", analogOuts: 6, analogIns: 2)
    /// UID of the old one-device 16-ch driver (same as the XL's user UID, but with no Engine).
    static let legacyUID = "VirtualMPCXL_UID"
    /// Every device the driver makes: never a speaker (that would feed back into the loop).
    static let allUIDs: Set<String> = [xl.userUID, xl.engineUID, live3.userUID, live3.engineUID]
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

    /// n frames of interleaved `stride`-channel floats; ring channel c = source channel first + c,
    /// silence where the source has no such channel. Oldest frames drop on overflow.
    func push(_ src: UnsafePointer<Float>, stride: Int, first: Int, _ n: Int) {
        os_unfair_lock_lock(lock); defer { os_unfair_lock_unlock(lock) }
        for i in 0..<n {
            let k = ((w + i) % size) * ch
            for c in 0..<ch {
                let s = first + c
                let v = s < stride ? src[i * stride + s] : 0
                buf[k + c] = Int16(max(-32768, min(32767, (v * 32768).rounded())))
            }
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

// MARK: - Fixed-ratio linear rate converter (interleaved), for the driver's input at 48 kHz

/// AUHAL converts rate on its output side but not on its input side, so when Live runs the pair
/// at 48 kHz the Engine's input arrives at 48 kHz and is brought to the VM's 44.1 kHz here.
/// Linear interpolation; the frame before each block is kept so blocks join without a seam.
final class RateConverter: @unchecked Sendable {
    let ch: Int
    let step: Double                         // input frames per output frame
    private var pos = 0.0                    // next output position, input-frame units (-1 = prev)
    private let prev: UnsafeMutablePointer<Float>

    init(channels: Int, from inRate: Double, to outRate: Double) {
        ch = channels
        step = inRate > 0 && outRate > 0 ? inRate / outRate : 1
        prev = .allocate(capacity: ch); prev.initialize(repeating: 0, count: ch)
    }
    deinit { prev.deallocate() }

    var isIdentity: Bool { step == 1 }

    /// Converts n input frames into out (capacity maxOut frames); returns frames written.
    func process(_ inp: UnsafePointer<Float>, _ n: Int, _ out: UnsafeMutablePointer<Float>, maxOut: Int) -> Int {
        guard n > 0 else { return 0 }
        if isIdentity {
            let m = min(n, maxOut)
            out.update(from: inp, count: m * ch)
            return m
        }
        var m = 0
        while pos <= Double(n - 1) && m < maxOut {
            let i0 = Int(pos.rounded(.down)), t = Float(pos - Double(i0))
            for c in 0..<ch {
                let a = i0 < 0 ? prev[c] : inp[i0 * ch + c]
                let b = i0 + 1 <= n - 1 ? inp[(i0 + 1) * ch + c] : a
                out[m * ch + c] = a + (b - a) * t
            }
            m += 1
            pos += step
        }
        pos -= Double(n)
        if pos < -1 { pos = -1 }                 // maxOut cut the block short: drop, never read back further
        prev.update(from: inp + (n - 1) * ch, count: ch)
        return m
    }
}
