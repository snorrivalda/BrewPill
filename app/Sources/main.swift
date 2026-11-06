// BrewPill.app: listens for Brew Pill BLE broadcasts, logs them, and serves a
// dashboard on http://localhost:8088 that is shown in the app window and can
// also be opened in any browser on this Mac.
import Cocoa
import CoreBluetooth
import Network
import WebKit

let port: UInt16 = 8088
let dashboardURL = URL(string: "http://localhost:\(port)/")!

let dataDir: URL = {
    let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("BrewPill")
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    return dir
}()

// MARK: - Readings

struct Reading {
    var t: Double           // unix seconds
    var device: String
    var tilt: Double        // as computed on the pill
    var temp: Double        // °C, as reported by the pill
    var vdd: Int            // mV
    var sg: Double          // as computed on the pill
    var rssi: Int
    var x: Int, y: Int, z: Int   // mg
    var flags: Int

    static let csvHeader = "t,device,tilt,temp,vdd,sg,rssi,x,y,z,flags\n"

    var csv: String {
        String(format: "%.0f,%@,%.2f,%.2f,%d,%.4f,%d,%d,%d,%d,%d\n",
               t, device, tilt, temp, vdd, sg, rssi, x, y, z, flags)
    }

    init(t: Double, device: String, tilt: Double, temp: Double, vdd: Int, sg: Double,
         rssi: Int, x: Int, y: Int, z: Int, flags: Int) {
        (self.t, self.device, self.tilt, self.temp, self.vdd, self.sg) = (t, device, tilt, temp, vdd, sg)
        (self.rssi, self.x, self.y, self.z, self.flags) = (rssi, x, y, z, flags)
    }

    init?(csv line: Substring) {
        let f = line.split(separator: ",").map(String.init)
        guard f.count == 11, let t = Double(f[0]) else { return nil }
        self.init(t: t, device: f[1], tilt: Double(f[2]) ?? 0, temp: Double(f[3]) ?? 0,
                  vdd: Int(f[4]) ?? 0, sg: Double(f[5]) ?? 0, rssi: Int(f[6]) ?? 0,
                  x: Int(f[7]) ?? 0, y: Int(f[8]) ?? 0, z: Int(f[9]) ?? 0, flags: Int(f[10]) ?? 0)
    }

    var json: [String: Any] {
        ["t": t, "device": device, "tilt": tilt, "temp": temp, "vdd": vdd, "sg": sg,
         "rssi": rssi, "x": x, "y": y, "z": z, "flags": flags]
    }

    var row: [Any] { [t, tilt, temp, vdd, sg, rssi, x, y, z, flags] }
}

/// Keeps the latest broadcast per pill and logs one reading per measurement to CSV.
final class Store {
    let csvURL = dataDir.appendingPathComponent("readings.csv")
    let stateURL = dataDir.appendingPathComponent("state.json")
    private(set) var live: [String: Reading] = [:]
    private(set) var rows: [Reading] = []
    private var lastLogged: [String: Reading] = [:]

    init() {
        if let text = try? String(contentsOf: csvURL, encoding: .utf8) {
            rows = text.split(separator: "\n").dropFirst().compactMap { Reading(csv: $0) }
            for r in rows { lastLogged[r.device] = r }
        } else {
            try? Reading.csvHeader.write(to: csvURL, atomically: true, encoding: .utf8)
        }
    }

    func add(_ r: Reading) {
        live[r.device] = r
        // The pill re-broadcasts each measurement for measure_s seconds; log it once,
        // plus a keep-alive every 5 minutes if nothing changes.
        if let last = lastLogged[r.device] {
            let changed = last.x != r.x || last.y != r.y || last.z != r.z || last.temp != r.temp
            if r.t - last.t < 20 || (!changed && r.t - last.t < 300) { return }
        }
        lastLogged[r.device] = r
        rows.append(r)
        if let h = try? FileHandle(forWritingTo: csvURL) {
            h.seekToEndOfFile()
            h.write(r.csv.data(using: .utf8)!)
            try? h.close()
        }
    }

