#include "pch.h"
#include "NewVmDialog.xaml.h"
#if __has_include("NewVmDialog.g.cpp")
#include "NewVmDialog.g.cpp"
#endif

#include "settings/Settings.h"
#include "vm/VMManager.h"

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <atlbase.h>
#include <shobjidl.h>

#include <chrono>
#include <cmath>
#include <coroutine>
#include <cwctype>
#include <optional>
#include <string>
#include <vector>

extern void HyprvAppLog(const wchar_t* fmt, ...);

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace
{
    // "(Not connected)" sentinel in the switch combo -> empty switchName.
    constexpr wchar_t const* kNotConnected = L"(Not connected)";

    // co_await this to hop back onto a WinUI 3 DispatcherQueue (see the
    // identical helper in VmSettingsDialog — resume_foreground only covers the
    // older Windows::System::DispatcherQueue).
    struct ResumeOnDispatcher
    {
        Microsoft::UI::Dispatching::DispatcherQueue dq;
        bool await_ready() const noexcept { return dq && dq.HasThreadAccess(); }
        void await_suspend(std::coroutine_handle<> h) const
        {
            if (!dq) { h.resume(); return; }
            dq.TryEnqueue([h] { h.resume(); });
        }
        void await_resume() const noexcept {}
    };

    std::wstring Trim(std::wstring s)
    {
        size_t a = s.find_first_not_of(L" \t\r\n");
        if (a == std::wstring::npos) return {};
        size_t b = s.find_last_not_of(L" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    bool FileExists(std::wstring const& path)
    {
        if (path.empty()) return false;
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    // Strip characters Windows won't allow in a filename so an auto-derived
    // "<dir>\<name>.vhdx" is always valid.
    std::wstring SanitizeFileName(std::wstring const& in)
    {
        std::wstring out;
        out.reserve(in.size());
        for (wchar_t c : in)
        {
            if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
                c == L'"'  || c == L'<' || c == L'>' || c == L'|')
                continue;
            out.push_back(c);
        }
        return Trim(out);
    }

    // Join a directory + "<name>.vhdx" with a single separator.
    std::wstring JoinVhdPath(std::wstring dir, std::wstring const& name)
    {
        std::wstring file = SanitizeFileName(name);
        if (file.empty() || dir.empty()) return {};
        if (dir.back() != L'\\' && dir.back() != L'/') dir.push_back(L'\\');
        return dir + file + L".vhdx";
    }

    // Modal Common Item Dialogs owned by the active top-level window (the
    // ContentDialog is an in-app popup over the main window, so GetActiveWindow
    // is the right owner). Same pattern as VmSettingsDialog's pickers.
    std::optional<std::wstring> PickOpenFile(
        wchar_t const* title, wchar_t const* label, wchar_t const* pattern)
    {
        CComPtr<IFileOpenDialog> dlg;
        if (FAILED(dlg.CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER)))
            return std::nullopt;
        COMDLG_FILTERSPEC filters[] = {
            { label,              pattern },
            { L"All files (*.*)", L"*.*"  },
        };
        dlg->SetFileTypes(ARRAYSIZE(filters), filters);
        dlg->SetTitle(title);
        if (FAILED(dlg->Show(GetActiveWindow()))) return std::nullopt;
        CComPtr<IShellItem> item;
        if (FAILED(dlg->GetResult(&item)) || !item) return std::nullopt;
        PWSTR p = nullptr;
        std::optional<std::wstring> result;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p)
            result = std::wstring(p);
        if (p) CoTaskMemFree(p);
        return result;
    }

    std::optional<std::wstring> PickSaveVhd(std::wstring const& suggestedName)
    {
        CComPtr<IFileSaveDialog> dlg;
        if (FAILED(dlg.CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER)))
            return std::nullopt;
        COMDLG_FILTERSPEC filters[] = {
            { L"Virtual hard disk (*.vhdx)", L"*.vhdx" },
        };
        dlg->SetFileTypes(ARRAYSIZE(filters), filters);
        dlg->SetDefaultExtension(L"vhdx");
        dlg->SetTitle(L"Create a virtual hard disk");
        if (!suggestedName.empty())
            dlg->SetFileName(suggestedName.c_str());
        if (FAILED(dlg->Show(GetActiveWindow()))) return std::nullopt;
        CComPtr<IShellItem> item;
        if (FAILED(dlg->GetResult(&item)) || !item) return std::nullopt;
        PWSTR p = nullptr;
        std::optional<std::wstring> result;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p)
            result = std::wstring(p);
        if (p) CoTaskMemFree(p);
        return result;
    }

    std::optional<std::wstring> PickFolder(wchar_t const* title)
    {
        CComPtr<IFileOpenDialog> dlg;
        if (FAILED(dlg.CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER)))
            return std::nullopt;
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS);
        dlg->SetTitle(title);
        if (FAILED(dlg->Show(GetActiveWindow()))) return std::nullopt;
        CComPtr<IShellItem> item;
        if (FAILED(dlg->GetResult(&item)) || !item) return std::nullopt;
        PWSTR p = nullptr;
        std::optional<std::wstring> result;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p)
            result = std::wstring(p);
        if (p) CoTaskMemFree(p);
        return result;
    }

    bool CheckBoxOn(Microsoft::UI::Xaml::Controls::CheckBox const& cb)
    {
        if (!cb) return false;
        auto r = cb.IsChecked();
        return r ? r.Value() : false;
    }
}

