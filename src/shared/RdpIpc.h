// RdpIpc.h — wire protocol between hyprv.exe (parent) and hyprv-rdphost.exe (child).
//
// The parent creates a duplex named pipe of the form \\.\pipe\hyprv-rdp-<guid>, then
// spawns the child with `--pipe=<guid>`. The child opens the pipe as client. After
// the Hello/HelloAck handshake, either side may send any message type from its
// direction at any time.
//
// All multi-byte integers are little-endian (Windows is always little-endian on the
// targets we ship). Structs use #pragma pack(1) so layout is portable across builds
// of either EXE. Variable-length payloads (strings, GUID-less helpers) come AFTER
// the fixed POD and use the pattern: uint32_t length-in-bytes, then raw UTF-8.
//
// This header is the ONLY shared file between the two projects. It must not pull
// in dependencies beyond <cstdint> and <cstddef>.

#pragma once

#include <cstdint>
#include <cstddef>

namespace hyprv::ipc
{
    // v2: Disconnected gained a `fatal` classification byte + a trailing UTF-8
    //     human-readable description string (from mstscax GetErrorDescription),
    //     so the parent can surface connect failures instead of churning blindly.
    // v3: RdpOptions gained `userByteLen` + a trailing UTF-8 username (after the
    //     server/domain bytes) so generic RDP connections (Remote Hosts) can
    //     pre-fill mstscax's credential prompt with the saved user name.
    inline constexpr uint8_t kProtocolVersion = 3;

    // The parent uses this; the child opens \\.\pipe\hyprv-rdp-<guid> as client.
    inline constexpr wchar_t kPipeNamePrefix[] = L"\\\\.\\pipe\\hyprv-rdp-";

    // 64 KiB is large enough to hold any plausible TypeText message inline.
    inline constexpr uint32_t kPipeBufferSize = 64 * 1024;

    // ---------------------------------------------------------------- Direction tags
    // Parent -> Child commands. Values 1..127.
    enum class P2C : uint8_t
    {
        Hello                        = 1,  // first message of session; handshake
        Connect                      = 2,  // generic remote RDP server connection
        ConnectLocalVm               = 3,  // Hyper-V VM console connection
        Disconnect                   = 4,
        Shutdown                     = 5,  // child should flush + exit cleanly
        Resize                       = 6,  // host area resized; non-enhanced fallback
        UpdateSessionDisplaySettings = 7,  // enhanced-session resolution change
        SetEnhanced                  = 8,  // toggle enhanced mode without losing tab
        SendCtrlAltDel               = 9,
        TypeText                     = 10,
    };

    // Child -> Parent notifications. Values 128..255 so a printed dump can see direction at a glance.
    enum class C2P : uint8_t
    {
        HelloAck       = 128,  // child acks handshake, reports selected version
        HwndReady      = 129,  // top-level HWND ready; parent may SetParent/AttachThreadInput
        Connecting     = 130,
        Connected      = 131,
        Disconnected   = 132,
        EnhancedReady  = 133,  // enhanced session reached usable state (post OnLoginComplete)
        DesktopResized = 134,  // ActiveX reported a new remote desktop size
        Error          = 135,
        MouseActivated = 136,
        LogLine        = 137,  // diagnostic text — payload is `LogLine` header + UTF-8 bytes
    };

#pragma pack(push, 1)

    // ---------------------------------------------------------------- Frame header
    // Every message: Header followed by exactly payloadSize bytes of payload.
    struct Header
    {
        uint8_t  type;          // P2C or C2P enum value; the direction is implicit from sender
        uint8_t  reserved0;
        uint16_t reserved1;
        uint32_t payloadSize;   // bytes that follow this 8-byte header
    };
    static_assert(sizeof(Header) == 8, "Header must be 8 bytes for wire-stability");

    // ---------------------------------------------------------------- Handshake
    struct Hello
    {
        uint8_t  minVersion;    // lowest protocol version this sender understands
        uint8_t  maxVersion;    // highest protocol version this sender understands
        uint16_t reserved;
    };

    struct HelloAck
    {
        uint8_t  selectedVersion;  // protocol version chosen for this session
        uint8_t  status;           // 0 = OK, 1 = incompatible (child should exit)
        uint16_t reserved;
    };