    var state: Data {
        get { (try? Data(contentsOf: stateURL)) ?? Data("{}".utf8) }
        set { try? newValue.write(to: stateURL, options: .atomic) }
    }
}

// MARK: - Bluetooth

final class Scanner: NSObject, CBCentralManagerDelegate {
    private var central: CBCentralManager!
    private let store: Store
    private(set) var status = "starting"

    init(store: Store) {
        self.store = store
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        switch c.state {
        case .poweredOn:
            status = "scanning"
            c.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
        case .unauthorized: status = "Bluetooth permission denied: allow BrewPill in System Settings > Privacy & Security > Bluetooth"
        case .poweredOff: status = "Bluetooth is turned off"
        case .unsupported: status = "Bluetooth LE is not supported on this Mac"
        default: status = "waiting for Bluetooth"
        }
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData ad: [String: Any], rssi: NSNumber) {
        // Manufacturer data: company 0xFFFF, version 1, then the payload from main.c
        guard let d = ad[CBAdvertisementDataManufacturerDataKey] as? Data, d.count >= 18 else { return }
        let b = [UInt8](d)
        guard b[0] == 0xFF, b[1] == 0xFF, b[2] == 1 else { return }
        func u16(_ i: Int) -> Int { Int(b[i]) | Int(b[i + 1]) << 8 }
        func s16(_ i: Int) -> Int { Int(Int16(bitPattern: UInt16(u16(i)))) }
        store.add(Reading(t: Date().timeIntervalSince1970, device: p.identifier.uuidString,
                          tilt: Double(u16(4)) / 100, temp: Double(s16(6)) / 100, vdd: u16(8),
                          sg: Double(u16(10)) / 10000, rssi: rssi.intValue,
                          x: s16(12), y: s16(14), z: s16(16), flags: Int(b[3])))
    }
}

// MARK: - Writing the pill's config page over the ST-Link

enum PillWriter {
    static let openocd = NSHomeDirectory() + "/.platformio/packages/tool-openocd"

    /// Same layout as struct config in brewpill/config.h
    static func blob(_ req: [String: Any]) -> Data {
        var d = Data()
        func put<T: FixedWidthInteger>(_ v: T) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        let poly = (req["poly"] as? [Double] ?? []) + [0, 0, 0, 0]
        put(UInt32(0x31435042))
        put(UInt16(clamping: req["measure"] as? Int ?? 60))
        put(UInt16(clamping: req["adv"] as? Int ?? 2000))
        put(UInt8(clamping: req["axis"] as? Int ?? 0))
        put(UInt8(clamping: req["color"] as? Int ?? 1))
        put(Int16(clamping: Int((((req["tempOffset"] as? Double) ?? 0) * 100).rounded())))
        for p in poly.prefix(4) { put(Float(p).bitPattern) }
        return d
    }

    static func write(_ req: [String: Any], done: @escaping (Bool, String) -> Void) {
        let file = FileManager.default.temporaryDirectory.appendingPathComponent("brewpill-config.bin")
        do { try blob(req).write(to: file) } catch { return done(false, "\(error)") }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: openocd + "/bin/openocd")
        p.arguments = ["-s", openocd + "/openocd/scripts", "-f", "interface/stlink.cfg",
                       "-f", "target/nordic/nrf51.cfg",
                       "-c", "program {\(file.path)} 0x3fc00 verify reset exit"]
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        DispatchQueue.global().async {
            var ok = false, out = ""
            do {
                try p.run()
                out = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
                p.waitUntilExit()
                ok = p.terminationStatus == 0 && out.contains("Verified OK")
            } catch {
                out = "Could not run OpenOCD at \(openocd): \(error)"
            }
            DispatchQueue.main.async { done(ok, out) }
        }
    }
}

// MARK: - HTTP server

struct Request {
    var method: String, path: String, query: [String: String]
    var headers: [String: String], body: Data