namespace winrt::hyprv_app::implementation
{
    NewVmDialog::NewVmDialog()
    {
        // Idempotent (the generated InitializeComponent guards on _contentLoaded);
        // called explicitly so the named elements are loaded before
        // PopulateDefaults touches them.
        InitializeComponent();
        this->PrimaryButtonClick({ this, &NewVmDialog::OnPrimaryButtonClick });
        PopulateDefaults();
    }

    void NewVmDialog::PopulateDefaults()
    {
        // Switch combo: "(Not connected)" + every host virtual switch. Default
        // to the "Default Switch" if present, else the first switch, else
        // "(Not connected)". WMI runs on this (UI) thread; GetVirtualSwitches
        // swallows its own exceptions.
        if (auto combo = switchCombo())
        {
            combo.Items().Clear();
            combo.Items().Append(winrt::box_value(winrt::hstring{ kNotConnected }));
            std::vector<std::wstring> switches;
            try { switches = hyprv::app::vm::VMManager::Instance().GetVirtualSwitches(); }
            catch (...) {}
            int preferred = 0, idx = 1;
            for (auto const& s : switches)
            {
                combo.Items().Append(winrt::box_value(winrt::hstring{ s }));
                if (preferred == 0 && _wcsicmp(s.c_str(), L"Default Switch") == 0)
                    preferred = idx;
                ++idx;
            }
            if (preferred == 0 && !switches.empty()) preferred = 1;
            combo.SelectedIndex(preferred);
        }

        // Default virtual processors to the host's logical-processor count
        // (what the user asked for) — capped at the NumberBox maximum. Hyper-V
        // allows up to the host LP count anyway.
        if (auto box = cpuBox())
        {
            DWORD lps = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
            if (lps == 0) lps = 1;
            double capped = std::min<double>(static_cast<double>(lps), box.Maximum());
            box.Value(capped);
        }

        try { m_defaultVhdDir = hyprv::app::vm::VMManager::Instance().GetDefaultVhdDirectory(); }
        catch (...) {}

        // Pre-fill the (disabled) custom-location box with the host default so
        // checking the box reveals an editable starting point.
        std::wstring defaultVmDir;
        try { defaultVmDir = hyprv::app::vm::VMManager::Instance().GetDefaultVmDirectory(); }
        catch (...) {}
        if (auto box = vmLocationBox(); box && !defaultVmDir.empty())
            box.Text(winrt::hstring{ defaultVmDir });

        RefreshAutoDiskPath();
        UpdateDiskPanels();
    }

    void NewVmDialog::OnCustomLocationToggled(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        bool on = CheckBoxOn(customLocationCheck());
        if (auto box = vmLocationBox())    box.IsEnabled(on);
        if (auto btn = vmLocationBrowse()) btn.IsEnabled(on);
    }

    void NewVmDialog::OnBrowseVmLocation(Windows::Foundation::IInspectable const&,
                                         Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (auto p = PickFolder(L"Choose where to store the virtual machine"))
            if (auto box = vmLocationBox()) box.Text(winrt::hstring{ *p });
    }

    void NewVmDialog::UpdateDiskPanels()
    {
        int mode = diskModeCombo() ? diskModeCombo().SelectedIndex() : 0;
        if (auto p = createDiskPanel())
            p.Visibility(mode == 0 ? Visibility::Visible : Visibility::Collapsed);
        if (auto p = existingDiskPanel())
            p.Visibility(mode == 1 ? Visibility::Visible : Visibility::Collapsed);
    }

