import CoreBluetooth
import Foundation

/// Reads a game controller through the RetroPad bridge (bridge/esp32-pad).
///
/// watchOS apps can't use game controllers, but they can read any Bluetooth LE
/// service. The bridge, an ESP32, connects to the controller and re-sends its
/// state as one: an 11-byte packet (see bridge/esp32-pad/src/pad_state.h).
final class PadBridge: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    enum Status {
        case unavailable   // Bluetooth off or not allowed
        case searching     // looking for a bridge
        case noController  // bridge connected, no controller on it
        case connected     // bridge and controller connected
    }

    static let serviceUUID = CBUUID(string: "6D3C0001-5F8E-4B7A-9C1E-2A7B52504144")
    static let stateUUID = CBUUID(string: "6D3C0002-5F8E-4B7A-9C1E-2A7B52504144")

    // PAD_BTN_* in pad_state.h
    private enum Pad {
        static let a: UInt16 = 1 << 0, b: UInt16 = 1 << 1, x: UInt16 = 1 << 2, y: UInt16 = 1 << 3
        static let lb: UInt16 = 1 << 4, rb: UInt16 = 1 << 5, view: UInt16 = 1 << 6, menu: UInt16 = 1 << 7
        static let up: UInt16 = 1 << 10, down: UInt16 = 1 << 11, left: UInt16 = 1 << 12, right: UInt16 = 1 << 13
    }

    private let input: InputState
    private let onStatus: (Status) -> Void
    private let queue = DispatchQueue(label: "pad-bridge")
    private var central: CBCentralManager!
    private var bridge: CBPeripheral?

    /// `onStatus` is called on the main thread whenever the status changes.
    init(input: InputState, onStatus: @escaping (Status) -> Void) {
        self.input = input
        self.onStatus = onStatus
        super.init()
        central = CBCentralManager(delegate: self, queue: queue)
    }

    private var status = Status.searching {
        didSet {
            if status != oldValue {
                let status = status
                DispatchQueue.main.async { self.onStatus(status) }
            }
        }
    }

    private func scan() {
        guard central.state == .poweredOn else {
            status = .unavailable
            return
        }
        guard bridge == nil else { return }
        status = .searching
        central.scanForPeripherals(withServices: [Self.serviceUUID])
    }

    private func lost() {
        bridge = nil
        input.setGamepad(buttons: 0, stick: .zero)
        scan()
    }

    // MARK: CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        scan()
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        central.stopScan()
        bridge = peripheral
        peripheral.delegate = self
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([Self.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        lost()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        lost()
    }

    // MARK: CBPeripheralDelegate

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        for service in peripheral.services ?? [] where service.uuid == Self.serviceUUID {
            peripheral.discoverCharacteristics([Self.stateUUID], for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        for characteristic in service.characteristics ?? [] where characteristic.uuid == Self.stateUUID {
            peripheral.setNotifyValue(true, for: characteristic)
            peripheral.readValue(for: characteristic) // current state, before any change
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        // version, seq, buttons (2), lx, ly, rx, ry, lt, rt, flags (version 2+)
        guard let data = characteristic.value, data.count >= 10 else { return }
        let bytes = [UInt8](data)
        let controller = bytes[0] < 2 || (bytes.count >= 11 && bytes[10] & 0x01 != 0)
        status = controller ? .connected : .noController
        let pad = UInt16(bytes[2]) | UInt16(bytes[3]) << 8
        let lx = Int8(bitPattern: bytes[4]), ly = Int8(bitPattern: bytes[5])
        let rx = Int8(bitPattern: bytes[6]), ry = Int8(bitPattern: bytes[7])
        let lt = bytes[8]
        input.setGamepad(buttons: Self.n64Buttons(pad: pad, rx: rx, ry: ry, lt: lt),
                         stick: CGVector(dx: CGFloat(lx) / 127, dy: CGFloat(ly) / 127))
    }

    /// Xbox layout to N64: A = A, X or B = B, LT = Z, LB / RB = L / R,
    /// Menu = Start, right stick = C buttons, d-pad = d-pad.
    private static func n64Buttons(pad: UInt16, rx: Int8, ry: Int8, lt: UInt8) -> UInt16 {
        var b = 0
        func map(_ bit: UInt16, _ n64: Int) {
            if pad & bit != 0 { b |= n64 }
        }
        map(Pad.a, WATCH_BTN_A)
        map(Pad.x, WATCH_BTN_B)
        map(Pad.b, WATCH_BTN_B)
        map(Pad.lb, WATCH_BTN_L)
        map(Pad.rb, WATCH_BTN_R)
        map(Pad.menu, WATCH_BTN_START)
        map(Pad.up, WATCH_BTN_D_UP)
        map(Pad.down, WATCH_BTN_D_DOWN)
        map(Pad.left, WATCH_BTN_D_LEFT)
        map(Pad.right, WATCH_BTN_D_RIGHT)
        if lt > 100 { b |= WATCH_BTN_Z }
        if rx > 64 { b |= WATCH_BTN_C_RIGHT }
        if rx < -64 { b |= WATCH_BTN_C_LEFT }
        if ry > 64 { b |= WATCH_BTN_C_UP }
        if ry < -64 { b |= WATCH_BTN_C_DOWN }
        return UInt16(b)
    }
}
