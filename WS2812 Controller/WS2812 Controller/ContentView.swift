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

            Button("Reconnect") {
                motionController.restartConnection()
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .disabled(!motionController.canReconnect)
            .frame(maxWidth: .infinity)

            Spacer()
        }
        .padding()
        .task {
            motionController.start()
        }
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

    func start() {
        guard !started else { return }
        started = true
        configureHotspot()
        startConnectionIfNeeded()
        beginMotionUpdates()
    }

    func restartConnection() {
        connectionQueue.async { [weak self] in
            guard let self = self else { return }
            self.connection?.cancel()
            self.connection = nil
            self.connectionReady = false
        }
        updateStatus("Reconnecting…", ready: false)
        startConnectionIfNeeded(delay: 0.5)
    }

    private func configureHotspot() {
        guard #available(iOS 11.0, *) else {
            updateStatus("Hotspot configuration unsupported")
            return
        }

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
                self.updateStatus("Already joined \(Self.wifiSSID)", ready: false)
                return
            } else if let error = error {
                self.updateStatus("Hotspot error: \(error.localizedDescription)", ready: false)
                return
            }
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
            self.sendMotionPacket(deviceMotion)
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
                self.updateStatus("UDP ready", ready: true)
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

    private func sendMotionPacket(_ motion: CMDeviceMotion) {
        guard connectionReady, let connection = connection else { return }
        let payload = String(
            format: "%.4f,%.4f,%.4f",
            motion.attitude.pitch,
            motion.attitude.roll,
            motion.attitude.yaw
        )
        guard let data = payload.data(using: .utf8) else { return }
        connectionQueue.async { [weak self] in
            guard let self = self, self.connectionReady else { return }
            connection.send(content: data, completion: .contentProcessed { sendError in
                if let sendError = sendError {
                    self.connectionReady = false
                    self.updateStatus("Send error: \(sendError.localizedDescription)", ready: false)
                    self.startConnectionIfNeeded(delay: 1.0)
                }
            })
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

#Preview {
    ContentView()
}