    void NewVmDialog::RefreshAutoDiskPath()
    {
        // Only auto-fill while the user hasn't taken over the path, the create
        // mode is active, and we know the host's default VHD directory.
        if (m_userSetDiskPath) return;
        auto box = diskPathBox();
        if (!box) return;
        std::wstring name = nameBox() ? Trim(std::wstring{ nameBox().Text() }) : std::wstring{};
        std::wstring path = (name.empty() || m_defaultVhdDir.empty())
            ? std::wstring{}
            : JoinVhdPath(m_defaultVhdDir, name);
        box.Text(winrt::hstring{ path });
    }

    void NewVmDialog::ShowError(std::wstring const& message)
    {
        if (auto bar = errorBar())
        {
            bar.Title(L"Can't create the VM");
            bar.Message(winrt::hstring{ message });
            bar.IsOpen(true);
        }
    }

    void NewVmDialog::OnNameChanged(Windows::Foundation::IInspectable const&,
                                    Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&)
    {
        RefreshAutoDiskPath();
    }

    void NewVmDialog::OnMemoryPresetChanged(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        auto combo = sender.try_as<Microsoft::UI::Xaml::Controls::ComboBox>();
        if (!combo) return;
        auto item = combo.SelectedItem()
            .try_as<Microsoft::UI::Xaml::Controls::ComboBoxItem>();
        if (!item) return;
        // Tag carries the MB amount as a string (XAML literal).
        if (auto tag = item.Tag().try_as<winrt::hstring>())
        {
            double mb = _wtof(tag->c_str());
            if (mb > 0 && memBox()) memBox().Value(mb);
        }
    }

