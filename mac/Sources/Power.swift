import Foundation
import IOKit.pwr_mgt

/// Keeps the Mac awake while a client is connected, including with the lid closed.
///
/// Lid-close sleep can only be turned off system-wide (`pmset disablesleep`), which needs root;
/// /etc/sudoers.d/macglass allows exactly that command without a password. Turning it back on is
/// delayed by a grace period, so restarting the client or MacGlass with the lid closed doesn't
/// put the Mac to sleep in between. The delayed restore runs as a separate process, so it also
/// happens if MacGlass quits (the LaunchAgent's AbandonProcessGroup keeps launchd from killing it
/// along with MacGlass), and connecting cancels it.
///
/// All of it runs on one serial queue: waiting for a command on the main thread would run the main
/// run loop meanwhile, and a quick reconnect's hold() or release() would re-enter halfway through.
final class PowerKeeper {
    static let grace = 180  // seconds

    private var assertions: [IOPMAssertionID] = []
    private var restore: Process?  // the pending restore, kept referenced while it runs
    private static let restoreTag = "macglass-restore-lid-sleep"
    private let queue = DispatchQueue(label: "macglass.power")

    init() {
        // A previous run may have left lid-close sleep off; restore it unless a client shows up.
        queue.sync { scheduleRestore() }
    }

    func hold() { queue.sync { holdNow() } }
    func release() { queue.sync { releaseNow() } }

    private func holdNow() {
        // With the lid closed and no MacGlass display yet, the Mac has no screen and its display
        // system sleeps; a virtual display created then can't change modes or be captured.
        var activity = IOPMAssertionID(0)
        IOPMAssertionDeclareUserActivity("MacGlass client connected" as CFString, kIOPMUserActiveLocal, &activity)
        cancelRestore()
        run("/usr/bin/sudo", ["-n", "/usr/bin/pmset", "-a", "disablesleep", "1"],
            failure: "pmset disablesleep failed (is /etc/sudoers.d/macglass installed?); closing the lid will sleep the Mac")
        guard assertions.isEmpty else { return }
        for type in [kIOPMAssertPreventUserIdleSystemSleep, kIOPMAssertPreventUserIdleDisplaySleep] {
            var id = IOPMAssertionID(0)
            if IOPMAssertionCreateWithName(type as CFString, IOPMAssertionLevel(kIOPMAssertionLevelOn),
                                           "MacGlass client connected" as CFString, &id) == kIOReturnSuccess {
                assertions.append(id)
            }
        }
    }

    private func releaseNow() {
        for id in assertions { IOPMAssertionRelease(id) }
        assertions.removeAll()
        scheduleRestore()
    }

    private func scheduleRestore() {
        cancelRestore()
        // `exec -a`-style naming via $0 lets cancelRestore find this process later.
        let script = "sleep \(Self.grace); exec /usr/bin/sudo -n /usr/bin/pmset -a disablesleep 0"
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c", script, Self.restoreTag]
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        do {
            try p.run()
            restore = p
            log("lid-close sleep returns in \(Self.grace / 60) min unless a client connects")
        } catch {
            log("cannot schedule restoring lid-close sleep: \(error)")
        }
    }

    private func cancelRestore() {
        run("/usr/bin/pkill", ["-f", Self.restoreTag], failure: nil)  // also one left by an earlier run
        restore = nil
    }

    private func run(_ path: String, _ args: [String], failure: String?) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: path)
        p.arguments = args
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        do {
            try p.run()
            p.waitUntilExit()
            if p.terminationStatus != 0, let failure { log(failure) }
        } catch {
            if let failure { log("\(failure): \(error)") }
        }
    }
}
