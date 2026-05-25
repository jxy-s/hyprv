#include "pch.h"
#include "VmSettingsDialog.xaml.h"
#if __has_include("VmSettingsDialog.g.cpp")
#include "VmSettingsDialog.g.cpp"
#endif

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Dispatching.h>

#include <atlbase.h>
#include <shobjidl.h>

#include <chrono>
#include <cmath>
#include <coroutine>
#include <cwctype>
#include <iterator>
#include <optional>
#include <string>

extern void HyprvAppLog(const wchar_t* fmt, ...);

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace
{
    // co_await this to hop back onto a Microsoft::UI::Dispatching::DispatcherQueue.
    // winrt::resume_foreground is only overloaded for the older
    // Windows::System::DispatcherQueue; WinUI 3's dispatcher needs this
    // adapter. HasThreadAccess short-circuits if we're already on the
    // right thread (cheap; avoids a needless TryEnqueue).
    struct ResumeOnDispatcher
    {
        Microsoft::UI::Dispatching::DispatcherQueue dq;
        bool await_ready() const noexcept
        {
            return dq && dq.HasThreadAccess();
        }
        void await_suspend(std::coroutine_handle<> h) const
        {
            if (!dq) { h.resume(); return; }
            dq.TryEnqueue([h] { h.resume(); });
        }
        void await_resume() const noexcept {}
    };

    // Same compact format the flyout uses — 12 hex chars to colon form.
    // Keeps the dialog from pulling in the whole MainWindow translation unit.
    std::wstring FormatMacForDialog(std::wstring const& raw)
    {
        if (raw.size() != 12) return raw;
        std::wstring out;
        out.reserve(17);
        for (size_t i = 0; i < 12; ++i)
        {
            out.push_back(raw[i]);
            if (i % 2 == 1 && i != 11) out.push_back(L':');
        }
        return out;
    }

    // Sentinel label in the switch ComboBox for "disconnect". Maps to an
    // empty switchName when handed to VMManager::SetNetworkAdapterSwitch.
    constexpr wchar_t const* kDisconnectLabel = L"(Not connected)";

    // Strip MAC separators (colons / dashes / spaces) and upcase, yielding the
    // bare 12-hex form Hyper-V stores. Caller validates the length.
    std::wstring NormalizeMac(std::wstring const& in)
    {
        std::wstring out;
        out.reserve(12);
        for (wchar_t c : in)
        {
            if ((c >= L'0' && c <= L'9') ||
                (c >= L'A' && c <= L'F') ||
                (c >= L'a' && c <= L'f'))
                out.push_back(static_cast<wchar_t>(towupper(c)));
        }
        return out;
    }

    // Modal file picker for the Storage section. Classic Common Item Dialog
    // (IFileOpenDialog) owned by the active top-level window — the
    // ContentDialog is an in-app popup over the main window, so
    // GetActiveWindow() is the right owner without plumbing an HWND down.
    // Returns nullopt on cancel. Runs synchronously (the dialog pumps its own
    // modal loop) — fine for a Browse click.
    std::optional<std::wstring> PickFileDialog(
        wchar_t const* title, wchar_t const* filterLabel, wchar_t const* filterPattern)
    {
        CComPtr<IFileOpenDialog> dlg;
        if (FAILED(dlg.CoCreateInstance(CLSID_FileOpenDialog,
                nullptr, CLSCTX_INPROC_SERVER)))
            return std::nullopt;
        COMDLG_FILTERSPEC filters[] = {
            { filterLabel,        filterPattern },
            { L"All files (*.*)", L"*.*"        },
        };
        dlg->SetFileTypes(ARRAYSIZE(filters), filters);
        dlg->SetTitle(title);
        if (FAILED(dlg->Show(GetActiveWindow())))
            return std::nullopt;   // cancelled or error
        CComPtr<IShellItem> item;
        if (FAILED(dlg->GetResult(&item)) || !item)
            return std::nullopt;
        PWSTR path = nullptr;
        std::optional<std::wstring> result;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
            result = std::wstring(path);
        if (path) CoTaskMemFree(path);
        return result;
    }

    std::optional<std::wstring> PickIsoFile()
    {
        return PickFileDialog(L"Select a disc image",
                              L"Disc image (*.iso)", L"*.iso");
    }
    std::optional<std::wstring> PickVhdFile()
    {
        return PickFileDialog(L"Select a virtual hard disk",
                              L"Virtual hard disk (*.vhdx;*.vhd)", L"*.vhdx;*.vhd");
    }

    // Folder picker (IFileOpenDialog + FOS_PICKFOLDERS) — for the checkpoint
    // file location. Same owner/lifetime story as PickFileDialog.
    std::optional<std::wstring> PickFolder(wchar_t const* title)
    {
        CComPtr<IFileOpenDialog> dlg;
        if (FAILED(dlg.CoCreateInstance(CLSID_FileOpenDialog,
                nullptr, CLSCTX_INPROC_SERVER)))
            return std::nullopt;
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS);
        dlg->SetTitle(title);
        if (FAILED(dlg->Show(GetActiveWindow())))
            return std::nullopt;
        CComPtr<IShellItem> item;
        if (FAILED(dlg->GetResult(&item)) || !item)
            return std::nullopt;
        PWSTR path = nullptr;
        std::optional<std::wstring> result;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
            result = std::wstring(path);
        if (path) CoTaskMemFree(path);
        return result;
    }

    // Save-dialog variant for "Create new VHD" — lets the user name a new file.
    std::optional<std::wstring> PickSaveVhdFile()
    {
        CComPtr<IFileSaveDialog> dlg;
        if (FAILED(dlg.CoCreateInstance(CLSID_FileSaveDialog,
                nullptr, CLSCTX_INPROC_SERVER)))
            return std::nullopt;
        COMDLG_FILTERSPEC filters[] = {
            { L"Virtual hard disk (*.vhdx)", L"*.vhdx" },
        };
        dlg->SetFileTypes(ARRAYSIZE(filters), filters);
        dlg->SetDefaultExtension(L"vhdx");
        dlg->SetTitle(L"Create new virtual hard disk");
        if (FAILED(dlg->Show(GetActiveWindow())))
            return std::nullopt;
        CComPtr<IShellItem> item;
        if (FAILED(dlg->GetResult(&item)) || !item)
            return std::nullopt;
        PWSTR path = nullptr;
        std::optional<std::wstring> result;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
            result = std::wstring(path);
        if (path) CoTaskMemFree(path);
        return result;
    }

    // Compact human-readable byte size for the read-only hard-disk rows.
    std::wstring FormatDiskSize(uint64_t bytes)
    {
        if (bytes == 0) return L"";
        double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
        wchar_t buf[32];
        if (gb >= 1.0) swprintf_s(buf, L"%.1f GB", gb);
        else           swprintf_s(buf, L"%.0f MB",
                                  static_cast<double>(bytes) / (1024.0 * 1024.0));
        return buf;
    }

    // Well-known Secure Boot template GUIDs, in the same order as the
    // secureBootTemplateCombo items. These are fixed Hyper-V identifiers
    // (same ones Set-VMFirmware -SecureBootTemplate selects). Stored in the
    // VSSD as braced lowercase strings; we write them in that canonical form.
    constexpr wchar_t const* kSecureBootTemplates[] = {
        L"{1734c6e8-3154-4dda-ba5f-a874cc483422}",  // 0: Microsoft Windows
        L"{272e7447-90a4-4563-a4b9-8e4ab00526ce}",  // 1: Microsoft UEFI Certificate Authority
        L"{4292d494-9bbe-49ae-8d4a-3c0e7d6a2e07}",  // 2: Open Source Shielded VM
    };

    // Normalize a template GUID for comparison: lowercase, drop braces and
    // surrounding whitespace. Hyper-V is consistent but a hand-edited config
    // or a different SKU could vary the casing/bracing, so be forgiving.
    std::wstring NormalizeGuid(std::wstring s)
    {
        std::wstring out;
        out.reserve(s.size());
        for (wchar_t c : s)
        {
            if (c == L'{' || c == L'}' || c == L' ' || c == L'\t') continue;
            out.push_back(static_cast<wchar_t>(towlower(c)));
        }
        return out;
    }

    // Map a stored SecureBootTemplateId to its combo index, or -1 if it
    // doesn't match any of the three well-known templates.
    int SecureBootTemplateIndex(std::wstring const& storedId)
    {
        std::wstring want = NormalizeGuid(storedId);
        if (want.empty()) return -1;
        for (int i = 0; i < static_cast<int>(std::size(kSecureBootTemplates)); ++i)
            if (NormalizeGuid(kSecureBootTemplates[i]) == want) return i;
        return -1;
    }

    // RDP "Initial size" presets - index to (width, height). Shared by the
    // load (select the matching item) and the Save diff (read the pick). Must
    // stay in lock-step with the rdpResolutionCombo items in the XAML.
    constexpr uint16_t kRdpResW[] = { 800, 1024, 1280, 1280, 1366, 1600, 1920, 2560 };
    constexpr uint16_t kRdpResH[] = { 600,  768,  720, 1024,  768,  900, 1080, 1440 };
    int RdpResolutionIndex(uint16_t w, uint16_t h)
    {
        for (int i = 0; i < static_cast<int>(std::size(kRdpResW)); ++i)
            if (kRdpResW[i] == w && kRdpResH[i] == h) return i;
        return 1;  // default 1024x768
    }
    // Display-scale combo index (0 = Auto) to percent. Lock-step with the
    // rdpScaleCombo items in the XAML.
    uint16_t RdpScalePercentForIndex(int idx)
    {
        switch (idx)
        {
        case 1:  return 100;
        case 2:  return 125;
        case 3:  return 150;
        case 4:  return 175;
        case 5:  return 200;
        default: return 0;   // Auto
        }
    }

    // Checkpoint type (UserSnapshotType) <-> checkpointTypeCombo index. Combo
    // order: 0=Production(3), 1=Production only(4), 2=Standard(5), 3=Disabled(2).
    int CheckpointTypeToIndex(uint16_t t)
    {
        switch (t) { case 3: return 0; case 4: return 1; case 5: return 2;
                     case 2: return 3; default: return 0; }
    }
    uint16_t CheckpointIndexToType(int i)
    {
        switch (i) { case 0: return 3; case 1: return 4; case 2: return 5;
                     case 3: return 2; default: return 3; }
    }
}

namespace winrt::hyprv_app::implementation
{
    VmSettingsDialog::VmSettingsDialog()
    {
        // ContentDialog's PrimaryButtonClick is the canonical extension
        // point for OK-handling. Wire here in the ctor so it survives
        // re-show. (No "applied" flag — we cancel the close on failure via
        // ContentDialogButtonClickEventArgs::Cancel = true.)
        this->PrimaryButtonClick({ this, &VmSettingsDialog::OnPrimaryButtonClick });
    }

    void VmSettingsDialog::SetVm(std::wstring const& vmGuid)
    {
        m_vmGuid = vmGuid;
        auto vmOpt = hyprv::app::vm::VMManager::Instance().GetByGuid(vmGuid);
        if (!vmOpt)
        {
            HyprvAppLog(L"[settings-dlg] SetVm: VM not found guid=%s", vmGuid.c_str());
            ShowError(L"VM not found.");
            return;
        }
        m_vmState = vmOpt->state;
        // Suffix the VM's display name onto the title — with the rail
        // tucked away on the left and the dialog floating over a tab,
        // it's not always obvious which VM you're editing. The static
        // XAML title is just the prefix; the per-VM portion is appended
        // here so it tracks SetVm() calls (a future re-show against a
        // different VM picks up the new name automatically).
        if (!vmOpt->elementName.empty())
        {
            std::wstring t = L"Virtual Machine Settings \x2014 ";
            t += vmOpt->elementName;
            this->Title(winrt::box_value(winrt::hstring{ t }));
        }
        LoadFromVm(*vmOpt);
        ApplyStateGating();
        // Default to the first Management section ("Name") — matches the
        // user's first reaching for "what's this VM called" and gives an
        // editable text field on open instead of a numeric form.
        if (auto nav = navManagement()) nav.SelectedIndex(0);
    }

    void VmSettingsDialog::OnNavSelectionChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        auto list = sender.try_as<Microsoft::UI::Xaml::Controls::ListView>();
        if (!list) return;

        // EARLY RETURN: this event also fires when we programmatically
        // clear the OTHER list's selection (SelectedIndex = -1). Without
        // this guard, that recursive fire would clear the list we just
        // wrote to and the user's click ends up showing nothing — visible
        // as "needs two clicks to switch sections".
        if (list.SelectedIndex() < 0) return;

        // Real user selection — clear the other list so the highlight
        // only shows on the currently-active item across both lists.
        if (list == navHardware()  && navManagement().SelectedIndex() != -1)
            navManagement().SelectedIndex(-1);
        if (list == navManagement() && navHardware().SelectedIndex() != -1)
            navHardware().SelectedIndex(-1);

        auto item = list.SelectedItem().try_as<
            Microsoft::UI::Xaml::Controls::ListViewItem>();
        if (!item) return;
        auto tag = winrt::unbox_value_or<winrt::hstring>(item.Tag(), L"");
        std::wstring key{ tag };

