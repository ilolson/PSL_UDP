//
//  ContentView.swift
//  WS2812 Controller
//
//  Created by isaac on 12/15/25.
//

import Combine
import CoreMotion
import Foundation
import Network
import NetworkExtension
import SwiftUI

struct ContentView: View {
    @StateObject private var motionController = MotionUDPManager()
    @State private var manualHueText = ""
    @State private var manualBrightnessText = ""

    var body: some View {
        VStack(spacing: 18) {
            VStack(spacing: 4) {
                Text("PSL WS2812 Controller")
                    .font(.title2)
                    .bold()
                Text("Auto-connecting to \(MotionUDPManager.wifiSSID) for motion-driven lighting.")
                    .font(.footnote)
                    .multilineTextAlignment(.center)
                    .foregroundColor(.secondary)
            }

            StatusCard(status: motionController.connectionStatus, ready: motionController.isReady)

            HStack(spacing: 16) {
                MetricCard(label: "Pitch", value: motionController.pitchDegrees, suffix: "°")
                MetricCard(label: "Roll", value: motionController.rollDegrees, suffix: "°")
                MetricCard(label: "Yaw", value: motionController.yawDegrees, suffix: "°")
            }

            VStack(spacing: 10) {
                Text("Hold a control and move your device to tweak the strip.")
                    .font(.footnote)
                    .multilineTextAlignment(.center)
                    .foregroundColor(.secondary)

                HStack(spacing: 12) {
                    HoldAdjustmentButton(
                        title: "Twist for Hue",
                        active: motionController.hueHoldActive
                    ) {
                        motionController.setHueAdjustmentActive($0)
                    }

                    HoldAdjustmentButton(
                        title: "Z-axis Brightness",
                        active: motionController.brightnessHoldActive
                    ) {
                        motionController.setBrightnessAdjustmentActive($0)
                    }
                }
            }
            .frame(maxWidth: .infinity)

            VStack(spacing: 12) {
                Text("Manual hue/brightness")
                    .font(.footnote)
                    .multilineTextAlignment(.center)
                    .foregroundColor(.secondary)

                HStack(spacing: 12) {
                    TextField("Hue (0-360)", text: $manualHueText)
                        .textFieldStyle(.roundedBorder)
                        .keyboardType(.decimalPad)
                        .submitLabel(.send)
                        .onSubmit(sendManualHue)
                    Button("Send") {
                        sendManualHue()
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.small)
                    .disabled(manualHueText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }

                HStack(spacing: 12) {
                    TextField("Brightness (0-100)", text: $manualBrightnessText)
                        .textFieldStyle(.roundedBorder)
                        .keyboardType(.decimalPad)
                        .submitLabel(.send)
                        .onSubmit(sendManualBrightness)
                    Button("Send") {
                        sendManualBrightness()
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.small)
                    .disabled(manualBrightnessText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }
            }
            .frame(maxWidth: .infinity)

            Button("Reconnect & Reset") {
                motionController.requestResetAndReconnect()
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .frame(maxWidth: .infinity)

            Spacer()
        }
        .padding()
        .task {
            motionController.start()
        }
    }

    private func sendManualHue() {
        guard let value = Double(manualHueText) else { return }
        motionController.sendManualHue(value)
        manualHueText = ""
    }

    private func sendManualBrightness() {
        guard let value = Double(manualBrightnessText) else { return }
        motionController.sendManualBrightness(value)
        manualBrightnessText = ""
    }
}

private struct StatusCard: View {
    let status: String
    let ready: Bool

    var body: some View {
        Label {
            Text(status)
                .font(.callout)
                .lineLimit(2)
        } icon: {
            Image(systemName: ready ? "antenna.radiowaves.left.and.right" : "exclamationmark.triangle")
                .foregroundStyle(ready ? .green : .orange)
        }
        .padding()
        .frame(maxWidth: .infinity)
        .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    }
}

private struct MetricCard: View {
    let label: String
    let value: Double
    let suffix: String

    var body: some View {
        VStack {
            Text(label)
                .font(.caption)
                .foregroundColor(.secondary)
            Text(value, format: .number.precision(.fractionLength(1)))
                .font(.title2)
                .bold()
            Text(suffix)
                .font(.caption2)
                .foregroundColor(.secondary)
        }
        .frame(maxWidth: .infinity)
        .padding()
        .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    }
}

private final class MotionUDPManager: ObservableObject {
    static let wifiSSID = "PSL_UDP"
    static let wifiPassword = "psludp123"
    static let targetHost = NWEndpoint.Host("192.168.4.1")
    static let targetPort = NWEndpoint.Port(integerLiteral: 4210)

    @Published private(set) var connectionStatus = "Idle"
    @Published private(set) var pitchDegrees = 0.0
    @Published private(set) var rollDegrees = 0.0
    @Published private(set) var yawDegrees = 0.0
    @Published private(set) var isReady = false
    @Published private(set) var hueHoldActive = false
    @Published private(set) var brightnessHoldActive = false

    var canReconnect: Bool {
        !connectionReady
    }

    private let motionManager = CMMotionManager()
    private let connectionQueue = DispatchQueue(label: "com.psl.motion-udp", qos: .userInitiated)
    private let motionQueue: OperationQueue = {
        let queue = OperationQueue()
        queue.name = "PSL Motion Queue"
        queue.qualityOfService = .userInteractive
        queue.maxConcurrentOperationCount = 1
        return queue
    }()

    private var connection: NWConnection?
    private var started = false
    private var connectionPending = false
    private var connectionReady = false
    private var pendingResetRequest = false
    private var hotspotConfigured = false
    private enum AdjustmentMode {
        case none
        case hue
        case brightness
    }

    private let adjustmentStateQueue = DispatchQueue(label: "com.psl.motion-udp.adjustment", qos: .userInteractive)
    private var adjustmentMode: AdjustmentMode = .none
    private var huePressed = false
    private var brightnessPressed = false
    private var lastBrightnessZ: Double?

    func start() {
        guard !started else { return }
        started = true
        configureHotspotIfNeeded()
        startConnectionIfNeeded()
        beginMotionUpdates()
    }

    func requestResetAndReconnect() {
        connectionQueue.async { [weak self] in
            self?.pendingResetRequest = true
        }
        restartConnection()
    }

    func restartConnection() {
        configureHotspotIfNeeded()
        connectionQueue.async { [weak self] in
            guard let self = self else { return }
            self.connection?.cancel()
            self.connection = nil
            self.connectionReady = false
        }
        updateStatus("Reconnecting…", ready: false)
        startConnectionIfNeeded(delay: 0.5)
    }

    func setHueAdjustmentActive(_ active: Bool) {
        hueHoldActive = active
        if active {
            brightnessHoldActive = false
        }
        updateAdjustmentState(hue: active, brightness: active ? false : nil)
    }

    func setBrightnessAdjustmentActive(_ active: Bool) {
        brightnessHoldActive = active
        if active {
            hueHoldActive = false
        }
        updateAdjustmentState(hue: active ? false : nil, brightness: active)
    }

    private func configureHotspotIfNeeded() {
        guard !hotspotConfigured else { return }
        guard #available(iOS 11.0, *) else {
            updateStatus("Hotspot configuration unsupported", ready: false)
            return
        }
        updateStatus("Joining \(Self.wifiSSID)…", ready: false)

        let configuration = NEHotspotConfiguration(
            ssid: Self.wifiSSID,
            passphrase: Self.wifiPassword,
            isWEP: false
        )
        configuration.joinOnce = false

        NEHotspotConfigurationManager.shared.apply(configuration) { [weak self] error in
            guard let self = self else { return }
            if let nsError = error as NSError?,
               nsError.domain == NEHotspotConfigurationErrorDomain,
               let code = NEHotspotConfigurationError(rawValue: nsError.code),
               code == .alreadyAssociated {
                self.hotspotConfigured = true
                self.updateStatus("Already joined \(Self.wifiSSID)", ready: false)
                return
            } else if let error = error {
                self.hotspotConfigured = false
                self.updateStatus("Hotspot error: \(error.localizedDescription)", ready: false)
                return
            }
            self.hotspotConfigured = true
            self.updateStatus("Hotspot joined \(Self.wifiSSID)", ready: false)
        }
    }

    private func beginMotionUpdates() {
        guard motionManager.isDeviceMotionAvailable else {
            updateStatus("Device motion unavailable", ready: false)
            return
        }
        motionManager.deviceMotionUpdateInterval = 1.0 / 60.0
        motionManager.startDeviceMotionUpdates(using: .xArbitraryZVertical, to: motionQueue) { [weak self] motion, error in
            guard let self = self else { return }
            if let error = error {
                self.updateStatus("Motion error: \(error.localizedDescription)", ready: false)
                return
            }
            guard let deviceMotion = motion else {
                return
            }
            let pitch = deviceMotion.attitude.pitch * 180.0 / .pi
            let roll = deviceMotion.attitude.roll * 180.0 / .pi
            let yaw = deviceMotion.attitude.yaw * 180.0 / .pi
            DispatchQueue.main.async {
                self.pitchDegrees = pitch
                self.rollDegrees = roll
                self.yawDegrees = yaw
            }
            if let adjustmentPayload = self.adjustmentPayload(for: deviceMotion) {
                self.sendUDP(adjustmentPayload)
            }
        }
    }

    private func startConnectionIfNeeded(delay: TimeInterval = 0) {
        guard !connectionPending else { return }
        connectionPending = true
        connectionQueue.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self = self else { return }
            self.connectionPending = false
            guard self.connection == nil else { return }
            self.setupConnection()
        }
    }

    private func setupConnection() {
        updateStatus("Connecting to PSL_UDP", ready: false)
        let connection = NWConnection(host: Self.targetHost, port: Self.targetPort, using: .udp)
        self.connection = connection
        connection.stateUpdateHandler = { [weak self] state in
            guard let self = self else { return }
            switch state {
            case .ready:
                self.connectionReady = true
                self.updateStatus("Connected to \(Self.wifiSSID)", ready: true)
                if self.pendingResetRequest {
                    self.pendingResetRequest = false
                    self.sendUDP("RESET")
                }
            case .waiting(let error):
                self.connectionReady = false
                self.updateStatus("Waiting: \(error.localizedDescription)", ready: false)
            case .failed(let error):
                self.connectionReady = false
                self.updateStatus("Connection failed: \(error.localizedDescription)", ready: false)
                self.startConnectionIfNeeded(delay: 1.0)
            case .cancelled:
                self.connectionReady = false
                self.updateStatus("Connection cancelled", ready: false)
            default:
                break
            }
        }
        connection.start(queue: connectionQueue)
    }

    private func adjustmentPayload(for motion: CMDeviceMotion) -> String? {
        switch currentAdjustmentMode() {
        case .none:
            return nil
        case .hue:
            let delta = motion.rotationRate.z * 5.0
            guard abs(delta) >= 0.05 else { return nil }
            return String(format: "H,%.4f", delta)
        case .brightness:
            let delta = brightnessDelta(for: motion.gravity.z)
            guard abs(delta) >= 0.002 else { return nil }
            return String(format: "B,%.4f", delta)
        }
    }

    private func sendUDP(_ payload: String) {
        guard connectionReady, let connection = connection else { return }
        guard let data = payload.data(using: .utf8) else { return }
        connectionQueue.async { [weak self] in
            guard let self = self,
                  self.connectionReady,
                  self.connection === connection else { return }
            connection.send(content: data, completion: .contentProcessed { sendError in
                if let sendError = sendError {
                    self.connectionReady = false
                    self.updateStatus("Send error: \(sendError.localizedDescription)", ready: false)
                    self.startConnectionIfNeeded(delay: 1.0)
                }
            })
        }
    }

    func sendManualHue(_ degrees: Double) {
        var normalized = degrees.truncatingRemainder(dividingBy: 360.0)
        if normalized < 0 {
            normalized += 360.0
        }
        let payload = String(format: "H_SET,%.4f", normalized)
        sendUDP(payload)
    }

    func sendManualBrightness(_ percent: Double) {
        let clamped = min(max(percent, 0.0), 100.0)
        let payload = String(format: "B_SET,%.4f", clamped)
        sendUDP(payload)
    }

    private func currentAdjustmentMode() -> AdjustmentMode {
        adjustmentStateQueue.sync {
            adjustmentMode
        }
    }

    private func brightnessDelta(for gravityZ: Double) -> Double {
        var delta = 0.0
        adjustmentStateQueue.sync {
            if let last = lastBrightnessZ {
                delta = (gravityZ - last) * -0.7
            }
            lastBrightnessZ = gravityZ
        }
        return delta
    }

    private func updateAdjustmentState(hue: Bool? = nil, brightness: Bool? = nil) {
        adjustmentStateQueue.async(flags: .barrier) { [weak self] in
            guard let self = self else { return }
            if let hueValue = hue {
                self.huePressed = hueValue
            }
            if let brightnessValue = brightness {
                self.brightnessPressed = brightnessValue
                if brightnessValue {
                    self.lastBrightnessZ = nil
                }
            }
            if self.huePressed {
                self.adjustmentMode = .hue
            } else if self.brightnessPressed {
                self.adjustmentMode = .brightness
            } else {
                self.adjustmentMode = .none
            }
            if !self.brightnessPressed {
                self.lastBrightnessZ = nil
            }
        }
    }

    private func updateStatus(_ text: String, ready: Bool? = nil) {
        DispatchQueue.main.async {
            self.connectionStatus = text
            if let ready = ready {
                self.isReady = ready
            }
        }
    }

    deinit {
        motionManager.stopDeviceMotionUpdates()
        connection?.cancel()
    }
}

private struct HoldAdjustmentButton: View {
    let title: String
    let active: Bool
    let onHoldChanged: (Bool) -> Void

    var body: some View {
        Button(action: {}) {
            Text(title)
                .font(.headline)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 10)
        }
        .buttonStyle(.borderedProminent)
        .tint(active ? .accentColor : .gray)
        .foregroundColor(.white)
        .controlSize(.large)
        .onLongPressGesture(minimumDuration: 0, pressing: onHoldChanged, perform: {})
    }
}

#Preview {
    ContentView()
}
