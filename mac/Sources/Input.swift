import AppKit
import Carbon.HIToolbox

/// Replays the client's keyboard and mouse as system input. Needs the Accessibility permission.
/// Only used from the server's read thread.
final class InputInjector {
    private let lock = NSLock()
    private var currentDisplay: CGDirectDisplayID = 0
    private let source = CGEventSource(stateID: .hidSystemState)
    private var position = CGPoint.zero
    private var buttons = Set<Int>()
    private var heldKeys = Set<CGKeyCode>()
    private var capsLock = false
    private var lastClick = (button: -1, time: 0.0, position: CGPoint.zero, count: 0)
    private var warned = false

    /// The display pointer positions refer to; 0 while there is none. Set from the main thread.
    var displayID: CGDirectDisplayID {
        get { lock.withLock { currentDisplay } }
        set { lock.withLock { currentDisplay = newValue } }
    }

    func handle(_ type: UInt32, _ p: Data) {
        guard displayID != 0, permitted else { return }
        switch type {
        case MG.pointer where p.count >= 8: pointer(x: p.readFloat(at: 0), y: p.readFloat(at: 4))
        case MG.button where p.count >= 2: button(Int(p[p.startIndex]), down: p[p.startIndex + 1] != 0)
        case MG.scroll where p.count >= 8: scroll(dx: p.readFloat(at: 0), dy: p.readFloat(at: 4))
        case MG.key where p.count >= 4:
            key(hid: p.readLE(UInt16.self, at: 0), down: p[p.startIndex + 2] != 0, isRepeat: p[p.startIndex + 3] != 0)
        case MG.releaseAll: releaseAll()
        default: break
        }
    }

    func releaseAll() {
        for code in heldKeys { key(code: code, down: false, isRepeat: false) }
        for b in buttons { button(b, down: false) }
    }

    private var permitted: Bool {
        if CGPreflightPostEventAccess() { return true }
        if !warned {
            warned = true
            CGRequestPostEventAccess()
            log("Accessibility permission is missing: allow MacGlass in System Settings to forward keyboard and mouse")
        }
        return false
    }

    private var flags: CGEventFlags {
        var f: CGEventFlags = capsLock ? .maskAlphaShift : []
        for code in heldKeys { if let m = modifierMasks[code] { f.insert(m) } }
        return f
    }

    private func pointer(x: Float, y: Float) {
        let b = CGDisplayBounds(displayID)
        position = CGPoint(
            x: b.minX + CGFloat(min(max(x, 0), 1)) * (b.width - 1),
            y: b.minY + CGFloat(min(max(y, 0), 1)) * (b.height - 1))
        let type: CGEventType, held: CGMouseButton
        if buttons.contains(0) {
            (type, held) = (.leftMouseDragged, .left)
        } else if buttons.contains(1) {
            (type, held) = (.rightMouseDragged, .right)
        } else if let other = buttons.min() {
            (type, held) = (.otherMouseDragged, CGMouseButton(rawValue: UInt32(other)) ?? .center)
        } else {
            (type, held) = (.mouseMoved, .left)
        }
        postMouse(type, held, clicks: 0)
    }

    private func button(_ b: Int, down: Bool) {
        guard (0..<5).contains(b) else { return }
        if down { buttons.insert(b) } else { buttons.remove(b) }
        let type: CGEventType
        switch b {
        case 0: type = down ? .leftMouseDown : .leftMouseUp
        case 1: type = down ? .rightMouseDown : .rightMouseUp
        default: type = down ? .otherMouseDown : .otherMouseUp
        }
        // macOS doesn't infer double clicks from synthetic events; count them like the HID driver would.
        if down {
            let now = ProcessInfo.processInfo.systemUptime
            let near = hypot(position.x - lastClick.position.x, position.y - lastClick.position.y) < 4
            let count = lastClick.button == b && near && now - lastClick.time < NSEvent.doubleClickInterval
                ? lastClick.count + 1 : 1
            lastClick = (b, now, position, count)
        }
        postMouse(type, CGMouseButton(rawValue: UInt32(b)) ?? .left, clicks: lastClick.button == b ? lastClick.count : 1)
    }

    private func postMouse(_ type: CGEventType, _ button: CGMouseButton, clicks: Int) {
        guard let e = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: position, mouseButton: button)
        else { return }
        if clicks > 0 { e.setIntegerValueField(.mouseEventClickState, value: Int64(clicks)) }
        e.flags = flags
        e.post(tap: .cghidEventTap)
    }

    private func scroll(dx: Float, dy: Float) {
        let pixelsPerStep: Float = 40
        guard let e = CGEvent(
            scrollWheelEvent2Source: source, units: .pixel, wheelCount: 2,
            wheel1: Int32((dy * pixelsPerStep).rounded()), wheel2: Int32((-dx * pixelsPerStep).rounded()), wheel3: 0)
        else { return }
        e.flags = flags
        e.post(tap: .cghidEventTap)
    }

    private func key(hid: UInt16, down: Bool, isRepeat: Bool) {
        guard let code = hidToMac[hid] else { return }
        key(code: code, down: down, isRepeat: isRepeat)
    }

    private func key(code: CGKeyCode, down: Bool, isRepeat: Bool) {
        if down { heldKeys.insert(code) } else { heldKeys.remove(code) }
        if code == CGKeyCode(kVK_CapsLock) && down { capsLock.toggle() }
        guard let e = CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: down) else { return }
        // Modifier keys are reported as flag changes, and every event carries the current flags.
        if modifierMasks[code] != nil || code == CGKeyCode(kVK_CapsLock) { e.type = .flagsChanged }
        if isRepeat { e.setIntegerValueField(.keyboardEventAutorepeat, value: 1) }
        e.flags = flags
        e.post(tap: .cghidEventTap)
    }
}