        // Show only the matching section. All section panels live at the
        // same grid position; toggling Visibility is enough.
        auto show = [&](Microsoft::UI::Xaml::Controls::StackPanel const& panel,
                        bool visible) {
            if (panel) panel.Visibility(visible
                ? Microsoft::UI::Xaml::Visibility::Visible
                : Microsoft::UI::Xaml::Visibility::Collapsed);
        };
        show(sectionName(),        key == L"name");
        show(sectionMemory(),      key == L"memory");
        show(sectionProcessor(),   key == L"processor");
        show(sectionFirmware(),    key == L"firmware");
        show(sectionSecurity(),    key == L"security");
        show(sectionStorage(),     key == L"storage");
        show(sectionNetwork(),     key == L"network");
        show(sectionRdp(),         key == L"rdp");
        show(sectionIntegration(), key == L"integration");
        show(sectionNotes(),       key == L"notes");
        show(sectionAutoActions(), key == L"autoactions");
        show(sectionCheckpoints(), key == L"checkpoints");
        show(sectionSmartPaging(), key == L"smartpaging");
        show(sectionComPorts(),    key == L"comports");
        show(sectionDebugger(),    key == L"debugger");
    }

    // Format a VLAN id list as a comma-separated string ("10,20,30") for the
    // trunk "Allowed VLANs" text box.
    static std::wstring FormatVlanList(std::vector<uint16_t> const& v)
    {
        std::wstring s;
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (i) s += L",";
            s += std::to_wstring(v[i]);
        }
        return s;
    }

    // Parse a trunk "Allowed VLANs" string — comma/semicolon separated tokens,
    // each an id ("20") or an inclusive range ("1-50"). Out-of-range / garbage
    // tokens are skipped. Mirrors PowerShell's -AllowedVlanIdList.
    static std::vector<uint16_t> ParseVlanList(std::wstring const& text)
    {
        std::vector<uint16_t> out;
        std::wstring tok;
        auto flush = [&](std::wstring t) {
            size_t a = t.find_first_not_of(L" \t");
            if (a == std::wstring::npos) return;
            size_t b = t.find_last_not_of(L" \t");
            t = t.substr(a, b - a + 1);
            try
            {
                auto dash = t.find(L'-');
                if (dash != std::wstring::npos)
                {
                    int lo = std::stoi(t.substr(0, dash));
                    int hi = std::stoi(t.substr(dash + 1));
                    if (lo > hi) std::swap(lo, hi);
                    for (int x = lo; x <= hi; ++x)
                        if (x >= 1 && x <= 4094) out.push_back(static_cast<uint16_t>(x));
                }
                else
                {
                    int x = std::stoi(t);
                    if (x >= 1 && x <= 4094) out.push_back(static_cast<uint16_t>(x));
                }
            }
            catch (...) { /* skip non-numeric token */ }
        };
        for (wchar_t c : text)
        {
            if (c == L',' || c == L';') { flush(tok); tok.clear(); }
            else tok += c;
        }
        flush(tok);
        return out;
    }

    // Build a compact "Advanced" expander (collapsed by default) that hides the
    // rarely-touched rows of a dense per-device card behind a single drill-in.
    // Caller appends the rows to .Content() via the returned StackPanel. The
    // chunky default Expander chrome is tamed by the ExpanderMinHeight /
    // ExpanderContentPadding overrides in this dialog's XAML resources.
    static Microsoft::UI::Xaml::Controls::StackPanel MakeAdvancedExpander(
        Microsoft::UI::Xaml::Controls::StackPanel const& parent,
        std::wstring const& header,
        std::wstring const& summary = std::wstring{})
    {
        Microsoft::UI::Xaml::Controls::Expander exp;
        if (summary.empty())
        {
            // Render the header as a 12pt TextBlock (not a bare string) so it
            // matches the NIC card's summary header — a plain string would inherit
            // the Expander template's larger default header font, making the disk
            // card's "Settings" visibly bigger than the NIC card's.
            Microsoft::UI::Xaml::Controls::TextBlock t;
            t.Text(winrt::hstring{ header });
            t.FontSize(12);
            t.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
            exp.Header(t);
        }
        else
        {
            // Title + a dimmed at-a-glance summary, so the COLLAPSED card still
            // shows the key state (e.g. a NIC's VLAN / MAC mode) without drilling
            // in. The summary rides the header so it tracks the expander chrome.
            Microsoft::UI::Xaml::Controls::StackPanel hp;
            hp.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
            hp.Spacing(8);
            Microsoft::UI::Xaml::Controls::TextBlock t;
            t.Text(winrt::hstring{ header });
            t.FontSize(12);
            t.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
            hp.Children().Append(t);
            Microsoft::UI::Xaml::Controls::TextBlock s;
            s.Text(winrt::hstring{ summary });
            s.FontSize(11);
            s.Opacity(0.6);   // dim, no theme-brush lookup needed
            s.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
            hp.Children().Append(s);
            exp.Header(hp);
        }
        exp.IsExpanded(false);
        exp.FontSize(12);
        exp.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
        exp.HorizontalContentAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
        // Compact the chunky default chrome. These MUST be LOCAL values — the
        // template pulls MinHeight (=48) and content Padding (=16) via
        // {StaticResource}, which a resource override can't reach; a local value
        // outranks the style setter and template-binds through to the header.
        exp.MinHeight(28);
        exp.Padding(Microsoft::UI::Xaml::ThicknessHelper::FromLengths(12, 8, 12, 10));
        Microsoft::UI::Xaml::Controls::StackPanel inner;
        inner.Spacing(6);
        exp.Content(inner);
        parent.Children().Append(exp);
        return inner;
    }

    void VmSettingsDialog::LoadFromVm(hyprv::app::vm::VirtualMachine const& vm)
    {
        // Name.
        m_origName = vm.elementName;
        if (auto box = nameBox()) box.Text(winrt::hstring{ m_origName });

        // Notes — VirtualMachine.notes is a single string (we collapse the
        // CIM StringArray to first-element on read).
        m_origNotes = vm.notes;
        if (auto box = notesBox()) box.Text(winrt::hstring{ m_origNotes });

        // Memory — read the FULL config fresh (the poll cache doesn't carry
        // buffer/weight, so loading from vm.* would show hardcoded defaults).
        m_origMemory = hyprv::app::vm::VMManager::Instance().GetMemoryConfig(m_vmGuid);
        if (auto b = memStartupBox())  b.Value(static_cast<double>(m_origMemory.startupMb));
        if (auto t = memDynamicToggle()) t.IsOn(m_origMemory.dynamicEnabled);
        if (auto b = memMinBox())      b.Value(static_cast<double>(m_origMemory.minMb));
        if (auto b = memMaxBox())      b.Value(static_cast<double>(m_origMemory.maxMb));
        if (auto b = memBufferBox())   b.Value(static_cast<double>(m_origMemory.targetBufferPct));
        // Memory weight: Low→High slider over the full WMI range (0..10000, in
        // 100-steps). Set the slider to the actual weight, then remember the
        // (possibly step-snapped) displayed value so a no-touch Save keeps the
        // exact original even if it isn't on a 100-step.
        if (auto s = memWeightSlider())
        {
            double w = static_cast<double>(m_origMemory.priority);
            if (w < 0.0) w = 0.0; else if (w > 10000.0) w = 10000.0;
            s.Value(w);
            m_origWeightSliderValue = static_cast<int>(std::llround(s.Value()));
            if (auto t = memWeightValueText())
                t.Text(winrt::hstring{ std::to_wstring(m_origWeightSliderValue) });
        }

        // Processor — also read fresh. Reservation/Limit are RAW WMI (percent ×
        // 1000); the UI shows 0..100 %, so divide by 1000 on load (and ×1000 on
        // save). Then refresh the derived "percent of total resources" read-outs.
        m_origProcessor = hyprv::app::vm::VMManager::Instance().GetProcessorConfig(m_vmGuid);
        if (auto b = cpuCountBox())       b.Value(static_cast<double>(m_origProcessor.count));
        if (auto b = cpuReservationBox()) b.Value(static_cast<double>(m_origProcessor.reservationPct) / 1000.0);
        if (auto b = cpuLimitBox())       b.Value(static_cast<double>(m_origProcessor.limitPct) / 1000.0);
        if (auto b = cpuWeightBox())      b.Value(static_cast<double>(m_origProcessor.weight));
        UpdateProcessorDerived();
        // Compatibility + NUMA (proc NUMA from m_origProcessor; max memory per
        // node from m_origMemory, loaded above).
        if (auto c = cpuCompatCheck())   c.IsChecked(m_origProcessor.limitProcessorFeatures);
        if (auto b = numaMaxProcsBox())  b.Value(static_cast<double>(m_origProcessor.maxProcessorsPerNumaNode));
        if (auto b = numaMaxNodesBox())  b.Value(static_cast<double>(m_origProcessor.maxNumaNodesPerSocket));
        if (auto b = numaThreadsBox())   b.Value(static_cast<double>(m_origProcessor.hwThreadsPerCore));
        if (auto b = numaMaxMemoryBox()) b.Value(static_cast<double>(m_origMemory.maxMemoryPerNumaNodeMb));

        // Firmware (Gen 2) / BIOS (Gen 1). Both generations have a boot order
        // (uint16 BootOrder for Gen 1, refs for Gen 2). Secure Boot now lives in
        // the SECURITY section (Gen 2 only — that whole nav item is hidden for
        // Gen 1), so Firmware/BIOS is just the boot-order list for both.
        m_isGen2 = (vm.generation == L"Generation 2");
        if (auto it = navFirmwareItem())
        {
            it.Visibility(Microsoft::UI::Xaml::Visibility::Visible);
            it.Content(box_value(winrt::hstring{ m_isGen2 ? L"Firmware" : L"BIOS" }));
        }
        if (auto t = firmwareSectionTitle())
            t.Text(winrt::hstring{ m_isGen2 ? L"Firmware" : L"BIOS" });
        m_origSecureBootEnabled = vm.secureBootEnabled.value_or(false);
        // Resolve the stored template GUID to a combo index. Unrecognized
        // (or absent) → fall back to index 0 (Microsoft Windows) for both
        // the displayed value AND the diff baseline, so a no-touch Save
        // never spuriously rewrites the template.
        {
            int resolved = SecureBootTemplateIndex(vm.secureBootTemplateId);
            m_origSecureBootTemplateIndex = (resolved >= 0) ? resolved : 0;
        }
        if (auto t = secureBootToggle()) t.IsOn(m_origSecureBootEnabled);
        if (auto c = secureBootTemplateCombo())
            c.SelectedIndex(m_origSecureBootTemplateIndex);

        // Boot order. One ListViewItem per device, in order; Content =
        // friendly description, Tag = the opaque token written back at Save
        // (a BootSourceOrder reference for Gen 2, the uint16 device code as a
        // string for Gen 1). m_origBootOrder captures the load-time order for
        // the diff. Empty (no boot devices) collapses the list + shows the hint.
        m_origBootOrder.clear();
        if (auto lv = bootOrderList())
        {
            lv.Items().Clear();
            if (m_isGen2)
            {
                auto boot = hyprv::app::vm::VMManager::Instance().GetBootOrder(m_vmGuid);
                for (auto const& e : boot)
                {
                    Microsoft::UI::Xaml::Controls::ListViewItem item;
                    item.Content(box_value(winrt::hstring{ e.description }));
                    item.Tag(box_value(winrt::hstring{ e.ref }));
                    item.FontSize(12);
                    lv.Items().Append(item);
                    m_origBootOrder.push_back(e.ref);
                }
            }
            else
            {
                auto boot = hyprv::app::vm::VMManager::Instance().GetBootOrderGen1(m_vmGuid);
                for (auto const& e : boot)
                {
                    Microsoft::UI::Xaml::Controls::ListViewItem item;
                    item.Content(box_value(winrt::hstring{ e.description }));
                    item.Tag(box_value(winrt::hstring{ std::to_wstring(e.code) }));
                    item.FontSize(12);
                    lv.Items().Append(item);
                    m_origBootOrder.push_back(std::to_wstring(e.code));
                }
            }
            const bool hasBoot = !m_origBootOrder.empty();
            if (auto g = bootOrderGrid())
                g.Visibility(hasBoot ? Microsoft::UI::Xaml::Visibility::Visible
                                     : Microsoft::UI::Xaml::Visibility::Collapsed);
            if (auto t = bootOrderEmptyText())
                t.Visibility(hasBoot ? Microsoft::UI::Xaml::Visibility::Collapsed
                                     : Microsoft::UI::Xaml::Visibility::Visible);
        }

        // Security (vTPM + state encryption) — Generation 2 only, same as
        // Firmware. Collapse the nav item for Gen 1. Originals captured for the
        // diff; nullopt (not reported) maps to Off for both display and
        // baseline so a no-touch Save is a no-op.
        if (auto it = navSecurityItem())
            it.Visibility(m_isGen2
                ? Microsoft::UI::Xaml::Visibility::Visible
                : Microsoft::UI::Xaml::Visibility::Collapsed);
        // Read security settings LIVE (not the poll cache) so a value saved
        // moments ago is reflected — the cache lags Save by up to a poll cycle.
        {
            auto sec = hyprv::app::vm::VMManager::Instance().GetVmSecurity(m_vmGuid);
            m_origTpmEnabled   = sec.tpmEnabled.value_or(vm.tpmEnabled.value_or(false));
            m_origEncryptState = sec.encryptState.value_or(vm.encryptStateEnabled.value_or(false));
            m_origShielded     = sec.shielded.value_or(false);
        }
        if (auto t = tpmToggle())          t.IsOn(m_origTpmEnabled);
        if (auto t = encryptStateToggle()) t.IsOn(m_origEncryptState);
        if (auto t = shieldToggle())       t.IsOn(m_origShielded);

        // Automatic start/stop actions. WMI enum ↔ combo index is +2 in both
        // directions (enum 2/3/4 ↔ index 0/1/2). When the VM hasn't reported
        // a value yet (0), fall back to Hyper-V's own defaults — start =
        // StartIfRunning (3), stop = Save (3) — for BOTH the displayed combo
        // and the diff baseline, so a no-touch Save never spuriously writes.
        {
            auto resolve = [](uint16_t v, uint16_t fallback) -> uint16_t {
                return (v >= 2 && v <= 4) ? v : fallback;
            };
            m_origAutoStartAction = resolve(vm.autoStartAction, 3);
            m_origAutoStopAction  = resolve(vm.autoStopAction, 3);
            if (auto c = autoStartCombo())
                c.SelectedIndex(static_cast<int>(m_origAutoStartAction) - 2);
            if (auto c = autoStopCombo())
                c.SelectedIndex(static_cast<int>(m_origAutoStopAction) - 2);
            m_origAutoStartDelaySeconds = vm.autoStartDelaySeconds;
            if (auto b = autoStartDelayBox())
            {
                if (m_origAutoStartDelaySeconds > 0)
                    b.Value(static_cast<double>(m_origAutoStartDelaySeconds));
                // else leave blank (default NaN) — 0 = no delay.
            }
        }

        // Checkpoints — type combo + file location. The location is editable
        // only when the VM has no checkpoints (Hyper-V rejects a SnapshotDataRoot
        // change otherwise); gate the box + show a hint when checkpoints exist.
        m_origCheckpointTypeIndex = CheckpointTypeToIndex(vm.userSnapshotType);
        m_origCheckpointLocation  = vm.snapshotDataRoot;
        m_origAutoCheckpoints     = vm.automaticCheckpointsEnabled;
        if (auto c = checkpointTypeCombo())
            c.SelectedIndex(m_origCheckpointTypeIndex);
        if (auto c = automaticCheckpointsCheck())
            c.IsChecked(m_origAutoCheckpoints);
        if (auto b = checkpointLocationBox())
            b.Text(winrt::hstring{ m_origCheckpointLocation });
        {
            const bool hasCheckpoints = !vm.snapshots.empty();
            if (auto b = checkpointLocationBox()) b.IsEnabled(!hasCheckpoints);
            if (auto b = checkpointBrowseButton()) b.IsEnabled(!hasCheckpoints);
            if (auto t = checkpointLocationHint())
                t.Visibility(hasCheckpoints
                    ? Microsoft::UI::Xaml::Visibility::Visible
                    : Microsoft::UI::Xaml::Visibility::Collapsed);
        }

        // Smart Paging file location.
        m_origSwapFileLocation = vm.swapFileDataRoot;
        if (auto b = smartPagingLocationBox())
            b.Text(winrt::hstring{ m_origSwapFileLocation });

        // COM ports — exactly two (COM 1 / COM 2). Capture refs + load-time
        // pipe paths for the diff; empty path = disconnected.
        m_com1Ref.clear(); m_com2Ref.clear();
        m_origCom1Path.clear(); m_origCom2Path.clear();
        for (auto const& p : hyprv::app::vm::VMManager::Instance().GetSerialPorts(m_vmGuid))
        {
            if (p.name == L"COM 1") { m_com1Ref = p.ref; m_origCom1Path = p.path; }
            else if (p.name == L"COM 2") { m_com2Ref = p.ref; m_origCom2Path = p.path; }
        }
        if (auto b = com1PathBox()) b.Text(winrt::hstring{ m_origCom1Path });
        if (auto b = com2PathBox()) b.Text(winrt::hstring{ m_origCom2Path });

        // Debugger (MANAGEMENT) — gated by the global feature toggle. Hide the
        // nav item entirely when off; otherwise load the per-VM exe override +
        // args, with the exe placeholder showing the current global default.
        {
            auto& sdbg = hyprv::app::settings::Settings::Instance();
            if (auto item = navDebuggerItem())
                item.Visibility(sdbg.DebuggerEnabled()
                    ? Microsoft::UI::Xaml::Visibility::Visible
                    : Microsoft::UI::Xaml::Visibility::Collapsed);
            m_origDebuggerExe  = sdbg.VmDebuggerExe(m_vmGuid);
            m_origDebuggerArgs = sdbg.VmDebuggerArgs(m_vmGuid);
            if (auto b = debuggerExeBox())
            {
                b.Text(winrt::hstring{ m_origDebuggerExe });
                b.PlaceholderText(winrt::hstring{ sdbg.DebuggerExe() });
            }
            if (auto b = debuggerArgsBox()) b.Text(winrt::hstring{ m_origDebuggerArgs });
        }

        // Integration services — query VMManager + build a CheckBox per
        // service. Tag carries the WMI class name for the diff path.
        // Cleared first so re-loading replaces rather than appends.
        m_origIntegrationStates.clear();
        m_integrationChecks.clear();
        if (auto host = integrationServicesHost())
        {
            host.Children().Clear();
            auto services = hyprv::app::vm::VMManager::Instance()
                .GetIntegrationServices(m_vmGuid);
            for (auto const& svc : services)
            {
                Microsoft::UI::Xaml::Controls::CheckBox cb;
                cb.Content(box_value(winrt::hstring{ svc.displayName }));
                cb.IsChecked(svc.enabled);
                cb.FontSize(12);
                cb.MinHeight(26);
                cb.Tag(box_value(winrt::hstring{ svc.className }));
                host.Children().Append(cb);
                m_integrationChecks.push_back(cb);
                m_origIntegrationStates.emplace_back(svc.className, svc.enabled);
            }
        }

        // Network adapters — one card per NIC. Each card carries:
        //   [name + MAC label]   [switch ComboBox]
        // ComboBox items: "(Not connected)" sentinel + every virtual switch
        // present on the host. The current connection is preselected; the
        // user changes selection, OnPrimaryButtonClick diffs against
        // m_origAdapterSwitches and calls SetNetworkAdapterSwitch only for
        // rows that actually changed.
        m_origAdapterSwitches.clear();
        m_networkRows.clear();
        m_pendingNics.clear();
        if (auto ah = networkAddHost()) ah.Children().Clear();
        if (auto host = networkAdaptersHost())
        {
            host.Children().Clear();
            auto switches = hyprv::app::vm::VMManager::Instance().GetVirtualSwitches();
            // Populate the "Add adapter..." flyout switch picker: the
            // disconnect sentinel followed by every host switch. Default to
            // the first real switch when one exists (most adds want a
            // connection), else the sentinel.
            if (auto c = addNicSwitchCombo())
            {
                c.Items().Clear();
                c.Items().Append(box_value(winrt::hstring{ kDisconnectLabel }));
                for (auto const& sw : switches)
                    c.Items().Append(box_value(winrt::hstring{ sw }));
                c.SelectedIndex(switches.empty() ? 0 : 1);
            }
            if (vm.networkAdapters.empty())
            {
                if (auto t = networkEmptyText())
                    t.Visibility(Microsoft::UI::Xaml::Visibility::Visible);
            }
            else
            {
                if (auto t = networkEmptyText())
                    t.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
                for (auto const& nic : vm.networkAdapters)
                {
                    Microsoft::UI::Xaml::Controls::Grid grid;
                    Microsoft::UI::Xaml::Controls::ColumnDefinition c0, c1;
                    // Auto-size the label column (the fixed 220 DIP left a big
                    // gap before the switch dropdown) + a tight 12 DIP gap.
                    c0.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                        0, Microsoft::UI::Xaml::GridUnitType::Auto));
                    c1.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                        1, Microsoft::UI::Xaml::GridUnitType::Star));
                    grid.ColumnDefinitions().Append(c0);
                    grid.ColumnDefinitions().Append(c1);
                    grid.ColumnSpacing(12);
                    grid.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);

                    Microsoft::UI::Xaml::Controls::TextBlock label;
                    std::wstring labelText = nic.name.empty()
                        ? std::wstring{ L"Network Adapter" } : nic.name;
                    label.Text(winrt::hstring{ labelText });
                    label.FontSize(12);
                    label.TextWrapping(Microsoft::UI::Xaml::TextWrapping::NoWrap);
                    label.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(label, 0);
                    grid.Children().Append(label);

                    Microsoft::UI::Xaml::Controls::ComboBox combo;
                    combo.FontSize(12);
                    combo.MinHeight(28);
                    combo.Tag(box_value(winrt::hstring{ nic.nicGuid }));
                    // First item is the disconnect sentinel; subsequent
                    // items are the host's virtual switches in the order
                    // GetVirtualSwitches returned them.
                    combo.Items().Append(box_value(winrt::hstring{ kDisconnectLabel }));
                    for (auto const& sw : switches)
                        combo.Items().Append(box_value(winrt::hstring{ sw }));

                    // Preselect: empty switchName OR !connected -> sentinel.
                    // Otherwise match the switch by name; if WMI reported a
                    // switch name that isn't in our list (rare race during
                    // a switch rename), fall back to the sentinel and let
                    // the user choose explicitly.
                    int selectedIndex = 0;
                    if (nic.connected && !nic.switchName.empty())
                    {
                        for (size_t i = 0; i < switches.size(); ++i)
                        {
                            if (switches[i] == nic.switchName)
                            {
                                selectedIndex = static_cast<int>(i + 1);
                                break;
                            }
                        }
                    }
                    combo.SelectedIndex(selectedIndex);
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(combo, 1);
                    grid.Children().Append(combo);

                    // MAC row: "MAC address" label + Dynamic checkbox + a
                    // 12-hex text box (enabled only when not dynamic). Pre-
                    // filled with the current MAC formatted with colons.
                    Microsoft::UI::Xaml::Controls::Grid macGrid;
                    Microsoft::UI::Xaml::Controls::ColumnDefinition m0, m1, m2;
                    m0.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                        220, Microsoft::UI::Xaml::GridUnitType::Pixel));
                    m1.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                        0, Microsoft::UI::Xaml::GridUnitType::Auto));
                    m2.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                        1, Microsoft::UI::Xaml::GridUnitType::Star));
                    macGrid.ColumnDefinitions().Append(m0);
                    macGrid.ColumnDefinitions().Append(m1);
                    macGrid.ColumnDefinitions().Append(m2);
                    macGrid.ColumnSpacing(8);
                    macGrid.Margin(Microsoft::UI::Xaml::ThicknessHelper::FromUniformLength(0));

                    Microsoft::UI::Xaml::Controls::TextBlock macLabel;
                    macLabel.Text(winrt::hstring{ L"MAC address" });
                    macLabel.FontSize(12);
                    macLabel.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(macLabel, 0);
                    macGrid.Children().Append(macLabel);

                    Microsoft::UI::Xaml::Controls::CheckBox dynCheck;
                    dynCheck.Content(winrt::box_value(winrt::hstring{ L"Dynamic" }));
                    dynCheck.FontSize(12);
                    dynCheck.MinHeight(26);
                    dynCheck.IsChecked(nic.dynamicMac);
                    dynCheck.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(dynCheck, 1);
                    macGrid.Children().Append(dynCheck);

                    Microsoft::UI::Xaml::Controls::TextBox macBox;
                    macBox.FontSize(12);
                    macBox.MinHeight(28);
                    macBox.Text(winrt::hstring{ FormatMacForDialog(nic.macAddress) });
                    macBox.IsEnabled(!nic.dynamicMac);
                    macBox.PlaceholderText(winrt::hstring{ L"00:15:5D:..." });
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(macBox, 2);
                    macGrid.Children().Append(macBox);

                    // Dynamic toggles the static box's editability.
                    dynCheck.Click([macBox, dynCheck](
                        winrt::Windows::Foundation::IInspectable const&,
                        Microsoft::UI::Xaml::RoutedEventArgs const&) {
                        auto ic = dynCheck.IsChecked();
                        bool dyn = ic ? ic.Value() : false;
                        macBox.IsEnabled(!dyn);
                    });

                    // VLAN. A mode ComboBox (Untagged / Access / Trunk / Private)
                    // swaps the sub-fields below it. Access mirrors Hyper-V
                    // Manager's NIC VLAN UI; Trunk/Private are PowerShell-only
                    // there (hyprv surfaces all three). Live edit (no VM-Off gate).
                    // Trunk verified; Private untested (needs an external switch —
                    // the internal/Default switch rejects private VLANs).
                    auto mkVlanLbl = [](wchar_t const* t) {
                        Microsoft::UI::Xaml::Controls::TextBlock b;
                        b.Text(winrt::hstring{ t });
                        b.FontSize(12);
                        b.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                        return b;
                    };
                    auto mkVlanNum = [](uint16_t val, int minV) {
                        Microsoft::UI::Xaml::Controls::NumberBox n;
                        n.FontSize(12); n.MinHeight(28);
                        n.Minimum(minV); n.Maximum(4094);
                        n.SpinButtonPlacementMode(
                            Microsoft::UI::Xaml::Controls::NumberBoxSpinButtonPlacementMode::Hidden);
                        n.Width(90);
                        n.Value(val ? static_cast<double>(val) : static_cast<double>(minV));
                        return n;
                    };
                    auto mkVlanSub = []() {
                        Microsoft::UI::Xaml::Controls::StackPanel p;
                        p.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
                        p.Spacing(8);
                        p.Margin(Microsoft::UI::Xaml::ThicknessHelper::FromLengths(12, 0, 0, 0));
                        p.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                        return p;
                    };

                    Microsoft::UI::Xaml::Controls::StackPanel vlanGrid;
                    vlanGrid.Spacing(4);

                    // Mode row: "VLAN" label + mode combo.
                    Microsoft::UI::Xaml::Controls::Grid vlanModeRow;
                    Microsoft::UI::Xaml::Controls::ColumnDefinition vm0, vm1;
                    vm0.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                        220, Microsoft::UI::Xaml::GridUnitType::Pixel));
                    vm1.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                        1, Microsoft::UI::Xaml::GridUnitType::Star));
                    vlanModeRow.ColumnDefinitions().Append(vm0);
                    vlanModeRow.ColumnDefinitions().Append(vm1);
                    vlanModeRow.ColumnSpacing(8);
                    auto vlanModeLbl = mkVlanLbl(L"VLAN");
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(vlanModeLbl, 0);
                    vlanModeRow.Children().Append(vlanModeLbl);
                    Microsoft::UI::Xaml::Controls::ComboBox vlanModeCombo;
                    vlanModeCombo.FontSize(12); vlanModeCombo.MinHeight(28);
                    vlanModeCombo.Width(150);
                    vlanModeCombo.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Left);
                    for (auto* m : { L"Untagged", L"Access", L"Trunk", L"Private" })
                        vlanModeCombo.Items().Append(winrt::box_value(winrt::hstring{ m }));
                    vlanModeCombo.SelectedIndex(nic.vlanMode <= 3 ? static_cast<int>(nic.vlanMode) : 0);
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(vlanModeCombo, 1);
                    vlanModeRow.Children().Append(vlanModeCombo);
                    vlanGrid.Children().Append(vlanModeRow);

                    // Access sub-panel: VLAN ID.
                    auto accessPanel = mkVlanSub();
                    accessPanel.Children().Append(mkVlanLbl(L"VLAN ID"));
                    auto vlanBox = mkVlanNum(nic.vlanMode == 1 ? nic.vlanId : 0, 1);
                    accessPanel.Children().Append(vlanBox);
                    vlanGrid.Children().Append(accessPanel);

                    // Trunk sub-panel: Native VLAN + Allowed list.
                    auto trunkPanel = mkVlanSub();
                    trunkPanel.Children().Append(mkVlanLbl(L"Native"));
                    auto nativeVlanBox = mkVlanNum(nic.nativeVlanId, 0);
                    trunkPanel.Children().Append(nativeVlanBox);
                    trunkPanel.Children().Append(mkVlanLbl(L"Allowed"));
                    Microsoft::UI::Xaml::Controls::TextBox trunkListBox;
                    trunkListBox.FontSize(12); trunkListBox.MinHeight(28);
                    trunkListBox.Width(180);
                    trunkListBox.PlaceholderText(winrt::hstring{ L"e.g. 10,20,30 or 1-50" });
                    trunkListBox.Text(winrt::hstring{ FormatVlanList(nic.trunkVlanList) });
                    trunkPanel.Children().Append(trunkListBox);
                    vlanGrid.Children().Append(trunkPanel);

                    // Private sub-panel: Primary + Secondary + role.
                    auto privatePanel = mkVlanSub();
                    privatePanel.Children().Append(mkVlanLbl(L"Primary"));
                    auto primaryVlanBox = mkVlanNum(nic.primaryVlanId, 0);
                    privatePanel.Children().Append(primaryVlanBox);
                    privatePanel.Children().Append(mkVlanLbl(L"Secondary"));
                    auto secondaryVlanBox = mkVlanNum(nic.secondaryVlanId, 0);
                    privatePanel.Children().Append(secondaryVlanBox);
                    Microsoft::UI::Xaml::Controls::ComboBox pvlanRoleCombo;
                    pvlanRoleCombo.FontSize(12); pvlanRoleCombo.MinHeight(28);
                    pvlanRoleCombo.Width(140);
                    for (auto* r : { L"Isolated", L"Community", L"Promiscuous" })
                        pvlanRoleCombo.Items().Append(winrt::box_value(winrt::hstring{ r }));
                    pvlanRoleCombo.SelectedIndex(nic.pvlanMode >= 1 && nic.pvlanMode <= 3
                        ? static_cast<int>(nic.pvlanMode - 1) : 0);
                    privatePanel.Children().Append(pvlanRoleCombo);
                    vlanGrid.Children().Append(privatePanel);

                    // Show only the active sub-panel; the combo swaps them.
                    auto applyVlanMode = [accessPanel, trunkPanel, privatePanel](int idx) {
                        using V = Microsoft::UI::Xaml::Visibility;
                        accessPanel.Visibility(idx == 1 ? V::Visible : V::Collapsed);
                        trunkPanel.Visibility(idx == 2 ? V::Visible : V::Collapsed);
                        privatePanel.Visibility(idx == 3 ? V::Visible : V::Collapsed);
                    };
                    applyVlanMode(vlanModeCombo.SelectedIndex());
                    vlanModeCombo.SelectionChanged([applyVlanMode, vlanModeCombo](
                        winrt::Windows::Foundation::IInspectable const&,
                        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
                        applyVlanMode(vlanModeCombo.SelectedIndex());
                    });

                    // Advanced features (Msvm_EthernetSwitchPortSecuritySettingData
                    // + ClusterMonitored): [Advanced label][5 toggle checkboxes
                    // wrapped 3/2]. Placed at the bottom of the card's options.
                    Microsoft::UI::Xaml::Controls::Grid advGrid;
                    {
                        Microsoft::UI::Xaml::Controls::ColumnDefinition a0, a1;
                        a0.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                            220, Microsoft::UI::Xaml::GridUnitType::Pixel));
                        a1.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                            1, Microsoft::UI::Xaml::GridUnitType::Star));
                        advGrid.ColumnDefinitions().Append(a0);
                        advGrid.ColumnDefinitions().Append(a1);
                        advGrid.ColumnSpacing(8);
                    }
                    Microsoft::UI::Xaml::Controls::TextBlock advLabel;
                    advLabel.Text(winrt::hstring{ L"Advanced" });
                    advLabel.FontSize(12);
                    // Top-aligned so it lines up with the first checkbox row
                    // (the value column is now two rows tall).
                    advLabel.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Top);
                    advLabel.Margin(Microsoft::UI::Xaml::ThicknessHelper::FromLengths(0, 4, 0, 0));
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(advLabel, 0);
                    advGrid.Children().Append(advLabel);

                    auto mkAdvCheck = [](wchar_t const* text, bool val) {
                        Microsoft::UI::Xaml::Controls::CheckBox cb;
                        cb.Content(winrt::box_value(winrt::hstring{ text }));
                        cb.FontSize(12);
                        cb.MinHeight(26);
                        cb.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                        cb.IsChecked(val);
                        return cb;
                    };
                    auto spoofCheck       = mkAdvCheck(L"MAC spoofing", nic.macSpoofing);
                    auto dhcpGuardCheck   = mkAdvCheck(L"DHCP guard",   nic.dhcpGuard);
                    auto routerGuardCheck = mkAdvCheck(L"Router guard", nic.routerGuard);
                    auto teamingCheck     = mkAdvCheck(L"NIC teaming",  nic.nicTeaming);
                    auto protectedCheck   = mkAdvCheck(L"Protected network", nic.clusterMonitored);
                    auto deviceNamingCheck = mkAdvCheck(L"Device naming", nic.deviceNaming);
                    auto ieeePriorityCheck = mkAdvCheck(L"IEEE priority tag", nic.ieeePriorityTag);
                    // Generic horizontal row helper (still used by the hardware-
                    // acceleration section below).
                    auto mkAdvRow = []() {
                        Microsoft::UI::Xaml::Controls::StackPanel sp;
                        sp.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
                        sp.Spacing(12);
                        return sp;
                    };
                    // Lay the 7 advanced checkboxes into a 3-row × 3-col Auto
                    // grid so the columns line up vertically. (A StackPanel per
                    // row sizes each checkbox to its own text, leaving the rows
                    // misaligned — "Device naming" didn't sit under "Router
                    // guard".)
                    Microsoft::UI::Xaml::Controls::Grid advChecks;
                    advChecks.ColumnSpacing(16);
                    advChecks.RowSpacing(4);
                    for (int ci = 0; ci < 3; ++ci)
                    {
                        Microsoft::UI::Xaml::Controls::ColumnDefinition cd;
                        cd.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                            0, Microsoft::UI::Xaml::GridUnitType::Auto));
                        advChecks.ColumnDefinitions().Append(cd);
                    }
                    for (int ri = 0; ri < 3; ++ri)
                    {
                        Microsoft::UI::Xaml::Controls::RowDefinition rd;
                        rd.Height(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                            0, Microsoft::UI::Xaml::GridUnitType::Auto));
                        advChecks.RowDefinitions().Append(rd);
                    }
                    auto placeAdv = [&advChecks](
                        Microsoft::UI::Xaml::Controls::CheckBox const& cb, int r, int c) {
                        Microsoft::UI::Xaml::Controls::Grid::SetRow(cb, r);
                        Microsoft::UI::Xaml::Controls::Grid::SetColumn(cb, c);
                        advChecks.Children().Append(cb);
                    };
                    placeAdv(spoofCheck,        0, 0);
                    placeAdv(dhcpGuardCheck,    0, 1);
                    placeAdv(routerGuardCheck,  0, 2);
                    placeAdv(teamingCheck,      1, 0);
                    placeAdv(protectedCheck,    1, 1);
                    placeAdv(deviceNamingCheck, 1, 2);
                    placeAdv(ieeePriorityCheck, 2, 0);
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(advChecks, 1);
                    advGrid.Children().Append(advChecks);

                    Microsoft::UI::Xaml::Controls::Grid pmGrid;
                    {
                        Microsoft::UI::Xaml::Controls::ColumnDefinition p0, p1;
                        p0.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                            220, Microsoft::UI::Xaml::GridUnitType::Pixel));
                        p1.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                            1, Microsoft::UI::Xaml::GridUnitType::Star));
                        pmGrid.ColumnDefinitions().Append(p0);
                        pmGrid.ColumnDefinitions().Append(p1);
                        pmGrid.ColumnSpacing(8);
                    }
                    Microsoft::UI::Xaml::Controls::TextBlock pmLabel;
                    pmLabel.Text(winrt::hstring{ L"Port mirroring" });
                    pmLabel.FontSize(12);
                    pmLabel.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(pmLabel, 0);
                    pmGrid.Children().Append(pmLabel);
                    Microsoft::UI::Xaml::Controls::ComboBox portMirrorCombo;
                    portMirrorCombo.FontSize(12);
                    portMirrorCombo.MinHeight(28);
                    portMirrorCombo.Width(180);
                    portMirrorCombo.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Left);
                    // Index == MonitorMode value: 0=None, 1=Destination, 2=Source.
                    portMirrorCombo.Items().Append(box_value(winrt::hstring{ L"None" }));
                    portMirrorCombo.Items().Append(box_value(winrt::hstring{ L"Destination" }));
                    portMirrorCombo.Items().Append(box_value(winrt::hstring{ L"Source" }));
                    portMirrorCombo.SelectedIndex(nic.portMirroring <= 2 ? nic.portMirroring : 0);
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(portMirrorCombo, 1);
                    pmGrid.Children().Append(portMirrorCombo);

                    // Bandwidth (Mbps) — [label][Min box][Max box]. 0 = none.
                    // Stored bits/sec; display rounds to whole Mbps so a
                    // no-touch Save is a no-op even if the stored value wasn't
                    // Mbps-aligned.
                    const uint32_t origBwMaxMbps = static_cast<uint32_t>(
                        (nic.bandwidthMaxBps + 500000ull) / 1000000ull);
                    const uint32_t origBwMinMbps = static_cast<uint32_t>(
                        (nic.bandwidthMinBps + 500000ull) / 1000000ull);
                    Microsoft::UI::Xaml::Controls::Grid bwGrid;
                    {
                        Microsoft::UI::Xaml::Controls::ColumnDefinition b0, b1;
                        b0.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                            220, Microsoft::UI::Xaml::GridUnitType::Pixel));
                        b1.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                            1, Microsoft::UI::Xaml::GridUnitType::Star));
                        bwGrid.ColumnDefinitions().Append(b0);
                        bwGrid.ColumnDefinitions().Append(b1);
                        bwGrid.ColumnSpacing(8);
                    }
                    Microsoft::UI::Xaml::Controls::TextBlock bwLabel;
                    bwLabel.Text(winrt::hstring{ L"Bandwidth (Mbps)" });
                    bwLabel.FontSize(12);
                    bwLabel.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(bwLabel, 0);
                    bwGrid.Children().Append(bwLabel);

                    Microsoft::UI::Xaml::Controls::StackPanel bwPanel;
                    bwPanel.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
                    bwPanel.Spacing(8);
                    bwPanel.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                    auto mkBwBox = [](double val) {
                        Microsoft::UI::Xaml::Controls::NumberBox nb;
                        nb.FontSize(12);
                        nb.MinHeight(28);
                        nb.Minimum(0);
                        nb.Maximum(1000000);   // 1 Tbps headroom; 0 = unlimited
                        nb.SpinButtonPlacementMode(
                            Microsoft::UI::Xaml::Controls::NumberBoxSpinButtonPlacementMode::Hidden);
                        nb.Width(96);
                        // 0 = unlimited / none → leave the box empty (default
                        // Value is NaN = blank) rather than showing a literal 0.
                        if (val > 0.0) nb.Value(val);
                        return nb;
                    };
                    Microsoft::UI::Xaml::Controls::TextBlock minLbl;
                    minLbl.Text(winrt::hstring{ L"Min" });
                    minLbl.FontSize(12);
                    minLbl.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                    auto bwMinBox = mkBwBox(static_cast<double>(origBwMinMbps));
                    Microsoft::UI::Xaml::Controls::TextBlock maxLbl;
                    maxLbl.Text(winrt::hstring{ L"Max" });
                    maxLbl.FontSize(12);
                    maxLbl.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                    auto bwMaxBox = mkBwBox(static_cast<double>(origBwMaxMbps));
                    bwPanel.Children().Append(minLbl);
                    bwPanel.Children().Append(bwMinBox);
                    bwPanel.Children().Append(maxLbl);
                    bwPanel.Children().Append(bwMaxBox);
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(bwPanel, 1);
                    bwGrid.Children().Append(bwPanel);

                    // Hardware acceleration — VMQ / SR-IOV / IPsec task
                    // offloading. [label][VMQ + SR-IOV] / [IPsec + Max SA].
                    Microsoft::UI::Xaml::Controls::Grid haGrid;
                    {
                        Microsoft::UI::Xaml::Controls::ColumnDefinition h0, h1;
                        h0.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                            220, Microsoft::UI::Xaml::GridUnitType::Pixel));
                        h1.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                            1, Microsoft::UI::Xaml::GridUnitType::Star));
                        haGrid.ColumnDefinitions().Append(h0);
                        haGrid.ColumnDefinitions().Append(h1);
                        haGrid.ColumnSpacing(8);
                    }
                    Microsoft::UI::Xaml::Controls::TextBlock haLabel;
                    haLabel.Text(winrt::hstring{ L"Hardware acceleration" });
                    haLabel.FontSize(12);
                    haLabel.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Top);
                    haLabel.Margin(Microsoft::UI::Xaml::ThicknessHelper::FromLengths(0, 4, 0, 0));
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(haLabel, 0);
                    haGrid.Children().Append(haLabel);

                    auto vmqCheck   = mkAdvCheck(L"Virtual machine queue (VMQ)", nic.vmqEnabled);
                    auto sriovCheck = mkAdvCheck(L"SR-IOV", nic.sriovEnabled);
                    auto ipsecCheck = mkAdvCheck(L"IPsec task offloading", nic.ipsecOffload);
                    Microsoft::UI::Xaml::Controls::NumberBox ipsecMaxBox;
                    ipsecMaxBox.FontSize(12);
                    ipsecMaxBox.MinHeight(28);
                    ipsecMaxBox.Minimum(1);
                    ipsecMaxBox.Maximum(4096);
                    ipsecMaxBox.SpinButtonPlacementMode(
                        Microsoft::UI::Xaml::Controls::NumberBoxSpinButtonPlacementMode::Hidden);
                    ipsecMaxBox.Width(80);
                    ipsecMaxBox.Value(static_cast<double>(nic.ipsecOffloadMaxSA));
                    ipsecMaxBox.IsEnabled(nic.ipsecOffload);
                    // Max-SA box is only meaningful while IPsec offload is on.
                    ipsecCheck.Click([ipsecCheck, ipsecMaxBox](
                        winrt::Windows::Foundation::IInspectable const&,
                        Microsoft::UI::Xaml::RoutedEventArgs const&) {
                        auto ic = ipsecCheck.IsChecked();
                        ipsecMaxBox.IsEnabled(ic ? ic.Value() : false);
                    });

                    auto haRow1 = mkAdvRow();
                    haRow1.Children().Append(vmqCheck);
                    haRow1.Children().Append(sriovCheck);
                    Microsoft::UI::Xaml::Controls::StackPanel haRow2;
                    haRow2.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
                    haRow2.Spacing(8);
                    haRow2.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                    haRow2.Children().Append(ipsecCheck);
                    Microsoft::UI::Xaml::Controls::TextBlock saLbl;
                    saLbl.Text(winrt::hstring{ L"Max SA" });
                    saLbl.FontSize(12);
                    saLbl.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                    haRow2.Children().Append(saLbl);
                    haRow2.Children().Append(ipsecMaxBox);
                    Microsoft::UI::Xaml::Controls::StackPanel haPanel;
                    haPanel.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Vertical);
                    haPanel.Spacing(4);
                    haPanel.Children().Append(haRow1);
                    haPanel.Children().Append(haRow2);
                    Microsoft::UI::Xaml::Controls::Grid::SetColumn(haPanel, 1);
                    haGrid.Children().Append(haPanel);

                    // Per-card "Remove this adapter" — gated to VM Off in
                    // ApplyStateGating. Checked rows are removed at Save.
                    Microsoft::UI::Xaml::Controls::CheckBox removeCheck;
                    removeCheck.Content(winrt::box_value(winrt::hstring{ L"Remove this adapter" }));
                    removeCheck.FontSize(12);
                    removeCheck.MinHeight(26);
                    removeCheck.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);

                    // Stack the card vertically. Only the switch (the common
                    // edit) shows by default; everything else (MAC / VLAN / port
                    // mirror / bandwidth / hardware acceleration / advanced
                    // flags) is tucked behind a collapsed "Advanced settings"
                    // expander so a VM with several NICs stays scannable. Remove
                    // sits last, outside the expander.
                    Microsoft::UI::Xaml::Controls::StackPanel cardPanel;
                    cardPanel.Spacing(6);
                    cardPanel.Children().Append(grid);
                    // Collapsed-card summary: show ONLY non-default state dimmed
                    // beside "Settings" — a VLAN tag/mode, and a static MAC. A
                    // default NIC (untagged + dynamic MAC) gets no summary at all,
                    // so the chip isn't cluttered with the defaults.
                    std::wstring nicSummary;
                    switch (nic.vlanMode)
                    {
                    case 1:  nicSummary = L"VLAN " + std::to_wstring(nic.vlanId); break;
                    case 2:  nicSummary = L"Trunk"; break;
                    case 3:  nicSummary = L"Private"; break;
                    default: break;   // untagged is the default — no label
                    }
                    if (!nic.dynamicMac)   // static MAC is a deliberate, non-default choice
                    {
                        if (!nicSummary.empty()) nicSummary += L" · ";
                        nicSummary += L"Static MAC";
                    }
                    auto advInner = MakeAdvancedExpander(cardPanel, L"Settings", nicSummary);
                    advInner.Children().Append(macGrid);
                    advInner.Children().Append(vlanGrid);
                    advInner.Children().Append(pmGrid);
                    advInner.Children().Append(bwGrid);
                    advInner.Children().Append(haGrid);
                    advInner.Children().Append(advGrid);
                    cardPanel.Children().Append(removeCheck);

                    Microsoft::UI::Xaml::Controls::Border card;
                    // "BorderedCard" Style provides CornerRadius / Padding
                    // / Background / BorderBrush / BorderThickness — all
                    // theme-aware via ThemeResource Setters.
                    if (auto st = Microsoft::UI::Xaml::Application::Current()
                            .Resources().TryLookup(winrt::box_value(
                                winrt::hstring{ L"BorderedCard" })))
                        card.Style(st.try_as<Microsoft::UI::Xaml::Style>());
                    card.Child(cardPanel);
                    host.Children().Append(card);

                    NetworkAdapterRow row;
                    row.nicGuid        = nic.nicGuid;
                    row.switchCombo    = combo;
                    row.origDynamicMac = nic.dynamicMac;
                    row.origMac        = nic.macAddress;
                    row.dynamicCheck   = dynCheck;
                    row.macBox         = macBox;
                    row.origVlanMode        = nic.vlanMode;
                    row.origVlanId          = nic.vlanId;
                    row.origNativeVlanId    = nic.nativeVlanId;
                    row.origTrunkList       = nic.trunkVlanList;
                    row.origPrimaryVlanId   = nic.primaryVlanId;
                    row.origSecondaryVlanId = nic.secondaryVlanId;
                    row.origPvlanMode       = nic.pvlanMode;
                    row.vlanModeCombo       = vlanModeCombo;
                    row.vlanBox             = vlanBox;
                    row.nativeVlanBox       = nativeVlanBox;
                    row.trunkListBox        = trunkListBox;
                    row.primaryVlanBox      = primaryVlanBox;
                    row.secondaryVlanBox    = secondaryVlanBox;
                    row.pvlanRoleCombo      = pvlanRoleCombo;
                    row.removeCheck    = removeCheck;
                    row.origAdvanced.macSpoofing   = nic.macSpoofing;
                    row.origAdvanced.dhcpGuard     = nic.dhcpGuard;
                    row.origAdvanced.routerGuard   = nic.routerGuard;
                    row.origAdvanced.nicTeaming    = nic.nicTeaming;
                    row.origAdvanced.portMirroring = nic.portMirroring;
                    row.origAdvanced.ieeePriorityTag = nic.ieeePriorityTag;
                    row.spoofCheck       = spoofCheck;
                    row.dhcpGuardCheck   = dhcpGuardCheck;
                    row.routerGuardCheck = routerGuardCheck;
                    row.teamingCheck     = teamingCheck;
                    row.ieeePriorityCheck = ieeePriorityCheck;
                    row.portMirrorCombo  = portMirrorCombo;
                    row.origClusterMonitored = nic.clusterMonitored;
                    row.protectedCheck   = protectedCheck;
                    row.origBwMaxMbps    = origBwMaxMbps;
                    row.origBwMinMbps    = origBwMinMbps;
                    row.bwMaxBox         = bwMaxBox;
                    row.bwMinBox         = bwMinBox;
                    row.origDeviceNaming = nic.deviceNaming;
                    row.deviceNamingCheck = deviceNamingCheck;
                    row.origOffload.vmq          = nic.vmqEnabled;
                    row.origOffload.sriov        = nic.sriovEnabled;
                    row.origOffload.ipsecOffload = nic.ipsecOffload;
                    row.origOffload.ipsecOffloadMaxSA = nic.ipsecOffloadMaxSA;
                    row.vmqCheck         = vmqCheck;
                    row.sriovCheck       = sriovCheck;
                    row.ipsecCheck       = ipsecCheck;
                    row.ipsecMaxBox      = ipsecMaxBox;
                    m_networkRows.push_back(std::move(row));
                    // Original = connected switch name, or "" if disconnected.
                    m_origAdapterSwitches.emplace_back(
                        nic.nicGuid,
                        (nic.connected ? nic.switchName : std::wstring{}));
                }
            }
        }

        // Storage — clear the queued-op state (fresh on each dialog open) +
        // queued cards, then build the controllers list + hard-disk / DVD
        // device cards from current VM state. RebuildStorageCards re-reads WMI
        // and is also called after an immediate Add/Remove SCSI controller, so
        // it must NOT touch the queued-op state below.
        m_pendingDvdAdds.clear();
        m_pendingAttachPaths.clear();
        m_pendingCreates.clear();
        m_pendingPhysAttaches.clear();
        m_queuedSlots.clear();
        if (auto ah = storageDvdAddHost()) ah.Children().Clear();
        if (auto ah = storageAttachHost()) ah.Children().Clear();
        RebuildStorageCards();

        // RDP section.
        //
        // Capture two pieces of "original" state:
        //   1. Whether a per-VM override exists (drives Set vs Clear at
        //      Save time).
        //   2. The effective options (override if set, else app defaults)
        //      — populated into the controls so opening the section
        //      always shows real values, never zeroed-out defaults.
        // Opening the dialog with the override toggle ON (use defaults)
        // is the typical case for VMs the user hasn't customized; OFF
        // means an override is in place. The toggle handler enables /
        // disables the rest of the section to match.
        auto& s = hyprv::app::settings::Settings::Instance();
        m_origRdpHasOverride = s.HasRdpOptionsOverride(m_vmGuid);
        m_origRdpOptions     = s.RdpOptionsFor(m_vmGuid);
        if (auto t = rdpUseDefaultsToggle())
            t.IsOn(!m_origRdpHasOverride);
        if (auto c = rdpAudioModeCombo())
            c.SelectedIndex(static_cast<int>(m_origRdpOptions.audioMode));
        if (auto c = rdpAudioCaptureCheck())
            c.IsChecked(m_origRdpOptions.audioCaptureRedirect);
        if (auto c = rdpClipboardCheck())
            c.IsChecked(m_origRdpOptions.redirectClipboard);
        if (auto c = rdpDrivesCheck())
            c.IsChecked(m_origRdpOptions.redirectDrives);
        if (auto c = rdpDevicesCheck())
            c.IsChecked(m_origRdpOptions.redirectDevices);
        if (auto c = rdpSmartCardsCheck())
            c.IsChecked(m_origRdpOptions.redirectSmartCards);
        if (auto c = rdpPortsCheck())
            c.IsChecked(m_origRdpOptions.redirectPorts);
        if (auto c = rdpColorDepthCombo())
        {
            int didx = 2;  // default to 32bpp (last item)
            if (m_origRdpOptions.colorDepth == 16) didx = 0;
            else if (m_origRdpOptions.colorDepth == 24) didx = 1;
            c.SelectedIndex(didx);
        }
        // Display scale — item 0 = Auto (0%), 1..5 = 100/125/150/175/200.
        if (auto c = rdpScaleCombo())
        {
            int sidx = 0;
            switch (m_origRdpOptions.dpiScaleOverridePercent)
            {
            case 100: sidx = 1; break;
            case 125: sidx = 2; break;
            case 150: sidx = 3; break;
            case 175: sidx = 4; break;
            case 200: sidx = 5; break;
            default:  sidx = 0; break;  // 0 / unknown → Auto
            }
            c.SelectedIndex(sidx);
        }
        // Initial size — preset list; unmatched (hand-edited) → 1024x768.
        if (auto c = rdpResolutionCombo())
            c.SelectedIndex(RdpResolutionIndex(
                m_origRdpOptions.initialDesktopWidth,
                m_origRdpOptions.initialDesktopHeight));
        // Sync the enabled/disabled state of the per-field controls to
        // match the toggle. Done last so all the SelectedIndex /
        // IsChecked sets above happen on a known-good state first.
        OnRdpUseDefaultsToggled({}, {});
    }

    void VmSettingsDialog::ApplyStateGating()
    {
        // Per-field rules (matches Hyper-V behavior):
        //   - Memory startup + dynamic toggle + min/max: VM must be Off
        //   - Memory weight: live
        //   - Processor count: VM must be Off
        //   - Processor reservation/limit/weight: live
        //   - Name: live
        //   - Notes: live
        //
        // STRICT Off only — Saved is NOT equivalent. Hyper-V *silently*
        // rejects processor/memory modifications on Saved VMs (the WMI
        // job completes with no error but no change applies). PowerShell
        // surfaces this as "Cannot change the processor settings... while
        // it is in a saved state." We disable the inputs to prevent the
        // confusing silent failure.
        const bool isOff = (m_vmState == hyprv::app::vm::VmState::Off);

        if (auto b = memStartupBox())   b.IsEnabled(isOff);
        if (auto t = memDynamicToggle()) t.IsEnabled(isOff);
        // Min/Max are live IF dynamic memory is already enabled; gating
        // here defers to "off-only" for simplicity (user enables dynamic
        // memory when the VM is off, then can tweak limits live).
        if (auto b = memMinBox())       b.IsEnabled(isOff);
        if (auto b = memMaxBox())       b.IsEnabled(isOff);

        if (auto b = cpuCountBox())     b.IsEnabled(isOff);
        // Compatibility + NUMA topology — all require the VM Off (verified).
        if (auto c = cpuCompatCheck())   c.IsEnabled(isOff);
        if (auto b = numaMaxProcsBox())  b.IsEnabled(isOff);
        if (auto b = numaMaxMemoryBox()) b.IsEnabled(isOff);
        if (auto b = numaMaxNodesBox())  b.IsEnabled(isOff);
        if (auto b = numaThreadsBox())   b.IsEnabled(isOff);
        if (auto b = numaUseHwTopologyButton()) b.IsEnabled(isOff);

        // Firmware/BIOS — all firmware-level config, VM must be Off. Boot
        // order exists for BOTH generations (Gen 1 BIOS codes / Gen 2 refs),
        // so it's editable whenever Off. Secure Boot is Gen 2 only. The
        // template combo additionally tracks the toggle (OnSecureBootToggled).
        const bool bootEditable = isOff;
        const bool secureBootEditable = m_isGen2 && isOff;
        if (auto t = secureBootToggle()) t.IsEnabled(secureBootEditable);
        if (auto lv = bootOrderList()) lv.IsEnabled(bootEditable);
        if (auto b = bootUpButton())   b.IsEnabled(bootEditable);
        if (auto b = bootDownButton()) b.IsEnabled(bootEditable);
        OnSecureBootToggled({}, {});

        // Security (vTPM + state encryption): Gen 2 only and Off only (Hyper-V
        // rejects security modifies on a running/saved VM). Nav item already
        // hidden for Gen 1; gate the toggles here.
        const bool secEditable = m_isGen2 && isOff;
        // vTPM + encryption are forced on + LOCKED while shielding is enabled
        // (a shielded VM requires both — letting the user turn them off leaves
        // an invalid state). Only the shield toggle itself is freely editable.
        const bool shieldedOn = shieldToggle() && shieldToggle().IsOn();
        if (auto t = shieldToggle())       t.IsEnabled(secEditable);
        if (auto t = tpmToggle())          t.IsEnabled(secEditable && !shieldedOn);
        if (auto t = encryptStateToggle()) t.IsEnabled(secEditable && !shieldedOn);

        // Automatic actions. Verified against live WMI: the START action is
        // modifiable in ANY VM state, but the STOP action is rejected while
        // the VM is running/saved — Hyper-V silently no-ops a combined
        // ModifySystemSettings that changes it (ret=4096, clean job, nothing
        // applied; the cmdlet surfaces it as 0x80041001). So leave the start
        // combo live and gate ONLY the stop combo to Off.
        if (auto c = autoStopCombo()) c.IsEnabled(isOff);

        // Hardware add/remove (NICs + DVD drives). Allowed when the VM is Off
        // (any generation) OR running Gen 2 — synthetic NIC and SCSI DVD-drive
        // hot-add both work there (verified against live WMI: add+remove on a
        // running Gen 2 VM succeeds with existing hardware untouched). The
        // Saved state is the one Hyper-V silently rejects, so it stays
        // excluded. Edits on existing devices (NIC switch/VLAN, DVD media
        // mount/eject) are live in every state regardless — EXCEPT a NIC's MAC
        // (a device property Hyper-V locks while running; gated below).
        const bool hwAddRemoveEditable =
            isOff || (m_isGen2 && m_vmState == hyprv::app::vm::VmState::Running);
        if (auto b = networkAddButton()) b.IsEnabled(hwAddRemoveEditable);
        for (auto const& row : m_networkRows)
        {
            if (row.removeCheck) row.removeCheck.IsEnabled(hwAddRemoveEditable);
            // MAC address is a DEVICE property Hyper-V locks while the VM runs
            // ("The MacAddress property cannot be modified because the virtual
            // machine is running" — verified live), so gate the dynamic/static
            // controls to Off, matching Hyper-V Manager. (Switch + VLAN are
            // connection-level settings and stay live in every state.)
            if (row.dynamicCheck) row.dynamicCheck.IsEnabled(isOff);
            if (row.macBox)
            {
                bool dyn = true;
                if (row.dynamicCheck)
                    if (auto ic = row.dynamicCheck.IsChecked()) dyn = ic.Value();
                row.macBox.IsEnabled(isOff && !dyn);
            }
        }
        if (auto b = storageDvdAddButton()) b.IsEnabled(hwAddRemoveEditable);
        for (auto const& row : m_dvdRows)
            if (row.removeCheck) row.removeCheck.IsEnabled(hwAddRemoveEditable);
        // Attach physical disk: Off only (the verified state; the host disk must
        // be offline anyway). Running hot-add wasn't verified, so stay strict.
        if (auto b = storagePhysAttachButton()) b.IsEnabled(isOff);

        // Add SCSI controller: Off only (Hyper-V rejects a running add —
        // verified) and capped at 4 SCSI controllers per VM.
        if (auto b = storageAddScsiButton())
        {
            int scsiCount = 0;
            for (auto const& c : m_controllers) if (c.isScsi) ++scsiCount;
            b.IsEnabled(isOff && scsiCount < 4);
        }
        // Per-controller Remove buttons (Off + empty + nothing queued on it).
        UpdateControllerRemoveButtons();

        // Greyed-out controls are the only affordance — no extra hint
        // text. The visual state is enough to communicate "you can't
        // change this right now".
    }

    void VmSettingsDialog::OnSecureBootToggled(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // The template only matters while Secure Boot is enabled, AND the
        // whole section is editable only for a Gen 2 VM that's Off. Combine
        // both: combo is live only when editable AND Secure Boot is on.
        const bool editable =
            m_isGen2 && (m_vmState == hyprv::app::vm::VmState::Off);
        const bool sbOn = secureBootToggle() ? secureBootToggle().IsOn() : false;
        if (auto c = secureBootTemplateCombo())
            c.IsEnabled(editable && sbOn);
    }

    void VmSettingsDialog::OnShieldingToggled(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Shielding requires vTPM + state encryption — reflect that live: turn
        // BOTH on when shielding is enabled and LOCK them on (they're mandatory
        // while shielded; turning vTPM off on a shielded VM leaves an invalid
        // shielded-but-no-TPM state). Mirrors ApplyStateGating's rule.
        const bool on = shieldToggle() && shieldToggle().IsOn();
        if (on)
        {
            if (auto t = tpmToggle())          t.IsOn(true);
            if (auto t = encryptStateToggle()) t.IsOn(true);
        }
        const bool secEditable =
            m_isGen2 && (m_vmState == hyprv::app::vm::VmState::Off);
        if (auto t = tpmToggle())          t.IsEnabled(secEditable && !on);
        if (auto t = encryptStateToggle()) t.IsEnabled(secEditable && !on);
    }

    void VmSettingsDialog::OnBootMoveUp(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto lv = bootOrderList();
        if (!lv) return;
        int idx = lv.SelectedIndex();
        if (idx <= 0) return;   // nothing selected, or already at top
        auto items = lv.Items();
        auto moved = items.GetAt(static_cast<uint32_t>(idx));
        items.RemoveAt(static_cast<uint32_t>(idx));
        items.InsertAt(static_cast<uint32_t>(idx - 1), moved);
        lv.SelectedIndex(idx - 1);   // keep selection on the moved entry
    }

    void VmSettingsDialog::OnBootMoveDown(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto lv = bootOrderList();
        if (!lv) return;
        int idx = lv.SelectedIndex();
        auto items = lv.Items();
        if (idx < 0 || idx >= static_cast<int>(items.Size()) - 1) return;
        auto moved = items.GetAt(static_cast<uint32_t>(idx));
        items.RemoveAt(static_cast<uint32_t>(idx));
        items.InsertAt(static_cast<uint32_t>(idx + 1), moved);
        lv.SelectedIndex(idx + 1);
    }

    // ---- Storage controllers + device placement --------------------------

    void VmSettingsDialog::RebuildStorageCards()
    {
        namespace MUXC = Microsoft::UI::Xaml::Controls;
        auto& vmm = hyprv::app::vm::VMManager::Instance();

        // Re-read controllers fresh (raw WMI; cheap, per dialog open / after an
        // immediate add/remove). usedSlots tell us which controllers are empty.
        m_controllers = vmm.GetStorageControllers(m_vmGuid);

        auto secondary = Microsoft::UI::Xaml::Application::Current()
            .Resources().TryLookup(winrt::box_value(winrt::hstring{
                L"TextFillColorSecondaryBrush" }))
            .try_as<Microsoft::UI::Xaml::Media::Brush>();
        auto cardStyle = Microsoft::UI::Xaml::Application::Current()
            .Resources().TryLookup(winrt::box_value(winrt::hstring{ L"BorderedCard" }))
            .try_as<Microsoft::UI::Xaml::Style>();

        // --- Controllers list (label + device count + per-SCSI Remove) ---
        m_controllerRemoveButtons.clear();
        if (auto host = storageControllerHost())
        {
            host.Children().Clear();
            for (auto const& c : m_controllers)
            {
                MUXC::Grid grid;
                grid.ColumnSpacing(8);
                MUXC::ColumnDefinition gc0, gc1;
                gc0.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                    1, Microsoft::UI::Xaml::GridUnitType::Star));
                gc1.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                    0, Microsoft::UI::Xaml::GridUnitType::Auto));
                grid.ColumnDefinitions().Append(gc0);
                grid.ColumnDefinitions().Append(gc1);

                MUXC::TextBlock label;
                std::wstring txt = c.label;
                if (!c.usedSlots.empty())
                    txt += L"  \x00b7  " + std::to_wstring(c.usedSlots.size())
                         + (c.usedSlots.size() == 1 ? L" device" : L" devices");
                label.Text(winrt::hstring{ txt });
                label.FontSize(12);
                label.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                MUXC::Grid::SetColumn(label, 0);
                grid.Children().Append(label);

                // Only SCSI controllers can be removed (IDE is fixed hardware).
                if (c.isScsi)
                {
                    MUXC::Button rm;
                    rm.Content(winrt::box_value(winrt::hstring{ L"Remove" }));
                    rm.FontSize(12);
                    MUXC::Grid::SetColumn(rm, 1);
                    auto weak = get_weak();
                    std::wstring ref = c.ref;
                    rm.Click([weak, ref](winrt::Windows::Foundation::IInspectable const&,
                                         Microsoft::UI::Xaml::RoutedEventArgs const&) {
                        if (auto self = weak.get())
                            self->RemoveScsiControllerByRef(ref);
                    });
                    grid.Children().Append(rm);
                    m_controllerRemoveButtons.emplace_back(c.ref, rm);
                }

                MUXC::Border card;
                if (cardStyle) card.Style(cardStyle);
                card.Child(grid);
                host.Children().Append(card);
            }
        }

        // --- Hard-disk cards (path / controller · slot / size + Detach) ---
        m_hddRows.clear();
        if (auto host = storageHddHost())
        {
            host.Children().Clear();
            auto disks = vmm.GetHardDisks(m_vmGuid);
            for (auto const& disk : disks)
            {
                MUXC::StackPanel panel;
                panel.Spacing(2);

                MUXC::TextBlock tb;
                std::wstring txt = disk.path;
                std::wstring meta = disk.controller.empty()
                    ? std::wstring{}
                    : disk.controller + L" \x00b7 Slot " + std::to_wstring(disk.slot);
                auto sz = FormatDiskSize(disk.fileSizeBytes);
                if (!sz.empty()) meta += (meta.empty() ? L"" : L"   ") + sz;
                // Mark a pass-through so it's distinguishable from a VHD file.
                if (disk.isPassthrough)
                    meta += (meta.empty() ? L"" : L"   ") + std::wstring{ L"Physical disk (pass-through)" };
                if (!meta.empty()) txt += L"\n" + meta;
                tb.Text(winrt::hstring{ txt });
                tb.FontSize(12);
                tb.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
                panel.Children().Append(tb);

                // "Detach" is built here but appended LAST (bottom of the card),
                // mirroring the NIC card's "Remove this adapter" placement —
                // device-specific options sit above, the remove control below.
                MUXC::CheckBox detach;
                detach.Content(winrt::box_value(winrt::hstring{ L"Detach" }));
                detach.FontSize(12);
                detach.MinHeight(26);
                detach.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);

                // QoS (Min/Max IOPS) — 0 = none/unlimited (shown blank).
                // Normalized 8 KB IOPS, written verbatim. Hot-settable, so no
                // state gate. Tucked behind a collapsed "Quality of Service"
                // expander; NOT shown for a pass-through disk (QoS lives on the
                // VHD SASD, which a pass-through doesn't have).
                MUXC::NumberBox iopsMinBox{ nullptr };
                MUXC::NumberBox iopsMaxBox{ nullptr };
                if (!disk.isPassthrough)
                {
                    MUXC::StackPanel qosPanel;
                    qosPanel.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
                    qosPanel.Spacing(8);
                    qosPanel.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                    // Label the row — the expander header is the generic
                    // "Settings", so the QoS context lives here inside.
                    MUXC::TextBlock qosLabel;
                    qosLabel.Text(winrt::hstring{ L"QoS (IOPS)" });
                    qosLabel.FontSize(12);
                    qosLabel.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                    auto mkQosBox = [](double val) {
                        MUXC::NumberBox nb;
                        nb.FontSize(12);
                        nb.MinHeight(28);
                        nb.Minimum(0);
                        nb.Maximum(1000000000);   // 1e9 headroom; 0 = none/unlimited
                        nb.SpinButtonPlacementMode(
                            Microsoft::UI::Xaml::Controls::NumberBoxSpinButtonPlacementMode::Hidden);
                        nb.Width(110);
                        if (val > 0.0) nb.Value(val);   // 0 → leave blank (NaN)
                        return nb;
                    };
                    MUXC::TextBlock qosMinLbl;
                    qosMinLbl.Text(winrt::hstring{ L"Min" });
                    qosMinLbl.FontSize(12);
                    qosMinLbl.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                    iopsMinBox = mkQosBox(static_cast<double>(disk.iopsMin));
                    MUXC::TextBlock qosMaxLbl;
                    qosMaxLbl.Text(winrt::hstring{ L"Max" });
                    qosMaxLbl.FontSize(12);
                    qosMaxLbl.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                    iopsMaxBox = mkQosBox(static_cast<double>(disk.iopsMax));
                    qosPanel.Children().Append(qosLabel);
                    qosPanel.Children().Append(qosMinLbl);
                    qosPanel.Children().Append(iopsMinBox);
                    qosPanel.Children().Append(qosMaxLbl);
                    qosPanel.Children().Append(iopsMaxBox);
                    // Collapsed-card summary: show the QoS limits dimmed beside
                    // "Settings" only when set (non-default), mirroring the NIC card.
                    std::wstring diskSummary;
                    if (disk.iopsMin > 0 && disk.iopsMax > 0)
                        diskSummary = L"QoS " + std::to_wstring(disk.iopsMin)
                                    + L"-" + std::to_wstring(disk.iopsMax) + L" IOPS";
                    else if (disk.iopsMin > 0)
                        diskSummary = L"QoS min " + std::to_wstring(disk.iopsMin) + L" IOPS";
                    else if (disk.iopsMax > 0)
                        diskSummary = L"QoS max " + std::to_wstring(disk.iopsMax) + L" IOPS";
                    MakeAdvancedExpander(panel, L"Settings", diskSummary)
                        .Children().Append(qosPanel);
                }

                panel.Children().Append(detach);   // remove control last (bottom)

                MUXC::Border card;
                if (cardStyle) card.Style(cardStyle);
                card.Child(panel);
                host.Children().Append(card);

                HardDiskRow row;
                row.vhdRef      = disk.vhdRef;
                row.driveRef    = disk.driveRef;
                row.detachCheck = detach;
                row.iopsMinBox  = iopsMinBox;
                row.iopsMaxBox  = iopsMaxBox;
                row.origIopsMin = disk.iopsMin;
                row.origIopsMax = disk.iopsMax;
                m_hddRows.push_back(std::move(row));
            }
            if (auto t = storageHddEmptyText())
                t.Visibility(disks.empty()
                    ? Microsoft::UI::Xaml::Visibility::Visible
                    : Microsoft::UI::Xaml::Visibility::Collapsed);
        }

        // --- DVD-drive cards (label / controller · slot / path + Browse/Eject) ---
        m_dvdRows.clear();
        if (auto host = storageDvdHost())
        {
            host.Children().Clear();
            auto drives = vmm.GetDvdDrives(m_vmGuid);
            for (auto const& drv : drives)
            {
                MUXC::StackPanel panel;
                panel.Spacing(4);

                MUXC::TextBlock label;
                label.Text(winrt::hstring{ drv.label });
                label.FontSize(12);
                panel.Children().Append(label);

                if (!drv.controller.empty())
                {
                    MUXC::TextBlock loc;
                    loc.Text(winrt::hstring{
                        drv.controller + L" \x00b7 Slot " + std::to_wstring(drv.slot) });
                    loc.FontSize(11);
                    if (secondary) loc.Foreground(secondary);
                    panel.Children().Append(loc);
                }

                MUXC::Grid grid;
                grid.ColumnSpacing(6);
                MUXC::ColumnDefinition c0, c1, c2;
                c0.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                    1, Microsoft::UI::Xaml::GridUnitType::Star));
                c1.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                    0, Microsoft::UI::Xaml::GridUnitType::Auto));
                c2.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
                    0, Microsoft::UI::Xaml::GridUnitType::Auto));
                grid.ColumnDefinitions().Append(c0);
                grid.ColumnDefinitions().Append(c1);
                grid.ColumnDefinitions().Append(c2);

                MUXC::TextBox pathBox;
                pathBox.Text(winrt::hstring{ drv.mediaPath });
                pathBox.FontSize(12);
                pathBox.MinHeight(28);
                pathBox.PlaceholderText(winrt::hstring{ L"(empty) — Browse for an ISO" });
                MUXC::Grid::SetColumn(pathBox, 0);
                grid.Children().Append(pathBox);

                MUXC::Button browse;
                browse.Content(winrt::box_value(winrt::hstring{ L"Browse..." }));
                browse.FontSize(12);
                MUXC::Grid::SetColumn(browse, 1);
                browse.Click([pathBox](winrt::Windows::Foundation::IInspectable const&,
                                       Microsoft::UI::Xaml::RoutedEventArgs const&) {
                    if (auto picked = PickIsoFile())
                        pathBox.Text(winrt::hstring{ *picked });
                });
                grid.Children().Append(browse);

                MUXC::Button eject;
                eject.Content(winrt::box_value(winrt::hstring{ L"Eject" }));
                eject.FontSize(12);
                MUXC::Grid::SetColumn(eject, 2);
                eject.Click([pathBox](winrt::Windows::Foundation::IInspectable const&,
                                      Microsoft::UI::Xaml::RoutedEventArgs const&) {
                    pathBox.Text(winrt::hstring{ L"" });
                });
                grid.Children().Append(eject);

                panel.Children().Append(grid);

                MUXC::CheckBox removeCheck;
                removeCheck.Content(winrt::box_value(winrt::hstring{ L"Remove this drive" }));
                removeCheck.FontSize(12);
                removeCheck.MinHeight(26);
                removeCheck.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                removeCheck.Click([pathBox, browse, eject, removeCheck](
                    winrt::Windows::Foundation::IInspectable const&,
                    Microsoft::UI::Xaml::RoutedEventArgs const&) {
                    auto ic = removeCheck.IsChecked();
                    bool rm = ic ? ic.Value() : false;
                    pathBox.IsEnabled(!rm);
                    browse.IsEnabled(!rm);
                    eject.IsEnabled(!rm);
                });
                panel.Children().Append(removeCheck);

                MUXC::Border card;
                if (cardStyle) card.Style(cardStyle);
                card.Child(panel);
                host.Children().Append(card);

                DvdDriveRow row;
                row.driveRef      = drv.driveRef;
                row.mediaRef      = drv.mediaRef;
                row.origMediaPath = drv.mediaPath;
                row.pathBox       = pathBox;
                row.removeCheck   = removeCheck;
                m_dvdRows.push_back(std::move(row));
            }
            if (auto t = storageDvdEmptyText())
                t.Visibility(drives.empty()
                    ? Microsoft::UI::Xaml::Visibility::Visible
                    : Microsoft::UI::Xaml::Visibility::Collapsed);
        }

        // Re-gate the add button + the new Remove buttons for the current state.
        ApplyStateGating();
    }

    void VmSettingsDialog::UpdateControllerRemoveButtons()
    {
        const bool isOff = (m_vmState == hyprv::app::vm::VmState::Off);
        for (auto& [ref, btn] : m_controllerRemoveButtons)
        {
            if (!btn) continue;
            bool empty = true;
            for (auto const& c : m_controllers)
                if (c.ref == ref) { empty = c.usedSlots.empty(); break; }
            auto qit = m_queuedSlots.find(ref);
            bool noQueued = (qit == m_queuedSlots.end() || qit->second.empty());
            // Only empty (no VM device, no queued device) SCSI controllers,
            // VM Off.
            btn.IsEnabled(isOff && empty && noQueued);
        }
    }

    winrt::fire_and_forget VmSettingsDialog::RemoveScsiControllerByRef(std::wstring ref)
    {
        auto strong = get_strong();
        std::wstring vmGuid = m_vmGuid;
        if (vmGuid.empty() || ref.empty()) co_return;
        auto dq = DispatcherQueue();

        if (auto ov = savingOverlay())
            ov.Visibility(Microsoft::UI::Xaml::Visibility::Visible);
        co_await winrt::resume_after(std::chrono::milliseconds(150));
        co_await ResumeOnDispatcher{ dq };

        bool ok = hyprv::app::vm::VMManager::Instance().RemoveScsiController(vmGuid, ref);
        if (ok)
        {
            m_queuedSlots.erase(ref);   // nothing can be queued on a gone ctrl
            RebuildStorageCards();      // re-reads controllers + renumbers
        }
        else
        {
            ShowError(L"Failed to remove the SCSI controller. It can only be "
                      L"removed while the VM is off and after detaching its "
                      L"devices.");
        }

        if (auto ov = savingOverlay())
            ov.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
    }

    void VmSettingsDialog::PopulatePlacement(
        Microsoft::UI::Xaml::Controls::ComboBox const& controllerCombo,
        Microsoft::UI::Xaml::Controls::ComboBox const& slotCombo)
    {
        if (!controllerCombo) return;
        // Link the slot combo to the controller combo so the shared
        // SelectionChanged handler can find its sibling.
        controllerCombo.Tag(slotCombo);
        controllerCombo.Items().Clear();
        for (auto const& c : m_controllers)
        {
            Microsoft::UI::Xaml::Controls::ComboBoxItem item;
            item.Content(winrt::box_value(winrt::hstring{ c.label }));
            item.Tag(winrt::box_value(winrt::hstring{ c.ref }));
            item.FontSize(12);
            controllerCombo.Items().Append(item);
        }
        controllerCombo.SelectedIndex(m_controllers.empty() ? -1 : 0);
        RefreshSlotFor(controllerCombo, slotCombo);
    }

    void VmSettingsDialog::RefreshSlotFor(
        Microsoft::UI::Xaml::Controls::ComboBox const& controllerCombo,
        Microsoft::UI::Xaml::Controls::ComboBox const& slotCombo)
    {
        if (!slotCombo) return;
        slotCombo.Items().Clear();
        std::wstring ref = ComboRef(controllerCombo);
        auto it = std::find_if(m_controllers.begin(), m_controllers.end(),
            [&](auto const& c) { return c.ref == ref; });
        if (it == m_controllers.end()) { slotCombo.SelectedIndex(-1); return; }

        // Free = [0, maxSlots) minus VM-used minus queued-this-session.
        std::set<int> taken(it->usedSlots.begin(), it->usedSlots.end());
        auto qit = m_queuedSlots.find(ref);
        if (qit != m_queuedSlots.end())
            taken.insert(qit->second.begin(), qit->second.end());
        for (int s = 0; s < it->maxSlots; ++s)
        {
            if (taken.count(s)) continue;
            Microsoft::UI::Xaml::Controls::ComboBoxItem item;
            item.Content(winrt::box_value(winrt::hstring{ L"Slot " + std::to_wstring(s) }));
            item.Tag(winrt::box_value(s));
            item.FontSize(12);
            slotCombo.Items().Append(item);
        }
        slotCombo.SelectedIndex(slotCombo.Items().Size() ? 0 : -1);
    }

    std::wstring VmSettingsDialog::ComboRef(
        Microsoft::UI::Xaml::Controls::ComboBox const& controllerCombo)
    {
        if (controllerCombo)
            if (auto sel = controllerCombo.SelectedItem()
                    .try_as<Microsoft::UI::Xaml::Controls::ComboBoxItem>())
                return std::wstring{ unbox_value_or<winrt::hstring>(sel.Tag(), L"") };
        return {};
    }

    int VmSettingsDialog::ComboSlot(
        Microsoft::UI::Xaml::Controls::ComboBox const& slotCombo)
    {
        if (slotCombo)
            if (auto sel = slotCombo.SelectedItem()
                    .try_as<Microsoft::UI::Xaml::Controls::ComboBoxItem>())
                return unbox_value_or<int>(sel.Tag(), -1);
        return -1;
    }

    void VmSettingsDialog::OnPlacementControllerChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        auto ctrl = sender.try_as<Microsoft::UI::Xaml::Controls::ComboBox>();
        if (!ctrl) return;
        auto slot = ctrl.Tag().try_as<Microsoft::UI::Xaml::Controls::ComboBox>();
        RefreshSlotFor(ctrl, slot);
    }

    void VmSettingsDialog::OnAttachFlyoutOpening(
        Windows::Foundation::IInspectable const&,
        Windows::Foundation::IInspectable const&)
    {
        PopulatePlacement(attachControllerCombo(), attachSlotCombo());
    }

    void VmSettingsDialog::OnCreateFlyoutOpening(
        Windows::Foundation::IInspectable const&,
        Windows::Foundation::IInspectable const&)
    {
        PopulatePlacement(createControllerCombo(), createSlotCombo());
    }

    void VmSettingsDialog::OnDvdFlyoutOpening(
        Windows::Foundation::IInspectable const&,
        Windows::Foundation::IInspectable const&)
    {
        PopulatePlacement(dvdControllerCombo(), dvdSlotCombo());
    }

    // Forward decl — MakeMetaTextBlock is defined further down (used by the
    // queued-attach card built in OnStorageAttachPhysical below).
    static Microsoft::UI::Xaml::Controls::TextBlock MakeMetaTextBlock(std::wstring const& text);

    // True if this process is running elevated (full admin token). Enumerating
    // offline host physical disks for pass-through requires elevation — the
    // Hyper-V Administrators group (which covers ordinary VM management) is NOT
    // enough, so a non-elevated hyprv sees an empty pass-through picker.
    static bool IsProcessElevated()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        TOKEN_ELEVATION elev{};
        DWORD sz = 0;
        bool elevated = false;
        if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &sz))
            elevated = (elev.TokenIsElevated != 0);
        CloseHandle(token);
        return elevated;
    }

    void VmSettingsDialog::OnPhysFlyoutOpening(
        Windows::Foundation::IInspectable const&,
        Windows::Foundation::IInspectable const&)
    {
        PopulatePlacement(physControllerCombo(), physSlotCombo());

        // Populate the offline host-disk picker. None available => show the
        // hint and disable the picker + Attach button. The hint distinguishes
        // "not elevated" (the common cause — admin is required to enumerate
        // physical disks) from "genuinely no offline disks".
        auto disks = hyprv::app::vm::VMManager::Instance().GetAvailablePhysicalDisks();
        if (auto combo = physDiskCombo())
        {
            combo.Items().Clear();
            for (auto const& d : disks)
            {
                Microsoft::UI::Xaml::Controls::ComboBoxItem item;
                std::wstring label = d.label;
                auto sz = FormatDiskSize(d.sizeBytes);
                if (!sz.empty() && label.find(sz) == std::wstring::npos)
                    label += L"  (" + sz + L")";
                item.Content(winrt::box_value(winrt::hstring{ label }));
                item.Tag(winrt::box_value(winrt::hstring{ d.devicePath }));
                item.FontSize(12);
                combo.Items().Append(item);
            }
            combo.SelectedIndex(disks.empty() ? -1 : 0);
        }
        const bool any = !disks.empty();
        if (auto t = physNoDisksText())
        {
            t.Text(winrt::hstring{ (!any && !IsProcessElevated())
                ? L"Run hyprv as administrator to attach a physical disk."
                : L"No offline physical disks are available." });
            t.Visibility(any ? Microsoft::UI::Xaml::Visibility::Collapsed
                             : Microsoft::UI::Xaml::Visibility::Visible);
        }
        if (auto b = physAttachButton()) b.IsEnabled(any);
        if (auto c = physDiskCombo())    c.IsEnabled(any);
    }

    void VmSettingsDialog::OnStorageAttachPhysical(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Read the picked disk (devicePath in Tag, label in Content) + the
        // controller/slot, then close the flyout.
        std::wstring devicePath, label;
        if (auto combo = physDiskCombo())
            if (auto sel = combo.SelectedItem()
                    .try_as<Microsoft::UI::Xaml::Controls::ComboBoxItem>())
            {
                devicePath = std::wstring{ unbox_value_or<winrt::hstring>(sel.Tag(), L"") };
                label      = std::wstring{ unbox_value_or<winrt::hstring>(sel.Content(), L"") };
            }
        std::wstring ctrlRef = ComboRef(physControllerCombo());
        int slot = ComboSlot(physSlotCombo());
        if (auto f = physDiskFlyout()) f.Hide();
        if (devicePath.empty()) return;

        PendingPhysAttach pa;
        pa.devicePath = devicePath;
        pa.label      = label;
        pa.ctrlRef    = ctrlRef;
        pa.slot       = slot;
        m_pendingPhysAttaches.push_back(pa);
        ConsumeQueuedSlot(pa.ctrlRef, pa.slot);

        if (auto h = storageAttachHost())
        {
            Microsoft::UI::Xaml::Controls::StackPanel panel;
            panel.Spacing(2);
            Microsoft::UI::Xaml::Controls::TextBlock pathTb;
            pathTb.Text(winrt::hstring{ L"Physical disk: " + label });
            pathTb.FontSize(12);
            pathTb.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
            panel.Children().Append(pathTb);
            panel.Children().Append(MakeMetaTextBlock(
                L"Attach to " + DestinationLabel(pa.ctrlRef, pa.slot)));
            Microsoft::UI::Xaml::Controls::Border card;
            if (auto st = Microsoft::UI::Xaml::Application::Current()
                    .Resources().TryLookup(winrt::box_value(
                        winrt::hstring{ L"BorderedCard" })))
                card.Style(st.try_as<Microsoft::UI::Xaml::Style>());
            card.Child(panel);
            h.Children().Append(card);
        }
    }

    void VmSettingsDialog::ConsumeQueuedSlot(std::wstring const& ctrlRef, int slot)
    {
        if (ctrlRef.empty() || slot < 0) return;
        m_queuedSlots[ctrlRef].insert(slot);
        // A queued device on a controller blocks that controller's removal.
        UpdateControllerRemoveButtons();
    }

    std::wstring VmSettingsDialog::DestinationLabel(
        std::wstring const& ctrlRef, int slot) const
    {
        std::wstring label = L"auto";
        for (auto const& c : m_controllers)
            if (c.ref == ctrlRef) { label = c.label; break; }
        if (slot >= 0) label += L" \x00b7 Slot " + std::to_wstring(slot);
        return label;
    }

    // Build a secondary "destination" meta line (the controller + slot a queued
    // op will land on) for the queued-op cards.
    static Microsoft::UI::Xaml::Controls::TextBlock MakeMetaTextBlock(std::wstring const& text)
    {
        Microsoft::UI::Xaml::Controls::TextBlock tb;
        tb.Text(winrt::hstring{ text });
        tb.FontSize(11);
        tb.Foreground(Microsoft::UI::Xaml::Application::Current()
            .Resources().TryLookup(winrt::box_value(winrt::hstring{
                L"TextFillColorSecondaryBrush" }))
            .try_as<Microsoft::UI::Xaml::Media::Brush>());
        return tb;
    }

    void VmSettingsDialog::OnStorageAttachVhd(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Read the picked controller + slot from the flyout, then close it
        // before the (modal) file picker opens.
        std::wstring ctrlRef = ComboRef(attachControllerCombo());
        int slot = ComboSlot(attachSlotCombo());
        if (auto f = attachVhdFlyout()) f.Hide();

        auto picked = PickVhdFile();
        if (!picked) return;

        PendingAttach pa;
        pa.path    = *picked;
        pa.ctrlRef = ctrlRef;
        pa.slot    = slot;
        m_pendingAttachPaths.push_back(pa);
        ConsumeQueuedSlot(pa.ctrlRef, pa.slot);

        if (auto h = storageAttachHost())
        {
            // Render the queued attach as a card matching the attached-disk
            // cards above (path on top, a destination note below).
            Microsoft::UI::Xaml::Controls::StackPanel panel;
            panel.Spacing(2);

            Microsoft::UI::Xaml::Controls::TextBlock pathTb;
            pathTb.Text(winrt::hstring{ pa.path });
            pathTb.FontSize(12);
            pathTb.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
            panel.Children().Append(pathTb);
            panel.Children().Append(MakeMetaTextBlock(
                L"Attach to " + DestinationLabel(pa.ctrlRef, pa.slot)));

            Microsoft::UI::Xaml::Controls::Border card;
            if (auto st = Microsoft::UI::Xaml::Application::Current()
                    .Resources().TryLookup(winrt::box_value(
                        winrt::hstring{ L"BorderedCard" })))
                card.Style(st.try_as<Microsoft::UI::Xaml::Style>());
            card.Child(panel);
            h.Children().Append(card);
        }
    }

    void VmSettingsDialog::OnStorageCreateVhd(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Read size + type from the inline inputs, then name the file.
        double gb = newVhdSizeBox() ? newVhdSizeBox().Value() : 0.0;
        if (!(gb >= 1.0)) gb = 1.0;   // NumberBox can yield NaN if cleared
        bool dynamic = true;
        if (auto c = newVhdDynamicCheck())
        {
            auto ic = c.IsChecked();
            dynamic = ic ? ic.Value() : true;
        }
        // Read the picked controller + slot, then close the popup before the
        // (modal) save dialog opens.
        std::wstring ctrlRef = ComboRef(createControllerCombo());
        int slot = ComboSlot(createSlotCombo());
        if (auto f = newVhdFlyout()) f.Hide();
        auto picked = PickSaveVhdFile();
        if (!picked) return;

        PendingCreate pc;
        pc.path      = *picked;
        pc.sizeBytes = static_cast<uint64_t>(gb) * 1024ull * 1024ull * 1024ull;
        pc.dynamic   = dynamic;
        pc.ctrlRef   = ctrlRef;
        pc.slot      = slot;
        m_pendingCreates.push_back(pc);
        ConsumeQueuedSlot(pc.ctrlRef, pc.slot);

        if (auto h = storageAttachHost())
        {
            Microsoft::UI::Xaml::Controls::StackPanel panel;
            panel.Spacing(2);

            Microsoft::UI::Xaml::Controls::TextBlock pathTb;
            pathTb.Text(winrt::hstring{ pc.path });
            pathTb.FontSize(12);
            pathTb.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
            panel.Children().Append(pathTb);

            panel.Children().Append(MakeMetaTextBlock(
                L"New " + std::to_wstring(static_cast<uint64_t>(gb)) + L" GB " +
                (dynamic ? std::wstring{ L"dynamic" } : std::wstring{ L"fixed" }) +
                L" \x00b7 " + DestinationLabel(pc.ctrlRef, pc.slot)));

            Microsoft::UI::Xaml::Controls::Border card;
            if (auto st = Microsoft::UI::Xaml::Application::Current()
                    .Resources().TryLookup(winrt::box_value(
                        winrt::hstring{ L"BorderedCard" })))
                card.Style(st.try_as<Microsoft::UI::Xaml::Style>());
            card.Child(panel);
            h.Children().Append(card);
        }
    }

    void VmSettingsDialog::OnNetworkAddAdapter(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Read the chosen switch ("(Not connected)" -> "" = disconnected),
        // queue the add, and render a card in networkAddHost. Applied at Save.
        std::wstring sw;
        if (auto c = addNicSwitchCombo())
        {
            if (auto sel = c.SelectedItem())
            {
                std::wstring s{ unbox_value_or<winrt::hstring>(sel, L"") };
                if (s != kDisconnectLabel) sw = std::move(s);
            }
        }
        if (auto f = addNicFlyout()) f.Hide();
        m_pendingNics.push_back(sw);

        if (auto h = networkAddHost())
        {
            Microsoft::UI::Xaml::Controls::StackPanel panel;
            panel.Spacing(2);

            Microsoft::UI::Xaml::Controls::TextBlock title;
            title.Text(winrt::hstring{ L"New Network Adapter" });
            title.FontSize(12);
            panel.Children().Append(title);

            Microsoft::UI::Xaml::Controls::TextBlock meta;
            meta.Text(winrt::hstring{
                sw.empty() ? std::wstring{ L"Not connected" }
                           : (L"Connect to " + sw) });
            meta.FontSize(11);
            meta.Foreground(Microsoft::UI::Xaml::Application::Current()
                .Resources().TryLookup(winrt::box_value(winrt::hstring{
                    L"TextFillColorSecondaryBrush" }))
                .try_as<Microsoft::UI::Xaml::Media::Brush>());
            panel.Children().Append(meta);

            Microsoft::UI::Xaml::Controls::Border card;
            if (auto st = Microsoft::UI::Xaml::Application::Current()
                    .Resources().TryLookup(winrt::box_value(
                        winrt::hstring{ L"BorderedCard" })))
                card.Style(st.try_as<Microsoft::UI::Xaml::Style>());
            card.Child(panel);
            h.Children().Append(card);
        }
    }

    void VmSettingsDialog::OnStorageAddDvdDrive(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Queue one empty DVD drive on the picked controller + slot (applied
        // at Save) and render a card. Read the flyout combos, then close it.
        PendingDvdAdd da;
        da.ctrlRef = ComboRef(dvdControllerCombo());
        da.slot    = ComboSlot(dvdSlotCombo());
        if (auto f = addDvdFlyout()) f.Hide();
        m_pendingDvdAdds.push_back(da);
        ConsumeQueuedSlot(da.ctrlRef, da.slot);
        if (auto h = storageDvdAddHost())
        {
            Microsoft::UI::Xaml::Controls::StackPanel panel;
            panel.Spacing(2);

            Microsoft::UI::Xaml::Controls::TextBlock title;
            title.Text(winrt::hstring{ L"New DVD Drive" });
            title.FontSize(12);
            panel.Children().Append(title);

            panel.Children().Append(MakeMetaTextBlock(
                L"Empty drive \x00b7 " + DestinationLabel(da.ctrlRef, da.slot)));

            Microsoft::UI::Xaml::Controls::Border card;
            if (auto st = Microsoft::UI::Xaml::Application::Current()
                    .Resources().TryLookup(winrt::box_value(
                        winrt::hstring{ L"BorderedCard" })))
                card.Style(st.try_as<Microsoft::UI::Xaml::Style>());
            card.Child(panel);
            h.Children().Append(card);
        }
    }

    winrt::fire_and_forget VmSettingsDialog::OnStorageAddScsiController(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Unlike attach/create (Save-batched), adding a controller applies
        // IMMEDIATELY so the new controller is selectable for subsequent
        // queued ops. Gated to Off (the button is disabled otherwise), but
        // VMManager also rejects a running add defensively.
        auto strong = get_strong();
        std::wstring vmGuid = m_vmGuid;
        if (vmGuid.empty()) co_return;
        auto dq = DispatcherQueue();

        if (auto b = storageAddScsiButton()) b.IsEnabled(false);
        if (auto ov = savingOverlay())
            ov.Visibility(Microsoft::UI::Xaml::Visibility::Visible);

        // Let the overlay paint before the UI-thread-bound WMI runs.
        co_await winrt::resume_after(std::chrono::milliseconds(150));
        co_await ResumeOnDispatcher{ dq };

        auto& vmm = hyprv::app::vm::VMManager::Instance();
        bool ok = vmm.AddScsiController(vmGuid);
        if (ok)
        {
            // Raw WMI is fresh (the add was synchronous on an Off VM). Rebuild
            // the controllers list + device cards so the new controller shows
            // up + is offered by the placement flyouts. (The next time a flyout
            // opens it re-reads m_controllers, so the new controller appears
            // there too.) RebuildStorageCards re-gates the buttons.
            RebuildStorageCards();
        }
        else
        {
            ShowError(L"Failed to add a SCSI controller. A VM can have at most "
                      L"4 SCSI controllers, and they can only be added while it "
                      L"is off.");
        }

        if (auto ov = savingOverlay())
            ov.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
        // Re-gate the add button (re-enabled here for the failure path; the
        // success path already re-gated via RebuildStorageCards).
        ApplyStateGating();
    }

    void VmSettingsDialog::OnCheckpointBrowse(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (auto picked = PickFolder(L"Select the checkpoint file location"))
            if (auto b = checkpointLocationBox())
                b.Text(winrt::hstring{ *picked });
    }

    void VmSettingsDialog::OnSmartPagingBrowse(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (auto picked = PickFolder(L"Select the smart paging file location"))
            if (auto b = smartPagingLocationBox())
                b.Text(winrt::hstring{ *picked });
    }

    void VmSettingsDialog::OnMemDynamicToggled(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Min/Max only meaningful when dynamic is on. Don't disable them
        // permanently — just visually deemphasize via Opacity. Keeps the
        // user's stored values visible while clearly indicating they're
        // inactive.
        bool dyn = memDynamicToggle() ? memDynamicToggle().IsOn() : false;
        if (auto b = memMinBox())    b.Opacity(dyn ? 1.0 : 0.5);
        if (auto b = memMaxBox())    b.Opacity(dyn ? 1.0 : 0.5);
        if (auto b = memBufferBox()) b.Opacity(dyn ? 1.0 : 0.5);   // buffer only applies to dynamic memory
    }

    void VmSettingsDialog::OnProcessorResourceChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&)
    {
        UpdateProcessorDerived();
    }

    void VmSettingsDialog::OnMemWeightChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&)
    {
        if (auto s = memWeightSlider())
            if (auto t = memWeightValueText())
                t.Text(winrt::hstring{
                    std::to_wstring(static_cast<int>(std::llround(s.Value()))) });
    }

    void VmSettingsDialog::OnUseHardwareTopology(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Populate the NUMA fields with THIS host's physical topology (a single
        // virtual NUMA node spanning all host processors + memory). Just fills
        // the boxes — the actual write happens on Save. (Hyper-V Manager's
        // default per-node memory is the host node minus an internal root
        // reserve we can't derive externally; the host total is functionally
        // equivalent and the user can fine-tune.)
        DWORD lp = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (lp < 1) lp = 1;
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        uint64_t hostMb = ::GlobalMemoryStatusEx(&ms)
            ? (ms.ullTotalPhys / (1024ull * 1024ull)) : 0;

        if (auto b = numaMaxProcsBox())  b.Value(static_cast<double>(lp));
        if (auto b = numaMaxMemoryBox()) if (hostMb > 0) b.Value(static_cast<double>(hostMb));
        if (auto b = numaMaxNodesBox())  b.Value(1.0);
        if (auto b = numaThreadsBox())   b.Value(0.0);   // inherit host SMT
    }

    void VmSettingsDialog::UpdateProcessorDerived()
    {
        // Hyper-V Manager's "Percent of total system resources" = the per-CPU
        // reserve/limit percentage scaled by (vCPU count / host logical
        // processors). E.g. limit 100 % on 16 vCPU of a 32-LP host = 50 %.
        double hostLp = static_cast<double>(::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        if (hostLp < 1.0) hostLp = 1.0;
        double count = cpuCountBox() ? cpuCountBox().Value() : 1.0;
        if (!(count >= 1.0)) count = 1.0;   // NumberBox can yield NaN when cleared

        auto pctOfTotal = [&](double perCpuPct) -> winrt::hstring
        {
            if (!(perCpuPct >= 0.0)) perCpuPct = 0.0;
            double total = perCpuPct * count / hostLp;
            if (total > 100.0) total = 100.0;
            return winrt::hstring{
                std::to_wstring(static_cast<int>(std::llround(total))) + L"% of total" };
        };
        if (auto t = cpuReserveTotalText())
            t.Text(pctOfTotal(cpuReservationBox() ? cpuReservationBox().Value() : 0.0));
        if (auto t = cpuLimitTotalText())
            t.Text(pctOfTotal(cpuLimitBox() ? cpuLimitBox().Value() : 0.0));
    }

    void VmSettingsDialog::OnRdpUseDefaultsToggled(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Toggle ON  = use app defaults  → per-VM controls disabled.
        // Toggle OFF = override defaults → per-VM controls enabled.
        // Leave the values visible (no Opacity dimming) so the user can
        // see what they're inheriting from defaults.
        bool useDefaults = rdpUseDefaultsToggle()
            ? rdpUseDefaultsToggle().IsOn()
            : true;
        bool en = !useDefaults;
        if (auto c = rdpAudioModeCombo())    c.IsEnabled(en);
        if (auto c = rdpAudioCaptureCheck()) c.IsEnabled(en);
        if (auto c = rdpClipboardCheck())    c.IsEnabled(en);
        if (auto c = rdpDrivesCheck())       c.IsEnabled(en);
        if (auto c = rdpDevicesCheck())      c.IsEnabled(en);
        if (auto c = rdpSmartCardsCheck())   c.IsEnabled(en);
        if (auto c = rdpPortsCheck())        c.IsEnabled(en);
        if (auto c = rdpColorDepthCombo())   c.IsEnabled(en);
        if (auto c = rdpScaleCombo())        c.IsEnabled(en);
        if (auto c = rdpResolutionCombo())   c.IsEnabled(en);
    }

    void VmSettingsDialog::ShowError(std::wstring const& message)
    {
        if (auto bar = errorBar())
        {
            bar.Message(winrt::hstring{ message });
            bar.IsOpen(true);
        }
        HyprvAppLog(L"[settings-dlg] error: %s", message.c_str());
    }

    winrt::fire_and_forget VmSettingsDialog::OnPrimaryButtonClick(
        Microsoft::UI::Xaml::Controls::ContentDialog const&,
        Microsoft::UI::Xaml::Controls::ContentDialogButtonClickEventArgs const& argsRef)
    {
        // CRITICAL: copy `argsRef` (a const& to a caller-owned stack value)
        // into the coroutine frame BEFORE any co_await. Otherwise the
        // reference dangles after the first suspend and a later
        // argsRef.Cancel(true) AVs trying to dereference a vtable from
        // a destroyed COM proxy. WinRT projection types are cheap copies
        // (smart-pointer refcount).
        auto args     = argsRef;
        auto deferral = args.GetDeferral();
        auto strong   = get_strong();    // keep `this` alive across awaits
        auto dq       = strong->DispatcherQueue();

        // Snapshot input values up front on the UI thread. We do this so
        // the actual edit calls can move to a background thread without
        // touching XAML state from there.
        struct Pending
        {
            std::wstring newName;
            std::wstring newNotes;
            hyprv::app::vm::VMManager::MemoryConfig    newMem{};
            hyprv::app::vm::VMManager::ProcessorConfig newCpu{};
            std::vector<std::pair<std::wstring, bool>> intChanges;  // class → wanted
            // Per-NIC switch changes (nicGuid -> wanted switch, "" = disconnect).
            std::vector<std::pair<std::wstring, std::wstring>> nicSwitchChanges;
            // Per-NIC MAC changes: nicGuid + desired mode (dynamic, or a
            // 12-hex static MAC). VMManager::SetNetworkAdapterMac applies it.
            struct NicMacChange { std::wstring nicGuid; bool dynamic; std::wstring staticMac; };
            std::vector<NicMacChange> nicMacChanges;
            // Per-NIC access VLAN changes: nicGuid + desired VLAN ID (0 =
            // untag). VMManager::SetNetworkAdapterVlan applies it.
            struct NicVlanChange { std::wstring nicGuid;
                                   hyprv::app::vm::VMManager::VlanConfig cfg; };
            std::vector<NicVlanChange> nicVlanChanges;
            // NICs to remove (nicGuid) and new NICs to add (switch name,
            // "" = disconnected). Gated to VM Off in ApplyStateGating.
            std::vector<std::wstring> nicRemoves;
            std::vector<std::wstring> nicAdds;
            // Per-NIC advanced-feature changes (MAC spoofing / guards / teaming
            // / port mirroring). VMManager::SetNetworkAdapterAdvanced applies
            // the whole set in one ModifyFeatureSettings.
            struct NicAdvChange { std::wstring nicGuid;
                                  hyprv::app::vm::VMManager::NicAdvancedFeatures features; };
            std::vector<NicAdvChange> nicAdvChanges;
            // Per-NIC bandwidth changes (bits/sec; 0 = none).
            struct NicBwChange { std::wstring nicGuid; uint64_t maxBps; uint64_t minBps; };
            std::vector<NicBwChange> nicBwChanges;
            // Per-NIC "protected network" (ClusterMonitored) changes.
            std::vector<std::pair<std::wstring, bool>> nicProtectedChanges;
            // Per-NIC device-naming changes.
            std::vector<std::pair<std::wstring, bool>> nicDeviceNamingChanges;
            // Per-NIC hardware-acceleration (offload) changes.
            struct NicOffloadChange { std::wstring nicGuid;
                                      hyprv::app::vm::VMManager::NicOffloadFeatures features; };
            std::vector<NicOffloadChange> nicOffloadChanges;
            // RDP: target override state for this VM after Save. wantOverride
            // distinguishes "set override to newRdp" from "clear override".
            // rdpChanged only true when the new state differs from the original
            // load-time snapshot — keeps a touch-nothing Save as a no-op.
            hyprv::app::settings::RdpOptions newRdp{};
            bool wantRdpOverride = false;
            bool rdpChanged      = false;
            // Firmware (Secure Boot). newSecureBootTemplateId is the braced
            // GUID for the chosen combo index; empty only if the index is
            // somehow out of range (SetSecureBoot then leaves the template
            // untouched).
            bool         newSecureBoot = false;
            std::wstring newSecureBootTemplateId;
            bool fwChanged   = false;
            // Boot order: the permuted Tag list (Gen 2 refs, or Gen 1 device
            // codes as strings), only populated when it differs from the
            // load-time order. bootIsGen1 picks the VMManager setter.
            std::vector<std::wstring> newBootOrder;
            bool bootChanged = false;
            bool bootIsGen1  = false;
            // DVD media changes: per changed drive, the refs + the desired ISO
            // ("" = eject). VMManager::SetDvdMedia infers mount/change/eject.
            struct DvdChange { std::wstring driveRef; std::wstring mediaRef; std::wstring newPath; };
            std::vector<DvdChange> dvdChanges;
            // DVD drive add/remove (the drive itself, not its media). Removes
            // carry (driveRef, mediaRef); adds carry the picked controller/slot.
            std::vector<std::pair<std::wstring, std::wstring>> dvdDriveRemoves;
            std::vector<PendingDvdAdd> dvdDriveAdds;
            // Hard disks: detaches (vhdRef, driveRef) for checked rows, and
            // attaches (VHD path + picked controller/slot via "Attach VHD...").
            std::vector<std::pair<std::wstring, std::wstring>> hddDetaches;
            std::vector<PendingAttach> hddAttaches;
            std::vector<PendingCreate> hddCreates;   // new VHDs to create + attach
            std::vector<PendingPhysAttach> physAttaches;  // pass-through disks to attach
            // Storage QoS edits: (vhdRef, minIops, maxIops) for changed disks.
            struct DiskQos { std::wstring vhdRef; uint64_t minIops; uint64_t maxIops; };
            std::vector<DiskQos> diskQosChanges;
            // Automatic start/stop actions. Raw WMI enum values (2/3/4);
            // autoChanged only when either differs from the load-time value.
            uint16_t newAutoStart = 0;
            uint16_t newAutoStop  = 0;
            uint32_t newAutoStartDelay = 0;
            bool autoChanged = false;
            // Checkpoints: type (UserSnapshotType value) + automatic-checkpoints
            // toggle + file location.
            uint16_t newCheckpointType = 0;
            bool newAutoCheckpoints = false;
            std::wstring newCheckpointLocation;
            bool checkpointChanged = false;
            // Smart Paging file location.
            std::wstring newSwapFileLocation;
            bool swapChanged = false;
            // COM ports: (ref, desired pipe path) for changed ports.
            struct ComChange { std::wstring ref; std::wstring path; };
            std::vector<ComChange> comChanges;
            // Debugger config — a Settings write (not WMI), like the RDP override.
            bool debuggerChanged = false;
            std::wstring newDebuggerExe, newDebuggerArgs;
            // Security (Gen 2 vTPM + state encryption).
            bool newTpm = false, tpmChanged = false;
            bool newEncryptState = false, encryptStateChanged = false;
            bool newShielded = false, shieldedChanged = false;
            bool nameChanged = false;
            bool notesChanged = false;
            bool memChanged = false;
            bool cpuChanged = false;
        } p;

        if (m_vmGuid.empty())
        {
            ShowError(L"No VM bound.");
            args.Cancel(true);
            deferral.Complete();
            co_return;
        }

        p.newName  = nameBox()  ? std::wstring{ nameBox().Text()  } : m_origName;
        p.newNotes = notesBox() ? std::wstring{ notesBox().Text() } : m_origNotes;
        p.nameChanged  = p.newName  != m_origName;
        p.notesChanged = p.newNotes != m_origNotes;
        if (p.nameChanged && p.newName.empty())
        {
            ShowError(L"Name cannot be empty.");
            args.Cancel(true);
            deferral.Complete();
            co_return;
        }

        p.newMem.startupMb       = memStartupBox()    ? static_cast<uint64_t>(memStartupBox().Value())    : m_origMemory.startupMb;
        p.newMem.dynamicEnabled  = memDynamicToggle() ? memDynamicToggle().IsOn()                          : m_origMemory.dynamicEnabled;
        p.newMem.minMb           = memMinBox()        ? static_cast<uint64_t>(memMinBox().Value())        : m_origMemory.minMb;
        p.newMem.maxMb           = memMaxBox()        ? static_cast<uint64_t>(memMaxBox().Value())        : m_origMemory.maxMb;
        p.newMem.targetBufferPct = memBufferBox()     ? static_cast<uint32_t>(memBufferBox().Value())     : m_origMemory.targetBufferPct;
        // Weight slider: only treat it as changed when the user moved off the
        // loaded value (so a no-touch Save preserves a non-100-aligned original).
        if (memWeightSlider())
        {
            int w = static_cast<int>(std::llround(memWeightSlider().Value()));
            p.newMem.priority = (w != m_origWeightSliderValue)
                ? static_cast<uint32_t>(w)
                : m_origMemory.priority;
        }
        else
            p.newMem.priority = m_origMemory.priority;
        p.newMem.maxMemoryPerNumaNodeMb = numaMaxMemoryBox()
            ? static_cast<uint64_t>(numaMaxMemoryBox().Value()) : m_origMemory.maxMemoryPerNumaNodeMb;
        p.memChanged =
            p.newMem.startupMb       != m_origMemory.startupMb      ||
            p.newMem.dynamicEnabled  != m_origMemory.dynamicEnabled ||
            p.newMem.minMb           != m_origMemory.minMb          ||
            p.newMem.maxMb           != m_origMemory.maxMb          ||
            p.newMem.targetBufferPct != m_origMemory.targetBufferPct||
            p.newMem.priority        != m_origMemory.priority       ||
            p.newMem.maxMemoryPerNumaNodeMb != m_origMemory.maxMemoryPerNumaNodeMb;

        // Reserve/Limit boxes are 0..100 %; convert back to raw WMI (×1000).
        p.newCpu.count          = cpuCountBox()       ? static_cast<uint16_t>(cpuCountBox().Value())                       : m_origProcessor.count;
        p.newCpu.reservationPct = cpuReservationBox() ? static_cast<uint64_t>(std::llround(cpuReservationBox().Value() * 1000.0)) : m_origProcessor.reservationPct;
        p.newCpu.limitPct       = cpuLimitBox()       ? static_cast<uint64_t>(std::llround(cpuLimitBox().Value() * 1000.0))       : m_origProcessor.limitPct;
        p.newCpu.weight         = cpuWeightBox()      ? static_cast<uint32_t>(cpuWeightBox().Value())                      : m_origProcessor.weight;
        {
            auto ic = cpuCompatCheck() ? cpuCompatCheck().IsChecked() : nullptr;
            p.newCpu.limitProcessorFeatures = ic ? ic.Value() : m_origProcessor.limitProcessorFeatures;
        }
        p.newCpu.maxProcessorsPerNumaNode = numaMaxProcsBox()
            ? static_cast<uint64_t>(numaMaxProcsBox().Value()) : m_origProcessor.maxProcessorsPerNumaNode;
        p.newCpu.maxNumaNodesPerSocket    = numaMaxNodesBox()
            ? static_cast<uint64_t>(numaMaxNodesBox().Value()) : m_origProcessor.maxNumaNodesPerSocket;
        p.newCpu.hwThreadsPerCore         = numaThreadsBox()
            ? static_cast<uint64_t>(numaThreadsBox().Value()) : m_origProcessor.hwThreadsPerCore;
        p.cpuChanged =
            p.newCpu.count          != m_origProcessor.count          ||
            p.newCpu.reservationPct != m_origProcessor.reservationPct ||
            p.newCpu.limitPct       != m_origProcessor.limitPct       ||
            p.newCpu.weight         != m_origProcessor.weight         ||
            p.newCpu.limitProcessorFeatures   != m_origProcessor.limitProcessorFeatures ||
            p.newCpu.maxProcessorsPerNumaNode != m_origProcessor.maxProcessorsPerNumaNode ||
            p.newCpu.maxNumaNodesPerSocket    != m_origProcessor.maxNumaNodesPerSocket    ||
            p.newCpu.hwThreadsPerCore         != m_origProcessor.hwThreadsPerCore;

        // Firmware (Secure Boot) — Gen 2 only. The section is hidden for
        // Gen 1 and the controls hold stale defaults, so never diff/apply
        // there. Baseline indices were captured in LoadFromVm such that a
        // no-touch Save is a no-op even when the stored template was
        // unrecognized.
        {
            bool newSb = m_origSecureBootEnabled;
            int  newTmpl = m_origSecureBootTemplateIndex;
            if (m_isGen2)
            {
                if (auto t = secureBootToggle()) newSb = t.IsOn();
                if (auto c = secureBootTemplateCombo())
                {
                    int idx = c.SelectedIndex();
                    if (idx >= 0) newTmpl = idx;
                }
            }
            p.newSecureBoot = newSb;
            p.newSecureBootTemplateId =
                (newTmpl >= 0 && newTmpl < static_cast<int>(std::size(kSecureBootTemplates)))
                    ? kSecureBootTemplates[newTmpl] : std::wstring{};
            p.fwChanged = m_isGen2 &&
                (newSb   != m_origSecureBootEnabled ||
                 newTmpl != m_origSecureBootTemplateIndex);
        }

        // Boot order (both generations) — read the current Tag order out of
        // the list (Gen 2 = BootSourceOrder refs, Gen 1 = device codes) and
        // diff against the load-time order. Vector equality covers both
        // reorder and the no-change case.
        {
            std::vector<std::wstring> cur;
            if (auto lv = bootOrderList())
            {
                auto items = lv.Items();
                cur.reserve(items.Size());
                for (uint32_t i = 0; i < items.Size(); ++i)
                {
                    auto lvi = items.GetAt(i).try_as<
                        Microsoft::UI::Xaml::Controls::ListViewItem>();
                    if (!lvi) continue;
                    cur.push_back(std::wstring{
                        unbox_value_or<winrt::hstring>(lvi.Tag(), L"") });
                }
            }
            if (cur != m_origBootOrder)
            {
                p.newBootOrder = std::move(cur);
                p.bootChanged  = true;
                p.bootIsGen1   = !m_isGen2;
            }
        }

        // Automatic start/stop actions — combo index +2 = WMI enum value.
        // Diff against the load-time enum so a no-touch Save is a no-op.
        {
            uint16_t newStart = m_origAutoStartAction;
            uint16_t newStop  = m_origAutoStopAction;
            if (auto c = autoStartCombo(); c && c.SelectedIndex() >= 0)
                newStart = static_cast<uint16_t>(c.SelectedIndex() + 2);
            if (auto c = autoStopCombo(); c && c.SelectedIndex() >= 0)
                newStop = static_cast<uint16_t>(c.SelectedIndex() + 2);
            uint32_t newDelay = m_origAutoStartDelaySeconds;
            if (auto b = autoStartDelayBox())
            {
                double v = b.Value();
                newDelay = (v >= 0.0) ? static_cast<uint32_t>(v + 0.5) : 0u;  // NaN/clear → 0
            }
            p.newAutoStart = newStart;
            p.newAutoStop  = newStop;
            p.newAutoStartDelay = newDelay;
            p.autoChanged  = (newStart != m_origAutoStartAction ||
                              newStop  != m_origAutoStopAction  ||
                              newDelay != m_origAutoStartDelaySeconds);
        }

        // Checkpoints — type combo + file location. The location box is
        // disabled when checkpoints exist, so its text stays == orig there.
        {
            int idx = checkpointTypeCombo() ? checkpointTypeCombo().SelectedIndex() : -1;
            if (idx < 0) idx = m_origCheckpointTypeIndex;
            std::wstring loc = checkpointLocationBox()
                ? std::wstring{ checkpointLocationBox().Text() } : m_origCheckpointLocation;
            auto b = loc.find_first_not_of(L" \t");
            auto e = loc.find_last_not_of(L" \t");
            if (b == std::wstring::npos) loc.clear();
            else loc = loc.substr(b, e - b + 1);
            bool autoChk = m_origAutoCheckpoints;
            if (auto c = automaticCheckpointsCheck())
            {
                auto ic = c.IsChecked();
                autoChk = ic ? ic.Value() : false;
            }
            p.newCheckpointType     = CheckpointIndexToType(idx);
            p.newAutoCheckpoints    = autoChk;
            p.newCheckpointLocation = loc;
            p.checkpointChanged     = (idx != m_origCheckpointTypeIndex) ||
                                      (autoChk != m_origAutoCheckpoints) ||
                                      (loc != m_origCheckpointLocation);
        }

        // Smart Paging file location — trim + diff against the load-time value.
        {
            std::wstring loc = smartPagingLocationBox()
                ? std::wstring{ smartPagingLocationBox().Text() } : m_origSwapFileLocation;
            auto b = loc.find_first_not_of(L" \t");
            auto e = loc.find_last_not_of(L" \t");
            if (b == std::wstring::npos) loc.clear();
            else loc = loc.substr(b, e - b + 1);
            p.newSwapFileLocation = loc;
            p.swapChanged = (loc != m_origSwapFileLocation);
        }

        // COM ports — trim each box + diff against the load-time pipe path.
        {
            auto trim = [](std::wstring s) {
                auto b = s.find_first_not_of(L" \t");
                auto e = s.find_last_not_of(L" \t");
                return (b == std::wstring::npos) ? std::wstring{} : s.substr(b, e - b + 1);
            };
            if (!m_com1Ref.empty())
            {
                std::wstring v = com1PathBox() ? trim(std::wstring{ com1PathBox().Text() })
                                               : m_origCom1Path;
                if (v != m_origCom1Path) p.comChanges.push_back({ m_com1Ref, v });
            }
            if (!m_com2Ref.empty())
            {
                std::wstring v = com2PathBox() ? trim(std::wstring{ com2PathBox().Text() })
                                               : m_origCom2Path;
                if (v != m_origCom2Path) p.comChanges.push_back({ m_com2Ref, v });
            }
        }

        // Debugger — trim both boxes + diff against the load-time values. A
        // Settings write (not WMI); applied alongside the RDP override below.
        {
            auto trim = [](std::wstring s) {
                auto b = s.find_first_not_of(L" \t");
                auto e = s.find_last_not_of(L" \t");
                return (b == std::wstring::npos) ? std::wstring{} : s.substr(b, e - b + 1);
            };
            std::wstring dbgExe = debuggerExeBox()
                ? trim(std::wstring{ debuggerExeBox().Text() }) : m_origDebuggerExe;
            std::wstring dbgArgs = debuggerArgsBox()
                ? trim(std::wstring{ debuggerArgsBox().Text() }) : m_origDebuggerArgs;
            p.newDebuggerExe  = dbgExe;
            p.newDebuggerArgs = dbgArgs;
            p.debuggerChanged = (dbgExe != m_origDebuggerExe)
                             || (dbgArgs != m_origDebuggerArgs);
        }

        // Security (Gen 2 only) — diff the toggles against load-time values.
        // Skip entirely for Gen 1 (controls hidden/disabled, never changed).
        if (m_isGen2)
        {
            if (auto t = tpmToggle())          p.newTpm = t.IsOn();
            if (auto t = encryptStateToggle()) p.newEncryptState = t.IsOn();
            if (auto t = shieldToggle())       p.newShielded = t.IsOn();
            p.tpmChanged           = (p.newTpm != m_origTpmEnabled);
            p.encryptStateChanged  = (p.newEncryptState != m_origEncryptState);
            p.shieldedChanged      = (p.newShielded != m_origShielded);
        }

        // DVD drives — remove (whole drive) wins over media diff. Otherwise
        // diff each row's path box against its load-time media: empty box =
        // eject; a path = mount (empty drive) or change (has media).
        for (auto const& row : m_dvdRows)
        {
            if (!row.pathBox) continue;
            if (row.removeCheck)
            {
                auto rc = row.removeCheck.IsChecked();
                if (rc && rc.Value())
                {
                    p.dvdDriveRemoves.emplace_back(row.driveRef, row.mediaRef);
                    continue;
                }
            }
            std::wstring cur{ row.pathBox.Text() };
            // Trim stray whitespace from pasted paths.
            auto b = cur.find_first_not_of(L" \t");
            auto e = cur.find_last_not_of(L" \t");
            if (b == std::wstring::npos) cur.clear();
            else cur = cur.substr(b, e - b + 1);
            if (cur != row.origMediaPath)
                p.dvdChanges.push_back({ row.driveRef, row.mediaRef, cur });
        }
        p.dvdDriveAdds = m_pendingDvdAdds;

        // Hard disks — detaches (checked rows) + attaches (queued paths) +
        // QoS edits. A detached row's QoS is skipped (the disk is going away).
        for (auto const& row : m_hddRows)
        {
            if (!row.detachCheck) continue;
            bool detaching = row.detachCheck.IsChecked() && row.detachCheck.IsChecked().Value();
            if (detaching)
            {
                p.hddDetaches.emplace_back(row.vhdRef, row.driveRef);
                continue;
            }
            // QoS diff — NumberBox yields NaN when cleared (= 0 / none).
            auto iops = [](Microsoft::UI::Xaml::Controls::NumberBox const& b) -> uint64_t {
                if (!b) return 0;
                double v = b.Value();
                if (!(v >= 0.0)) v = 0.0;
                return static_cast<uint64_t>(v + 0.5);
            };
            uint64_t newMin = iops(row.iopsMinBox);
            uint64_t newMax = iops(row.iopsMaxBox);
            if (newMin != row.origIopsMin || newMax != row.origIopsMax)
                p.diskQosChanges.push_back({ row.vhdRef, newMin, newMax });
        }
        p.hddAttaches = m_pendingAttachPaths;
        p.hddCreates  = m_pendingCreates;
        p.physAttaches = m_pendingPhysAttaches;

        for (auto const& cb : m_integrationChecks)
        {
            if (!cb) continue;
            auto cls = unbox_value_or<winrt::hstring>(cb.Tag(), L"");
            std::wstring className{ cls };
            bool current = cb.IsChecked() ? cb.IsChecked().Value() : false;
            bool original = current;
            for (auto const& kv : m_origIntegrationStates)
            {
                if (kv.first == className) { original = kv.second; break; }
            }
            if (current != original)
                p.intChanges.emplace_back(std::move(className), current);
        }

        // NIC switch changes — read each ComboBox's current selection,
        // map back to the switch name ("(Not connected)" -> ""), diff
        // against m_origAdapterSwitches by nicGuid.
        for (auto const& row : m_networkRows)
        {
            if (!row.switchCombo) continue;

            // Remove wins over edits: a NIC queued for removal skips its
            // switch / MAC / VLAN diffs (no point editing what's going away).
            if (row.removeCheck)
            {
                auto rc = row.removeCheck.IsChecked();
                if (rc && rc.Value())
                {
                    p.nicRemoves.push_back(row.nicGuid);
                    continue;
                }
            }

            std::wstring chosen;  // empty == disconnect
            if (auto sel = row.switchCombo.SelectedItem())
            {
                auto label = unbox_value_or<winrt::hstring>(sel, L"");
                std::wstring s{ label };
                if (s != kDisconnectLabel) chosen = std::move(s);
            }
            std::wstring original = chosen;
            for (auto const& kv : m_origAdapterSwitches)
            {
                if (kv.first == row.nicGuid) { original = kv.second; break; }
            }
            if (chosen != original)
                p.nicSwitchChanges.emplace_back(row.nicGuid, std::move(chosen));

            // MAC diff. Changed when the dynamic flag flipped, or it's static
            // and the 12-hex value differs from the load-time MAC.
            if (row.dynamicCheck)
            {
                auto ic = row.dynamicCheck.IsChecked();
                bool newDyn = ic ? ic.Value() : false;
                std::wstring newMac = row.macBox
                    ? NormalizeMac(std::wstring{ row.macBox.Text() }) : std::wstring{};
                bool macChanged = (newDyn != row.origDynamicMac) ||
                                  (!newDyn && newMac != row.origMac);
                if (macChanged)
                {
                    if (!newDyn && newMac.size() != 12)
                    {
                        ShowError(L"MAC address must be 12 hexadecimal digits "
                                  L"(e.g. 00:15:5D:01:3D:02).");
                        args.Cancel(true);
                        deferral.Complete();
                        co_return;
                    }
                    p.nicMacChanges.push_back({ row.nicGuid, newDyn, std::move(newMac) });
                }
            }

            // VLAN diff. Build a VlanConfig from the mode combo + sub-fields and
            // compare per-mode to the load-time state; any change → SetNetworkAdapterVlan.
            if (row.vlanModeCombo)
            {
                int modeIdx = row.vlanModeCombo.SelectedIndex();
                if (modeIdx < 0) modeIdx = 0;
                auto numOf = [](Microsoft::UI::Xaml::Controls::NumberBox const& b, int lo) -> uint16_t {
                    if (!b) return 0;
                    double v = b.Value();          // NaN when cleared
                    if (!(v >= lo)) v = static_cast<double>(lo);
                    if (v > 4094.0) v = 4094.0;
                    return static_cast<uint16_t>(v);
                };
                hyprv::app::vm::VMManager::VlanConfig cfg;
                cfg.mode = static_cast<uint16_t>(modeIdx);
                if (modeIdx == 1)
                {
                    cfg.accessVlanId = numOf(row.vlanBox, 1);
                }
                else if (modeIdx == 2)
                {
                    cfg.nativeVlanId = numOf(row.nativeVlanBox, 0);
                    if (row.trunkListBox)
                        cfg.trunkVlanList = ParseVlanList(std::wstring{ row.trunkListBox.Text() });
                }
                else if (modeIdx == 3)
                {
                    cfg.primaryVlanId   = numOf(row.primaryVlanBox, 0);
                    cfg.secondaryVlanId = numOf(row.secondaryVlanBox, 0);
                    int role = row.pvlanRoleCombo ? row.pvlanRoleCombo.SelectedIndex() : 0;
                    if (role < 0) role = 0;
                    cfg.pvlanMode = static_cast<uint8_t>(role + 1);
                }
                bool changed = (cfg.mode != row.origVlanMode);
                if (!changed)
                {
                    switch (cfg.mode)
                    {
                    case 1: changed = (cfg.accessVlanId != row.origVlanId); break;
                    case 2: changed = (cfg.nativeVlanId != row.origNativeVlanId)
                                   || (cfg.trunkVlanList != row.origTrunkList); break;
                    case 3: changed = (cfg.primaryVlanId   != row.origPrimaryVlanId)
                                   || (cfg.secondaryVlanId != row.origSecondaryVlanId)
                                   || (cfg.pvlanMode       != row.origPvlanMode); break;
                    default: break;   // untagged → untagged: no change
                    }
                }
                // A trunk with no allowed VLANs is invalid — Hyper-V rejects it.
                if (changed && cfg.mode == 2 && cfg.trunkVlanList.empty())
                {
                    ShowError(L"Trunk mode needs at least one allowed VLAN "
                              L"(e.g. 10,20,30 or 1-50).");
                    args.Cancel(true);
                    deferral.Complete();
                    co_return;
                }
                if (changed)
                    p.nicVlanChanges.push_back({ row.nicGuid, cfg });
            }

            // Advanced-features diff — read the 4 checks + port-mirror combo,
            // compare the whole set against the load-time snapshot.
            {
                auto chk = [](Microsoft::UI::Xaml::Controls::CheckBox const& c, bool fb) {
                    if (!c) return fb;
                    auto ib = c.IsChecked();
                    return ib ? ib.Value() : fb;
                };
                hyprv::app::vm::VMManager::NicAdvancedFeatures adv;
                adv.macSpoofing = chk(row.spoofCheck,       row.origAdvanced.macSpoofing);
                adv.dhcpGuard   = chk(row.dhcpGuardCheck,   row.origAdvanced.dhcpGuard);
                adv.routerGuard = chk(row.routerGuardCheck, row.origAdvanced.routerGuard);
                adv.nicTeaming  = chk(row.teamingCheck,     row.origAdvanced.nicTeaming);
                adv.ieeePriorityTag = chk(row.ieeePriorityCheck, row.origAdvanced.ieeePriorityTag);
                if (row.portMirrorCombo)
                {
                    int pmIdx = row.portMirrorCombo.SelectedIndex();
                    adv.portMirroring = static_cast<uint8_t>(pmIdx < 0 ? 0 : pmIdx);
                }
                else
                    adv.portMirroring = row.origAdvanced.portMirroring;
                if (adv.macSpoofing     != row.origAdvanced.macSpoofing ||
                    adv.dhcpGuard       != row.origAdvanced.dhcpGuard   ||
                    adv.routerGuard     != row.origAdvanced.routerGuard ||
                    adv.nicTeaming      != row.origAdvanced.nicTeaming  ||
                    adv.portMirroring   != row.origAdvanced.portMirroring ||
                    adv.ieeePriorityTag != row.origAdvanced.ieeePriorityTag)
                    p.nicAdvChanges.push_back({ row.nicGuid, adv });
            }

            // Bandwidth diff — compare at whole-Mbps granularity (the box
            // resolution) so a no-touch Save never rewrites a non-aligned
            // stored value. New bits/sec = Mbps * 1,000,000.
            {
                auto mbps = [](Microsoft::UI::Xaml::Controls::NumberBox const& b) -> uint32_t {
                    if (!b) return 0;
                    double v = b.Value();
                    if (!(v >= 0.0)) v = 0.0;   // NumberBox NaN when cleared
                    return static_cast<uint32_t>(v + 0.5);
                };
                uint32_t newMaxMbps = mbps(row.bwMaxBox);
                uint32_t newMinMbps = mbps(row.bwMinBox);
                if (newMaxMbps != row.origBwMaxMbps || newMinMbps != row.origBwMinMbps)
                    p.nicBwChanges.push_back({ row.nicGuid,
                        static_cast<uint64_t>(newMaxMbps) * 1000000ull,
                        static_cast<uint64_t>(newMinMbps) * 1000000ull });
            }

            // Protected network (ClusterMonitored) — separate WMI object.
            if (row.protectedCheck)
            {
                auto ic = row.protectedCheck.IsChecked();
                bool prot = ic ? ic.Value() : true;
                if (prot != row.origClusterMonitored)
                    p.nicProtectedChanges.push_back({ row.nicGuid, prot });
            }

            // Device naming (DeviceNamingEnabled on the synthetic port).
            if (row.deviceNamingCheck)
            {
                auto ic = row.deviceNamingCheck.IsChecked();
                bool dn = ic ? ic.Value() : false;
                if (dn != row.origDeviceNaming)
                    p.nicDeviceNamingChanges.push_back({ row.nicGuid, dn });
            }

            // Hardware acceleration (VMQ / SR-IOV / IPsec offload feature setting).
            {
                auto chk2 = [](Microsoft::UI::Xaml::Controls::CheckBox const& c, bool fb) {
                    if (!c) return fb;
                    auto ib = c.IsChecked();
                    return ib ? ib.Value() : fb;
                };
                hyprv::app::vm::VMManager::NicOffloadFeatures off;
                off.vmq          = chk2(row.vmqCheck,   row.origOffload.vmq);
                off.sriov        = chk2(row.sriovCheck, row.origOffload.sriov);
                off.ipsecOffload = chk2(row.ipsecCheck, row.origOffload.ipsecOffload);
                off.ipsecOffloadMaxSA = row.origOffload.ipsecOffloadMaxSA;
                if (row.ipsecMaxBox)
                {
                    double v = row.ipsecMaxBox.Value();
                    if (v >= 1.0) off.ipsecOffloadMaxSA = static_cast<uint32_t>(v + 0.5);
                }
                if (off.vmq          != row.origOffload.vmq          ||
                    off.sriov        != row.origOffload.sriov        ||
                    off.ipsecOffload != row.origOffload.ipsecOffload ||
                    (off.ipsecOffload &&
                     off.ipsecOffloadMaxSA != row.origOffload.ipsecOffloadMaxSA))
                    p.nicOffloadChanges.push_back({ row.nicGuid, off });
            }
        }
        p.nicAdds = m_pendingNics;

        // RDP diff. Settings::RdpOptions is purely hyprv-side state
        // (no WMI involvement) — diff happens here on the UI thread but
        // the Apply runs alongside the WMI sets below since it's just
        // an unordered_map write.
        {
            using AudioMode = hyprv::app::settings::RdpOptions::AudioMode;
            p.wantRdpOverride =
                rdpUseDefaultsToggle() ? !rdpUseDefaultsToggle().IsOn() : false;
            p.newRdp = m_origRdpOptions;  // start from origin, overlay UI
            if (auto c = rdpAudioModeCombo())
            {
                int idx = c.SelectedIndex();
                if (idx >= 0 && idx <= 2)
                    p.newRdp.audioMode = static_cast<AudioMode>(idx);
            }
            auto cbVal = [](Microsoft::UI::Xaml::Controls::CheckBox const& c, bool fb) {
                if (!c) return fb;
                auto ib = c.IsChecked();
                return ib ? ib.Value() : fb;
            };
            p.newRdp.audioCaptureRedirect = cbVal(rdpAudioCaptureCheck(), p.newRdp.audioCaptureRedirect);
            p.newRdp.redirectClipboard    = cbVal(rdpClipboardCheck(),    p.newRdp.redirectClipboard);
            p.newRdp.redirectDrives       = cbVal(rdpDrivesCheck(),       p.newRdp.redirectDrives);
            p.newRdp.redirectDevices      = cbVal(rdpDevicesCheck(),      p.newRdp.redirectDevices);
            p.newRdp.redirectSmartCards   = cbVal(rdpSmartCardsCheck(),   p.newRdp.redirectSmartCards);
            p.newRdp.redirectPorts        = cbVal(rdpPortsCheck(),        p.newRdp.redirectPorts);
            if (auto c = rdpColorDepthCombo())
            {
                int idx = c.SelectedIndex();
                if (idx == 0) p.newRdp.colorDepth = 16;
                else if (idx == 1) p.newRdp.colorDepth = 24;
                else if (idx == 2) p.newRdp.colorDepth = 32;
            }
            if (auto c = rdpScaleCombo())
                p.newRdp.dpiScaleOverridePercent =
                    RdpScalePercentForIndex(c.SelectedIndex());
            if (auto c = rdpResolutionCombo())
            {
                int idx = c.SelectedIndex();
                if (idx >= 0 && idx < static_cast<int>(std::size(kRdpResW)))
                {
                    p.newRdp.initialDesktopWidth  = kRdpResW[idx];
                    p.newRdp.initialDesktopHeight = kRdpResH[idx];
                }
            }
            // Two cases that count as "changed":
            //   (a) toggle state flipped (override vs defaults), OR
            //   (b) toggle stayed OFF (override) but the values differ
            //       from the captured origin.
            // No need to compare values when toggle stayed ON (defaults) —
            // the values inherited from defaults don't matter.
            bool valuesDiffer =
                p.newRdp.audioMode             != m_origRdpOptions.audioMode             ||
                p.newRdp.audioCaptureRedirect  != m_origRdpOptions.audioCaptureRedirect  ||
                p.newRdp.redirectClipboard     != m_origRdpOptions.redirectClipboard     ||
                p.newRdp.redirectDrives        != m_origRdpOptions.redirectDrives        ||
                p.newRdp.redirectDevices       != m_origRdpOptions.redirectDevices       ||
                p.newRdp.redirectSmartCards    != m_origRdpOptions.redirectSmartCards    ||
                p.newRdp.redirectPorts         != m_origRdpOptions.redirectPorts         ||
                p.newRdp.colorDepth            != m_origRdpOptions.colorDepth            ||
                p.newRdp.dpiScaleOverridePercent != m_origRdpOptions.dpiScaleOverridePercent ||
                p.newRdp.initialDesktopWidth   != m_origRdpOptions.initialDesktopWidth   ||
                p.newRdp.initialDesktopHeight  != m_origRdpOptions.initialDesktopHeight;
            p.rdpChanged =
                (p.wantRdpOverride != m_origRdpHasOverride) ||
                (p.wantRdpOverride && valuesDiffer);
        }

        // Nothing actually changed — bail without spinner or cache wait.
        if (!p.nameChanged && !p.notesChanged && !p.memChanged && !p.cpuChanged
            && !p.fwChanged && !p.bootChanged && p.dvdChanges.empty()
            && p.dvdDriveRemoves.empty() && p.dvdDriveAdds.empty()
            && p.hddDetaches.empty() && p.hddAttaches.empty() && p.hddCreates.empty()
            && p.physAttaches.empty() && p.diskQosChanges.empty()
            && p.intChanges.empty() && p.nicSwitchChanges.empty()
            && p.nicMacChanges.empty() && p.nicVlanChanges.empty()
            && p.nicRemoves.empty() && p.nicAdds.empty()
            && p.nicAdvChanges.empty() && p.nicBwChanges.empty()
            && p.nicProtectedChanges.empty()
            && p.nicDeviceNamingChanges.empty() && p.nicOffloadChanges.empty()
            && !p.rdpChanged && !p.autoChanged && !p.checkpointChanged
            && !p.swapChanged && p.comChanges.empty() && !p.debuggerChanged
            && !p.tpmChanged && !p.encryptStateChanged && !p.shieldedChanged)
        {
            deferral.Complete();
            co_return;
        }

        // Show the "Saving..." overlay.
        if (auto ov = savingOverlay()) ov.Visibility(Microsoft::UI::Xaml::Visibility::Visible);

        // Park on a short timer (a background thread-pool timer — the UI is
        // free meanwhile) so the UI thread actually RENDERS the overlay
        // before the UI-thread-bound WMI work below hogs it. A single
        // resume_background round-trip wasn't enough now that storage edits
        // (create/attach) can block the UI for a second or two — the spinner
        // appeared late. ~150 ms reliably lands the first paint.
        co_await winrt::resume_after(std::chrono::milliseconds(150));
        co_await ResumeOnDispatcher{ dq };

        // Sets MUST run on the UI thread: VMManager's WMI scope was
        // built on the UI STA and the IWbemServices proxy is apartment-
        // bound. Calling from a thread-pool MTA thread returns
        // RPC_E_WRONG_THREAD (0x8001010E). UI freezes briefly here
        // (~100-500 ms per Set), but the spinner glyph is already
        // painted from the round-trip above.
        std::wstring vmGuid = m_vmGuid;
        auto& vmm = hyprv::app::vm::VMManager::Instance();
        std::vector<std::wstring> errors;

        if (p.nameChanged   && !vmm.RenameVM(vmGuid, p.newName))   errors.push_back(L"Failed to rename VM.");
        if (p.notesChanged  && !vmm.SetNotes(vmGuid, p.newNotes))  errors.push_back(L"Failed to save notes.");
        if (p.memChanged    && !vmm.SetMemoryConfig(vmGuid, p.newMem))     errors.push_back(L"Failed to save memory settings.");
        if (p.cpuChanged    && !vmm.SetProcessorConfig(vmGuid, p.newCpu))  errors.push_back(L"Failed to save processor settings.");
        if (p.fwChanged     && !vmm.SetSecureBoot(vmGuid, p.newSecureBoot, p.newSecureBootTemplateId))
            errors.push_back(L"Failed to update firmware settings.");
        if (p.bootChanged)
        {
            bool bootOk;
            if (p.bootIsGen1)
            {
                // Gen 1 Tags are device codes ("0".."3") — parse to uint16.
                std::vector<uint16_t> codes;
                codes.reserve(p.newBootOrder.size());
                for (auto const& t : p.newBootOrder)
                    codes.push_back(static_cast<uint16_t>(std::stoul(t)));
                bootOk = vmm.SetBootOrderGen1(vmGuid, codes);
            }
            else
            {
                bootOk = vmm.SetBootOrder(vmGuid, p.newBootOrder);
            }
            if (!bootOk) errors.push_back(L"Failed to update boot order.");
        }
        if (p.checkpointChanged &&
            !vmm.SetCheckpointConfig(vmGuid, p.newCheckpointType,
                                     p.newAutoCheckpoints, p.newCheckpointLocation))
            errors.push_back(L"Failed to update checkpoint settings.");
        if (p.swapChanged &&
            !vmm.SetSmartPagingFileLocation(vmGuid, p.newSwapFileLocation))
            errors.push_back(L"Failed to update the smart paging file location.");
        for (auto const& cc : p.comChanges)
        {
            if (!vmm.SetSerialPortConnection(vmGuid, cc.ref, cc.path))
                errors.push_back(L"Failed to update a COM port.");
        }
        if (p.debuggerChanged)
            hyprv::app::settings::Settings::Instance().SetVmDebugger(
                vmGuid, p.newDebuggerExe, p.newDebuggerArgs);
        if (p.autoChanged   && !vmm.SetAutomaticActions(vmGuid, p.newAutoStart,
                                                        p.newAutoStop, p.newAutoStartDelay))
            errors.push_back(L"Failed to update automatic actions.");
        if (p.tpmChanged    && !vmm.SetVmTpm(vmGuid, p.newTpm))
            errors.push_back(L"Failed to update the virtual TPM.");
        if (p.encryptStateChanged && !vmm.SetVmStateEncryption(vmGuid, p.newEncryptState))
            errors.push_back(L"Failed to update state encryption.");
        // After TPM/encryption so an enable (which forces both on) is the final
        // word and isn't undone by a same-Save TPM/encryption toggle.
        if (p.shieldedChanged && !vmm.SetVmShielded(vmGuid, p.newShielded))
            errors.push_back(L"Failed to update VM shielding.");
        for (auto const& dc : p.dvdChanges)
        {
            if (!vmm.SetDvdMedia(vmGuid, dc.driveRef, dc.mediaRef, dc.newPath))
                errors.push_back(L"Failed to update a DVD drive.");
        }
        for (auto const& [driveRef, mediaRef] : p.dvdDriveRemoves)
        {
            if (!vmm.RemoveDvdDrive(vmGuid, driveRef, mediaRef))
                errors.push_back(L"Failed to remove a DVD drive.");
        }
        for (auto const& da : p.dvdDriveAdds)
        {
            if (!vmm.AddDvdDrive(vmGuid, da.ctrlRef, da.slot))
                errors.push_back(L"Failed to add a DVD drive.");
        }
        for (auto const& [vhdRef, driveRef] : p.hddDetaches)
        {
            if (!vmm.DetachVhd(vmGuid, vhdRef, driveRef))
                errors.push_back(L"Failed to detach a hard disk.");
        }
        for (auto const& a : p.hddAttaches)
        {
            if (!vmm.AttachVhd(vmGuid, a.path, a.ctrlRef, a.slot))
                errors.push_back(L"Failed to attach a hard disk.");
        }
        for (auto const& c : p.hddCreates)
        {
            if (!vmm.CreateAndAttachVhd(vmGuid, c.path, c.sizeBytes, c.dynamic,
                                        c.ctrlRef, c.slot))
                errors.push_back(L"Failed to create a hard disk.");
        }
        for (auto const& q : p.diskQosChanges)
        {
            if (!vmm.SetDiskQos(vmGuid, q.vhdRef, q.minIops, q.maxIops))
                errors.push_back(L"Failed to update hard-disk QoS (IOPS).");
        }
        for (auto const& pa : p.physAttaches)
        {
            if (!vmm.AttachPhysicalDisk(vmGuid, pa.devicePath, pa.ctrlRef, pa.slot))
                errors.push_back(L"Failed to attach a physical disk.");
        }
        for (auto const& [cls, want] : p.intChanges)
        {
            if (!vmm.SetIntegrationServiceEnabled(vmGuid, cls, want))
                errors.push_back(L"Failed to update an integration service.");
        }
        for (auto const& [nicGuid, sw] : p.nicSwitchChanges)
        {
            if (!vmm.SetNetworkAdapterSwitch(vmGuid, nicGuid, sw))
                errors.push_back(L"Failed to update a network adapter.");
        }
        for (auto const& mc : p.nicMacChanges)
        {
            if (!vmm.SetNetworkAdapterMac(vmGuid, mc.nicGuid, mc.dynamic, mc.staticMac))
                errors.push_back(L"Failed to update a network adapter MAC address.");
        }
        for (auto const& vc : p.nicVlanChanges)
        {
            if (!vmm.SetNetworkAdapterVlan(vmGuid, vc.nicGuid, vc.cfg))
                errors.push_back(L"Failed to update a network adapter VLAN.");
        }
        for (auto const& ac : p.nicAdvChanges)
        {
            if (!vmm.SetNetworkAdapterAdvanced(vmGuid, ac.nicGuid, ac.features))
                errors.push_back(L"Failed to update network adapter advanced features.");
        }
        for (auto const& bc : p.nicBwChanges)
        {
            if (!vmm.SetNetworkAdapterBandwidth(vmGuid, bc.nicGuid, bc.maxBps, bc.minBps))
                errors.push_back(L"Failed to update network adapter bandwidth.");
        }
        for (auto const& [nicGuid, on] : p.nicProtectedChanges)
        {
            if (!vmm.SetNetworkAdapterProtectedNetwork(vmGuid, nicGuid, on))
                errors.push_back(L"Failed to update network adapter protected-network setting.");
        }
        for (auto const& [nicGuid, on] : p.nicDeviceNamingChanges)
        {
            if (!vmm.SetNetworkAdapterDeviceNaming(vmGuid, nicGuid, on))
                errors.push_back(L"Failed to update network adapter device naming.");
        }
        for (auto const& oc : p.nicOffloadChanges)
        {
            if (!vmm.SetNetworkAdapterOffload(vmGuid, oc.nicGuid, oc.features))
                errors.push_back(L"Failed to update network adapter hardware acceleration.");
        }
        for (auto const& nicGuid : p.nicRemoves)
        {
            if (!vmm.RemoveNetworkAdapter(vmGuid, nicGuid))
                errors.push_back(L"Failed to remove a network adapter.");
        }
        for (auto const& sw : p.nicAdds)
        {
            if (!vmm.AddNetworkAdapter(vmGuid, sw))
                errors.push_back(L"Failed to add a network adapter.");
        }
        // RDP options — pure Settings write, no WMI involvement (and so
        // no apartment-bound proxy to worry about). Either install the
        // override or clear it depending on what the toggle says.
        if (p.rdpChanged)
        {
            auto& s = hyprv::app::settings::Settings::Instance();
            if (p.wantRdpOverride)
                s.SetRdpOptionsOverride(vmGuid, p.newRdp);
            else
                s.ClearRdpOptionsOverride(vmGuid);
            // Signal the opener to reconnect this VM's live session so the new
            // RDP options apply without a manual close + reopen.
            m_rdpOptionsChanged = true;
        }

        // Long wait on the bg thread so the ProgressRing actually
        // animates. KickPollAndWait only does atomic reads + sleep_for,
        // no COM — safe from any apartment.
        co_await winrt::resume_background();
        vmm.KickPollAndWait(15000);

        // Back to UI thread for spinner teardown + error display + deferral
        // completion. See ResumeOnDispatcher comment above for why we
        // don't use winrt::resume_foreground here.
        co_await ResumeOnDispatcher{ dq };

        if (auto ov = savingOverlay()) ov.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);

        if (!errors.empty())
        {
            ShowError(errors.front());
            args.Cancel(true);
        }
        deferral.Complete();
    }
}
