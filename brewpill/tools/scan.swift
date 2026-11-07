// Listen for Brew Pill broadcasts with the Mac's Bluetooth. No installs needed:
//   swift tools/scan.swift [seconds]
import CoreBluetooth
import Foundation

final class Scanner: NSObject, CBCentralManagerDelegate {
    var central: CBCentralManager!

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        if c.state == .poweredOn {
            print("Scanning for BrewPill...")
            c.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
        } else {
            print("Bluetooth not available (state \(c.state.rawValue)); check System Settings > Privacy > Bluetooth")
        }
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData ad: [String: Any], rssi: NSNumber) {
        // Payload version 2: flags, pill ID, raw die temp, vdd, x/y/z mg (see brewpill/main.c)
        guard let d = ad[CBAdvertisementDataManufacturerDataKey] as? Data, d.count >= 18,
              d[d.startIndex] == 0xFF, d[d.startIndex + 1] == 0xFF, d[d.startIndex + 2] == 2 else { return }
        let b = [UInt8](d)
        func u16(_ i: Int) -> Int { Int(b[i]) | Int(b[i + 1]) << 8 }
        func s16(_ i: Int) -> Int { Int(Int16(bitPattern: UInt16(u16(i)))) }
        let acc = b[3] & 1 != 0 ? "" : "  (accelerometer not responding)"
        let id = b[4..<8].map { String(format: "%02X", $0) }.joined()
        print(String(format: "%@  RSSI %4d  x %+5d  y %+5d  z %+5d mg  %5.2f °C  %d mV%@",
                     id, rssi.intValue, s16(12), s16(14), s16(16),
                     Double(s16(8)) / 100, u16(10), acc))
    }
}

let seconds = CommandLine.arguments.count > 1 ? Double(CommandLine.arguments[1]) ?? 0 : 0
let scanner = Scanner()
if seconds > 0 { RunLoop.main.run(until: Date().addingTimeInterval(seconds)) } else { RunLoop.main.run() }