    // ---------------------------------------------------------------- Window plumbing
    struct HwndReady
    {
        uint64_t hwnd;  // child's top-level HWND, cast from HWND
    };

    // ---------------------------------------------------------------- RdpOptions payload
    // Bit flags packed into RdpOptions::flags.
    enum RdpFlags : uint32_t
    {
        Flag_EnhancedSession        = 1u << 0,
        Flag_FrameBufferRedirection = 1u << 1,
        Flag_MultiMonitor           = 1u << 2,
        Flag_EnhancedGraphics       = 1u << 3,
        Flag_FontSmoothing          = 1u << 4,
        Flag_DesktopComposition     = 1u << 5,
        Flag_HardwareAssist         = 1u << 6,
        Flag_RedirectClipboard      = 1u << 7,
        Flag_RedirectDrives         = 1u << 8,
        Flag_RedirectDevices        = 1u << 9,
        Flag_RedirectPorts          = 1u << 10,
        Flag_RedirectSmartCards     = 1u << 11,
        Flag_AudioCaptureRedirect   = 1u << 12,
    };

    enum class AudioMode : uint8_t
    {
        Redirect     = 0,  // play locally
        PlayOnServer = 1,
        None         = 2,  // mute
    };

    struct RdpOptions
    {
        uint16_t  port;            // typically 3389 remote, 2179 local VM
        uint16_t  desktopWidth;
        uint16_t  desktopHeight;
        uint16_t  colorDepth;      // 16, 24, or 32
        uint32_t  flags;           // RdpFlags bitmask
        uint8_t   audioMode;       // AudioMode value
        // DPI scale factor as a percent (96 DPI -> 100, 144 DPI -> 150, etc).
        // The child sets mstscax's DesktopScaleFactor extended property from
        // this before Connect so the credential UI is sized correctly for the
        // parent's display DPI — without it, mstscax assumes 100% DPI and
        // renders the pre-login credential UI at a fixed pixel size that
        // appears tiny in the upper-left of the popup on high-DPI screens
        // (the original "credential UI off-center" complaint). 0 means "not
        // set" → child defaults to 100. Matches VMPlex's RdpClient.cs:118-119.
        uint16_t  dpiScalePercent;
        uint8_t   reserved;        // pad to keep struct layout stable
        uint32_t  serverByteLen;   // utf-8 length following this struct (no null terminator)
        uint32_t  domainByteLen;   // utf-8 length following the server bytes (no null)
        uint32_t  userByteLen;     // utf-8 length following the domain bytes (no null) — v3
        // bytes: char server[serverByteLen]; char domain[domainByteLen]; char user[userByteLen];
    };

    // ---------------------------------------------------------------- Connect variants
    struct Connect
    {
        RdpOptions options;
        // server + domain UTF-8 bytes follow (per RdpOptions layout)
    };

    struct ConnectLocalVm
    {
        uint8_t    vmGuid[16];     // raw 16-byte GUID (Hyper-V VM "Name" field)
        RdpOptions options;        // server should be "localhost"; domain typically empty
    };

    // ---------------------------------------------------------------- Resize commands
    struct Resize
    {
        uint16_t width;
        uint16_t height;
    };

    // Mirrors IMsRdpClient9::UpdateSessionDisplaySettings args 1:1.
    struct UpdateSessionDisplaySettings
    {
        uint32_t width;
        uint32_t height;
        uint32_t physWidth;     // physical width in mm
        uint32_t physHeight;    // physical height in mm
        uint32_t orientation;   // typically 0 (landscape)
        uint32_t desktopScale;  // percent, typically 100
        uint32_t deviceScale;   // percent, typically 100
    };

    // ---------------------------------------------------------------- Misc commands
    struct SetEnhanced
    {
        uint8_t  enabled;       // 0 or 1
        uint8_t  reserved[3];
    };

    struct TypeText
    {
        uint32_t byteLen;       // UTF-8 byte count following this struct
        // bytes: char text[byteLen];
    };

    // SendCtrlAltDel and Shutdown have no payload (Header.payloadSize == 0).

    // ---------------------------------------------------------------- Child notifications
    struct Connecting
    {
        // empty
    };

    struct Connected
    {
        uint8_t  enhancedActive;     // 0/1 — whether the established session is enhanced
        uint8_t  reserved[3];
        uint32_t desktopWidth;
        uint32_t desktopHeight;
    };

