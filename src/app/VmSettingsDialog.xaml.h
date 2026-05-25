#pragma once

#include "VmSettingsDialog.g.h"

#include "settings/Settings.h"
#include "vm/VMManager.h"

#include <string>
#include <map>
#include <set>

namespace winrt::hyprv_app::implementation
{
    // Modal "Edit hardware..." dialog. Lifted from `Hyper-V Manager`'s
    // Settings dialog: name / memory / processor / notes for v1. Future
    // sections (integration services, NICs, disks) drop into the same
    // scrolling form in VmSettingsDialog.xaml.
    //
    // Lifecycle:
    //   1. Construct via winrt::make<VmSettingsDialog>()
    //   2. SetVm(guid) — captures the VM GUID + populates inputs from
    //      VMManager::GetByGuid(guid). Applies per-field IsEnabled gating
    //      from the VM's current state.
    //   3. dlg.XamlRoot(parentXamlRoot); co_await dlg.ShowAsync();
    //   4. On primary-button click, OnPrimaryButtonClick snapshot-and-diffs
    //      the inputs against the captured originals and calls the matching
    //      VMManager edit methods only for fields that actually changed.
    struct VmSettingsDialog : VmSettingsDialogT<VmSettingsDialog>
    {
        VmSettingsDialog();

        // Bind to a specific VM. Reads the current settings via
        // VMManager::GetByGuid and applies them to the form inputs.
        // Should be called before ShowAsync.
        void SetVm(std::wstring const& vmGuid);

        // True after a Primary Save that changed the effective RDP options for
        // this VM. The opener reconnects the VM's live session so the change
        // applies without a manual close + reopen.
        bool RdpOptionsChanged() const { return m_rdpOptionsChanged; }