private let modifierMasks: [CGKeyCode: CGEventFlags] = [
    CGKeyCode(kVK_Shift): .maskShift, CGKeyCode(kVK_RightShift): .maskShift,
    CGKeyCode(kVK_Control): .maskControl, CGKeyCode(kVK_RightControl): .maskControl,
    CGKeyCode(kVK_Option): .maskAlternate, CGKeyCode(kVK_RightOption): .maskAlternate,
    CGKeyCode(kVK_Command): .maskCommand, CGKeyCode(kVK_RightCommand): .maskCommand,
]

/// USB HID keyboard usages (what SDL scancodes are) to macOS virtual key codes.
/// The PC's Super keys become Command and Alt becomes Option.
private let hidToMac: [UInt16: CGKeyCode] = {
    let pairs: [(UInt16, Int)] = [
        (4, kVK_ANSI_A), (5, kVK_ANSI_B), (6, kVK_ANSI_C), (7, kVK_ANSI_D), (8, kVK_ANSI_E), (9, kVK_ANSI_F),
        (10, kVK_ANSI_G), (11, kVK_ANSI_H), (12, kVK_ANSI_I), (13, kVK_ANSI_J), (14, kVK_ANSI_K), (15, kVK_ANSI_L),
        (16, kVK_ANSI_M), (17, kVK_ANSI_N), (18, kVK_ANSI_O), (19, kVK_ANSI_P), (20, kVK_ANSI_Q), (21, kVK_ANSI_R),
        (22, kVK_ANSI_S), (23, kVK_ANSI_T), (24, kVK_ANSI_U), (25, kVK_ANSI_V), (26, kVK_ANSI_W), (27, kVK_ANSI_X),
        (28, kVK_ANSI_Y), (29, kVK_ANSI_Z),
        (30, kVK_ANSI_1), (31, kVK_ANSI_2), (32, kVK_ANSI_3), (33, kVK_ANSI_4), (34, kVK_ANSI_5),
        (35, kVK_ANSI_6), (36, kVK_ANSI_7), (37, kVK_ANSI_8), (38, kVK_ANSI_9), (39, kVK_ANSI_0),
        (40, kVK_Return), (41, kVK_Escape), (42, kVK_Delete), (43, kVK_Tab), (44, kVK_Space),
        (45, kVK_ANSI_Minus), (46, kVK_ANSI_Equal), (47, kVK_ANSI_LeftBracket), (48, kVK_ANSI_RightBracket),
        (49, kVK_ANSI_Backslash), (50, kVK_ANSI_Backslash), (51, kVK_ANSI_Semicolon), (52, kVK_ANSI_Quote),
        (53, kVK_ANSI_Grave), (54, kVK_ANSI_Comma), (55, kVK_ANSI_Period), (56, kVK_ANSI_Slash),
        (57, kVK_CapsLock),
        (58, kVK_F1), (59, kVK_F2), (60, kVK_F3), (61, kVK_F4), (62, kVK_F5), (63, kVK_F6),
        (64, kVK_F7), (65, kVK_F8), (66, kVK_F9), (67, kVK_F10), (68, kVK_F11), (69, kVK_F12),
        (70, kVK_F13), (72, kVK_F15),  // Print Screen, Pause
        (73, kVK_Help), (74, kVK_Home), (75, kVK_PageUp), (76, kVK_ForwardDelete), (77, kVK_End),
        (78, kVK_PageDown), (79, kVK_RightArrow), (80, kVK_LeftArrow), (81, kVK_DownArrow), (82, kVK_UpArrow),
        (83, kVK_ANSI_KeypadClear), (84, kVK_ANSI_KeypadDivide), (85, kVK_ANSI_KeypadMultiply),
        (86, kVK_ANSI_KeypadMinus), (87, kVK_ANSI_KeypadPlus), (88, kVK_ANSI_KeypadEnter),
        (89, kVK_ANSI_Keypad1), (90, kVK_ANSI_Keypad2), (91, kVK_ANSI_Keypad3), (92, kVK_ANSI_Keypad4),
        (93, kVK_ANSI_Keypad5), (94, kVK_ANSI_Keypad6), (95, kVK_ANSI_Keypad7), (96, kVK_ANSI_Keypad8),
        (97, kVK_ANSI_Keypad9), (98, kVK_ANSI_Keypad0), (99, kVK_ANSI_KeypadDecimal),
        (100, kVK_ISO_Section), (103, kVK_ANSI_KeypadEquals),
        (104, kVK_F13), (105, kVK_F14), (106, kVK_F15), (107, kVK_F16), (108, kVK_F17), (109, kVK_F18),
        (110, kVK_F19), (111, kVK_F20),
        (127, kVK_Mute), (128, kVK_VolumeUp), (129, kVK_VolumeDown),
        (224, kVK_Control), (225, kVK_Shift), (226, kVK_Option), (227, kVK_Command),
        (228, kVK_RightControl), (229, kVK_RightShift), (230, kVK_RightOption), (231, kVK_RightCommand),
    ]
    return Dictionary(uniqueKeysWithValues: pairs.map { ($0.0, CGKeyCode($0.1)) })
}()