    static func parse(_ buf: Data) -> Request? {
        guard let end = buf.range(of: Data("\r\n\r\n".utf8)) else { return nil }
        let head = String(decoding: buf[..<end.lowerBound], as: UTF8.self).components(separatedBy: "\r\n")
        let parts = head[0].split(separator: " ")
        guard parts.count >= 2 else { return nil }
        var headers: [String: String] = [:]
        for line in head.dropFirst() {
            if let i = line.firstIndex(of: ":") {
                headers[line[..<i].lowercased()] = line[line.index(after: i)...].trimmingCharacters(in: .whitespaces)
            }
        }
        let length = Int(headers["content-length"] ?? "0") ?? 0
        let body = buf[end.upperBound...]
        guard body.count >= length else { return nil }
        let comps = URLComponents(string: String(parts[1]))
        var query: [String: String] = [:]
        for q in comps?.queryItems ?? [] { query[q.name] = q.value ?? "" }
        return Request(method: String(parts[0]), path: comps?.path ?? "/", query: query,
                       headers: headers, body: Data(body.prefix(length)))
    }
}

final class Server {
    private let listener: NWListener
    private let store: Store
    private let scanner: Scanner

    init(store: Store, scanner: Scanner) throws {
        self.store = store
        self.scanner = scanner
        let params = NWParameters.tcp
        params.requiredInterfaceType = .loopback       // this Mac only
        params.allowLocalEndpointReuse = true
        listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: port)!)
        listener.newConnectionHandler = { [weak self] c in
            c.start(queue: .main)
            self?.receive(c, Data())
        }
        listener.start(queue: .main)
    }

    private func receive(_ c: NWConnection, _ buf: Data) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, done, error in
            var buf = buf
            if let data { buf.append(data) }
            if let req = Request.parse(buf) {
                self?.handle(req) { status, type, body in Server.send(c, status, type, body) }
            } else if done || error != nil || buf.count > 2_000_000 {
                c.cancel()
            } else {
                self?.receive(c, buf)
            }
        }
    }

    private static func send(_ c: NWConnection, _ status: Int, _ type: String, _ body: Data) {
        let reason = [200: "OK", 400: "Bad Request", 403: "Forbidden", 404: "Not Found", 500: "Error"][status] ?? "OK"
        let head = "HTTP/1.1 \(status) \(reason)\r\nContent-Type: \(type)\r\nContent-Length: \(body.count)\r\n" +
                   "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
        c.send(content: Data(head.utf8) + body, completion: .contentProcessed { _ in c.cancel() })
    }

    private func json(_ obj: Any) -> Data {
        (try? JSONSerialization.data(withJSONObject: obj)) ?? Data("{}".utf8)
    }

    private func handle(_ r: Request, reply: @escaping (Int, String, Data) -> Void) {
        let jsonType = "application/json"
        // Refuse other websites: the Host must be ours (DNS rebinding), and writes
        // need a custom header, which cross-origin pages can't send without CORS.
        let host = r.headers["host"] ?? ""
        guard host == "localhost:\(port)" || host == "127.0.0.1:\(port)" else {
            return reply(403, "text/plain", Data("Forbidden".utf8))
        }
        if r.method != "GET" && r.headers["x-brewpill"] != "1" {
            return reply(403, "text/plain", Data("Forbidden".utf8))
        }

        switch (r.method, r.path) {
        case ("GET", "/"):
            let url = Bundle.main.url(forResource: "index", withExtension: "html")
            let page = url.flatMap { try? Data(contentsOf: $0) } ?? Data("index.html missing".utf8)
            reply(200, "text/html; charset=utf-8", page)

        case ("GET", "/api/live"):
            let devices = store.live.values.sorted { $0.t > $1.t }.map { $0.json }
            reply(200, jsonType, json(["bluetooth": scanner.status, "now": Date().timeIntervalSince1970,
                                       "devices": devices]))

        case ("GET", "/api/readings"):
            let device = r.query["device"]
            let since = Double(r.query["since"] ?? "") ?? 0
            let until = Double(r.query["until"] ?? "") ?? .infinity
            let rows = store.rows.filter { (device == nil || $0.device == device) && $0.t >= since && $0.t <= until }
            reply(200, jsonType, json(["columns": ["t", "tilt", "temp", "vdd", "sg", "rssi", "x", "y", "z", "flags"],
                                       "rows": rows.map { $0.row }]))

        case ("GET", "/api/readings.csv"):
            reply(200, "text/csv", (try? Data(contentsOf: store.csvURL)) ?? Data())

        case ("GET", "/api/state"):
            reply(200, jsonType, store.state)

        case ("PUT", "/api/state"):
            guard (try? JSONSerialization.jsonObject(with: r.body)) is [String: Any] else {
                return reply(400, "text/plain", Data("Expected a JSON object".utf8))
            }
            store.state = r.body
            reply(200, jsonType, json(["ok": true]))

        case ("POST", "/api/write-pill"):
            guard let req = (try? JSONSerialization.jsonObject(with: r.body)) as? [String: Any] else {
                return reply(400, "text/plain", Data("Expected a JSON object".utf8))
            }
            PillWriter.write(req) { [weak self] ok, out in
                reply(200, jsonType, self?.json(["ok": ok, "output": String(out.suffix(1500))]) ?? Data())
            }

        default:
            reply(404, "text/plain", Data("Not found".utf8))
        }
    }
}