        // XAML event handlers — public so the generated .g.cpp can bind them.
        void OnMemDynamicToggled(Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::RoutedEventArgs const&);
        // Recompute the processor "Percent of total system resources" read-outs
        // when vCPU count / reserve / limit change.
        void OnProcessorResourceChanged(Windows::Foundation::IInspectable const&,
                                        Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
        // Update the live memory-weight value readout next to the slider.
        void OnMemWeightChanged(Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        // "Use hardware topology" — fill the NUMA fields with the host's
        // physical topology (applied on Save).
        void OnUseHardwareTopology(Windows::Foundation::IInspectable const&,
                                   Microsoft::UI::Xaml::RoutedEventArgs const&);
        // Live update of the RDP section's controls IsEnabled based on
        // the "Use app defaults" toggle. ON = controls disabled (using
        // defaults). OFF = controls live (user supplies per-VM values).
        void OnRdpUseDefaultsToggled(Windows::Foundation::IInspectable const&,
                                     Microsoft::UI::Xaml::RoutedEventArgs const&);
        // The template combo is only meaningful while Secure Boot is on —
        // this enables/disables it to match (on top of the VM-Off + Gen 2
        // state gating applied in ApplyStateGating).
        void OnSecureBootToggled(Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::RoutedEventArgs const&);
        // Shielding implies vTPM + state encryption — turn both on when shielding
        // is enabled and lock encryption on (mandatory while shielded).
        void OnShieldingToggled(Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::RoutedEventArgs const&);
        // Move the selected boot entry up/down in bootOrderList. Pure
        // in-list reorder; the new order is read back + diffed at Save.
        void OnBootMoveUp(Windows::Foundation::IInspectable const&,
                          Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnBootMoveDown(Windows::Foundation::IInspectable const&,
                            Microsoft::UI::Xaml::RoutedEventArgs const&);
        // "Attach VHD..." — file-picks a VHD/VHDX and queues it for attach at
        // Save (appends to m_pendingAttachPaths + a row in storageAttachHost).
        void OnStorageAttachVhd(Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::RoutedEventArgs const&);
        // "Create new VHD..." — reads the size/dynamic inputs, save-picks a
        // path, and queues a create+attach (m_pendingCreates + a card).
        void OnStorageCreateVhd(Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::RoutedEventArgs const&);
        // "Add adapter..." (in the flyout) — reads the switch ComboBox and
        // queues a new NIC for add at Save (m_pendingNics + a card).
        void OnNetworkAddAdapter(Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::RoutedEventArgs const&);
        // "Add DVD drive" — queues an empty DVD drive for add at Save
        // (m_pendingDvdAdds + a card in storageDvdAddHost).
        void OnStorageAddDvdDrive(Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::RoutedEventArgs const&);
        // A placement flyout's "Controller" combo selection changed —
        // repopulate its sibling "Location" (slot) combo (found via the
        // controller combo's Tag) with that controller's free slots. Shared by
        // the attach / create / add-DVD flyouts.
        void OnPlacementControllerChanged(Windows::Foundation::IInspectable const&,
                                          Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        // Per-flyout Opening handlers: repopulate that flyout's controller +
        // slot combos from current state (free slots exclude VM-used + queued).
        void OnAttachFlyoutOpening(Windows::Foundation::IInspectable const&,
                                   Windows::Foundation::IInspectable const&);
        void OnCreateFlyoutOpening(Windows::Foundation::IInspectable const&,
                                   Windows::Foundation::IInspectable const&);
        void OnDvdFlyoutOpening(Windows::Foundation::IInspectable const&,
                                Windows::Foundation::IInspectable const&);
        // "Attach physical disk" flyout: Opening repopulates the offline-disk
        // picker + placement combos; the Attach button queues a pass-through
        // attach (m_pendingPhysAttaches + a card in storageAttachHost).
        void OnPhysFlyoutOpening(Windows::Foundation::IInspectable const&,
                                 Windows::Foundation::IInspectable const&);
        void OnStorageAttachPhysical(Windows::Foundation::IInspectable const&,
                                     Microsoft::UI::Xaml::RoutedEventArgs const&);
        // "Add SCSI controller" — applies IMMEDIATELY (not Save-batched, so the
        // new controller is selectable for subsequent attach/create), then
        // rebuilds the controllers + device cards. Gated to Off (Hyper-V rejects
        // a running add) and capped at 4.
        winrt::fire_and_forget OnStorageAddScsiController(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        // "Browse..." for the checkpoint file location — folder picker.
        void OnCheckpointBrowse(Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::RoutedEventArgs const&);
        // "Browse..." for the smart-paging file location — folder picker.
        void OnSmartPagingBrowse(Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::RoutedEventArgs const&);
        // Left-nav selection change. Both navHardware and navManagement
        // route here; we deselect the other to keep selection mutually
        // exclusive across the two lists, then show the matching section
        // on the right.
        void OnNavSelectionChanged(Windows::Foundation::IInspectable const& sender,
                                   Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

    private:
        // ContentDialog primary-button handler. Runs as a coroutine so the
        // dialog can show a "Saving..." spinner while the synchronous
        // VMManager work runs and so the post-save cache wait happens
        // off the UI thread (spinner keeps animating). The deferral
        // returned by args.GetDeferral() keeps the dialog open until
        // we Complete() it.
        winrt::fire_and_forget OnPrimaryButtonClick(
            Microsoft::UI::Xaml::Controls::ContentDialog const&,
            Microsoft::UI::Xaml::Controls::ContentDialogButtonClickEventArgs const&);

        // Populate inputs from current VM state. Called by SetVm.
        void LoadFromVm(hyprv::app::vm::VirtualMachine const& vm);

        // Update IsEnabled on each input based on m_vmState; show/hide
        // section-level hint TextBlocks when any input in a section is
        // disabled.
        void ApplyStateGating();

        // Refresh the processor "Percent of total system resources" read-outs
        // from the current reserve/limit/count + host logical-processor count.
        void UpdateProcessorDerived();

        // Surface an error in the top-of-dialog InfoBar. Called when a
        // VMManager edit returns false.
        void ShowError(std::wstring const& message);

        std::wstring                    m_vmGuid;
        hyprv::app::vm::VmState         m_vmState = hyprv::app::vm::VmState::Unknown;

        // Snapshot of original values — for the apply-only-changed-fields
        // diff. Populated by LoadFromVm.
        std::wstring                    m_origName;
        std::wstring                    m_origNotes;
        hyprv::app::vm::VMManager::MemoryConfig    m_origMemory{};
        hyprv::app::vm::VMManager::ProcessorConfig m_origProcessor{};
        // Memory-weight slider value (0..10000) the dialog loaded; Save only
        // treats weight as changed when the slider moves off this, preserving an
        // exact non-100-aligned original on a no-touch Save.
        int                                        m_origWeightSliderValue = 5000;

        // Integration services: dynamically populated CheckBoxes. The
        // checkboxes are added to the integrationServicesHost StackPanel
        // by LoadFromVm; each has Tag = className. m_origIntegrationStates
        // maps className → original Enabled value for the diff.
        std::vector<std::pair<std::wstring, bool>> m_origIntegrationStates;
        std::vector<Microsoft::UI::Xaml::Controls::CheckBox> m_integrationChecks;

        // Network adapters: per-NIC switch ComboBox + the original switch
        // name captured at load. The diff in OnPrimaryButtonClick compares
        // the ComboBox's current selection against m_origAdapterSwitches
        // and calls SetNetworkAdapterSwitch only for NICs that changed.
        // Tag carries the NIC GUID (NetworkAdapter::nicGuid). The first
        // ComboBox item is always "(Not connected)" — a non-switch
        // sentinel mapped to empty switchName on save.
        struct NetworkAdapterRow
        {
            std::wstring                              nicGuid;
            Microsoft::UI::Xaml::Controls::ComboBox   switchCombo{ nullptr };
            // MAC editing. dynamicCheck on = Hyper-V-assigned; off = the
            // 12-hex value in macBox. Diffed against origDynamicMac/origMac.
            bool                                      origDynamicMac = true;
            std::wstring                              origMac;   // 12 hex, "" if none
            Microsoft::UI::Xaml::Controls::CheckBox   dynamicCheck{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBox    macBox{ nullptr };
            // VLAN. vlanModeCombo selects Untagged/Access/Trunk/Private (index ==
            // OperationMode). The access/trunk/private sub-fields feed a
            // VMManager::VlanConfig; diffed against the orig* values below.
            uint16_t                                  origVlanMode = 0;
            uint16_t                                  origVlanId = 0;          // access ID
            uint16_t                                  origNativeVlanId = 0;    // trunk native
            std::vector<uint16_t>                     origTrunkList;           // trunk allowed
            uint16_t                                  origPrimaryVlanId = 0;   // private
            uint16_t                                  origSecondaryVlanId = 0; // private
            uint8_t                                   origPvlanMode = 0;       // private role 1/2/3
            Microsoft::UI::Xaml::Controls::ComboBox   vlanModeCombo{ nullptr };
            Microsoft::UI::Xaml::Controls::NumberBox  vlanBox{ nullptr };        // access
            Microsoft::UI::Xaml::Controls::NumberBox  nativeVlanBox{ nullptr };  // trunk
            Microsoft::UI::Xaml::Controls::TextBox    trunkListBox{ nullptr };   // trunk
            Microsoft::UI::Xaml::Controls::NumberBox  primaryVlanBox{ nullptr }; // private
            Microsoft::UI::Xaml::Controls::NumberBox  secondaryVlanBox{ nullptr };// private
            Microsoft::UI::Xaml::Controls::ComboBox   pvlanRoleCombo{ nullptr }; // private
            // Per-card "Remove this adapter" (gated to VM Off). When checked,
            // Save calls VMManager::RemoveNetworkAdapter(nicGuid).
            Microsoft::UI::Xaml::Controls::CheckBox   removeCheck{ nullptr };
            // Advanced features (Msvm_EthernetSwitchPortSecuritySettingData):
            // MAC spoofing / DHCP guard / router guard / teaming checkboxes +
            // a port-mirroring combo (0=None,1=Destination,2=Source). Diffed
            // against origAdvanced; any change → SetNetworkAdapterAdvanced.
            hyprv::app::vm::VMManager::NicAdvancedFeatures origAdvanced{};
            Microsoft::UI::Xaml::Controls::CheckBox   spoofCheck{ nullptr };
            Microsoft::UI::Xaml::Controls::CheckBox   dhcpGuardCheck{ nullptr };
            Microsoft::UI::Xaml::Controls::CheckBox   routerGuardCheck{ nullptr };
            Microsoft::UI::Xaml::Controls::CheckBox   teamingCheck{ nullptr };
            // IEEE 802.1p priority tag (AllowIeeePriorityTag — same security
            // feature setting / origAdvanced as the four checks above).
            Microsoft::UI::Xaml::Controls::CheckBox   ieeePriorityCheck{ nullptr };
            Microsoft::UI::Xaml::Controls::ComboBox   portMirrorCombo{ nullptr };
            // "Protected network" (ClusterMonitored on the synthetic port —
            // a separate WMI object from the security feature setting above).
            bool                                      origClusterMonitored = true;
            Microsoft::UI::Xaml::Controls::CheckBox   protectedCheck{ nullptr };
            // Bandwidth limits (Msvm_EthernetSwitchPortBandwidthSettingData),
            // edited in Mbps. Originals are the load-time values rounded to
            // whole Mbps so a no-touch Save is a no-op (avoids re-writing a
            // non-Mbps-aligned stored value). 0 = unlimited / none.
            uint32_t                                  origBwMaxMbps = 0;
            uint32_t                                  origBwMinMbps = 0;
            Microsoft::UI::Xaml::Controls::NumberBox  bwMaxBox{ nullptr };
            Microsoft::UI::Xaml::Controls::NumberBox  bwMinBox{ nullptr };
            // Device naming (DeviceNamingEnabled on the synthetic port).
            bool                                      origDeviceNaming = false;
            Microsoft::UI::Xaml::Controls::CheckBox   deviceNamingCheck{ nullptr };
            // Hardware acceleration (VMQ / SR-IOV / IPsec offload — one
            // Msvm_EthernetSwitchPortOffloadSettingData feature setting).
            hyprv::app::vm::VMManager::NicOffloadFeatures origOffload{};
            Microsoft::UI::Xaml::Controls::CheckBox   vmqCheck{ nullptr };
            Microsoft::UI::Xaml::Controls::CheckBox   sriovCheck{ nullptr };
            Microsoft::UI::Xaml::Controls::CheckBox   ipsecCheck{ nullptr };
            Microsoft::UI::Xaml::Controls::NumberBox  ipsecMaxBox{ nullptr };
        };
        std::vector<NetworkAdapterRow>             m_networkRows;
        // Pending new NICs queued by "Add adapter..." (switch name, "" =
        // disconnected). Applied at Save via VMManager::AddNetworkAdapter.
        std::vector<std::wstring>                  m_pendingNics;

        // Storage / DVD drives. One row per DVD drive, built in LoadFromVm.
        // The path TextBox holds the mounted ISO (empty = no disc); Browse
        // fills it, Eject clears it. Save diffs box.Text() against
        // origMediaPath per row and calls VMManager::SetDvdMedia (which infers
        // mount/change/eject). driveRef/mediaRef identify the WMI objects.
        struct DvdDriveRow
        {
            std::wstring                            driveRef;
            std::wstring                            mediaRef;       // empty = drive currently has no disc
            std::wstring                            origMediaPath;  // ISO at load ("" = empty)
            Microsoft::UI::Xaml::Controls::TextBox  pathBox{ nullptr };
            // Per-card "Remove this drive" (gated like NIC add/remove). When
            // checked, Save calls RemoveDvdDrive and the media diff is skipped.
            Microsoft::UI::Xaml::Controls::CheckBox removeCheck{ nullptr };
        };
        std::vector<DvdDriveRow>                   m_dvdRows;
        // Empty DVD drives queued by "Add DVD drive", applied at Save via
        // VMManager::AddDvdDrive — each carries the picked controller + slot.
        struct PendingDvdAdd
        {
            std::wstring ctrlRef;
            int          slot = -1;
        };
        std::vector<PendingDvdAdd>                 m_pendingDvdAdds;

        // Checkpoints section. Originals for the diff: combo index (0=Production
        // /1=ProductionOnly/2=Standard/3=Disabled) + the file location. The
        // location field is editable only when the VM has no checkpoints.
        int                                        m_origCheckpointTypeIndex = 0;
        std::wstring                               m_origCheckpointLocation;
        // Smart Paging file location original (for the diff).
        std::wstring                               m_origSwapFileLocation;
        // COM ports — refs + load-time pipe paths for the diff. A VM always has
        // exactly two (COM 1 / COM 2); empty ref => that port wasn't found.
        std::wstring                               m_com1Ref, m_com2Ref;
        std::wstring                               m_origCom1Path, m_origCom2Path;
        // Debugger section (MANAGEMENT, shown only when the global feature
        // toggle is on). Load-time exe override + args for the diff.
        std::wstring                               m_origDebuggerExe, m_origDebuggerArgs;

        // Storage / hard disks. One row per attached VHD, built in LoadFromVm
        // with a "Detach" checkbox; m_pendingAttachPaths holds VHDs queued via
        // "Attach VHD...". Save detaches the checked rows and attaches the
        // queued paths (VMManager::DetachVhd / AttachVhd).
        struct HardDiskRow
        {
            std::wstring                            vhdRef;
            std::wstring                            driveRef;
            Microsoft::UI::Xaml::Controls::CheckBox detachCheck{ nullptr };
            // Storage QoS (normalized 8 KB IOPS). origIops* are the load-time
            // values for the diff; the boxes are blank when 0 (= none/unlimited).
            Microsoft::UI::Xaml::Controls::NumberBox iopsMinBox{ nullptr };
            Microsoft::UI::Xaml::Controls::NumberBox iopsMaxBox{ nullptr };
            uint64_t                                origIopsMin = 0;
            uint64_t                                origIopsMax = 0;
        };
        std::vector<HardDiskRow>                   m_hddRows;
        // "Attach VHD..." queue: an existing file to attach at Save, on the
        // picked controller + slot.
        struct PendingAttach
        {
            std::wstring path;
            std::wstring ctrlRef;
            int          slot = -1;
        };
        std::vector<PendingAttach>                 m_pendingAttachPaths;
        // "Create new VHD..." queue: a new file to create + attach at Save.
        struct PendingCreate
        {
            std::wstring path;
            uint64_t     sizeBytes = 0;
            bool         dynamic   = true;
            std::wstring ctrlRef;
            int          slot = -1;
        };
        std::vector<PendingCreate>                 m_pendingCreates;
        // "Attach physical disk..." queue: an offline host disk to attach as
        // pass-through at Save, on the picked controller + slot.
        struct PendingPhysAttach
        {
            std::wstring devicePath;   // Msvm_DiskDrive __PATH
            std::wstring label;        // display label (ElementName)
            std::wstring ctrlRef;
            int          slot = -1;
        };
        std::vector<PendingPhysAttach>             m_pendingPhysAttaches;

        // Storage controllers + device placement. m_controllers is the VM's
        // SCSI/IDE controllers (re-read fresh by RebuildStorageCards on load +
        // after Add/Remove SCSI controller); m_queuedSlots tracks slots already
        // consumed by queued (not-yet-saved) attach/create/DVD ops per
        // controller, so a placement flyout never offers the same slot twice in
        // one Save session.
        std::vector<hyprv::app::vm::VMManager::StorageController> m_controllers;
        std::map<std::wstring, std::set<int>>      m_queuedSlots;
        // Per-controller "Remove" buttons (ref -> button), built by
        // RebuildStorageCards; UpdateControllerRemoveButtons re-evaluates their
        // enabled state (Off + SCSI + no VM-used + no queued slots).
        std::vector<std::pair<std::wstring,
            Microsoft::UI::Xaml::Controls::Button>> m_controllerRemoveButtons;
        // Rebuild the controllers list + hard-disk + DVD device cards from
        // current VM state (re-reads GetStorageControllers/GetHardDisks/
        // GetDvdDrives). Does NOT touch the queued-op state/cards — only the
        // VM-side device view — so it's safe to call after an immediate
        // Add/Remove SCSI controller without wiping queued attaches.
        void RebuildStorageCards();
        void UpdateControllerRemoveButtons();
        // Remove the SCSI controller `ref` immediately (Off only, empty only),
        // then rebuild the cards. Wired to each empty controller's Remove btn.
        winrt::fire_and_forget RemoveScsiControllerByRef(std::wstring ref);
        // Placement-flyout helpers (shared by attach/create/add-DVD). Populate
        // fills `controllerCombo` from m_controllers + links its slot combo via
        // Tag; RefreshSlotFor fills `slotCombo` with the selected controller's
        // free slots; ComboRef/ComboSlot read the current selection.
        void PopulatePlacement(Microsoft::UI::Xaml::Controls::ComboBox const& controllerCombo,
                               Microsoft::UI::Xaml::Controls::ComboBox const& slotCombo);
        void RefreshSlotFor(Microsoft::UI::Xaml::Controls::ComboBox const& controllerCombo,
                            Microsoft::UI::Xaml::Controls::ComboBox const& slotCombo);
        std::wstring ComboRef(Microsoft::UI::Xaml::Controls::ComboBox const& controllerCombo);
        int          ComboSlot(Microsoft::UI::Xaml::Controls::ComboBox const& slotCombo);
        // Mark (ctrlRef, slot) consumed by a just-queued op + refresh the
        // controller Remove buttons (a queued device blocks its controller's
        // removal).
        void ConsumeQueuedSlot(std::wstring const& ctrlRef, int slot);
        // Human label for a controller ref ("SCSI Controller 0 · Slot 2"), used
        // on the queued-op cards.
        std::wstring DestinationLabel(std::wstring const& ctrlRef, int slot) const;
        // nicGuid -> originally-connected switch name ("" if disconnected
        // at load time). Same shape as m_origIntegrationStates so the diff
        // loop reads similarly.
        std::vector<std::pair<std::wstring, std::wstring>> m_origAdapterSwitches;

        // Firmware section. Gen 1 VMs have no Secure Boot — m_isGen2 gates
        // both the nav item's visibility and the section's edit-ability.
        // Originals captured at load for the apply-only-changed-fields diff.
        bool                            m_isGen2 = false;
        bool                            m_origSecureBootEnabled = false;
        // Index into secureBootTemplateCombo (0 = Windows, 1 = UEFI CA,
        // 2 = Open Source Shielded). Resolved from the VM's stored
        // SecureBootTemplateId GUID at load; -1 if unrecognized.
        int                             m_origSecureBootTemplateIndex = 0;
        // Boot order: the original BootSourceOrder reference strings in their
        // load-time order. Save reads bootOrderList's current order and only
        // calls SetBootOrder if it differs. Empty for Gen 1 / no boot entries.
        std::vector<std::wstring>       m_origBootOrder;

        // RDP section. Two pieces: whether the VM had a per-VM override at
        // open time (drives the Save logic — clear vs. set the override),
        // and a snapshot of the effective options at open (override if set,
        // else app defaults). The control values diff against this snapshot
        // so an OK-without-changes is a no-op.
        bool                            m_origRdpHasOverride = false;
        hyprv::app::settings::RdpOptions m_origRdpOptions{};
        // Set true on a Save that changed the effective RDP options. The opener
        // (the context-menu "Settings…" coroutine) reads RdpOptionsChanged()
        // after ShowAsync to reconnect this VM's live session.
        bool                            m_rdpOptionsChanged = false;

        // Security section (Gen 2 only). vTPM + state-encryption originals
        // captured at load for the diff.
        bool                            m_origTpmEnabled   = false;
        bool                            m_origEncryptState = false;
        bool                            m_origShielded     = false;

        // Automatic start/stop actions. Stored as the raw WMI enum values
        // (start: 2=Nothing/3=IfRunning/4=Always; stop: 2=TurnOff/3=Save/
        // 4=ShutDown). Combo index ↔ enum mapping is index+2 in both
        // directions. 0 means "VM never reported a value" — treated as a
        // no-diff baseline so a no-touch Save never writes.
        uint16_t                        m_origAutoStartAction = 0;
        uint16_t                        m_origAutoStopAction  = 0;
        uint32_t                        m_origAutoStartDelaySeconds = 0;
        // "Use automatic checkpoints" original (Checkpoints section).
        bool                            m_origAutoCheckpoints = false;
    };
}

namespace winrt::hyprv_app::factory_implementation
{
    struct VmSettingsDialog : VmSettingsDialogT<VmSettingsDialog, implementation::VmSettingsDialog>
    {
    };
}
