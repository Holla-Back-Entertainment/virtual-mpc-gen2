import AppKit
import WebKit

// Virtual MPC: native shell for the lab. Starts/attaches the VM, shows the panel page,
// and hands audio + MIDI to PCMPlayer / MIDIBridge (other files in this folder).

struct Model {
    let id: String, title: String, panel: Int, midi: UInt16, pcm: UInt16
    /// The model's devices in the Virtual MPC driver and their channel map (AudioRouting.swift).
    let audio: AudioMap
    /// Rear MIDI ports beyond External MIDI: (name suffix, lab TCP port, Mac -> MPC?). MIDI In n
    /// are CoreMIDI destinations, Out n sources; lab.py PORTS midi_*.
    var rearMIDI: [(String, UInt16, Bool)] = []
    static let all = [Model(id: "acvc", title: "MPC XL", panel: 8865, midi: 5652, pcm: 5657, audio: .xl,
                            rearMIDI: [("MIDI In 1", 5751, true), ("MIDI In 2", 5752, true),
                                       ("MIDI Out A", 5753, false), ("MIDI Out B", 5754, false),
                                       ("MIDI Out C", 5755, false), ("MIDI Out D", 5756, false)]),
                      Model(id: "acvg", title: "MPC Live III", panel: 8875, midi: 5662, pcm: 5667, audio: .live3,
                            rearMIDI: [("MIDI In 1", 5761, true), ("MIDI In 2", 5762, true),
                                       ("MIDI Out A", 5763, false), ("MIDI Out B", 5764, false)])]

    /// CoreMIDI ports: External MIDI keeps one two-way port "Virtual <title>"; the rear ports are one-way.
    var midiPorts: [(name: String, port: UInt16, toMPC: Bool, fromMPC: Bool)] {
        [("Virtual \(title)", midi, true, true)] +
            rearMIDI.map { ("Virtual \(title) \($0.0)", $0.1, $0.2, !$0.2) }
    }
}