    // discReason/extendedReason are raw values from IMsTscAxEvents::OnDisconnected.
    // See RdpClient.cs:445 for the table of meaningful (discReason, extendedReason) pairs.
    //
    // `fatal` classifies the disconnect: mstscax disconnect-reason codes 0..3
    // (NoInfo / LocalNotError / RemoteByUser / ByServer) are benign/expected
    // (clean logoff, server-initiated, guest reboot); anything > 3 is a real
    // error worth surfacing (socket/timeout/auth/protocol). The child also
    // appends a UTF-8 description string (descByteLen bytes immediately after
    // this fixed struct) produced by IMsRdpClient::GetErrorDescription, so the
    // parent can show the same localized text mstscax itself would.
    struct Disconnected
    {
        int32_t  discReason;
        int32_t  extendedReason;
        uint8_t  fatal;          // 1 = surfaceable error, 0 = benign/expected
        uint8_t  reserved[3];
        uint32_t descByteLen;    // UTF-8 description bytes following this struct
        // char desc[descByteLen]
    };

    struct EnhancedReady
    {
        uint8_t  ready;
        uint8_t  reserved[3];
    };

    struct DesktopResized
    {
        uint32_t width;
        uint32_t height;
    };

    // Higher-level errors the parent should surface in the tab UI.
    enum class RdpErrorCode : uint32_t
    {
        None                       = 0,
        BasicSessionWithShieldedVm = 1,  // matches RdpClient.RdpError.BasicSessionWithShieldedVm
        HostStartupFailed          = 2,
        ProtocolError              = 3,  // wire protocol mismatch or framing error
    };

    struct Error
    {
        uint32_t code;          // RdpErrorCode value
    };

    // MouseActivated has no payload (Header.payloadSize == 0).

    // ---------------------------------------------------------------- Diagnostic logging
    // The rdphost child sends a LogLine message for every line its HyprvLog
    // would have written. The parent receives + writes it through its own
    // unified logger (with a "[rdphost pid=N]" prefix), so we end up with one
    // hyprv.log instead of a separate hyprv-rdphost.log. Payload layout:
    //   LogLine     header (fixed)
    //   uint8_t[]   UTF-8 text bytes (no null terminator; LogLine.byteLen says how many)
    struct LogLine
    {
        uint8_t  level;         // 0=trace, 1=info, 2=warn, 3=error  (not yet acted on)
        uint8_t  reserved[3];
        uint32_t byteLen;       // UTF-8 byte count following this struct
    };

#pragma pack(pop)

    // ---------------------------------------------------------------- Sanity sizes
    // These are a compile-time check that the layout doesn't shift accidentally —
    // any change to a fixed payload struct is a wire-incompatible protocol bump.
    static_assert(sizeof(Hello)                        == 4,  "Hello layout drift");
    static_assert(sizeof(HelloAck)                     == 4,  "HelloAck layout drift");
    static_assert(sizeof(HwndReady)                    == 8,  "HwndReady layout drift");
    static_assert(sizeof(RdpOptions)                   == 28, "RdpOptions layout drift");
    static_assert(sizeof(Connect)                      == 28, "Connect layout drift");
    static_assert(sizeof(ConnectLocalVm)               == 44, "ConnectLocalVm layout drift");
    static_assert(sizeof(Resize)                       == 4,  "Resize layout drift");
    static_assert(sizeof(UpdateSessionDisplaySettings) == 28, "UpdateSessionDisplaySettings layout drift");
    static_assert(sizeof(SetEnhanced)                  == 4,  "SetEnhanced layout drift");
    static_assert(sizeof(TypeText)                     == 4,  "TypeText layout drift");
    static_assert(sizeof(Connected)                    == 12, "Connected layout drift");
    static_assert(sizeof(Disconnected)                 == 16, "Disconnected layout drift");
    static_assert(sizeof(EnhancedReady)                == 4,  "EnhancedReady layout drift");
    static_assert(sizeof(DesktopResized)               == 8,  "DesktopResized layout drift");
    static_assert(sizeof(Error)                        == 4,  "Error layout drift");
    static_assert(sizeof(LogLine)                      == 8,  "LogLine layout drift");
}
