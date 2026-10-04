//
//  CoreRegistry.swift
//  Unified multi-core shell
//
//  Enforces the multi-core ABI contract on the Swift side: ABI version
//  negotiation, JIT capability gating, content routing, and instantiation.
//
//  Deliberately contains no UIKit. Everything here is testable on its own, and
//  the SwiftUI layer is a thin projection of this state. That split matters
//  because the emulator cores are the part that needs a Mac to validate -- the
//  policy around them should not also be waiting on an iOS build.
//

import Foundation
import EmuCoreAPI

// MARK: - Availability

/// Why a core can or cannot be offered right now.
public enum CoreAvailability: Equatable {
    /// Registered, ABI understood, no unmet requirements.
    case available
    /// Declares EMU_CAP_REQUIRES_JIT but this process has no JIT entitlement.
    case requiresJIT
    /// Core was built against a newer major ABI than the shell understands.
    case unsupportedABI(coreMajor: UInt32, shellMajor: UInt32)
    /// Core failed its own self-check at registration.
    case invalidCore(String)

    public var isSelectable: Bool { self == .available }
}

// MARK: - Descriptor

public struct CoreDescriptor: Identifiable, Equatable {

    public let id: String
    public let displayName: String
    public let version: String
    public let summary: String?
    public let capabilities: EmuCoreCapability
    public let availability: CoreAvailability

    public init(info: EmuCoreInfo, availability: CoreAvailability) {
        // A core with a NULL string field should not be able to crash the
        // library UI. Degrade to a placeholder instead.
        self.id = CoreDescriptor.string(info.id) ?? "unknown"
        self.displayName = CoreDescriptor.string(info.display_name)
            ?? CoreDescriptor.string(info.id) ?? "Unnamed core"
        self.version = CoreDescriptor.string(info.version) ?? "0"
        self.summary = CoreDescriptor.string(info.summary)
        self.capabilities = EmuCoreCapability(rawValue: info.capabilities)
            ?? EMU_CAP_NONE
        self.availability = availability
    }

    private static func string(_ pointer: UnsafePointer<CChar>?) -> String? {
        pointer.map { String(cString: $0) }
    }

    public var requiresJIT: Bool {
        capabilities.contains(.REQUIRES_JIT)
    }

    /// Short line for the library UI explaining a disabled core.
    public var unavailableReason: String? {
        switch availability {
        case .available:
            return nil
        case .requiresJIT:
            return "Needs JIT, which is not enabled on this device. iOS 17.4+ required."
        case .unsupportedABI(let coreMajor, let shellMajor):
            return "Needs core ABI \(coreMajor), this app provides \(shellMajor)."
        case .invalidCore(let detail):
            return "Core failed validation: \(detail)"
        }
    }
}

// MARK: - Registry

public final class CoreRegistry {

    public static let shared = CoreRegistry()

    /// Cores that passed registration, in registration order.
    private var descriptors: [CoreDescriptor] = []
    private var tables: [UnsafePointer<EmuCoreVTable>] = []

    /// Whether JIT is currently available to this process.
    ///
    /// Resolved once at startup. It cannot change while the app is running:
    /// enabling JIT means re-signing the process with the entitlement (StikDebug
    /// or equivalent), which requires a relaunch.
    public let jitAvailable: Bool

    private init() {
        self.jitAvailable = emu_host_jit_available() != 0
        self.bootstrap()
    }

    // MARK: Startup

    private func bootstrap() {
        let reported = emu_registered_core_count()

        // Trust the C array, but reconcile against the count C reports so a
        // registration bug surfaces immediately rather than as a core that
        // silently never appears.
        var walked: [UnsafePointer<EmuCoreVTable>] = []
        for index in 0..<reported {
            if let table = emu_core_at(index) {
                walked.append(table)
            }
        }

        for table in walked {
            register(table)
        }

        if walked.count != reported {
            NSLog("[CoreRegistry] registration mismatch: C reported \(reported) " +
                  "cores but only \(walked.count) could be read")
        }
    }

    /// Public entry point for hosts that construct their own registry (tests).
    public func register(_ table: UnsafePointer<EmuCoreVTable>) {
        guard let infoPointer = table.pointee.info?() else {
            let placeholder = CoreDescriptor(
                id: "invalid",
                displayName: "Invalid core",
                version: "0",
                summary: nil,
                capabilities: EMU_CAP_NONE,
                availability: .invalidCore("info() returned NULL")
            )
            descriptors.append(placeholder)
            return
        }

        let info = infoPointer.pointee
        let availability = evaluate(info: info)
        let descriptor = CoreDescriptor(info: info, availability: availability)
        descriptors.append(descriptor)
        tables.append(table)

        if availability.isSelectable {
            NSLog("[CoreRegistry] registered core \(descriptor.id) " +
                  "(\(descriptor.displayName) \(descriptor.version))")
        } else {
            NSLog("[CoreRegistry] registered core \(descriptor.id) but it is " +
                  "unavailable: \(descriptor.unavailableReason ?? "unknown")")
        }
    }

