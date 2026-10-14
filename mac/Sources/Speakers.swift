import CoreAudio
import Foundation

/// Mutes the Mac's speakers while a client is connected, since the audio plays on Linux instead.
/// The muted device is remembered so a crash can't leave the Mac muted past the next launch.
/// Speakers the user had muted themselves are left alone. macOS unmutes the output whenever its
/// volume changes (slider, volume keys, apps), so while connected any unmute is undone at once.
final class SpeakerMute {
    private static let defaultsKey = "mutedOutputDeviceUID"
    private var muted: AudioDeviceID?
    private var watched = Set<AudioDeviceID>()
    private var guarding: AudioDeviceID?  // the device kept muted; used on `queue`
    private let queue = DispatchQueue(label: "macglass.speakers")

    init() {
        guard let uid = UserDefaults.standard.string(forKey: Self.defaultsKey) else { return }
        if let device = Self.device(uid: uid) { _ = Self.setMute(device, false) }
        UserDefaults.standard.removeObject(forKey: Self.defaultsKey)
        log("unmuted the Mac speakers left muted by a previous run")
    }

    func mute() {
        guard muted == nil, let device = Self.defaultOutput(), Self.isMuted(device) == false else { return }
        guard Self.setMute(device, true) else {
            log("the Mac's output device can't be muted; its speakers will keep playing")
            return
        }
        muted = device
        UserDefaults.standard.set(Self.uid(device), forKey: Self.defaultsKey)
        queue.sync { guarding = device }
        if watched.insert(device).inserted {  // one listener per device for good; `guarding` decides if it acts
            var addr = Self.address(kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput)
            let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
                guard let self, self.guarding == device, Self.isMuted(device) == false else { return }
                _ = Self.setMute(device, true)
            }
            AudioObjectAddPropertyListenerBlock(device, &addr, queue, block)
        }
        log("Mac speakers muted while connected")
    }

    func unmute() {
        guard let device = muted else { return }
        queue.sync { guarding = nil }  // on the listener's queue: no re-mute can follow
        _ = Self.setMute(device, false)
        muted = nil
        UserDefaults.standard.removeObject(forKey: Self.defaultsKey)
        log("Mac speakers unmuted")
    }

    private static func address(_ selector: AudioObjectPropertySelector,
                                _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: kAudioObjectPropertyElementMain)
    }

    private static func defaultOutput() -> AudioDeviceID? {
        var addr = address(kAudioHardwarePropertyDefaultOutputDevice)
        var device = AudioDeviceID(0)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &device)
        return status == noErr && device != 0 ? device : nil
    }

    private static func isMuted(_ device: AudioDeviceID) -> Bool? {
        var addr = address(kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput)
        var value = UInt32(0)
        var size = UInt32(MemoryLayout<UInt32>.size)
        guard AudioObjectGetPropertyData(device, &addr, 0, nil, &size, &value) == noErr else { return nil }
        return value != 0
    }

    private static func setMute(_ device: AudioDeviceID, _ on: Bool) -> Bool {
        var addr = address(kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput)
        var value = UInt32(on ? 1 : 0)
        return AudioObjectSetPropertyData(device, &addr, 0, nil, UInt32(MemoryLayout<UInt32>.size), &value) == noErr
    }

    private static func uid(_ device: AudioDeviceID) -> String? {
        var addr = address(kAudioDevicePropertyDeviceUID)
        var uid: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(device, &addr, 0, nil, &size, &uid) == noErr else { return nil }
        return uid?.takeRetainedValue() as String?
    }

    private static func device(uid: String) -> AudioDeviceID? {
        var addr = address(kAudioHardwarePropertyTranslateUIDToDevice)
        var cfUID = uid as CFString
        var device = AudioDeviceID(0)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = withUnsafePointer(to: &cfUID) {
            AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr,
                                       UInt32(MemoryLayout<CFString>.size), $0, &size, &device)
        }
        return status == noErr && device != 0 ? device : nil
    }
}