// MARK: - App

final class AppDelegate: NSObject, NSApplicationDelegate {
    var window: NSWindow!
    var store: Store!
    var scanner: Scanner!
    var server: Server?

    func applicationDidFinishLaunching(_ note: Notification) {
        store = Store()
        scanner = Scanner(store: store)
        do {
            server = try Server(store: store, scanner: scanner)
        } catch {
            let alert = NSAlert()
            alert.messageText = "BrewPill couldn't start its dashboard on port \(port)"
            alert.informativeText = "Is another copy of BrewPill already running?\n\n\(error)"
            alert.runModal()
            NSApp.terminate(nil)
        }
        buildMenu()

        let web = WKWebView(frame: .zero)
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1100, height: 780),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        window.title = "BrewPill"
        window.contentView = web
        window.setFrameAutosaveName("BrewPillMain")
        window.center()
        window.makeKeyAndOrderFront(nil)
        // Give the listener a moment to come up before loading
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { web.load(URLRequest(url: dashboardURL)) }
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { false }

    func applicationShouldHandleReopen(_ app: NSApplication, hasVisibleWindows: Bool) -> Bool {
        window.makeKeyAndOrderFront(nil)
        return true
    }

    @objc func openInBrowser() { NSWorkspace.shared.open(dashboardURL) }
    @objc func showDataFolder() { NSWorkspace.shared.open(dataDir) }
    @objc func reload() { (window.contentView as? WKWebView)?.reload() }

    private func buildMenu() {
        let main = NSMenu()
        let appItem = NSMenuItem(); main.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "Open Dashboard in Browser", action: #selector(openInBrowser), keyEquivalent: "b")
        appMenu.addItem(withTitle: "Show Data Folder", action: #selector(showDataFolder), keyEquivalent: "")
        appMenu.addItem(withTitle: "Reload", action: #selector(reload), keyEquivalent: "r")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide BrewPill", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appMenu.addItem(withTitle: "Quit BrewPill", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu

        // Edit menu so copy/paste work in the dashboard's fields
        let editItem = NSMenuItem(); main.addItem(editItem)
        let edit = NSMenu(title: "Edit")
        edit.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        edit.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "Z")
        edit.addItem(.separator())
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editItem.submenu = edit

        let winItem = NSMenuItem(); main.addItem(winItem)
        let win = NSMenu(title: "Window")
        win.addItem(withTitle: "Minimize", action: #selector(NSWindow.miniaturize(_:)), keyEquivalent: "m")
        win.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        winItem.submenu = win
        NSApp.mainMenu = main
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
