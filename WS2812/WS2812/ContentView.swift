//
//  ContentView.swift
//  WS2812
//
//  Created by isaac on 12/15/25.
//

import SwiftUI
import Combine
import CoreBluetooth
import CoreMotion

private let serviceUUID = CBUUID(string: "21436587-A9CB-ED0F-1032-547698BADCFE")
private let commandCharacteristicUUID = CBUUID(string: "0C1D-2E3F-4051-6273-8495A6B7C8D9EAFB")
private let maxLights = 300

final class BLEManager: NSObject, ObservableObject {
    @Published var status: String = "scanning for PSL Motion"

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var commandCharacteristic: CBCharacteristic?

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    func sendCommand(_ text: String) {
        guard let peripheral = peripheral,
              let characteristic = commandCharacteristic else {
            status = "waiting for Peripheral"
            return
        }
        let data = Data(text.utf8)
        peripheral.writeValue(data, for: characteristic, type: .withoutResponse)
    }
}

extension BLEManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            status = "scanning..."
            central.scanForPeripherals(withServices: [serviceUUID])
        default:
            status = "Bluetooth unavailable"
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String : Any], rssi RSSI: NSNumber) {
        self.peripheral = peripheral
        status = "connecting to \(peripheral.name ?? "device")"
        central.stopScan()
        peripheral.delegate = self
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        status = "discovering services"
        peripheral.discoverServices([serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        status = "connection failed"
        central.scanForPeripherals(withServices: [serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        status = "disconnected"
        commandCharacteristic = nil
        central.scanForPeripherals(withServices: [serviceUUID])
    }
}

extension BLEManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }
        for service in services where service.uuid == serviceUUID {
            peripheral.discoverCharacteristics([commandCharacteristicUUID], for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let characteristics = service.characteristics else { return }
        for characteristic in characteristics where characteristic.uuid == commandCharacteristicUUID {
            commandCharacteristic = characteristic
            status = "connected to PSL Motion"
            return
        }
    }
}

final class MotionManager: ObservableObject {
    @Published var yaw: Double = 0
    @Published var pitch: Double = 0
    @Published var roll: Double = 0

    private let motion = CMMotionManager()
    private let queue = OperationQueue()

    init() {
        motion.deviceMotionUpdateInterval = 1.0 / 30.0
        motion.startDeviceMotionUpdates(to: queue) { [weak self] motion, _ in
            guard let motion = motion else { return }
            DispatchQueue.main.async {
                self?.yaw = motion.attitude.yaw
                self?.pitch = motion.attitude.pitch
                self?.roll = motion.attitude.roll
            }
        }
    }

    deinit {
        motion.stopDeviceMotionUpdates()
    }
}

struct ContentView: View {
    @StateObject private var bleManager = BLEManager()
    @StateObject private var motionManager = MotionManager()

    @State private var holdingHue = false
    @State private var holdingBrightness = false
    @State private var holdingStart = false
    @State private var holdingEnd = false

    @State private var lastHueSent: Double?
    @State private var lastBrightnessSent: Double?
    @State private var segmentStart = 1
    @State private var segmentEnd = maxLights

    var body: some View {
        VStack(spacing: 24) {
            Text(bleManager.status)
                .font(.headline)

            VStack(spacing: 8) {
                Text(String(format: "Hue: %.0f°", lastHueSent ?? 0))
                Text(String(format: "Brightness: %.0f%%", lastBrightnessSent ?? 50))
                Text("Segment: \(segmentStart) … \(segmentEnd)")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }

            Grid(alignment: .center, horizontalSpacing: 12, verticalSpacing: 12) {
                GridRow {
                    holdButton(title: "Hue", isActive: holdingHue, action: { holdingHue = $0 })
                    holdButton(title: "Brightness", isActive: holdingBrightness, action: { holdingBrightness = $0 })
                }
                GridRow {
                    holdButton(title: "Start Endpoint", isActive: holdingStart, action: { holdingStart = $0 })
                    holdButton(title: "End Endpoint", isActive: holdingEnd, action: { holdingEnd = $0 })
                }
            }
            .padding(.horizontal)
        }
        .padding()
        .onChange(of: motionManager.yaw) { _, newValue in
            updateHue(with: newValue)
            updateSegmentStart(with: newValue)
        }
        .onChange(of: motionManager.pitch) { _, newValue in
            updateBrightness(with: newValue)
        }
        .onChange(of: motionManager.roll) { _, newValue in
            updateSegmentEnd(with: newValue)
        }
    }

    private func holdButton(title: String, isActive: Bool, action: @escaping (Bool) -> Void) -> some View {
        Button(action: {}) {
            Text(title)
                .bold()
                .frame(maxWidth: .infinity)
                .padding()
                .background(isActive ? Color.blue.opacity(0.8) : Color.gray.opacity(0.2))
                .cornerRadius(12)
        }
        .onLongPressGesture(minimumDuration: 0.05, pressing: { pressing in
            action(pressing)
        }, perform: {})
    }

    private func updateHue(with yaw: Double) {
        guard holdingHue else { return }
        let normalized = normalize(yaw, from: -.pi, to: .pi)
        let hue = normalized * 360
        if lastHueSent == nil || abs(hue - lastHueSent!) >= 0.3 {
            lastHueSent = hue
            bleManager.sendCommand(String(format: "H_SET,%.1f", hue))
        }
    }

    private func updateBrightness(with pitch: Double) {
        guard holdingBrightness else { return }
        let normalized = normalize(pitch, from: -.pi / 2, to: .pi / 2)
        let brightness = normalized * 100
        if lastBrightnessSent == nil || abs(brightness - (lastBrightnessSent ?? brightness)) >= 0.2 {
            lastBrightnessSent = brightness
            bleManager.sendCommand(String(format: "B_SET,%.1f", brightness))
        }
    }

    private func updateSegmentStart(with yaw: Double) {
        guard holdingStart else { return }
        let normalized = normalize(yaw, from: -.pi, to: .pi)
        let index = max(1, min(maxLights, Int(round(normalized * Double(maxLights - 1))) + 1))
        guard index != segmentStart else { return }
        segmentStart = index
        bleManager.sendCommand("SEG_START,\(segmentStart)")
        if segmentEnd < segmentStart {
            segmentEnd = segmentStart
            bleManager.sendCommand("SEG_END,\(segmentEnd)")
        }
    }

    private func updateSegmentEnd(with roll: Double) {
        guard holdingEnd else { return }
        let normalized = normalize(roll, from: -.pi, to: .pi)
        let index = max(1, min(maxLights, Int(round(normalized * Double(maxLights - 1))) + 1))
        guard index != segmentEnd else { return }
        segmentEnd = index
        bleManager.sendCommand("SEG_END,\(segmentEnd)")
        if segmentStart > segmentEnd {
            segmentStart = segmentEnd
            bleManager.sendCommand("SEG_START,\(segmentStart)")
        }
    }

    private func normalize(_ value: Double, from lower: Double, to upper: Double) -> Double {
        let clamped = min(max(value, lower), upper)
        return (clamped - lower) / (upper - lower)
    }
}

#Preview {
    ContentView()
}
