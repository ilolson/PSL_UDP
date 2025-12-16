//
//  ContentView.swift
//  WS2812
//
//  Created by isaac on 12/15/25.
//

import SwiftUI
import Combine
import CoreBluetooth

private let serviceUUID = CBUUID(string: "21436587-A9CB-ED0F-1032-547698BADCFE")
private let commandCharacteristicUUID = CBUUID(string: "0C1D2E3F-4051-6273-8495-A6B7C8D9EAFB")
private let maxLights = 300
private let bleDeviceName = "PSL Motion"
private let bleShortName = "PSL Mtn"

final class BLEManager: NSObject, ObservableObject {
    @Published var status: String = "scanning for PSL Motion"

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var commandCharacteristic: CBCharacteristic?
    private var serviceDiscoveryAttempts = 0

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
        let writeType: CBCharacteristicWriteType = characteristic.properties.contains(.writeWithoutResponse) ? .withoutResponse : .withResponse
        peripheral.writeValue(data, for: characteristic, type: writeType)
    }
}

extension BLEManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            status = "scanning..."
            central.scanForPeripherals(withServices: nil)
        default:
            status = "Bluetooth unavailable"
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String : Any], rssi RSSI: NSNumber) {
        let displayName = peripheral.name ?? advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? "device"
        let serviceUUIDs = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []
        guard displayName == bleDeviceName || displayName == bleShortName || serviceUUIDs.contains(serviceUUID) else {
            return
        }

        status = "connecting to \(displayName)"
        self.peripheral = peripheral
        central.stopScan()
        peripheral.delegate = self
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        status = "discovering services"
        serviceDiscoveryAttempts = 0
        peripheral.discoverServices(nil)
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        status = "connection failed"
        self.peripheral = nil
        commandCharacteristic = nil
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
            central.scanForPeripherals(withServices: nil)
        }
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        status = "disconnected"
        self.peripheral = nil
        commandCharacteristic = nil
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            central.scanForPeripherals(withServices: nil)
        }
    }
}

extension BLEManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            status = "service discovery error: \(error.localizedDescription)"
            scheduleServiceDiscovery(peripheral)
            return
        }
        guard let services = peripheral.services else {
            status = "service discovery returned empty list"
            scheduleServiceDiscovery(peripheral)
            return
        }
        status = "services: \(services.map(\.uuid.uuidString).joined(separator: ","))"
        if let service = services.first(where: { $0.uuid == serviceUUID }) {
            peripheral.discoverCharacteristics([commandCharacteristicUUID], for: service)
            return
        }
        scheduleServiceDiscovery(peripheral)
    }

    private func scheduleServiceDiscovery(_ peripheral: CBPeripheral) {
        serviceDiscoveryAttempts += 1
        guard serviceDiscoveryAttempts <= 5 else {
            status = "service not found"
            return
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) {
            peripheral.discoverServices(nil)
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

struct ContentView: View {
    @StateObject private var bleManager = BLEManager()
    @State private var hue: Double = 0
    @State private var brightness: Double = 50
    @State private var segmentStart = 1
    @State private var segmentEnd = maxLights

    var body: some View {
        VStack(spacing: 24) {
            Text(bleManager.status)
                .font(.headline)

            VStack(spacing: 8) {
                Text(String(format: "Hue: %.0f°", hue))
                Text(String(format: "Brightness: %.0f%%", brightness))
                Text("Segment: \(segmentStart) … \(segmentEnd)")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }

            sliderView(
                title: "Hue",
                value: $hue,
                range: 0...360,
                format: "%.0f°",
                onChange: { sendHue($0) }
            )

            sliderView(
                title: "Brightness",
                value: $brightness,
                range: 0...100,
                format: "%.0f%%",
                onChange: { sendBrightness($0) }
            )

            sliderView(
                title: "Segment Start",
                value: Binding(
                    get: { Double(segmentStart) },
                    set: { updateSegmentStart(Int($0.rounded())) }
                ),
                range: 1...Double(maxLights),
                format: "%.0f",
                onChange: { _ in }
            )

            sliderView(
                title: "Segment End",
                value: Binding(
                    get: { Double(segmentEnd) },
                    set: { updateSegmentEnd(Int($0.rounded())) }
                ),
                range: 1...Double(maxLights),
                format: "%.0f",
                onChange: { _ in }
            )
        }
        .padding()
    }

    private func sliderView(title: String, value: Binding<Double>, range: ClosedRange<Double>, format: String, onChange: @escaping (Double) -> Void) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("\(title): \(String(format: format, value.wrappedValue))")
                .fontWeight(.semibold)
            Slider(value: value, in: range)
                .onChange(of: value.wrappedValue) { _, newValue in
                    onChange(newValue)
                }
        }
        .padding(.horizontal)
    }

    private func sendHue(_ value: Double) {
        bleManager.sendCommand(String(format: "H_SET,%.1f", value))
    }

    private func sendBrightness(_ value: Double) {
        bleManager.sendCommand(String(format: "B_SET,%.1f", value))
    }

    private func updateSegmentStart(_ value: Int) {
        let clamped = max(1, min(value, maxLights))
        segmentStart = clamped
        if segmentEnd < segmentStart {
            segmentEnd = segmentStart
        }
        bleManager.sendCommand("SEG_START,\(segmentStart)")
    }

    private func updateSegmentEnd(_ value: Int) {
        let clamped = max(1, min(value, maxLights))
        segmentEnd = clamped
        if segmentStart > segmentEnd {
            segmentStart = segmentEnd
        }
        bleManager.sendCommand("SEG_END,\(segmentEnd)")
    }
}

#Preview {
    ContentView()
}
