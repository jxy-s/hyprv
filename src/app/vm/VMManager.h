// VMManager — process-wide singleton that owns the live list of Hyper-V VMs.
//
// On first access opens a WmiScope to root\virtualization\v2, resolves the
// host's Msvm_VirtualSystemManagementService + Msvm_VirtualSystemSnapshotService
// singletons, takes one shot at refreshing the VM list, and then runs a
// background thread that polls Msvm_VirtualSystemManagementService::
// GetSummaryInformation every second. The polled batch call is the only
// reliable way to keep the live counters (ProcessorLoad / MemoryUsage /
// UpTime / Heartbeat) fresh — fetching Msvm_SummaryInformation via per-VM
// association leaves those dynamic fields at zero.
//
// Three WMI subscriptions on Msvm_ComputerSystem still ride alongside the
// poller, so VM creation/deletion/state transitions surface within the
// __InstanceModificationEvent WITHIN 1 polling interval rather than waiting
// for the next 1 s tick.
//
// Consumers subscribe via SetOnChanged() to learn that *something* changed
// and re-read the current snapshot via GetAll(). OnChanged fires on both the
// WMI worker thread and the poller — marshal to the UI dispatcher inside.

#pragma once

#include "VirtualMachine.h"
#include "../wmi/WmiScope.h"
#include "../wmi/WmiSubscription.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hyprv::app::vm
{
    class VMManager
    {
    public:
        // Lazy singleton accessor. NEVER throws: if root\virtualization\v2 can't
        // be opened (Hyper-V not installed, or the user lacks access), construction
        // records a connect status (see GetConnectStatus) and skips the poll thread
        // instead of throwing — the welcome page surfaces an actionable message.
        // (It's constructed unguarded from App::OnLaunched, so a throw here would
        // crash startup.) Subsequent calls return the cached instance.
        static VMManager& Instance();

        // Why the initial Hyper-V connection failed, if it did. The welcome page
        // reads this to show an actionable error instead of spinning "Loading
        // VMs…" forever.
        enum class ConnectStatus { Ok, HyperVUnavailable, AccessDenied, Other };
        ConnectStatus GetConnectStatus() const { return m_connectStatus; }
        // User-facing message for the current connect status ("" when Ok).
        std::wstring ConnectErrorMessage() const;
        // Re-attempt the Hyper-V connection (the welcome page's Retry button).
        // Returns true if now connected (and starts the poll thread). Call from
        // the UI thread; a no-op returning true when already connected.
        bool RetryConnect();

        // Current snapshot of all VMs. Cheap copy; callers can hold and inspect.
        std::vector<VirtualMachine> GetAll() const;

        // Look up a single VM by GUID. Returns std::nullopt if not found.
        std::optional<VirtualMachine> GetByGuid(std::wstring const& guid) const;

        // True once at least one poll iteration has completed (i.e. m_vms has
        // been populated from a real WMI snapshot at least once). Used by
        // tab restore + VM tab pages to distinguish "VM not in cache because
        // VMManager hasn't polled yet" from "VM not in cache because it was
        // deleted." A bare GetByGuid returning nullopt is ambiguous between
        // those two during cold start.
        bool HasFirstSnapshot() const { return m_pollGen.load() > 0; }

        using OnChangedFn = std::function<void()>;
        using SubToken    = uint64_t;
        // Subscribe to "something changed" notifications. Returns a token to pass
        // to RemoveOnChanged. MULTIPLE subscribers are fanned out — tab tear-out
        // means N MainWindows each need ticks (a single-sink setter would starve
        // all but the last-activated window). Fires on either the WMI worker
        // thread or the 1 s poller thread; marshal to the UI dispatcher inside.
        SubToken AddOnChanged(OnChangedFn cb);
        void     RemoveOnChanged(SubToken token);

        // Error notification — fired when an async VM operation FAILS (a state-
        // change job that completes with an error, or a request WMI rejects
        // up front). Fires OFF the UI thread (a job-watcher thread), so the
        // callback must marshal to the UI itself (like OnChanged). vmName is the
        // VM's display name; message is the job's ErrorDescription (or a generic
        // reason). Pass nullptr to unsubscribe.
        using ErrorFn = std::function<void(std::wstring const& vmName,
                                           std::wstring const& message)>;
        SubToken AddOnError(ErrorFn cb);
        void     RemoveOnError(SubToken token);

        // Invoke Msvm_ComputerSystem::RequestStateChange for the VM with the
        // given GUID. Returns true if WMI accepted the request (0 = success,
        // 4096 = async job started). On 4096 a background watcher waits for the
        // job and, if it FAILS, fires OnError; an up-front rejection fires
        // OnError immediately. The state transition itself is reflected by the
        // next OnChanged.
        bool RequestStateChange(std::wstring const& guid, VmStateChange state);

        // Graceful guest shutdown via the integration-services
        // Msvm_ShutdownComponent::InitiateShutdown — NOT
        // Msvm_ComputerSystem::RequestStateChange(Shutdown). The IS path
        // leaves the VM in the Running state until the guest OS actually
        // halts, so a force-off (Turn off) remains valid throughout (the
        // RequestStateChange path instead drops the VM into the Stopping
        // state, where a follow-up RequestStateChange(Disabled) is rejected
        // with 32775 — see CLAUDE.md gotcha #22). force=false is graceful
        // (a blocking guest app can stall the shutdown — that's exactly when
        // the user reaches for Turn off, which now works); force=true closes
        // apps without prompting. Requires integration services up: returns
        // false if the VM has no Msvm_ShutdownComponent (e.g. IS not running).
        bool ShutdownVM(std::wstring const& guid, bool force);

        // Snapshot operations — all are synchronous "request fired" calls;
        // Hyper-V runs the work as an async job and a follow-up OnChanged
        // reflects the new snapshot tree. Each returns true if WMI accepted.
        // snapshotPath is the WMI __PATH stored in Snapshot::path.
        bool TakeSnapshot(std::wstring const& guid);
        // vmGuid is the owning VM (the snapshot's __PATH InstanceID is the
        // snapshot's OWN guid, not the VM's, so the caller must supply it — it's
        // needed for the progress blink and to turn the VM off when required).
        // ApplySnapshot requires the VM Off (Hyper-V rejects a running apply with
        // 32775); when it's running/paused/saved it auto-stops first (turn off,
        // wait, then apply — matches Hyper-V Manager). DeleteSnapshot works in
        // any state.
        bool ApplySnapshot(std::wstring const& vmGuid, std::wstring const& snapshotPath);
        bool DeleteSnapshot(std::wstring const& vmGuid, std::wstring const& snapshotPath,
                            bool subtree);
        // Rename a snapshot (sets its VSSD ElementName via ModifySystemSettings,
        // like RenameVM). Works in any VM state.
        bool RenameSnapshot(std::wstring const& snapshotPath, std::wstring const& newName);

        // ---- Guest input ----------------------------------------------------
        // Both go via Msvm_Keyboard, associated to the VM's Msvm_ComputerSystem.
        // Synchronous WMI invocations; returns true if WMI accepted the call.

        // Inject Ctrl+Alt+Del. Equivalent to mstscax's SendOnVirtualChannel
        // dance, but works regardless of enhanced/basic and doesn't need the
        // rdphost child.
        bool TypeCtrlAltDel(std::wstring const& guid);

        // Type a string into the guest as if the user pressed each key.
        // Useful for "paste clipboard text" into pre-login screens or basic-
        // session VMs where clipboard sharing isn't available.
        bool TypeText(std::wstring const& guid, std::wstring const& text);

        // ---- VM lifecycle ---------------------------------------------------
        // Destroy (delete) the VM via Msvm_VirtualSystemManagementService::
        // DestroySystem. The VM must already be Off — Hyper-V rejects destroy
        // on running/saved VMs. Caller should confirm via a dialog before
        // invoking, and ensure RequestStateChange(Disabled) has completed.
        //
        // deleteVhds: when true, the VM's attached virtual hard-disk FILES are
        // deleted from disk after the destroy job completes (pass-through disks
        // are skipped — there's no file to delete). The paths are captured
        // BEFORE the destroy (while the VM still exists) and the files removed
        // once the VM — which held them open — is gone. Hyper-V Manager never
        // deletes VHDs; this is an opt-in convenience surfaced as a checkbox on
        // the Delete confirmation.
        bool DestroyVM(std::wstring const& guid, bool deleteVhds = false);

        // ---- New VM creation (the welcome-page "New VM..." wizard) ---------
        // Everything the wizard collects. The orchestration is one new WMI
        // write (DefineSystem) plus a sequence of already-verified Add*/Set*
        // helpers. NOTE (verified reversibly): a RAW DefineSystem VM ships with
        // NO SCSI controller, NO NIC and NO DVD drive — unlike the New-VM
        // cmdlet — so CreateVM adds a SCSI controller for Gen 2 before attaching
        // storage, adds a NIC only if a switch is chosen, and adds a DVD drive
        // only when an install ISO is supplied. Gen 1 already has its two IDE
        // controllers from DefineSystem.
        struct NewVmConfig
        {
            std::wstring name;
            int          generation     = 2;       // 1 or 2
            uint64_t     startupMemoryMb = 2048;
            bool         dynamicMemory  = true;
            uint32_t     cpuCount       = 2;

            // Storage. CreateNew makes + attaches a new VHDX (newVhdSizeBytes,
            // dynamicVhd); UseExisting attaches vhdPath; None leaves the VM
            // diskless. For CreateNew an empty vhdPath is auto-derived as
            // "<host default VHD dir>\<name>.vhdx".
            enum class Disk { None, CreateNew, UseExisting };
            Disk         diskMode       = Disk::CreateNew;
            std::wstring vhdPath;
            uint64_t     newVhdSizeBytes = 0;
            bool         dynamicVhd     = true;

            // Network: virtual-switch ElementName to connect a fresh NIC to.
            // Empty = create the VM with no network adapter.
            std::wstring switchName;

            // Install media: an ISO to mount in a DVD drive (and boot first).
            // Empty = no DVD drive / install media.
            std::wstring isoPath;

            // Where the VM's configuration + checkpoints + smart-paging file
            // live (Hyper-V Manager's "Store the virtual machine in a different
            // location"). Empty = the host default (DefaultExternalDataRoot).
            // When set, CreateVM writes ConfigurationDataRoot / SnapshotDataRoot
            // / SwapFileDataRoot on the VSSD before DefineSystem (verified: the
            // VM config lands under this path and the dir is auto-created).
            std::wstring vmStoragePath;
        };
        // Create a VM from `cfg`. Returns the new VM's GUID on success (so the
        // caller can open a tab for it), or an empty string if the core create
        // (DefineSystem + memory + processor) failed. Device steps (SCSI / disk
        // / NIC / DVD / boot order) are best-effort: a failure there is logged
        // but still yields a usable VM the user can finish in Settings. Runs on
        // the caller's apartment (uses m_scope, like the other Set*/Add*
        // methods) — call from the UI thread.
        std::wstring CreateVM(NewVmConfig const& cfg);

        // Host's default virtual-hard-disk directory
        // (Msvm_VirtualSystemManagementServiceSettingData.DefaultVirtualHardDiskPath),
        // e.g. "C:\\Users\\Public\\Documents\\Hyper-V\\Virtual Hard Disks". Used
        // by the wizard to pre-fill the new-VHD path. Empty on failure.
        std::wstring GetDefaultVhdDirectory() const;

        // Host's default virtual-machine config directory
        // (Msvm_VirtualSystemManagementServiceSettingData.DefaultExternalDataRoot),
        // e.g. "V:\\Hyper-V". Used by the wizard's "store the VM in a different
        // location" default. Empty on failure.
        std::wstring GetDefaultVmDirectory() const;

        // ---- VM edit operations (settings editor) --------------------------
        // All these go through Msvm_VirtualSystemManagementService::
        // ModifySystemSettings (for VSSD-level properties — name, notes) or
        // ModifyResourceSettings (for resource-level — memory, processor).
        // Each is synchronous; returns true if WMI accepted (ret 0 or 4096).
        // The next OnChanged tick reflects the change.
        //
        // State gating is the caller's responsibility — Hyper-V will reject
        // most resource-setting changes on a running VM. The settings dialog
        // surfaces this with per-field disabled state + hint text; these
        // VMManager methods just forward to WMI and report success/failure.

        bool RenameVM (std::wstring const& guid, std::wstring const& newName);
        bool SetNotes (std::wstring const& guid, std::wstring const& notes);

        // Memory: startup quantity + dynamic on/off + min/max bounds +
        // priority weight (0-10000). When dynamicEnabled is false, min/max
        // are ignored by Hyper-V (only startup applies). Units: MB for the
        // three size fields.
        struct MemoryConfig
        {
            uint64_t startupMb     = 0;
            bool     dynamicEnabled= false;
            uint64_t minMb         = 0;
            uint64_t maxMb         = 0;
            uint32_t targetBufferPct = 20;   // dynamic-memory buffer % (5..2000)
            uint32_t priority      = 5000;   // 0..10000, default mid
            // NUMA: max memory per virtual NUMA node, in MB (Hyper-V's
            // MaxMemoryBlocksPerNumaNode — a "block" is 1 MB, verified). Shown
            // on the processor NUMA subpage; defaults to the host node size.
            uint64_t maxMemoryPerNumaNodeMb = 0;
        };
        bool SetMemoryConfig(std::wstring const& guid, MemoryConfig const& cfg);

        // Processor: virtual CPU count + reservation/limit + weight. NOTE the
        // units: `reservation`/`limit` are RAW WMI values
        // (`Msvm_ProcessorSettingData.Reservation`/`Limit`), which are
        // **percent × 1000** — Hyper-V Manager's "100 %" limit is stored as
        // 100000. The settings dialog converts to/from 0..100 % at the UI edge;
        // these stay raw so SetProcessorConfig writes WMI directly. weight is
        // 0..10000 (default 100).
        struct ProcessorConfig
        {
            uint16_t count           = 1;
            uint64_t reservationPct  = 0;        // raw WMI (percent × 1000)
            uint64_t limitPct        = 100000;   // raw WMI (percent × 1000)
            uint32_t weight          = 100;
            // Compatibility: limit the processor features the VM may use, so it
            // can migrate to a host with a different CPU version
            // (Msvm_ProcessorSettingData.LimitProcessorFeatures).
            bool     limitProcessorFeatures = false;
            // NUMA topology caps (0 = host default). Max logical processors per
            // virtual NUMA node + max NUMA nodes per socket.
            uint64_t maxProcessorsPerNumaNode = 0;
            uint64_t maxNumaNodesPerSocket    = 0;
            // Hardware threads per core exposed to the guest (SMT). 0 = inherit
            // the host setting; always written (0 is a valid value, unlike the
            // caps above where 0 means "leave unset").
            uint64_t hwThreadsPerCore         = 0;
        };
        bool SetProcessorConfig(std::wstring const& guid, ProcessorConfig const& cfg);

        // Fresh per-VM reads of the full memory / processor config (incl. the
        // fields not carried in the poll cache — buffer, weight, reservation,
        // limit). The settings dialog calls these on open so it shows the VM's
        // REAL resource-control values, not hardcoded defaults.
        MemoryConfig    GetMemoryConfig(std::wstring const& guid) const;
        ProcessorConfig GetProcessorConfig(std::wstring const& guid) const;

        // ---- Firmware (Gen 2 only) ----------------------------------------
        // Toggle Secure Boot and pick the certificate-authority template the
        // firmware trusts. templateId is the braced GUID string Hyper-V
        // stores in Msvm_VirtualSystemSettingData.SecureBootTemplateId (see
        // the well-known IDs in VmSettingsDialog). Both properties go through
        // ModifySystemSettings on the realised VSSD — same path as RenameVM.
        //
        // VM must be Off: Secure Boot is firmware-level config and Hyper-V
        // rejects the modify on running/saved VMs. Gen 1 VMs have no Secure
        // Boot at all; the caller gates the UI to Gen 2 so this is never
        // invoked for them. Returns true if WMI accepted + the job completed.
        bool SetSecureBoot(std::wstring const& guid, bool enabled,
                           std::wstring const& templateId);

        // ---- Automatic start/stop actions ---------------------------------
        // What Hyper-V does to this VM when the host boots / shuts down.
        // startAction: 2=Nothing, 3=StartIfRunning, 4=Always.
        // stopAction:  2=TurnOff, 3=Save, 4=ShutDown.
        // startDelaySeconds: delay before the start action fires (staggers
        // boot); written as the CIM interval AutomaticStartupActionDelay.
        // Plain config prefs — changeable in any VM state.
        bool SetAutomaticActions(std::wstring const& guid,
                                 uint16_t startAction, uint16_t stopAction,
                                 uint32_t startDelaySeconds);

        // ---- Checkpoints --------------------------------------------------
        // Checkpoint type + file location on the realised VSSD, via
        // ModifySystemSettings. snapshotType: 2=Disabled, 3=Production (fall
        // back to standard), 4=ProductionOnly, 5=Standard. snapshotDataRoot is
        // where new checkpoint files (.avhdx/.vmrs) are written — Hyper-V only
        // allows changing it when the VM has NO existing checkpoints (the
        // caller gates the location field on that). Type is changeable in any
        // state. Pass the VM's current location unchanged to edit only the
        // type. Returns true if WMI accepted + the job completed.
        bool SetCheckpointConfig(std::wstring const& guid,
                                 uint16_t snapshotType,
                                 bool automaticCheckpoints,
                                 std::wstring const& snapshotDataRoot);

        // ---- Smart Paging file location -----------------------------------
        // Where Hyper-V writes the smart-paging file (transient dynamic-memory
        // overflow used at boot). SwapFileDataRoot on the realised VSSD via
        // ModifySystemSettings. Plain path config — changeable in any state.
        bool SetSmartPagingFileLocation(std::wstring const& guid,
                                        std::wstring const& path);

        // ---- Security (vTPM, state encryption) ----------------------------
        // Both modify Msvm_SecuritySettingData via Msvm_SecurityService::
        // ModifySecuritySettings. VM must be Off (Gen 2 only). Enabling vTPM
        // additionally provisions a local key protector first (generated from
        // the host's UntrustedGuardian in root\Microsoft\Windows\Hgs) if the
        // VM doesn't already have one. Returns true on success.
        bool SetVmTpm(std::wstring const& guid, bool enabled);
        bool SetVmStateEncryption(std::wstring const& guid, bool enabled);
        // Enable/disable VM shielding (Msvm_SecuritySettingData.ShieldingRequested).
        // Shielding is a COMPOSITE — enabling it also pins vTPM + state
        // encryption (setting ShieldingRequested alone is a silent no-op; the
        // gotcha-#27 family). Disabling clears only ShieldingRequested (leaves
        // encryption on, matching Set-VMSecurityPolicy). Off + Gen 2. Note: full
        // shielding needs HGS attestation — with a local key protector this is a
        // "soft" shield + may restrict host-console (VMConnect) access.
        bool SetVmShielded(std::wstring const& guid, bool enabled);

        // Live read of a VM's security settings (vTPM + state encryption),
        // straight from Msvm_SecuritySettingData — NOT the 1 s poll cache.
        // The settings dialog calls this on open so a just-saved change is
        // reflected immediately (the cache lags Save by up to a poll cycle).
        // nullopt members mean "no security setting" (Gen 1 / not reported).
        // const + uses m_scope, so call from the UI apartment.
        struct SecurityInfo
        {
            std::optional<bool> tpmEnabled;
            std::optional<bool> encryptState;
            std::optional<bool> shielded;     // ShieldingRequested
        };
        SecurityInfo GetVmSecurity(std::wstring const& guid) const;

        // ---- Boot order (Gen 2 firmware) ----------------------------------
        // One bootable device in the VM's firmware boot order. ref is the WMI
        // reference path of the backing Msvm_BootSourceSettingData (an element
        // of the VSSD's BootSourceOrder array) — opaque; only used to write
        // the reordered list back verbatim. description is the human-readable
        // BootSourceDescription ("Windows Boot Manager", "EFI Network", …).
        // What a Gen 2 boot entry actually boots — resolved by correlating the
        // Msvm_BootSourceSettingData to its backing device (the boot source's
        // InstanceID is the device RASD's InstanceID + "\\B"). Lets the UI show
        // friendly labels like Hyper-V Manager (instead of the raw, cryptic
        // "EFI SCSI Device") and lets CreateVM find the DVD by kind.
        enum class BootKind { Other, HardDrive, Dvd, Network, File };
        struct BootEntry
        {
            std::wstring ref;
            std::wstring description;   // friendly: "DVD Drive — install.iso"
            BootKind     kind = BootKind::Other;
        };
        // Read the current boot order for a Gen 2 VM, in order. Resolves each
        // BootSourceOrder reference to its Msvm_BootSourceSettingData to get
        // a display label. Returns empty for Gen 1 VMs (which use a different
        // uint16 BootOrder mechanism — not yet writable, see codegen TODO)
        // and for VMs with no boot sources. Lazy / on-demand (per-entry WMI
        // resolution), called by the settings dialog on open — NOT on the
        // 1 s poll. const + uses m_scope, so call from the UI apartment.
        std::vector<BootEntry> GetBootOrder(std::wstring const& guid) const;
        // Write a reordered boot order. orderedRefs MUST be the same ref
        // strings GetBootOrder returned (the BootSourceOrder element paths),
        // just permuted — Hyper-V matches by reference, and reconstructing
        // the strings risks a silent no-op. Sets BootSourceOrder on the
        // realised VSSD via ModifySystemSettings. VM should be Off (firmware
        // config). Returns true if WMI accepted + the job completed.
        //
        // NOTE: boot-source references regenerate after a reorder, so a ref
        // captured before one write is stale for a second. Within a single
        // dialog session (read on open, write once at Save) this is a non-
        // issue; re-read via GetBootOrder for any subsequent edit.
        bool SetBootOrder(std::wstring const& guid,
                          std::vector<std::wstring> const& orderedRefs);

        // ---- Gen 1 BIOS boot order (Msvm_VirtualSystemSettingData.BootOrder)
        // Gen 1 VMs use a uint16[] of device-type codes (0=Floppy, 1=CD/DVD,
        // 2=Hard Drive (IDE), 3=Legacy Network Adapter) instead of the Gen 2
        // BootSourceOrder ref list. One Gen1BootEntry per code, in order.
        struct Gen1BootEntry
        {
            uint16_t     code = 0;
            std::wstring description;
        };
        // Read the Gen 1 boot order, in order, mapping each code to a label.
        // Empty for Gen 2 (which uses GetBootOrder) or a VM with no codes.
        // Lazy / on-demand — call from the UI apartment like GetBootOrder.
        std::vector<Gen1BootEntry> GetBootOrderGen1(std::wstring const& guid) const;
        // Write a reordered Gen 1 boot order (the same codes GetBootOrderGen1
        // returned, permuted). Sets BootOrder on the realised VSSD via
        // ModifySystemSettings. VM MUST be Off — Hyper-V silently no-ops the
        // modify on a Saved/running VM (ret=4096, clean job, nothing applied;
        // verified against live WMI). Returns true if WMI accepted + completed.
        bool SetBootOrderGen1(std::wstring const& guid,
                              std::vector<uint16_t> const& orderedCodes);

        // ---- DVD drives (removable storage) -------------------------------
        // One virtual DVD drive on a VM. A drive always exists as a
        // Msvm_ResourceAllocationSettingData (ResourceType 16); the disc
        // (an ISO) is a separate Msvm_StorageAllocationSettingData "media"
        // object that exists ONLY while something is mounted. So:
        //   mediaRef/mediaPath empty  => drive is empty (no disc)
        //   mediaRef set              => disc mounted, mediaPath is the ISO
        struct DvdDrive
        {
            std::wstring driveRef;   // __PATH of the drive RASD (Parent for a mount)
            std::wstring mediaRef;   // __PATH of the media SASD; empty when no disc
            std::wstring mediaPath;  // mounted ISO path; empty when no disc
            std::wstring label;      // display label, e.g. "DVD Drive 1"
            std::wstring controller; // numbered label, e.g. "SCSI Controller 0"
            int          slot = 0;   // AddressOnParent on the controller
        };
        // Lazy/on-demand (per-VM WMI), called by the settings dialog on open.
        std::vector<DvdDrive> GetDvdDrives(std::wstring const& guid) const;

        // Mount / change / eject the disc in a DVD drive. The op is inferred
        // from the current vs. desired media:
        //   newIsoPath empty + drive has a disc -> EJECT  (RemoveResourceSettings)
        //   newIsoPath set   + drive empty      -> MOUNT  (AddResourceSettings,
        //                                                   clones the default media template)
        //   newIsoPath set   + drive has a disc -> CHANGE (ModifyResourceSettings)
        // driveRef / mediaRef come from a GetDvdDrives entry. DVD media hot-
        // swaps, so this works whether the VM is running or off. Returns true
        // if WMI accepted + the job completed.
        bool SetDvdMedia(std::wstring const& guid,
                         std::wstring const& driveRef,
                         std::wstring const& mediaRef,
                         std::wstring const& newIsoPath);

        // Add an empty DVD drive (no media). Picks the first SCSI controller
        // (Gen 2) or IDE controller (Gen 1) + next free slot, clones the
        // default "Synthetic DVD Drive" template (ResourceType 16), and
        // AddResourceSettings. A DVD drive is the same RASD shape as a disk
        // drive (gotcha #23's drive-add step) with a different ResourceSubType
        // and no second (media) layer. SCSI hot-add works on a running Gen 2
        // VM. Returns true if it sticks. controllerRef/slot pick the target
        // (see AttachVhd for the convention); empty/-1 auto-picks.
        bool AddDvdDrive(std::wstring const& guid,
                         std::wstring const& controllerRef = {},
                         int slot = -1);

        // Remove a DVD drive: eject its media first (RemoveResourceSettings on
        // the media SASD) if a disc is mounted, then remove the drive RASD —
        // both via the manual RemoveResourceSettings path (mirrors DetachVhd).
        // driveRef/mediaRef come from a GetDvdDrives entry (mediaRef empty when
        // the drive has no disc).
        bool RemoveDvdDrive(std::wstring const& guid,
                            std::wstring const& driveRef,
                            std::wstring const& mediaRef);

        // ---- Hard disks (VHD/VHDX attach/detach) --------------------------
        // One attached virtual hard disk. The Hyper-V model layers as
        // controller (SCSI/IDE RASD) → disk-drive RASD → VHD SASD; vhdRef is
        // the VHD `Msvm_StorageAllocationSettingData` and driveRef is its
        // parent disk-drive `Msvm_ResourceAllocationSettingData`. Both are
        // needed to detach (remove the VHD then the drive).
        struct HardDisk
        {
            std::wstring vhdRef;        // VHD SASD __PATH (empty for pass-through)
            std::wstring driveRef;      // disk-drive RASD __PATH (the detach target)
            std::wstring path;          // VHDX/VHD file, or the physical disk label
            std::wstring controller;    // display label, e.g. "SCSI Controller 0"
            int          slot = 0;      // AddressOnParent on the controller
            uint64_t     fileSizeBytes = 0;
            // Storage QoS — normalized 8 KB IOPS (IOPSReservation / IOPSLimit
            // on the VHD SASD). 0 = no reservation / no limit. Not applicable to
            // pass-through disks (no SASD), so left 0 + the dialog hides the row.
            uint64_t     iopsMin = 0;   // IOPSReservation (minimum)
            uint64_t     iopsMax = 0;   // IOPSLimit (maximum)
            // True for a physical (pass-through) disk: a single RT-17 RASD with
            // ResourceSubType "Physical Disk Drive" whose HostResource points at
            // a host Msvm_DiskDrive (no VHD SASD layer). vhdRef is empty; detach
            // removes driveRef directly.
            bool         isPassthrough = false;
        };
        // Lazy/on-demand (per-VM WMI), called by the settings dialog on open.
        // Surfaces BOTH virtual hard disks (VHD SASDs) and pass-through physical
        // disks (RT-17 "Physical Disk Drive" RASDs).
        std::vector<HardDisk> GetHardDisks(std::wstring const& guid) const;

        // ---- Physical (pass-through) disks --------------------------------
        // An OFFLINE host physical disk available for pass-through. Hyper-V
        // exposes these as Msvm_DiskDrive instances with a DriveNumber set (an
        // online disk has none); devicePath is the instance __PATH that goes
        // verbatim into the pass-through drive's HostResource.
        struct PhysicalDisk
        {
            std::wstring devicePath;    // Msvm_DiskDrive __PATH (-> HostResource)
            std::wstring label;         // ElementName, e.g. "Disk 4 2.00 GB Bus 0 Lun 3 Target 0"
            uint32_t     driveNumber = 0;
            uint64_t     sizeBytes = 0; // MaxMediaSize
        };
        // Enumerate offline host disks available for pass-through. Lazy/on-demand
        // (called by the settings dialog's "Attach physical disk" flyout).
        std::vector<PhysicalDisk> GetAvailablePhysicalDisks() const;

        // Attach an offline host physical disk as pass-through. SINGLE-layer
        // (unlike AttachVhd's two layers): clone the default "Physical Disk
        // Drive" RT-17 template, set Parent=controller + AddressOnParent=slot +
        // HostResource=[devicePath] (the host Msvm_DiskDrive __PATH), and one
        // AddResourceSettings. No VHD StorageAllocationSettingData. Detach reuses
        // DetachVhd with an empty vhdRef (it removes only the drive RASD).
        // VERIFIED reversibly (cmdlet round-trip on an offline throwaway disk):
        // ResourceSubType "Microsoft:Hyper-V:Physical Disk Drive", HostResource =
        // the Msvm_DiskDrive path, SASD count unchanged. Gated to VM Off (the
        // verified state; the disk must be offline on the host anyway).
        // controllerRef/slot pick the target; empty/-1 auto-picks (SCSI first).
        bool AttachPhysicalDisk(std::wstring const& guid,
                                std::wstring const& devicePath,
                                std::wstring const& controllerRef = {},
                                int slot = -1);

        // Set storage QoS (min/max IOPS) on an attached VHD. Writes
        // IOPSReservation (min) + IOPSLimit (max) on the VHD
        // Msvm_StorageAllocationSettingData via ModifyResourceSettings — the
        // same shape as the SetDvdMedia "change" path. Values are normalized
        // 8 KB IOPS, used verbatim (UI value == raw WMI value — verified);
        // 0 = no reservation / no limit. Hot-settable (works on a running VM,
        // verified), so the caller imposes no state gate. vhdRef is the SASD
        // __PATH from a GetHardDisks entry. Returns true if it sticks.
        bool SetDiskQos(std::wstring const& guid,
                        std::wstring const& vhdRef,
                        uint64_t minIops, uint64_t maxIops);

        // ---- COM (serial) ports -------------------------------------------
        // A virtual COM port (Msvm_SerialPortSettingData, ResourceType 21).
        // Every VM has exactly two (COM 1 / COM 2) by default; they can't be
        // added/removed. `path` is the host named pipe it's connected to
        // (Connection[0], e.g. "\\.\pipe\name"); empty = disconnected.
        struct SerialPort
        {
            std::wstring ref;    // Msvm_SerialPortSettingData __PATH (modify target)
            std::wstring name;   // ElementName, "COM 1" / "COM 2"
            std::wstring path;   // connected named pipe; empty = disconnected
        };
        // Lazy/on-demand (per-VM WMI), called by the settings dialog on open.
        // Sorted by name (COM 1 before COM 2).
        std::vector<SerialPort> GetSerialPorts(std::wstring const& guid) const;

        // Connect a COM port to a host named pipe, or disconnect it. Writes
        // Connection (a string array) on the Msvm_SerialPortSettingData via
        // ModifyResourceSettings — the same shape as SetDiskQos. pipePath empty
        // => Connection=[] (disconnect); else Connection=[pipePath]. VERIFIED
        // reversibly + hot-settable on a running VM, so no state gate. portRef
        // is from a GetSerialPorts entry. Returns true if it sticks.
        bool SetSerialPortConnection(std::wstring const& guid,
                                     std::wstring const& portRef,
                                     std::wstring const& pipePath);

        // ---- Storage controllers (settings editor destination picker) -----
        // One SCSI/IDE controller a new disk/DVD drive can be placed on. SCSI
        // controllers carry no intrinsic index (Hyper-V numbers them purely by
        // enumeration order — verified: a 2nd SCSI controller has an empty
        // Address, identical ElementName); IDE controllers carry Address 0/1.
        // So `number` is the enumeration index within the controller's kind.
        // usedSlots are the AddressOnParent values already occupied by child
        // drives (disk + DVD share the slot space). SCSI max 64 slots, IDE 2.
        struct StorageController
        {
            std::wstring ref;       // controller RASD __PATH
            std::wstring label;     // "SCSI Controller 0", "IDE Controller 0"
            bool         isScsi = true;
            int          number = 0;
            int          maxSlots = 64;
            std::vector<int> usedSlots;
        };
        // IDE controllers first (by Address), then SCSI (by InstanceID, stable).
        std::vector<StorageController> GetStorageControllers(std::wstring const& guid) const;

        // Add a synthetic SCSI controller (clones the default "Synthetic SCSI
        // Controller" template + AddResourceSettings). Hyper-V REJECTS this on
        // a running VM ("cannot add device 'Synthetic SCSI Controller' while the
        // virtual machine is running" — verified), so the caller gates it to
        // Off. Up to 4 SCSI controllers per VM (Hyper-V limit). Returns true if
        // it sticks. Applied immediately (not Save-batched) so the new
        // controller appears in the picker for subsequent attach/create.
        bool AddScsiController(std::wstring const& guid);

        // Remove a SCSI controller (RemoveResourceSettings on the controller
        // RASD). The caller only removes EMPTY controllers — a controller with
        // child drives is left disabled in the UI (detach its devices first).
        // Off only (symmetric with AddScsiController). Returns true if it
        // sticks. Applied immediately, like the add.
        bool RemoveScsiController(std::wstring const& guid,
                                  std::wstring const& controllerRef);

        // Attach an EXISTING VHD/VHDX to the VM. Picks the first SCSI
        // controller (Gen 2) or IDE controller (Gen 1), the next free slot,
        // and does two AddResourceSettings: a disk-drive (cloned from the
        // default "Synthetic Disk Drive" template, Parent=controller +
        // AddressOnParent=slot), then the VHD (cloned from the default
        // "Virtual Hard Disk" template, Parent=the new drive +
        // HostResource=path). Returns true if both stick. (Creating a NEW VHD
        // via Msvm_ImageManagementService is a separate, deferred flow.)
        // controllerRef/slot pick the target controller + AddressOnParent;
        // empty/-1 auto-picks (SCSI preferred, lowest free slot) as before.
        bool AttachVhd(std::wstring const& guid, std::wstring const& vhdxPath,
                       std::wstring const& controllerRef = {},
                       int slot = -1);

        // Create a NEW VHDX at `path` (sizeBytes max internal size; dynamic =
        // grow-on-write vs. fixed = pre-allocated) via
        // Msvm_ImageManagementService::CreateVirtualHardDisk, then attach it
        // (AttachVhd). The SettingData is a spawned Msvm_VirtualHardDiskSettingData
        // (Type 3=dynamic / 2=fixed, Format 3=VHDX). Returns true only if both
        // the create and the attach succeed. Note: a FIXED disk's creation job
        // can take a while (it zeroes the full size); dynamic is fast.
        bool CreateAndAttachVhd(std::wstring const& guid,
                                std::wstring const& path,
                                uint64_t sizeBytes,
                                bool dynamic,
                                std::wstring const& controllerRef = {},
                                int slot = -1);

        // Detach a hard disk: remove the VHD SASD, then its disk-drive RASD
        // (both via the manual RemoveResourceSettings path). The VHDX file on
        // disk is NOT deleted. vhdRef/driveRef come from a GetHardDisks entry.
        bool DetachVhd(std::wstring const& guid,
                       std::wstring const& vhdRef,
                       std::wstring const& driveRef);

        // ---- Switch enumeration (settings editor dropdowns) ---------------
        // Returns the ElementName of every Msvm_VirtualEthernetSwitch on the
        // host. Used by the settings dialog's NIC switch ComboBox.
        std::vector<std::wstring> GetVirtualSwitches() const;

        // ---- Network adapter edits ----------------------------------------
        // Change the virtual switch that the NIC identified by nicGuid (the
        // NetworkAdapter::nicGuid field) is connected to. Pass an empty
        // switchName to disconnect — Hyper-V Manager's "Not connected"
        // affordance maps to HostResource=[] + EnabledState=3 (Disabled) on
        // the underlying Msvm_EthernetPortAllocationSettingData row; this is
        // the same wire shape PowerShell's Disconnect-VMNetworkAdapter
        // emits. Live: works whether the VM is running or off.
        //
        // Returns true if WMI accepted + the async job completed cleanly.
        // Does not add or remove NICs — those use AddResourceSettings /
        // RemoveResourceSettings (not wired yet; pending codegen fix for
        // ReferenceArray in-params).
        bool SetNetworkAdapterSwitch(std::wstring const& vmGuid,
                                     std::wstring const& nicGuid,
                                     std::wstring const& switchName);

        // Set a NIC's MAC address mode. dynamic=true → Hyper-V assigns from
        // the host pool (writes Address="" + StaticMacAddress=false; the EMPTY
        // address is what marks it dynamic — writing all-zeros, as the
        // Set-VMNetworkAdapter -DynamicMacAddress cmdlet does, is rejected as
        // "invalid MAC"). dynamic=false → static override (writes the given
        // 12-hex Address + StaticMacAddress=true). staticMac is ignored when
        // dynamic. Modifies Msvm_SyntheticEthernetPortSettingData via
        // ModifyResourceSettings. Returns true if WMI accepted + job completed.
        bool SetNetworkAdapterMac(std::wstring const& vmGuid,
                                  std::wstring const& nicGuid,
                                  bool dynamic,
                                  std::wstring const& staticMac);

        // "Protected network" — Msvm_SyntheticEthernetPortSettingData.
        // ClusterMonitored (a bool on the synthetic port itself, NOT a feature
        // setting). ModifyResourceSettings like SetNetworkAdapterMac. Live.
        bool SetNetworkAdapterProtectedNetwork(std::wstring const& vmGuid,
                                               std::wstring const& nicGuid,
                                               bool clusterMonitored);

        // Add a synthetic network adapter to the VM. This is two layered
        // AddResourceSettings (mirrors what Add-VMNetworkAdapter does on the
        // wire — a raw add of just the port leaves no connection object):
        //   1. clone the default "Synthetic Ethernet Port" template, set
        //      ElementName + a fresh VirtualSystemIdentifiers GUID (Hyper-V
        //      assigns the MAC from the pool), AddResourceSettings → the NIC.
        //   2. re-query the new port (by the VSI GUID we set — its
        //      ResultingResourceSettings comes back as embedded XML, gotcha
        //      #9), clone the default "Ethernet Connection" template, set
        //      Parent=new port + HostResource/EnabledState for the requested
        //      switch (empty switchName → disconnected: HostResource=[] +
        //      EnabledState=3), AddResourceSettings → the connection.
        // Gated to VM Off by the dialog (hardware add). Returns true if both
        // adds stick.
        bool AddNetworkAdapter(std::wstring const& vmGuid,
                               std::wstring const& switchName);

        // Remove the synthetic NIC identified by nicGuid: RemoveResourceSettings
        // on its Ethernet Connection allocation (child) first, then on the
        // synthetic port (parent) — both via the manual SpawnMethodIn /
        // SetReferenceArray path (the generated Remove wrapper is a stub).
        // Removing the allocation first is unconditionally safe regardless of
        // whether Hyper-V would cascade. Gated to VM Off by the dialog.
        bool RemoveNetworkAdapter(std::wstring const& vmGuid,
                                  std::wstring const& nicGuid);

        // Set (or clear) a NIC's access VLAN. vlanId in 1..4094 puts the NIC
        // in access mode on that VLAN; vlanId == 0 untags it (removes the VLAN
        // feature setting). The VLAN is a Msvm_EthernetSwitchPortVlanSettingData
        // feature layered on the NIC's Ethernet Connection allocation:
        //   set   → AddFeatureSettings (no existing) / ModifyFeatureSettings
        //           (existing) with OperationMode=1 (Access) + AccessVlanId.
        //   untag → RemoveFeatureSettings on the existing VLAN setting.
        // Live: works whether the VM is running or off.
        bool SetNetworkAdapterVlan(std::wstring const& vmGuid,
                                   std::wstring const& nicGuid,
                                   uint16_t vlanId);

        // Full VLAN config (Msvm_EthernetSwitchPortVlanSettingData). mode ==
        // OperationMode: 0 = untagged (remove the setting), 1 = Access,
        // 2 = Trunk, 3 = Private. Hyper-V Manager's GUI does Access only; trunk/
        // private are PowerShell-only there. Trunk verified live; Private coded
        // from the documented shape but UNTESTED (needs an external switch — the
        // internal/Default switch rejects private VLANs with 0x80070057).
        struct VlanConfig
        {
            uint16_t mode = 0;                       // OperationMode
            uint16_t accessVlanId = 0;               // mode 1
            uint16_t nativeVlanId = 0;               // mode 2
            std::vector<uint16_t> trunkVlanList;     // mode 2: TrunkVlanIdArray
            uint16_t primaryVlanId = 0;              // mode 3
            uint16_t secondaryVlanId = 0;            // mode 3
            uint8_t  pvlanMode = 0;                  // mode 3: 1=Isolated 2=Community 3=Promiscuous
            std::vector<uint16_t> secondaryVlanList; // mode 3: SecondaryVlanIdArray (promiscuous)
        };
        bool SetNetworkAdapterVlan(std::wstring const& vmGuid,
                                   std::wstring const& nicGuid,
                                   VlanConfig const& cfg);

        // Advanced NIC features (the "Advanced Features" tab in Hyper-V
        // Manager). All live on a single Msvm_EthernetSwitchPortSecuritySettingData
        // feature setting layered on the port allocation — same Add/Modify
        // FeatureSettings mechanism as the access VLAN (gotcha #28).
        struct NicAdvancedFeatures
        {
            bool    macSpoofing   = false;  // AllowMacSpoofing
            bool    dhcpGuard     = false;  // EnableDhcpGuard
            bool    routerGuard   = false;  // EnableRouterGuard
            bool    nicTeaming    = false;  // AllowTeaming
            uint8_t portMirroring = 0;      // MonitorMode: 0=None, 1=Destination, 2=Source
            bool    ieeePriorityTag = false; // AllowIeeePriorityTag (802.1p)
        };
        // Apply the full advanced-feature set in one shot. Modifies the NIC's
        // existing security setting, or clones the default template + adds one
        // if absent (a fresh NIC has none). Live: works running or off.
        bool SetNetworkAdapterAdvanced(std::wstring const& vmGuid,
                                       std::wstring const& nicGuid,
                                       NicAdvancedFeatures const& features);

        // Set per-NIC bandwidth limits (Msvm_EthernetSwitchPortBandwidthSettingData,
        // a feature setting on the port allocation). maxBps = Limit (maximum),
        // minBps = Reservation (minimum, absolute); both bits/sec, 0 = none.
        // Modify-or-add like SetNetworkAdapterAdvanced. NOTE: an absolute
        // minimum is only honored when the virtual switch is in absolute
        // minimum-bandwidth mode; a weight-mode switch may reject it (surfaced
        // as a failed modify). Live: works running or off.
        bool SetNetworkAdapterBandwidth(std::wstring const& vmGuid,
                                        std::wstring const& nicGuid,
                                        uint64_t maxBps,
                                        uint64_t minBps);

        // "Device naming" — Msvm_SyntheticEthernetPortSettingData.
        // DeviceNamingEnabled (a bool on the synthetic port, like
        // ClusterMonitored). ModifyResourceSettings. Live.
        bool SetNetworkAdapterDeviceNaming(std::wstring const& vmGuid,
                                           std::wstring const& nicGuid,
                                           bool enabled);

        // Hardware acceleration — VMQ / SR-IOV / IPsec task offloading. All on
        // ONE Msvm_EthernetSwitchPortOffloadSettingData feature setting on the
        // connection (which ALWAYS exists by default → usually a Modify). VMQ /
        // SR-IOV are 0/100 weights; IPsec is a max-SA count (0 = off). Live.
        struct NicOffloadFeatures
        {
            bool     vmq          = false;   // VMQOffloadWeight (0/100)
            bool     sriov        = false;   // IOVOffloadWeight (0/100)
            bool     ipsecOffload = false;   // IPSecOffloadLimit > 0
            uint32_t ipsecOffloadMaxSA = 512;
        };
        bool SetNetworkAdapterOffload(std::wstring const& vmGuid,
                                      std::wstring const& nicGuid,
                                      NicOffloadFeatures const& features);

        // ---- Integration services ----------------------------------------
        // Per-VM list of guest integration components (Heartbeat, KVP, OS
        // shutdown, Time sync, Backup VSS, Guest services). Each has an
        // ElementName (user-visible label) and an Enabled flag.
        struct IntegrationService
        {
            std::wstring className;     // e.g. "Msvm_ShutdownComponentSettingData"
            std::wstring displayName;   // e.g. "Operating system shutdown"
            bool         enabled = false;
        };
        std::vector<IntegrationService> GetIntegrationServices(std::wstring const& guid) const;
        bool SetIntegrationServiceEnabled(std::wstring const& guid,
                                          std::wstring const& className,
                                          bool enabled);

        // Wake the poll thread AND block (briefly) until at least one full
        // refresh cycle has run. Called by the settings dialog after a
        // batch of Set* edits so the in-memory snapshot reflects the
        // changes before the dialog closes. Without this, reopening the
        // dialog immediately would show stale values. timeoutMs is a
        // safety cap; the typical wait is one poll cycle.
        void KickPollAndWait(int timeoutMs = 2000);

    private:
        VMManager();
        ~VMManager();
        VMManager(VMManager const&) = delete;
        VMManager& operator=(VMManager const&) = delete;

        // Open the WMI scope + snapshot service + subscriptions. Returns true on
        // success; on failure sets m_connectStatus/m_connectDetail and tears any
        // partial state back down. Shared by the ctor and RetryConnect; never
        // throws (catches WmiException internally).
        bool TryConnect();

        // Ensure the VM has a valid key protector before a security-setting
        // modify (vTPM / state encryption). Provisions a local KP from the
        // host's UntrustedGuardian (lazily opening m_hgsScope) when none
        // exists; re-fetches `ssd` afterward (the ref regenerates). Returns
        // false on failure. svc is the Msvm_SecurityService instance.
        bool EnsureKeyProtector(hyprv::wmi::WmiObject& svc,
                                hyprv::wmi::WmiObject& ssd,
                                std::wstring const& guid);

        // Refresh every VM via a GetSummaryInformation call on the provided
        // service. The scope+service must belong to the calling thread's
        // apartment — cross-apartment proxy use returns RPC_E_WRONG_THREAD.
        // perfScope is an optional root\cimv2 scope for reading the Hyper-V
        // dynamic-memory balancer perf counters (Memory Demand source).
        // Pass nullptr to skip — assigned values still work, demand collapses
        // to assigned, and Memory Status reads "OK".
        void UpdateAll(hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService& vsms,
                       hyprv::wmi::WmiScope* perfScope);

        // Background polling loop — wakes every kPollIntervalMs *or* when a
        // subscription nudges m_kickPoll, then calls UpdateAll + NotifyChanged.
        void PollLoop();

        void NotifyChanged();
        // Fire m_onError (thread-safe snapshot under m_lock, like NotifyChanged).
        void NotifyError(std::wstring const& vmName, std::wstring const& message);
        // Spawn a detached thread that builds its own WMI scope (the UI scope is
        // STA-bound — gotcha #8), waits for the state-change job at `jobPath`,
        // and fires NotifyError if it ends in failure. Clears the optimistic
        // pending-blink for `guid` when the job finishes. vmName for the message.
        void WatchStateChangeJob(std::wstring jobPath, std::wstring guid,
                                 std::wstring vmName);
        // Generic async-job watcher for non-state-change operations (snapshots,
        // delete VM). Detached thread + own MTA scope (gotcha #8); on failure
        // fires NotifyError with the job's ErrorDescription, or a fallback built
        // from `actionLabel` (a short verb phrase, e.g. "create the checkpoint").
        // Also manages the optimistic status-dot blink for `guid` (held for the
        // minimum visible duration, then cleared when the job ends) — the VM's
        // own state doesn't move for a snapshot, so the poll-based clear never
        // fires and the watcher is the authoritative blink-stop. Pass an empty
        // guid to skip blink handling.
        // deleteFilesOnSuccess: file paths to delete once the job completes
        // cleanly (used by DestroyVM's "also delete VHDs" — the VM holds the
        // files open until it's gone, so deletion must wait for the job).
        void WatchJob(std::wstring jobPath, std::wstring guid,
                      std::wstring vmName, std::wstring actionLabel,
                      std::vector<std::wstring> deleteFilesOnSuccess = {});
        // Detached MTA worker (own scope — gotcha #8): SAVE the VM (preserving
        // its state), apply the snapshot, then START it back. Used when
        // ApplySnapshot is invoked on a Running/Paused VM (Hyper-V rejects a
        // running apply with 32775). A failed apply leaves the VM Saved, so the
        // resume restores the original state. Surfaces failures via NotifyError
        // and clears the progress blink at the end.
        void SaveThenApplySnapshot(std::wstring vmGuid, std::wstring snapshotPath,
                                   std::wstring vmName);
        // VM display name from the cache (falls back to the GUID) for dialogs.
        std::wstring VmDisplayName(std::wstring const& vmGuid) const;
        // Drop the optimistic pending-blink for `guid` (clears m_pendingStateChange
        // + the live cache flag) and fire a render. Called when a state-change
        // job completes/fails so the dot stops blinking even if the VM never
        // left its original state (e.g. a start that failed).
        void ClearPendingStateChange(std::wstring const& guid);
        // Start the optimistic pending-blink for `guid` NOW: record the current
        // state + timestamp, flip the live cache flag, fire a render. Shared by
        // every user-initiated power action (RequestStateChange AND ShutdownVM,
        // which uses a different WMI path). Safe on the UI thread. An optional
        // `label` (e.g. "Taking snapshot…") drives the in-panel progress row and
        // is routed into statusText until Hyper-V's native job text takes over.
        void MarkPendingStateChange(std::wstring const& guid,
                                    std::wstring const& label = {});

        // Best-effort boot-order promotion used by CreateVM: move the DVD/CD
        // entry to the front so a new VM with an install ISO boots the
        // installer. Handles both the Gen 2 BootSourceOrder ref list and the
        // Gen 1 uint16 BootOrder codes. Swallows failures (logs only).
        void PromoteDvdToBootFront(std::wstring const& guid, int generation);

        // Wake the poll thread now instead of waiting up to kPollIntervalMs
        // for its next tick. Used by the WMI subscription kick lambda and
        // by user-edit methods (RenameVM, SetMemoryConfig, etc.) on the
        // success path. Fire-and-forget — see public KickPollAndWait for
        // the variant that blocks until the cache refresh completes.
        void KickPoll();

        static constexpr int kPollIntervalMs = 1000;
        // Minimum visible duration of the optimistic pending-blink, so even very
        // fast state changes (turn off / quick start) pulse noticeably instead
        // of flickering once. Honored by both the poll clear and the watcher.
        static constexpr std::chrono::milliseconds kMinPendingBlink{ 1200 };

        // The main-thread scope. Built in the ctor on the UI thread (a STA)
        // and used for everything that fires from that apartment —
        // subscriptions plus the snapshot-service method invocations
        // triggered by user clicks.
        std::unique_ptr<hyprv::wmi::WmiScope>        m_scope;
        // Lazily-opened scope to root\Microsoft\Windows\Hgs — only needed when
        // generating a local key protector for a fresh vTPM enable. Built on
        // the UI STA (first use is from the settings-dialog Save, which runs
        // its WMI batch there). Null until first use.
        std::unique_ptr<hyprv::wmi::WmiScope>        m_hgsScope;
        std::unique_ptr<hyprv::wmi::WmiSubscription> m_createSub;
        std::unique_ptr<hyprv::wmi::WmiSubscription> m_deleteSub;
        std::unique_ptr<hyprv::wmi::WmiSubscription> m_modifySub;
        // Snapshot service is invoked from the UI thread by the flyout's
        // take/apply/delete buttons, so it lives on m_scope.
        hyprv::wmi::hyperv::Msvm_VirtualSystemSnapshotService m_snapSvc;

        // Initial-connection status. UI-thread only (written in TryConnect from the
        // ctor / RetryConnect, read by the welcome page) — the poll thread never
        // touches it, so no lock. Ok unless opening root\virtualization\v2 failed.
        ConnectStatus m_connectStatus = ConnectStatus::Ok;
        std::wstring  m_connectDetail;   // raw WMI error text (Other case + logs)

        mutable std::mutex             m_lock;
        std::vector<VirtualMachine>    m_vms;
        // Multicast subscriber lists (one entry per open window). Guarded by
        // m_lock; snapshotted under the lock before firing (NotifyChanged/Error)
        // so a callback can't deadlock by re-entering the manager.
        std::vector<std::pair<SubToken, OnChangedFn>> m_onChangedSubs;
        std::vector<std::pair<SubToken, ErrorFn>>     m_onErrorSubs;   // guarded by m_lock
        SubToken                                      m_nextSubToken = 1;
        // Optimistic pending-blink tracking (guarded by m_lock). guid -> the
        // state at request time + when it was requested. A VM stays "pending"
        // (dot blinks) until its state moves off `oldState`, the job completes,
        // or the entry ages out (safety net).
        struct PendingInfo
        {
            VmState                              oldState = VmState::Unknown;
            std::chrono::steady_clock::time_point requestedAt;
            // Optional human progress label for a labeled async op (snapshot
            // take/apply/delete). Empty for plain power actions (blink only).
            // Re-stamped onto VirtualMachine::pendingJobLabel each poll, and
            // routed into statusText (when native status is empty) so the home
            // list / rail / flyout state line all show "Taking snapshot…".
            std::wstring                          label;
        };
        std::unordered_map<std::wstring, PendingInfo> m_pendingStateChange;

        // Poll thread shutdown + wakeup. m_stop+cv lets the dtor request
        // exit without waiting up to a full kPollIntervalMs; m_kickPoll lets
        // subscription callbacks (on WMI sink threads) request an immediate
        // refresh without doing WMI work themselves.
        std::atomic<bool>              m_stop{ false };
        std::atomic<bool>              m_kickPoll{ false };
        std::mutex                     m_stopLock;
        std::condition_variable        m_stopCv;
        std::thread                    m_pollThread;
        // Incremented at the end of every poll iteration. HasFirstSnapshot()
        // uses it; KickPollAndWait uses the request handshake below instead.
        std::atomic<uint64_t>          m_pollGen{ 0 };
        // Request/serviced handshake for KickPollAndWait. KickPollAndWait
        // increments m_pollReq; each poll cycle snapshots m_pollReq at its
        // START and publishes that value to m_pollServiced when it finishes.
        // Waiting until m_pollServiced >= our request guarantees a cycle that
        // BEGAN AFTER the edit completed — so a cycle already in-flight when
        // the edit kicked (carrying pre-edit WMI data) can't satisfy the wait.
        // Fixes "reopen the settings dialog and the change isn't reflected
        // until you close + reopen a second time."
        std::atomic<uint64_t>          m_pollReq{ 0 };
        std::atomic<uint64_t>          m_pollServiced{ 0 };
    };
}
