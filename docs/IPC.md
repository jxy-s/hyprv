# hyprv ↔ rdphost IPC

Wire-protocol reference. Source of truth: `src/shared/RdpIpc.h` (read it — it has more inline detail than this doc).

## Transport

- **Named pipe**, one per child. Name: `\\.\pipe\hyprv-rdp-<guid>` (the GUID is the pipe id passed to the child via `--pipe=<guid>` on the command line).
- Parent (`hyprv.exe`) is the **server**. Creates the pipe, spawns the child suspended, assigns to the kill-on-job-close job, resumes.
- Child (`hyprv-rdphost.exe`) connects, completes a `Hello`/`HelloAck` handshake, sends `HwndReady` once its popup HWND exists.
- **Protocol version: 3** (`kProtocolVersion`). The handshake negotiates min/max; both EXEs ship together so they always agree. v2 enriched `Disconnected` (added the `fatal` byte + trailing description string — see "Disconnect classification" below). v3 added `RdpOptions.userByteLen` + a trailing UTF-8 username (after the server/domain bytes) so generic RDP connections (Remote Hosts) can pre-fill mstscax's credential prompt with the saved user name.

## Frame format

Every message is a fixed 8-byte header followed by `payloadSize` bytes:

```c++
struct Header {
    uint8_t  type;          // P2C or C2P enum
    uint8_t  reserved0;
    uint16_t reserved1;
    uint32_t payloadSize;
};
```

Direction is implicit in the sender. `type` enum values 0-127 are parent→child (P2C); 128-255 are child→parent (C2P). A printed dump can tell direction at a glance.

All payload structs are `#pragma pack(push, 1)` + `static_assert(sizeof(X) == N, "X layout drift")` so a refactor that grows a field will fail to compile until you update both sides + the assert.

## P2C — parent to child

| Value | Name                          | Payload                                                          | When                                                 |
|------:|-------------------------------|------------------------------------------------------------------|------------------------------------------------------|
| 1     | `Hello`                       | `Hello` (4B)                                                     | First message after pipe-connect                     |
| 2     | `Connect`                     | `Connect` + UTF-8 server + domain + username                    | Generic RDP connect (Remote Hosts) — `RdpHost::OnConnect` |
| 3     | `ConnectLocalVm`              | `ConnectLocalVm` + UTF-8 server                                  | Start a Hyper-V VM session — vmGuid + RdpOptions     |
| 4     | `Disconnect`                  | _(none)_                                                         | Tear down mstscax session, keep child alive          |
| 5     | `Shutdown`                    | _(none)_                                                         | Child posts `WM_QUIT`, process exits                 |
| 6     | `Resize`                      | `Resize` (4B)                                                    | Container resize hint (basic mode only)              |
| 7     | `UpdateSessionDisplaySettings`| `UpdateSessionDisplaySettings` (28B)                             | Enhanced post-login dynamic resize                   |
| 8     | `SetEnhanced`                 | `SetEnhanced` (4B)                                               | Flip enhanced flag — needs reconnect after           |
| 9     | `SendCtrlAltDel`              | _(none)_                                                         | (Reserved — parent uses Msvm_Keyboard.TypeCtrlAltDel) |
| 10    | `TypeText`                    | `TypeText` (4B)                                                  | (Reserved — parent uses Msvm_Keyboard.TypeText)      |

## C2P — child to parent

| Value | Name             | Payload                            | When                                                |
|------:|------------------|------------------------------------|-----------------------------------------------------|
| 128   | `HelloAck`       | `HelloAck` (4B)                    | Child responds to `Hello`                           |
| 129   | `HwndReady`      | `HwndReady` (8B — popup HWND)      | After popup window is created; parent does EmbedInto |
| 130   | `Connecting`     | _(none)_                           | mstscax `OnConnecting` (DISPID 1)                   |
| 131   | `Connected`      | `Connected` (12B)                  | mstscax `OnConnected` (DISPID 2)                    |
| 132   | `Disconnected`   | `Disconnected` (16B) + UTF-8 desc  | mstscax `OnDisconnected` (DISPID 4) — see below      |
| 133   | `EnhancedReady`  | `EnhancedReady` (4B)               | mstscax `OnLoginComplete` (DISPID 3) — enhanced ready |
| 134   | `DesktopResized` | `DesktopResized` (8B)              | mstscax `OnRemoteDesktopSizeChange` (DISPID 12)     |
| 135   | `Error`          | `Error` (4B — RdpErrorCode)        | Setup / connect failure (parent maps code → message) |
| 136   | `MouseActivated` | _(none)_                           | `WM_MOUSEACTIVATE` in popup (focus handoff hint)    |
| 137   | `LogLine`        | `LogLine` (8B) + UTF-8 text bytes  | Child's `HyprvLog` forwards each line to parent     |

## Connect-flow timing

