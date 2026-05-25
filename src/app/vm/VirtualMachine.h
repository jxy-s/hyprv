// VirtualMachine — wrapper around a single Hyper-V VM (Msvm_ComputerSystem).
//
// For task #11 this is a minimal data carrier: GUID, name, and EnabledState.
// Task #12 will expand this to a full observable model with PropertyChanged
// notifications, snapshot list, the RequestStateChange action, etc.

#pragma once

#include "../wmi/generated/HyperV.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hyprv::app::vm
{
    // Hyper-V EnabledState values (Msvm_ComputerSystem). Documented in:
    // https://learn.microsoft.com/en-us/windows/win32/hyperv_v2/msvm-computersystem
    enum class VmState : uint16_t
    {
        Unknown          = 0,
        Other            = 1,
        Running          = 2,
        Off              = 3,
        Stopping         = 4,
        Saved            = 6,
        Paused           = 9,
        Starting         = 10,
        Reset            = 11,
        Saving           = 32773,
        Pausing          = 32776,
        Resuming         = 32777,
        FastSaved        = 32779,
        FastSaving       = 32780,
        Hibernated       = 32781,
    };

    // Maximum number of history samples retained per VM for the flyout
    // sparklines. At 1 sample/sec this is exactly 60 seconds of trail —
    // matches Hyper-V Manager's own counter pane (which shows ~1 min of past).
    constexpr size_t kHistoryDepth = 60;

    // One disk attachment on a VM. kind distinguishes a Virtual Hard Disk
    // (VHD/VHDX/AVHDX) from an attached ISO. fileSizeBytes is the actual
    // on-disk size of the file (cheap stat) — VHD virtual/max size needs
    // a heavier Msvm_ImageManagementService call and is left for later.
    enum class DiskKind { Hdd, Dvd, Other };
    struct VirtualDisk
    {
        DiskKind     kind = DiskKind::Other;
        std::wstring path;          // full host-side path
        uint64_t     fileSizeBytes = 0;
    };

    // One virtual NIC on a VM, mirroring what Hyper-V Manager's Networking
    // tab shows per row. macAddress is stored as 12 raw hex chars (no
    // separators) — formatted with colons at display time.
    struct NetworkAdapter
    {
        // The NIC's own GUID — second GUID embedded in the synthetic-port
        // setting data's InstanceID ("Microsoft:<VM-GUID>\<NIC-GUID>").
        // Stable across polls and used by the settings editor as the
        // address for per-NIC edits (e.g. SetNetworkAdapterSwitch).
        std::wstring nicGuid;
        std::wstring name;          // ElementName, typically "Network Adapter"
        std::wstring macAddress;    // 12 hex chars, e.g. "00155D013D0F"
        bool         dynamicMac = false;
        std::wstring switchName;    // "Default Switch" / "" if disconnected
        // Access VLAN ID (Msvm_EthernetSwitchPortVlanSettingData, OperationMode
        // 1=Access). 0 == untagged / no VLAN feature setting.
        uint16_t     vlanId = 0;
        // Full VLAN mode (OperationMode): 0 = untagged (no setting), 1 = Access,
        // 2 = Trunk, 3 = Private. Hyper-V Manager's GUI exposes Access only;
        // trunk/private are PowerShell-only there (hyprv surfaces all three).
        // `vlanId` above carries the Access VLAN (mode 1).
        uint16_t     vlanMode = 0;
        // Trunk mode (2): the native/untagged VLAN + the set of allowed tagged
        // VLANs (TrunkVlanIdArray). Empty list when not in trunk mode.
        uint16_t     nativeVlanId = 0;
        std::vector<uint16_t> trunkVlanList;
        // Private mode (3): primary + secondary VLAN and the PVLAN role
        // (PvlanMode: 1 = Isolated, 2 = Community, 3 = Promiscuous). The
        // promiscuous role also carries a list of reachable secondary VLANs.
        uint16_t     primaryVlanId   = 0;
        uint16_t     secondaryVlanId = 0;
        uint8_t      pvlanMode       = 0;
        std::vector<uint16_t> secondaryVlanList;
        // Advanced features (Msvm_EthernetSwitchPortSecuritySettingData, a
        // feature setting on the port allocation). All default off / None when
        // the NIC has no security setting (Hyper-V adds one on first edit).
        bool         macSpoofing = false;
        bool         dhcpGuard   = false;
        bool         routerGuard = false;
        bool         nicTeaming  = false;
        uint8_t      portMirroring = 0;   // MonitorMode: 0=None, 1=Destination, 2=Source
        // IEEE 802.1p priority tagging — allow the VM to send/honor priority-tagged
        // frames (Msvm_EthernetSwitchPortSecuritySettingData.AllowIeeePriorityTag,
        // a sibling bool of macSpoofing on the same security feature setting).
        bool         ieeePriorityTag = false;
        // "Protected network" (Msvm_SyntheticEthernetPortSettingData.ClusterMonitored)
        // — move the VM to another cluster node if this NIC loses connectivity.
        // Default true (Hyper-V's default). Only meaningful on clustered hosts.
        bool         clusterMonitored = true;
        // Bandwidth limits (Msvm_EthernetSwitchPortBandwidthSettingData, yet
        // another feature setting on the port allocation). Raw bits/sec;
        // 0 = unlimited / no reservation. UI shows Mbps (value / 1,000,000).
        uint64_t     bandwidthMaxBps = 0;   // Limit (maximum)
        uint64_t     bandwidthMinBps = 0;   // Reservation (minimum, absolute)
        // Device naming — expose this adapter's name into the guest
        // (Msvm_SyntheticEthernetPortSettingData.DeviceNamingEnabled, a bool on
        // the port like clusterMonitored).
        bool         deviceNaming = false;
        // Hardware acceleration (Msvm_EthernetSwitchPortOffloadSettingData — a
        // feature setting on the connection that ALWAYS exists by default).
        //   VMQ:    VMQOffloadWeight  (0 = off, 100 = on)
        //   SR-IOV: IOVOffloadWeight  (0 = off, 100 = on)
        //   IPsec:  IPSecOffloadLimit (0 = off, else the max # offloaded SAs)
        bool         vmqEnabled   = false;
        bool         sriovEnabled = false;
        bool         ipsecOffload = false;
        uint32_t     ipsecOffloadMaxSA = 512;
        std::vector<std::wstring> ipv4;
        std::vector<std::wstring> ipv6;
        bool         connected = false;
    };

    // One snapshot entry on a VM. The path is the WMI __PATH for the
    // Msvm_VirtualSystemSettingData backing the snapshot — fed back to
    // ApplySnapshot / DestroySnapshot to identify which snapshot to act on.
    //
    // parentInstanceId / depth let the flyout render checkpoints as a tree
    // (preorder DFS with depth-based indentation). Hyper-V's `Parent`
    // property is a full WMI path containing the parent VSSD's InstanceID;
    // we extract just the InstanceID substring for matching.
    struct Snapshot
    {
        std::wstring path;
        std::wstring instanceId;
        std::wstring parentInstanceId;   // empty for root snapshots
        std::wstring elementName;
        int          depth = 0;          // 0 for root, +1 per descent
        std::optional<std::chrono::system_clock::time_point> creationTime;
        // True for the snapshot the VM's live state currently descends from
        // (Hyper-V's Msvm_MostCurrentSnapshotInBranch — the "Now" position).
        // Lets the UI mark which branch the VM is sitting on.
        bool         isCurrent = false;
    };

    // Argument values for Msvm_ComputerSystem::RequestStateChange. Mirrors
    // VMPlex's StateChange enum (VirtualMachine.cs:49-58). Differs from VmState
    // — these are *target* states, not current states.
    enum class VmStateChange : uint16_t
    {
        Enabled  = 2,    // Start / Resume from Off, Saved, or Paused
        Disabled = 3,    // Turn off (hard) — yanks the plug, no chance to save
        Shutdown = 4,    // Shut down (graceful) — guest via integration services
        Offline  = 6,    // Save — suspend to disk
        Quiesce  = 9,    // Pause — freeze in memory
        Reboot   = 10,   // Restart (graceful) — guest via integration services
        Reset    = 11,   // Reset (hard) — equivalent to physical reset button
    };

    struct VirtualMachine
    {
        std::wstring guid;         // Msvm_ComputerSystem.Name (the VM's GUID)
        std::wstring elementName;  // user-visible display name
        VmState      state = VmState::Unknown;
        // Optimistic UI flag: set the instant the user requests a state change
        // (start/stop/pause/...) and held until the VM's state actually moves
        // (or the job completes). Lets the status dot blink immediately instead
        // of waiting ~1 poll for Hyper-V to report the transitional state.
        // Set/cleared by VMManager (m_pendingStateChange); NOT a WMI value.
        bool         pendingStateChange = false;
        // Human label for an in-flight labeled async op (snapshot take/apply/
        // delete), e.g. "Taking snapshot…". Empty for plain power actions and
        // stable VMs. Drives the info-panel snapshots progress row; also routed
        // into statusText (below) when Hyper-V has no native job text yet.
        // Set/cleared by VMManager (m_pendingStateChange.label); NOT a WMI value.
        std::wstring pendingJobLabel;
        // WMI-provided live status from Msvm_SummaryInformation.AsynchronousTasks
        // — e.g. "Saving Virtual Machine (35%)", "Restoring Virtual Machine (10%)".
        // Empty for stable VMs. Matches VMPlex's MakeStatusText pattern exactly.
        std::wstring statusText;
        // Msvm_ComputerSystem.EnhancedSessionModeState — true only when WMI
        // reports the value AllowedAndAvailable (2). Drives whether the RDP
        // host attempts enhanced mode (integration-services backed) or plain
        // basic (frame-buffer) mode. Linux guests without LIS, the firmware/
        // boot screen, and pre-login on some SKUs all fall to false here.
        bool         enhancedSessionAvailable = false;

        // ---- Summary (Msvm_SummaryInformation) -------------------------------
        // Populated by VMManager::Reload. All have safe defaults so a freshly
        // constructed VirtualMachine renders as "(unknown)" / "-" everywhere.
        uint16_t     numProcessors    = 0;
        // Memory triplet: assigned is the host-allocated RAM (static when
        // dynamic memory is off, or set to startup at the host's discretion
        // when on). available is the buffer Hyper-V keeps free beyond what
        // the guest is requesting — derived demand = assigned - available.
        // demand is what the guest is actively pulling and the value we
        // sparkline because it's the one that moves second-to-second.
        uint64_t     memoryAssignedMb = 0;
        int32_t      memoryAvailableMb = 0;  // raw WMI field, no longer used for demand
        uint64_t     memoryDemandMb   = 0;
        // Current memory pressure as reported by the Hyper-V dynamic memory
        // balancer perf counter — demand expressed as a percentage of the
        // physical memory assigned (>100 = demand exceeds assigned). 0 means
        // we have no perf data (fixed memory VM, or VM stopped). This is the
        // canonical source for both Memory Demand and Memory Status; the
        // SummaryInformation.MemoryAvailable field above is a different
        // "buffer beyond 5% target" calc and isn't useful for the UI.
        uint32_t     memoryPressurePct = 0;
        uint16_t     processorLoadPct = 0;   // 0..100
        uint64_t     uptimeMs         = 0;   // since last boot
        std::optional<std::chrono::system_clock::time_point> creationTime;
        std::wstring guestOs;
        std::wstring configVersion;
        // "Generation 1" / "Generation 2" — derived from VirtualSystemSubType.
        std::wstring generation;
        std::wstring notes;
        // CIM HeartbeatComponent state. 2 = OK, 6 = Error, 12 = NoContact,
        // 13 = LostCommunication. 0 = service not running. See Msvm_HeartbeatComponent.
        uint16_t     heartbeatState   = 0;
        // True when the guest's Shutdown integration service is up — i.e. a
        // Msvm_ShutdownComponent exists for this VM (keyed by SystemName=GUID,
        // present only while running with the service reporting). Gates the
        // "Shut down" action: graceful shutdown via InitiateShutdown is
        // impossible without it (no LIS / pre-login / Linux without
        // hyperv-daemons) — Turn off is the only path then. False when off or
        // when IS hasn't come up yet after boot.
        bool         shutdownServiceAvailable = false;

        // Ring-style history buffers (oldest first, newest at end). Populated
        // only while the VM is running; cleared when it stops so a fresh boot
        // gets a clean sparkline. kHistoryDepth caps each at 60 samples.
        std::vector<uint16_t> cpuHistoryPct;
        std::vector<uint32_t> memoryHistoryMb;

        // Snapshot list for this VM in preorder DFS order — roots first,
        // each followed by its descendants. Use Snapshot::depth for indent.
        std::vector<Snapshot> snapshots;

        // ---- VirtualSystemSettingData (current realised config) -------------
        // Populated alongside the summary info by querying Msvm_VSSD with
        // VirtualSystemType = 'Microsoft:Hyper-V:System:Realized'.
        std::optional<bool> secureBootEnabled;
        // Secure Boot template GUID (Msvm_VirtualSystemSettingData.
        // SecureBootTemplateId). Braced lowercase, e.g.
        // "{1734c6e8-3154-4dda-ba5f-a874cc483422}" (Microsoft Windows).
        // Empty when not reported (Gen 1, or a freshly-loaded snapshot).
        // Picks which CA the firmware trusts — Linux Gen 2 guests need the
        // "Microsoft UEFI Certificate Authority" template, not the Windows one.
        std::wstring secureBootTemplateId;
        // Automatic start/stop actions (Msvm_VirtualSystemSettingData).
        // Startup: 2 = Nothing, 3 = Start if it was running when the host's
        // VMMS service stopped, 4 = Always start automatically. Shutdown:
        // 2 = Turn off, 3 = Save state, 4 = Shut down (guest). 0 = unknown
        // (pre-first-poll / read failure).
        uint16_t     autoStartAction = 0;
        uint16_t     autoStopAction  = 0;
        // Delay (seconds) before the automatic start action fires — staggers
        // boot across VMs. Parsed from the CIM interval AutomaticStartupActionDelay.
        uint32_t     autoStartDelaySeconds = 0;
        // Security settings (Msvm_SecuritySettingData). vTPM on/off and
        // "encrypt state + live-migration traffic". nullopt when not reported
        // (pre-first-poll, or a generation/host without a security setting).
        std::optional<bool> tpmEnabled;
        std::optional<bool> encryptStateEnabled;
        std::wstring configDataRoot;    // disk path where .vmcx + .vmrs live
        std::wstring snapshotDataRoot;  // disk path for checkpoint .avhdx files
        std::wstring swapFileDataRoot;  // Smart Paging file location (SwapFileDataRoot)
        // Checkpoint type (Msvm_VirtualSystemSettingData.UserSnapshotType):
        // 2 = Disabled, 3 = Production (fall back to standard), 4 = Production
        // only, 5 = Standard. 0 = not yet reported.
        uint16_t     userSnapshotType = 0;
        // "Use automatic checkpoints" (Msvm_VirtualSystemSettingData.
        // AutomaticSnapshotsEnabled) — auto-create a checkpoint when the VM
        // starts (client Hyper-V feature).
        bool         automaticCheckpointsEnabled = false;
        // BIOS / firmware GUID — distinct from the VM's own GUID, this one
        // is the one the guest OS sees as its SMBIOS UUID.
        std::wstring biosGuid;

        // ---- Memory settings (Msvm_MemorySettingData) -----------------------
        // Static config (host's intent) rather than runtime (what the VM is
        // currently using). Hyper-V Manager's Memory tab shows the full
        // picture: startup + dynamic min/max + current assigned/demand.
        // VirtualQuantity is documented in MB.
        uint64_t memStartupMb        = 0;  // VirtualQuantity
        bool     dynamicMemoryEnabled = false;
        uint64_t memMinMb            = 0;  // Reservation (when dynamic on)
        uint64_t memMaxMb            = 0;  // Limit (when dynamic on)

        // ---- Guest network IPs (Msvm_GuestNetworkAdapterConfiguration) -----
        // Aggregate v4/v6 across all of the VM's NICs. The per-NIC breakdown
        // lives in `networkAdapters` below.
        std::vector<std::wstring> guestIpv4;
        std::vector<std::wstring> guestIpv6;

        // Per-NIC detail (Msvm_SyntheticEthernetPortSettingData joined to
        // Msvm_EthernetPortAllocationSettingData -> Msvm_VirtualEthernetSwitch
        // + Msvm_GuestNetworkAdapterConfiguration). Empty if the VM has no
        // virtual NICs (rare) or while WMI hasn't reported settings yet.
        std::vector<NetworkAdapter> networkAdapters;

        // Disk attachments — VHDs + DVD ISOs from Msvm_StorageAllocationSettingData.
        std::vector<VirtualDisk> disks;

        // ---- KVP (Msvm_KvpExchangeComponent.GuestIntrinsicExchangeItems) ----
        // Reported by the in-guest integration services when running. All
        // empty when the VM is off, has no integration services, or is on a
        // guest OS that doesn't ship them (older Linux distros). IPs are
        // explicitly NOT here — Microsoft deprecated the NetworkAddressIPv4 /
        // IPv6 KVP keys; see guestIpv4 / guestIpv6 above.
        std::wstring kvpOsName;        // e.g. "Windows 11 Pro"
        std::wstring kvpOsVersion;     // e.g. "10.0.22631"
        std::wstring kvpOsBuildNumber; // e.g. "22631"
        std::wstring kvpFqdn;          // FullyQualifiedDomainName
        std::wstring kvpIntegrationServicesVersion;

        // Convenience predicates mirroring VMPlex's IsRunning / IsPaused etc.
        bool IsRunning() const noexcept { return state == VmState::Running; }
        bool IsPaused()  const noexcept { return state == VmState::Paused; }
        bool IsSaved()   const noexcept { return state == VmState::Saved; }
        bool IsOff()     const noexcept { return state == VmState::Off; }
        bool IsPoweredOn() const noexcept { return IsRunning() || IsPaused(); }

        // True when the VM is mid-transition (Starting/Stopping/Saving/etc.).
        // UI shows an amber dot + status suffix while this is true.
        bool IsTransitioning() const noexcept
        {
            switch (state)
            {
            case VmState::Stopping:
            case VmState::Starting:
            case VmState::Reset:
            case VmState::Saving:
            case VmState::Pausing:
            case VmState::Resuming:
            case VmState::FastSaving:
                return true;
            default:
                return false;
            }
        }

        // Tooltip-only label for stable states (paused, saved, hibernated).
        // Empty when an async job is in flight (the live statusText takes over)
        // or when the VM is in an unambiguous Running/Off state.
        std::wstring StableStatusLabel() const
        {
            switch (state)
            {
            case VmState::Running:     return L"Running";
            case VmState::Off:         return L"Off";
            case VmState::Paused:      return L"Paused";
            case VmState::Saved:       return L"Saved";
            case VmState::Hibernated:  return L"Hibernated";
            default:                   return {};
            }
        }
    };

}