    /// Core availability policy, isolated so it can be unit tested directly.
    func evaluate(info: EmuCoreInfo) -> CoreAvailability {
        // Major versions must match exactly. Minor may differ either way; cores
        // are only permitted to add optional vtable entries within a major.
        if info.abi_version_major > UInt32(EMU_CORE_ABI_VERSION_MAJOR) {
            return .unsupportedABI(coreMajor: info.abi_version_major,
                                   shellMajor: UInt32(EMU_CORE_ABI_VERSION_MAJOR))
        }

        let capabilities = EmuCoreCapability(rawValue: info.capabilities) ?? EMU_CAP_NONE
        if capabilities.contains(.REQUIRES_JIT) && !jitAvailable {
            return .requiresJIT
        }

        return .available
    }

    // MARK: Queries

    public var allDescriptors: [CoreDescriptor] { descriptors }

    public var selectableDescriptors: [CoreDescriptor] {
        descriptors.filter { $0.availability.isSelectable }
    }

    public func descriptor(withID id: String) -> CoreDescriptor? {
        descriptors.first { $0.id == id }
    }

    private func table(forID id: String) -> UnsafePointer<EmuCoreVTable>? {
        guard let index = descriptors.firstIndex(where: { $0.id == id }) else {
            return nil
        }
        return tables[index]
    }

    // MARK: Content routing

    /// Ask each selectable core whether it can handle this file, and return the
    /// best match. Requires an instance, since probe_content is per-core.
    public func routeContent(
        atPath path: String,
        host: EmuHostApi,
        config: EmuCoreConfig
    ) throws -> CoreDescriptor {
        for descriptor in selectableDescriptors {
            guard let table = table(forID: descriptor.id) else { continue }
            guard let create = table.pointee.create else { continue }

            var status = EMU_OK
            guard let instance = create(&config, &host, &status) else {
                continue
            }
            defer {
                table.pointee.destroy?(instance)
            }

            let probe = table.pointee.probe_content
            if probe?(instance, path) == EMU_OK {
                return descriptor
            }
        }

        throw CoreRegistryError.noCoreHandlesContent(path)
    }

    // MARK: Instantiation

    /// Create a live core instance. The caller owns it and must call destroy.
    public func makeInstance(
        id: String,
        config: EmuCoreConfig,
        host: EmuHostApi
    ) throws -> EmuCore {
        guard let table = table(forID: id) else {
            throw CoreRegistryError.unknownCore(id)
        }
        guard let descriptor = descriptor(withID: id) else {
            throw CoreRegistryError.unknownCore(id)
        }
        guard descriptor.availability.isSelectable else {
            throw CoreRegistryError.coreUnavailable(
                id: id,
                reason: descriptor.unavailableReason ?? "unavailable"
            )
        }
        guard let create = table.pointee.create else {
            throw CoreRegistryError.coreMissingEntryPoint(id, "create")
        }

        var status = EMU_OK
        guard let instance = create(&config, &host, &status) else {
            throw CoreRegistryError.creationFailed(id: id, status: status)
        }
        return instance
    }

    /// Release an instance. Safe to call with nil.
    public func destroyInstance(_ instance: EmuCore?, id: String) {
        guard let instance = instance else { return }
        guard let table = table(forID: id) else { return }
        table.pointee.destroy?(instance)
    }
}

// MARK: - Errors

public enum CoreRegistryError: Error, CustomStringConvertible {
    case unknownCore(String)
    case coreUnavailable(id: String, reason: String)
    case coreMissingEntryPoint(String, String)
    case creationFailed(id: String, status: EmuStatus)
    case noCoreHandlesContent(String)

    public var description: String {
        switch self {
        case .unknownCore(let id):
            return "No registered core with id '\(id)'."
        case .coreUnavailable(let id, let reason):
            return "Core '\(id)' is unavailable: \(reason)"
        case .coreMissingEntryPoint(let id, let entry):
            return "Core '\(id)' does not implement \(entry)."
        case .creationFailed(let id, let status):
            return "Core '\(id)' failed to initialise (status \(status.rawValue))."
        case .noCoreHandlesContent(let path):
            return "No available core can run '\(path)'."
        }
    }
}