The protocol is intentionally simple — single-pipe, blocking handshake on the parent's main thread. **Do not send C2P frames during the parent's synchronous handshake window** (between sending `Hello` and reading `HwndReady`). The parent's `ReadExact` is mode-blind; an unexpected `LogLine` arriving mid-handshake gets consumed as a `HwndReady` header and the parent bails. This is why `g_ipcLogger` in `src/rdphost/main.cpp` is only set AFTER `HwndReady` is sent and `StartReceiveLoop` is running — by then the parent's async rx thread is consuming the pipe and knows how to dispatch every C2P type.

```
parent                                            child
  │                                                 │
  ├──── CreateProcess hyprv-rdphost --pipe=guid ──▶│
  │                                                  ConnectNamedPipe
  │ ◀──── (pipe connected) ───────────────────────┤
  │                                                 │
  ├──── P2C::Hello ──────────────────────────────▶│
  │                                                 │ IpcClient::Handshake
  │ ◀──── C2P::HelloAck ─────────────────────────┤
  │                                                 │
  │                                                  CreateWindowEx (popup)
  │                                                  RdpHost::Activate (AtlAxWin)
  │                                                 │
  │ ◀──── C2P::HwndReady ────────────────────────┤
  │                                                 │
  ├──── EmbedInto cross-process SetWindowPos ───▶│
  │     (changes ownership, may change rect)        │ ← WM_SIZE fires if rect changed
  │                                                 │
  ├──── P2C::ConnectLocalVm (or Connect) ────────▶│
  │                                                  RdpHost::OnConnectLocalVm
  │ ◀──── (async) C2P::DesktopResized ───────────┤  put_DesktopWidth/Height + Connect()
  │ ◀──── (async) C2P::Connecting ───────────────┤   ← mstscax DISPID 1
  │ ◀──── (async) C2P::Connected ────────────────┤   ← mstscax DISPID 2
  │ ◀──── (async) C2P::EnhancedReady (enhanced) ─┤   ← mstscax DISPID 3
  │ ◀──── (async) C2P::LogLine x N (continuous) ─┤   ← rdphost diag stream
  │                                                 │
  │ ◀── pipe break OR P2C::Shutdown ─ tear-down ──┤
```

## Disconnect classification (connect-error surfacing)

`Disconnected` carries the raw mstscax `discReason`/`extendedReason` **plus** a `fatal` byte and an optional trailing UTF-8 description (`descByteLen` bytes after the fixed struct):

- **`fatal`** — the child sets it to `discReason > 3`. mstscax reason codes 0..3 (NoInfo / LocalNotError / RemoteByUser / ByServer) are benign/expected (clean logoff, server-initiated, guest reboot); anything higher (socket / timeout / auth / protocol) is a real error.
- **description** — only populated when `fatal`; it's the localized text from `IMsRdpClient::GetErrorDescription(discReason, ext)`, i.e. the same message mstscax itself would show. Benign drops send `descByteLen = 0` to keep the common reboot/logoff frame small.

The parent (`VmTabPage`) does NOT surface every fatal drop — it gates on whether the attempt ever reached `Connected`:

- An **established session** that drops (was Connected) is treated as a benign reboot/logoff regardless of reason → reconnect quietly (fast 700ms basic / 2s enhanced).
- A **connect attempt** that never reached Connected and keeps failing with `fatal` on a **Running** VM increments a counter; after `kMaxConnectFailures` (4) consecutive failures it stops retrying and renders the error inline in the tab placeholder (the description + a **Retry** button). The optimistic pre-connect window (VM still powering up) never counts — those failures are expected.

`Error` (HostStartupFailed / ProtocolError / BasicSessionWithShieldedVm) is always terminal for the attempt; the parent maps the `RdpErrorCode` to a friendly string and surfaces it through the same placeholder + Retry path. Surfacing is **non-modal** (inline placeholder, not a dialog) so reconnect churn can't spam modal popups.

## When you add a new IPC message

1. Add the enum value in either `P2C` or `C2P` (don't reuse retired numbers — append).
2. Add the payload struct in `src/shared/RdpIpc.h` under `#pragma pack(push, 1)`.
3. Add a `static_assert(sizeof(X) == N, "X layout drift")` at the bottom.
4. On the parent side (`src/app/rdp/RdpHostClient.cpp`):
   - If P2C: add a public method like `void X(args)` that builds the payload + calls `m_impl->SendP2C(P2C::X, ...)`.
   - If C2P: add a `std::function` callback field in `RdpHostClient.h` and a dispatch case in `Impl::DispatchEvent`.
5. On the child side:
   - If P2C: add a dispatch case in `src/rdphost/main.cpp` `DispatchIpcMessage`. Route into `RdpHost::OnX(...)`.
   - If C2P: send via `IpcClient::Send(C2P::X, ...)` from wherever the trigger lives (DISPID handler, periodic timer, etc.).
6. Build both targets — the `static_assert` is your guard against forgetting half the change.

Treat `src/shared/RdpIpc.h` changes as a wire-protocol bump. They need to land atomically — both `hyprv.exe` and `hyprv-rdphost.exe` are rebuilt together from the same source.