    void NewVmDialog::OnDiskModeChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        UpdateDiskPanels();
        // Switching back into create mode should re-derive the auto path.
        RefreshAutoDiskPath();
    }

    void NewVmDialog::OnBrowseNewDisk(Windows::Foundation::IInspectable const&,
                                      Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        std::wstring suggested =
            nameBox() ? SanitizeFileName(std::wstring{ nameBox().Text() }) : std::wstring{};
        if (!suggested.empty()) suggested += L".vhdx";
        if (auto p = PickSaveVhd(suggested))
        {
            m_userSetDiskPath = true;
            if (auto box = diskPathBox()) box.Text(winrt::hstring{ *p });
        }
    }

    void NewVmDialog::OnBrowseExistingDisk(Windows::Foundation::IInspectable const&,
                                           Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (auto p = PickOpenFile(L"Select a virtual hard disk",
                                  L"Virtual hard disk (*.vhdx;*.vhd)", L"*.vhdx;*.vhd"))
            if (auto box = existingDiskBox()) box.Text(winrt::hstring{ *p });
    }

    void NewVmDialog::OnBrowseIso(Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (auto p = PickOpenFile(L"Select a disc image",
                                  L"Disc image (*.iso)", L"*.iso"))
            if (auto box = isoBox()) box.Text(winrt::hstring{ *p });
    }

    winrt::fire_and_forget NewVmDialog::OnPrimaryButtonClick(
        Microsoft::UI::Xaml::Controls::ContentDialog const&,
        Microsoft::UI::Xaml::Controls::ContentDialogButtonClickEventArgs const& argsRef)
    {
        // Copy args into the coroutine frame before any co_await (the const&
        // dangles after the first suspend — same hazard as VmSettingsDialog).
        auto args     = argsRef;
        auto deferral = args.GetDeferral();
        auto strong   = get_strong();
        auto dq       = strong->DispatcherQueue();

        if (auto bar = errorBar()) bar.IsOpen(false);

        // ---- Snapshot every input on the UI thread up front. ----
        std::wstring name = nameBox() ? Trim(std::wstring{ nameBox().Text() }) : std::wstring{};
        int generation = (genCombo() && genCombo().SelectedIndex() == 1) ? 1 : 2;

        double memv = memBox() ? memBox().Value() : 2048.0;
        uint64_t startupMb = std::isnan(memv) ? 2048ull : static_cast<uint64_t>(memv);

        double cpuv = cpuBox() ? cpuBox().Value() : 2.0;
        uint32_t cpuCount = std::isnan(cpuv) ? 1u : static_cast<uint32_t>(cpuv);

        int diskMode = diskModeCombo() ? diskModeCombo().SelectedIndex() : 0;

        std::wstring switchName;
        if (switchCombo())
            if (auto sel = switchCombo().SelectedItem())
            {
                auto s = winrt::unbox_value_or<winrt::hstring>(sel, winrt::hstring{});
                if (!s.empty() && _wcsicmp(s.c_str(), kNotConnected) != 0)
                    switchName = std::wstring{ s };
            }

        std::wstring isoPath = isoBox() ? Trim(std::wstring{ isoBox().Text() }) : std::wstring{};

        // Custom VM location — only when the checkbox is on.
        std::wstring vmStoragePath;
        if (CheckBoxOn(customLocationCheck()) && vmLocationBox())
            vmStoragePath = Trim(std::wstring{ vmLocationBox().Text() });

        hyprv::app::vm::VMManager::NewVmConfig cfg;
        cfg.name            = name;
        cfg.generation      = generation;
        cfg.startupMemoryMb = startupMb;
        cfg.dynamicMemory   = CheckBoxOn(dynamicMemCheck());
        cfg.cpuCount        = cpuCount;
        cfg.switchName      = switchName;
        cfg.isoPath         = isoPath;
        cfg.vmStoragePath   = vmStoragePath;

        // ---- Validate. ----
        std::wstring err;
        if (name.empty())
            err = L"Enter a name for the virtual machine.";
        else if (CheckBoxOn(customLocationCheck()) && vmStoragePath.empty())
            err = L"Choose a folder for the virtual machine, or uncheck \"store in a different location\".";

        if (err.empty())
        {
            if (diskMode == 0)   // create new
            {
                cfg.diskMode = hyprv::app::vm::VMManager::NewVmConfig::Disk::CreateNew;
                double gv = diskSizeBox() ? diskSizeBox().Value() : 0.0;
                uint64_t gb = std::isnan(gv) ? 0ull : static_cast<uint64_t>(gv);
                cfg.newVhdSizeBytes = gb * 1024ull * 1024ull * 1024ull;
                cfg.dynamicVhd = CheckBoxOn(diskDynamicCheck());
                std::wstring p = diskPathBox() ? Trim(std::wstring{ diskPathBox().Text() })
                                               : std::wstring{};
                if (p.empty()) p = JoinVhdPath(m_defaultVhdDir, name);
                cfg.vhdPath = p;
                if (gb == 0)        err = L"Enter a maximum size for the new virtual hard disk.";
                else if (p.empty()) err = L"Choose a location for the new virtual hard disk.";
                else if (FileExists(p))
                    err = L"A file already exists at the chosen disk location. Pick another name.";
            }
            else if (diskMode == 1)   // use existing
            {
                cfg.diskMode = hyprv::app::vm::VMManager::NewVmConfig::Disk::UseExisting;
                std::wstring p = existingDiskBox() ? Trim(std::wstring{ existingDiskBox().Text() })
                                                   : std::wstring{};
                cfg.vhdPath = p;
                if (p.empty())            err = L"Select an existing virtual hard disk.";
                else if (!FileExists(p))  err = L"The selected virtual hard disk wasn't found.";
            }
            else
            {
                cfg.diskMode = hyprv::app::vm::VMManager::NewVmConfig::Disk::None;
            }
        }

        if (err.empty() && !isoPath.empty() && !FileExists(isoPath))
            err = L"The selected ISO image wasn't found.";

        if (!err.empty())
        {
            ShowError(err);
            args.Cancel(true);        // keep the dialog open
            deferral.Complete();
            co_return;
        }

        // ---- Run the create behind the overlay. ----
        if (auto ov = creatingOverlay()) ov.Visibility(Visibility::Visible);
        // Let the overlay paint before the (UI-thread) WMI batch blocks.
        co_await winrt::resume_after(std::chrono::milliseconds(150));
        co_await ResumeOnDispatcher{ dq };

        // CreateVM uses m_scope (UI STA), so it must run on this thread.
        std::wstring guid = hyprv::app::vm::VMManager::Instance().CreateVM(cfg);

        // Wait for a poll cycle off the UI thread so the new VM is in the cache
        // by the time we open a tab for it.
        co_await winrt::resume_background();
        if (!guid.empty())
            hyprv::app::vm::VMManager::Instance().KickPollAndWait(15000);
        co_await ResumeOnDispatcher{ dq };

        if (auto ov = creatingOverlay()) ov.Visibility(Visibility::Collapsed);

        if (guid.empty())
        {
            ShowError(L"Couldn't create the virtual machine. See the log for details.");
            args.Cancel(true);
            deferral.Complete();
            co_return;
        }

        HyprvAppLog(L"[newvm-dlg] created guid=%s name=%s", guid.c_str(), name.c_str());
        m_createdGuid = guid;
        m_createdName = name;
        deferral.Complete();   // closes the dialog with a Primary result
    }
}