enum State: String { case stopped, starting, running, stopping = "shutting down" }

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let defaults = UserDefaults.standard
    var model = Model.all[0]
    var state = State.stopped { didSet { updateTitle() } }
    var weStarted = false
    var vm: Process?
    var player: PCMPlayer?
    var bridges: [MIDIBridge] = []
    var window: NSWindow!
    var web: WKWebView!
    var activity: NSObjectProtocol?
    var quitting = false
    let logURL = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/VirtualMPC.log")

    var repo: String { defaults.string(forKey: "repo") ?? NSString("~/Projects/mpc-sample/lab/virtual-mpc-gen2").expandingTildeInPath }

    func applicationDidFinishLaunching(_ n: Notification) {
        defaults.register(defaults: ["hearDirect": true])
        activity = ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .latencyCritical], reason: "Virtual MPC audio and MIDI")
        model = Model.all.first { $0.id == defaults.string(forKey: "model") } ?? Model.all[0]
        buildWindow()
        buildMenu()
        NSApp.activate(ignoringOtherApps: true)
        start()
    }

    // MARK: window
    func buildWindow() {
        let cfg = WKWebViewConfiguration()
        let css = "var s=document.createElement('style');s.textContent='#sound{display:none !important}';(document.head||document.documentElement).appendChild(s);"
        cfg.userContentController.addUserScript(WKUserScript(source: css, injectionTime: .atDocumentEnd, forMainFrameOnly: true))
        web = WKWebView(frame: .zero, configuration: cfg)
        web.uiDelegate = self
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1280, height: 900),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.contentView = web
        window.delegate = self
        window.center()
        window.makeKeyAndOrderFront(nil)
        updateTitle()
    }

    func updateTitle() { window?.title = "Virtual MPC \(model.title.replacingOccurrences(of: "MPC ", with: "")) \u{2014} \(state.rawValue)" }
    func windowShouldClose(_ s: NSWindow) -> Bool { NSApp.terminate(nil); return false }

    // MARK: menu
    func buildMenu() {
        let main = NSMenu()
        func top(_ title: String) -> NSMenu {
            let item = NSMenuItem(); main.addItem(item)
            let m = NSMenu(title: title); item.submenu = m; return m
        }
        let app = top("Virtual MPC")
        app.addItem(withTitle: "Quit Virtual MPC", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        let edit = top("Edit")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        let mm = top("Model"); mm.delegate = self
        let am = top("Audio"); am.delegate = self
        let vm = top("MPC")
        vm.addItem(withTitle: "Start MPC", action: #selector(startAction), keyEquivalent: "")
        vm.addItem(withTitle: "Stop MPC", action: #selector(stopAction), keyEquivalent: "")
        vm.addItem(withTitle: "Reload Panel", action: #selector(reloadAction), keyEquivalent: "r")
        NSApp.mainMenu = main
        modelMenu = mm; audioMenu = am
    }
    var modelMenu: NSMenu!, audioMenu: NSMenu!

    @objc func startAction() { start() }
    @objc func reloadAction() { web.reload() }
    @objc func stopAction() { Task { await stopVM(); } }
    @objc func pickModel(_ s: NSMenuItem) {
        guard let m = Model.all.first(where: { $0.id == s.representedObject as? String }), m.id != model.id else { return }
        defaults.set(m.id, forKey: "model")
        Task {
            if weStarted { await stopVM() } else { stopBridges() }
            model = m; state = .stopped
            start()
        }
    }
    @objc func pickAudio(_ s: NSMenuItem) {
        let uid = s.representedObject as? String
        defaults.set(uid, forKey: "audioDevice")
        player?.deviceUID = uid
    }
    @objc func toggleHear(_ s: NSMenuItem) {
        let on = !defaults.bool(forKey: "hearDirect")
        defaults.set(on, forKey: "hearDirect")
        player?.hearDirect = on
    }

    // MARK: VM
    func python() -> String {
        for p in ["/opt/homebrew/bin/python3", "/Users/\(NSUserName())/miniconda3/bin/python3", "/usr/local/bin/python3", "/usr/bin/python3"]
            where FileManager.default.isExecutableFile(atPath: p) { return p }
        return "/usr/bin/python3"
    }

    func runLab(_ args: [String]) -> Process {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: python())
        p.arguments = ["lab.py"] + args
        p.currentDirectoryURL = URL(fileURLWithPath: repo)
        var env = ProcessInfo.processInfo.environment
        env["MODEL"] = model.id; env["PYTHONUNBUFFERED"] = "1"
        env["PATH"] = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
        p.environment = env
        if let fh = sharedLog { p.standardOutput = fh; p.standardError = fh }
        return p
    }

    lazy var sharedLog: FileHandle? = logHandle()   // one handle for every child process (was one leaked fd per run)

    func logHandle() -> FileHandle? {
        if !FileManager.default.fileExists(atPath: logURL.path) { FileManager.default.createFile(atPath: logURL.path, contents: nil) }
        guard let fh = try? FileHandle(forWritingTo: logURL) else { return nil }
        _ = try? fh.seekToEnd(); return fh
    }

    func log(_ s: String) {
        if let fh = logHandle() { fh.write(Data("[app] \(s)\n".utf8)); try? fh.close() }
    }

    nonisolated static func linked(panel: Int) async -> Bool {
        guard let url = URL(string: "http://127.0.0.1:\(panel)/surface") else { return false }
        var req = URLRequest(url: url); req.timeoutInterval = 2
        guard let (data, _) = try? await URLSession.shared.data(for: req),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return false }
        return (j["link"] as? Bool) == true
    }

    func start() {
        guard state == .stopped else { return }
        state = .starting
        let m = model
        Task {
            let attach = await Self.linked(panel: m.panel)
            guard model.id == m.id, state == .starting else { return }   // stopped or switched meanwhile
            if attach {
                log("attached to running \(m.title)")
                weStarted = false
            } else {
                let p = runLab(["run", "--headless"])
                do { try p.run() } catch {
                    log("could not start lab.py: \(error)"); state = .stopped; alert("Could not start lab.py", "\(error)\nRepo: \(repo)"); return
                }
                vm = p; weStarted = true
                log("started \(m.title) (pid \(p.processIdentifier)), MODEL=\(m.id)")
                var up = false
                for _ in 0..<600 {                       // up to 10 min
                    if model.id != m.id || state != .starting { return }
                    if await Self.linked(panel: m.panel) { up = true; break }
                    if !p.isRunning { break }
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                }
                guard up else {
                    log("VM did not link"); if p.isRunning { p.interrupt() }   // SIGINT: lab.py's cleanup stops QEMU; SIGTERM would orphan it
                    vm = nil; weStarted = false; state = .stopped
                    alert("The \(m.title) did not come up", "See ~/Library/Logs/VirtualMPC.log"); return
                }
                if !defaults.bool(forKey: "firstrun-\(m.id)") {
                    let f = runLab(["firstrun"])
                    try? f.run(); await Task.detached { f.waitUntilExit() }.value
                    defaults.set(true, forKey: "firstrun-\(m.id)")
                }
            }
            guard model.id == m.id, state == .starting else { return }
            state = .running
            startBridges()
            web.load(URLRequest(url: URL(string: "http://127.0.0.1:\(m.panel)/")!))
        }
    }

    func startBridges() {
        stopBridges()   // never two players fighting over the one audio socket
        let pl = PCMPlayer(port: model.pcm, panel: model.panel, map: model.audio)
        pl.deviceUID = defaults.string(forKey: "audioDevice")
        pl.hearDirect = defaults.bool(forKey: "hearDirect")
        pl.start(); player = pl
        for s in model.midiPorts {
            let b = MIDIBridge(name: s.name, port: s.port, toMPC: s.toMPC, fromMPC: s.fromMPC)
            do { try b.start(); bridges.append(b) } catch { log("MIDI bridge \(s.name) failed: \(error)") }
        }
    }

    func stopBridges() { bridges.forEach { $0.stop() }; bridges = []; player?.stop(); player = nil }

    func stopVM() async {
        guard state != .stopping else { return }
        state = .stopping
        stopBridges()
        let p = runLab(["stop"])
        if (try? p.run()) != nil {
            let done = await withTaskGroup(of: Bool.self) { g in
                g.addTask { await Task.detached { p.waitUntilExit() }.value; return true }
                g.addTask { try? await Task.sleep(nanoseconds: 90_000_000_000); return false }
                let r = await g.next() ?? false; g.cancelAll(); return r
            }
            if !done { p.terminate(); log("lab.py stop timed out") }
        }
        if let v = vm, v.isRunning { v.interrupt() }   // lab.py run exits on its own once stopped
        vm = nil; weStarted = false
        state = .stopped
    }

    func alert(_ title: String, _ text: String) {
        let a = NSAlert(); a.messageText = title; a.informativeText = text; a.runModal()
    }

    // MARK: quit
    func applicationShouldTerminate(_ app: NSApplication) -> NSApplication.TerminateReply {
        if quitting { return .terminateLater }
        quitting = true
        stopBridges()
        guard weStarted, state != .stopped else { return .terminateNow }
        Task { await stopVM(); NSApp.reply(toApplicationShouldTerminate: true) }
        return .terminateLater
    }
}

extension AppDelegate: NSMenuDelegate, WKUIDelegate {
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        if menu === modelMenu {
            for m in Model.all {
                let i = NSMenuItem(title: m.title, action: #selector(pickModel(_:)), keyEquivalent: "")
                i.representedObject = m.id; i.state = m.id == model.id ? .on : .off; menu.addItem(i)
            }
        } else if menu === audioMenu {
            let cur = defaults.string(forKey: "audioDevice")
            let d = NSMenuItem(title: "System Default", action: #selector(pickAudio(_:)), keyEquivalent: "")
            d.state = cur == nil ? .on : .off; menu.addItem(d)
            for dev in PCMPlayer.outputDevices() {
                let i = NSMenuItem(title: dev.name, action: #selector(pickAudio(_:)), keyEquivalent: "")
                i.representedObject = dev.uid; i.state = dev.uid == cur ? .on : .off; menu.addItem(i)
            }
            menu.addItem(.separator())
            let h = NSMenuItem(title: "Hear MPC Directly", action: #selector(toggleHear(_:)), keyEquivalent: "")
            h.state = defaults.bool(forKey: "hearDirect") ? .on : .off; menu.addItem(h)
            let m = NSMenuItem(title: "Routing: \(player?.mode ?? "off")", action: nil, keyEquivalent: ""); m.isEnabled = false
            menu.addItem(m)
        }
    }

    // The page's mic input: grant without a second prompt (macOS already asks once).
    func webView(_ w: WKWebView, requestMediaCapturePermissionFor o: WKSecurityOrigin, initiatedByFrame f: WKFrameInfo,
                 type: WKMediaCaptureType, decisionHandler: @escaping (WKPermissionDecision) -> Void) {
        decisionHandler(o.host == "127.0.0.1" ? .grant : .deny)
    }
}

@main
enum Main {
    @MainActor static func main() {
        if CommandLine.arguments.contains("--selftest") {
            MIDIBridge.selfTest(); PCMPlayer.selfTest(); Model.selfTest(); exit(0)
        }
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.delegate = delegate
        app.setActivationPolicy(.regular)
        app.run()
    }
}
