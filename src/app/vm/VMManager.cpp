#include "VMManager.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include <winrt/Windows.Data.Xml.Dom.h>

extern void HyprvAppLog(const wchar_t* fmt, ...);

namespace hyprv::app::vm
{
    VMManager& VMManager::Instance()
    {
        static VMManager s_instance;
        return s_instance;
    }

    // Property IDs for Msvm_VirtualSystemManagementService::GetSummaryInformation.
    // Without an ID in this list the corresponding Msvm_SummaryInformation
    // field comes back null — VMPlex got away with a smaller set because it
    // pulled static fields (name, state) from Msvm_ComputerSystem in parallel.
    // We use SummaryInformation as the single source, so request everything
    // the flyout + rail actually display. ThumbnailImage (7) is intentionally
    // skipped — it's a 320x240 RGB565 blob we don't render yet.
    // Source: Hyper-V Microsoft.HyperV PowerShell module + MSDN reference at
    // learn.microsoft.com/en-us/previous-versions/windows/desktop/virtual/
    // getsummaryinformation-msvm-virtualsystemmanagementservice
    // Snapshots (107) and VirtualSystemSubType (121) are intentionally
    // omitted — the Snapshot index returns nothing useful on this build,
    // and SubType is null too. Both come from a direct Msvm_VirtualSystemSettingData
    // query in UpdateAll instead.
    static const std::vector<uint32_t> kSummaryInfoRequest = {
        0,    // Name (Guid)
        1,    // ElementName (user-facing VM name)
        2,    // CreationTime
        3,    // Notes
        4,    // NumberOfProcessors
        5,    // EnabledState (fallback only — state comes from Msvm_ComputerSystem)
        10,   // Version
        101,  // ProcessorLoad
        103,  // MemoryUsage
        104,  // Heartbeat
        105,  // Uptime
        106,  // GuestOperatingSystem
        108,  // AsynchronousTasks
        112,  // MemoryAvailable
    };

    VMManager::VMManager()
    {
        // Connect on construction, but NEVER throw out of here — VMManager is a
        // singleton built unguarded from App::OnLaunched, so a throw would crash
        // startup. On failure TryConnect records a status the welcome page surfaces
        // (Hyper-V not installed / no access) and we skip starting the poll thread.
        if (TryConnect())
            m_pollThread = std::thread([this] { PollLoop(); });
    }

    bool VMManager::TryConnect()
    {
        try
        {
            m_scope = std::make_unique<hyprv::wmi::WmiScope>(L"root\\virtualization\\v2");
            HyprvAppLog(L"[vmm] connected to root\\virtualization\\v2");

            // Resolve the snapshot service on the main scope — it's the only
            // service we invoke from the UI thread (via the flyout buttons).
            // The management service is resolved on the polling thread instead,
            // since the polling thread's apartment can't share an STA-bound
            // proxy with the main thread.
            m_snapSvc = hyprv::wmi::hyperv::Msvm_VirtualSystemSnapshotService(
                m_scope->GetInstance(L"Msvm_VirtualSystemSnapshotService"));
            if (!m_snapSvc) HyprvAppLog(L"[vmm] WARNING: Msvm_VirtualSystemSnapshotService missing");

            // Subscriptions exist purely to kick the polling thread sooner than
            // its 1 s cadence on create/delete/modify events. They never do WMI
            // work themselves — that runs on the polling thread only.
            auto kick = [this](hyprv::wmi::WmiObject, hyprv::wmi::WmiObject) {
                KickPoll();
            };
            m_createSub = m_scope->Subscribe(
                L"SELECT * FROM __InstanceCreationEvent WITHIN 1 "
                L"WHERE TargetInstance ISA 'Msvm_ComputerSystem'", kick);
            m_deleteSub = m_scope->Subscribe(
                L"SELECT * FROM __InstanceDeletionEvent WITHIN 1 "
                L"WHERE TargetInstance ISA 'Msvm_ComputerSystem'", kick);
            m_modifySub = m_scope->Subscribe(
                L"SELECT * FROM __InstanceModificationEvent WITHIN 1 "
                L"WHERE TargetInstance ISA 'Msvm_ComputerSystem'", kick);
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            // Classify the HRESULT so the UI shows an actionable message. Literal
            // values + named comments avoid pulling wbemcli.h into this TU.
            if (e.hr == static_cast<HRESULT>(0x8004100E))          // WBEM_E_INVALID_NAMESPACE
                m_connectStatus = ConnectStatus::HyperVUnavailable;  //   -> Hyper-V not installed/enabled
            else if (e.hr == static_cast<HRESULT>(0x80041003)      // WBEM_E_ACCESS_DENIED
                  || e.hr == static_cast<HRESULT>(0x80070005))     // E_ACCESSDENIED
                m_connectStatus = ConnectStatus::AccessDenied;       //   -> not elevated / not in Hyper-V Admins
            else
                m_connectStatus = ConnectStatus::Other;
            m_connectDetail = e.whatW;
            HyprvAppLog(L"[vmm] connect failed (status=%d, hr=0x%08X): %s",
                        static_cast<int>(m_connectStatus),
                        static_cast<unsigned>(e.hr), e.whatW.c_str());
            // Drop any half-built state so a later RetryConnect starts clean.
            m_createSub.reset();
            m_deleteSub.reset();
            m_modifySub.reset();
            m_snapSvc = {};
            m_scope.reset();
            return false;
        }
        m_connectStatus = ConnectStatus::Ok;
        m_connectDetail.clear();
        return true;
    }

    std::wstring VMManager::ConnectErrorMessage() const
    {
        switch (m_connectStatus)
        {
        case ConnectStatus::HyperVUnavailable:
            return L"Hyper-V isn't enabled on this PC. Turn it on in Windows Features "
                   L"and restart hyprv.";
        case ConnectStatus::AccessDenied:
            return L"hyprv can't access Hyper-V. Run it as administrator, or add your "
                   L"account to the Hyper-V Administrators group and sign back in.";
        case ConnectStatus::Other:
            return L"Couldn't connect to Hyper-V on this PC. " + m_connectDetail;
        case ConnectStatus::Ok:
        default:
            return L"";
        }
    }

    bool VMManager::RetryConnect()
    {
        if (m_connectStatus == ConnectStatus::Ok) return true;
        if (!TryConnect()) return false;
        // First successful connect — start the poller (the ctor skipped it).
        if (!m_pollThread.joinable())
            m_pollThread = std::thread([this] { PollLoop(); });
        return true;
    }

    VMManager::~VMManager()
    {
        {
            std::lock_guard<std::mutex> lk(m_stopLock);
            m_stop.store(true);
        }
        m_stopCv.notify_all();
        if (m_pollThread.joinable()) m_pollThread.join();
    }

    // Build the "VM is doing X" status string the same way VMPlex does in
    // VirtualMachine.cs:258 (MakeStatusText). Reads Msvm_ConcreteJob entries
    // from a Msvm_SummaryInformation.AsynchronousTasks array, picks the active
    // ones (CIM JobState == 4 = Running), formats each as either
    //   "<Caption> (<PercentComplete>%)"  when 0 < pct < 100, or
    //   "<Caption> - <last StatusDescription>" otherwise, or just "<Caption>".
    // Multiple jobs are joined by ", " — rare, but possible.
    static std::wstring BuildStatusTextFromTasks(
        std::vector<hyprv::wmi::WmiObject>& tasks)
    {
        std::vector<std::wstring> parts;
        for (auto& t : tasks)
        {
            if (!t) continue;
            hyprv::wmi::hyperv::Msvm_ConcreteJob job(std::move(t));
            uint16_t jobState = job.JobState().value_or(0);
            // CIM JobState: 4 = Running. Skip anything else — completed jobs
            // linger in AsynchronousTasks for a while and would otherwise
            // accumulate as stale "Job completed successfully" entries.
            if (jobState != 4) continue;

            std::wstring caption = job.Caption().value_or(L"");
            uint16_t pct = job.PercentComplete().value_or(0);

            if (pct > 0 && pct < 100)
            {
                wchar_t buf[64];
                swprintf_s(buf, L" (%u%%)", static_cast<unsigned>(pct));
                parts.push_back(caption + buf);
                continue;
            }
            auto descs = job.StatusDescriptions();
            if (!descs.empty() && !descs.back().empty())
                parts.push_back(caption + L" - " + descs.back());
            else
                parts.push_back(caption);
        }
        std::wstring out;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (i > 0) out += L", ";
            out += parts[i];
        }
        return out;
    }

    // Decode one Msvm_KvpExchangeDataItem XML blob and copy its Name+Data
    // into `out`. The XML shape is:
    //   <INSTANCE CLASSNAME="Msvm_KvpExchangeDataItem">
    //     <PROPERTY NAME="Name"><VALUE>OSName</VALUE></PROPERTY>
    //     <PROPERTY NAME="Data"><VALUE>Windows 11 Pro</VALUE></PROPERTY>
    //     <PROPERTY NAME="Source" TYPE="uint16"><VALUE>0</VALUE></PROPERTY>
    //   </INSTANCE>
    // Bad/unrecognised XML is silently skipped — non-critical data.
    static void ParseKvpItem(std::wstring const& xml,
                             std::unordered_map<std::wstring, std::wstring>& out)
    {
        try
        {
            winrt::Windows::Data::Xml::Dom::XmlDocument doc;
            doc.LoadXml(winrt::hstring{ xml });
            auto nameNode = doc.SelectSingleNode(L"//PROPERTY[@NAME='Name']/VALUE");
            auto dataNode = doc.SelectSingleNode(L"//PROPERTY[@NAME='Data']/VALUE");
            if (!nameNode) return;
            std::wstring key = std::wstring{ nameNode.InnerText() };
            std::wstring val = dataNode ? std::wstring{ dataNode.InnerText() }
                                        : std::wstring{};
            if (!key.empty()) out[std::move(key)] = std::move(val);
        }
        catch (...) { /* ignore — best-effort */ }
    }

    // Hyper-V's Parent property on Msvm_VirtualSystemSettingData is a full
    // WMI path containing the parent VSSD's InstanceID. We just need the
    // InstanceID substring to match snapshots together — match VMPlex's
    // approach (Snapshot.cs:118-119): "Parent.Contains(parent.InstanceID)".
    // Returns "" if no parent token is found.
    // Hyper-V InstanceIDs include the owning VM's GUID, but the position
    // varies by class:
    //   Msvm_MemorySettingData          -> "Microsoft:<VM-GUID>\..."
    //   Msvm_GuestNetworkAdapterConfig  -> "Microsoft:GuestNetwork\<VM-GUID>\<NIC-GUID>"
    //   Msvm_VirtualSystemSettingData   -> "Microsoft:<VM-GUID>" (templates use "Definition")
    // Rather than special-casing each, scan for the first GUID-shaped
    // substring (36 chars with hyphens at positions 8/13/18/23). Returns
    // empty if no GUID is found.
    static std::wstring FindFirstGuidInInstanceId(std::wstring const& iid)
    {
        if (iid.size() < 36) return {};
        for (size_t i = 0; i + 36 <= iid.size(); ++i)
        {
            if (iid[i + 8]  == L'-' && iid[i + 13] == L'-' &&
                iid[i + 18] == L'-' && iid[i + 23] == L'-')
            {
                return iid.substr(i, 36);
            }
        }
        return {};
    }

    // Like FindFirstGuidInInstanceId but returns every GUID, in order. Used
    // for InstanceIDs that embed both a VM GUID and a NIC/resource GUID
    // (e.g. "Microsoft:<VM-GUID>\<NIC-GUID>").
    static std::vector<std::wstring> FindAllGuidsInInstanceId(std::wstring const& iid)
    {
        std::vector<std::wstring> out;
        for (size_t i = 0; i + 36 <= iid.size(); )
        {
            if (iid[i + 8]  == L'-' && iid[i + 13] == L'-' &&
                iid[i + 18] == L'-' && iid[i + 23] == L'-')
            {
                out.push_back(iid.substr(i, 36));
                i += 36;
            }
            else
            {
                ++i;
            }
        }
        return out;
    }

    // Pulls the Name="..." value out of a WMI object reference path. Used to
    // resolve the Msvm_VirtualEthernetSwitch GUID that Msvm_EthernetPortAlloc
    // ationSettingData.HostResource points at.
    //
    // WMI paths look like
    //   \\HOST\ns:ClassName.Key1="v1",Name="v2"
    // and the bare substring "Name=\"" also occurs inside "CreationClassName=\"".
    // Anchor to the leading separator (`.` for the first key, `,` for the
    // rest) so we don't trigger on that false positive.
    static std::wstring ExtractNamePropertyFromPath(std::wstring const& path)
    {
        if (path.empty()) return {};
        size_t pos = std::wstring::npos;
        for (auto* delim : { L".Name=\"", L",Name=\"" })
        {
            auto p = path.find(delim);
            if (p != std::wstring::npos && (pos == std::wstring::npos || p < pos))
                pos = p;
        }
        if (pos == std::wstring::npos) return {};
        pos += 7;  // both delimiters are 7 chars
        auto end = path.find(L'"', pos);
        if (end == std::wstring::npos) return {};
        return path.substr(pos, end - pos);
    }

    static std::wstring ExtractInstanceIdFromPath(std::wstring const& path)
    {
        if (path.empty()) return {};
        // Path is of the form: \\.\root\virtualization\v2:Msvm_VirtualSystemSettingData.InstanceID="..."
        auto pos = path.find(L"InstanceID=\"");
        if (pos == std::wstring::npos) return {};
        pos += 12;  // skip past InstanceID="
        auto end = path.find(L'"', pos);
        if (end == std::wstring::npos) return {};
        return path.substr(pos, end - pos);
    }

    // CIM datetime *interval* <-> seconds. Interval format is
    // "ddddddddHHMMSS.mmmmmm:000" (8 days / 2 hr / 2 min / 2 sec / 6 us / :000).
    // Used for AutomaticStartupActionDelay. We only surface whole seconds.
    static uint32_t CimIntervalToSeconds(std::wstring const& s)
    {
        if (s.size() < 14) return 0;
        auto num = [&](size_t off, size_t len) -> uint64_t {
            uint64_t v = 0;
            for (size_t i = 0; i < len; ++i)
            {
                wchar_t c = s[off + i];
                if (c < L'0' || c > L'9') return 0;
                v = v * 10 + (c - L'0');
            }
            return v;
        };
        uint64_t days = num(0, 8), hh = num(8, 2), mm = num(10, 2), ss = num(12, 2);
        return static_cast<uint32_t>(days * 86400 + hh * 3600 + mm * 60 + ss);
    }
    static std::wstring SecondsToCimInterval(uint32_t total)
    {
        uint32_t days = total / 86400; total %= 86400;
        uint32_t hh = total / 3600;    total %= 3600;
        uint32_t mm = total / 60;      uint32_t ss = total % 60;
        wchar_t buf[32];
        swprintf_s(buf, L"%08u%02u%02u%02u.000000:000", days, hh, mm, ss);
        return std::wstring{ buf };
    }

    // Build a preorder DFS of the snapshot list and stamp each one with its
    // tree depth. Snapshots whose parent isn't in the list (or whose Parent
    // points at the active settings, which we already filtered out) become
    // roots. Within each level, sort by CreationTime so the tree is stable
    // and natural-feeling.
    static std::vector<Snapshot> OrderSnapshotsAsTree(std::vector<Snapshot> flat)
    {
        if (flat.empty()) return flat;

        std::unordered_map<std::wstring, size_t> byId;
        byId.reserve(flat.size());
        for (size_t i = 0; i < flat.size(); ++i)
            byId[flat[i].instanceId] = i;

        std::vector<std::vector<size_t>> children(flat.size());
        std::vector<size_t> roots;
        for (size_t i = 0; i < flat.size(); ++i)
        {
            auto const& parentId = flat[i].parentInstanceId;
            auto it = parentId.empty() ? byId.end() : byId.find(parentId);
            if (it == byId.end()) roots.push_back(i);
            else                  children[it->second].push_back(i);
        }

        auto byCreation = [&](size_t a, size_t b) {
            auto const& ca = flat[a].creationTime;
            auto const& cb = flat[b].creationTime;
            if (!ca) return false;
            if (!cb) return true;
            return *ca < *cb;
        };
        std::sort(roots.begin(), roots.end(), byCreation);
        for (auto& vec : children) std::sort(vec.begin(), vec.end(), byCreation);

        std::vector<Snapshot> out;
        out.reserve(flat.size());
        // Iterative DFS to avoid stack growth on pathological chains.
        std::vector<std::pair<size_t, int>> stack;
        for (auto rit = roots.rbegin(); rit != roots.rend(); ++rit)
            stack.emplace_back(*rit, 0);
        while (!stack.empty())
        {
            auto [idx, depth] = stack.back();
            stack.pop_back();
            flat[idx].depth = depth;
            out.push_back(flat[idx]);
            auto const& kids = children[idx];
            for (auto kit = kids.rbegin(); kit != kids.rend(); ++kit)
                stack.emplace_back(*kit, depth + 1);
        }
        return out;
    }

    void VMManager::UpdateAll(hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService& vsms,
                              hyprv::wmi::WmiScope* perfScope)
    {
        if (!vsms) return;

        // Dynamic memory pressure perf counter — Hyper-V Manager's "Memory
        // Demand" and "Memory Status" both derive from this counter, NOT
        // from Msvm_SummaryInformation. CurrentPressure is the percent of
        // PhysicalMemory the guest is currently using (>100 means demand
        // exceeds assigned and the host is behind on ballooning up). Keyed
        // by the VM's ElementName since the perf counter exposes the
        // friendly name, not the GUID.
        struct PerfMem {
            uint32_t pressurePct      = 0;
            uint32_t physicalMemoryMb = 0;
        };
        std::unordered_map<std::wstring, PerfMem> perfByName;
        if (perfScope)
        {
            try
            {
                auto rows = perfScope->Query(
                    L"SELECT Name, CurrentPressure, PhysicalMemory "
                    L"FROM Win32_PerfFormattedData_BalancerStats_HyperVDynamicMemoryVM");
                perfByName.reserve(rows.size());
                for (auto& r : rows)
                {
                    auto name = r.GetString(L"Name").value_or(L"");
                    if (name.empty()) continue;
                    PerfMem pm;
                    pm.pressurePct      = r.GetUInt32(L"CurrentPressure").value_or(0);
                    pm.physicalMemoryMb = r.GetUInt32(L"PhysicalMemory").value_or(0);
                    perfByName[name] = pm;
                }
            }
            catch (hyprv::wmi::WmiException const& e)
            {
                HyprvAppLog(L"[vmm] perf query threw: %s", e.whatW.c_str());
            }
        }

        // Two queries per tick:
        //
        //  1. Msvm_ComputerSystem (filtered to "Virtual Machine") — gives us
        //     the canonical EnabledState. The matching property index in
        //     GetSummaryInformation is unreliable on at least some Windows
        //     builds (returns null even when requested), and state is
        //     load-bearing for the rail dot color + flyout header.
        //
        //  2. GetSummaryInformation on the same VMs — gives us the dynamic
        //     counters (ProcessorLoad, MemoryUsage, Heartbeat, Uptime), the
        //     async-task list, and the snapshot references. These are the
        //     fields that the WMI-class itself only populates when explicitly
        //     requested through this method.
        //
        // We merge by GUID. WmiScope is the polling thread's own scope so
        // both calls happen in the MTA apartment cleanly.
        std::unordered_map<std::wstring, uint16_t> stateByGuid;
        // Msvm_ComputerSystem.EnhancedSessionModeState values:
        //   0 = NotAllowed (host disabled it / disallowed for this VM)
        //   2 = AllowedAndAvailable (VM is up AND integration services report
        //       enhanced is currently usable)
        //   3 = AllowedButNotAvailable (allowed in principle, not right now —
        //       VM off, pre-login, integration services not running yet, etc.)
        // We only set Flag_EnhancedSession on connect when this is 2; anything
        // else falls back to basic-session (VMBus frame-buffer) mode so VMs
        // without integration services (Linux without LIS, boot screen, etc.)
        // still render instead of hanging at a blank popup.
        std::unordered_map<std::wstring, uint16_t> enhancedByGuid;
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT Name, EnabledState, EnhancedSessionModeState "
                L"FROM Msvm_ComputerSystem WHERE Caption='Virtual Machine'");
            stateByGuid.reserve(rows.size());
            enhancedByGuid.reserve(rows.size());
            for (auto& r : rows)
            {
                hyprv::wmi::hyperv::Msvm_ComputerSystem cs(std::move(r));
                auto guid = cs.Name().value_or(L"");
                if (guid.empty()) continue;
                stateByGuid[guid] = cs.EnabledState().value_or(0);
                enhancedByGuid[guid] = cs.EnhancedSessionModeState().value_or(0);
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] state query threw: %s", e.whatW.c_str());
            // Fall through — we'll still emit a partial snapshot using whatever
            // EnabledState the summary call returns.
        }

        // Per-VM realised settings (Generation, Secure Boot, paths, BIOS GUID).
        // The VirtualSystemSubType property index that GetSummaryInformation
        // is supposed to surface is null on this build, so we query VSSD
        // directly. Filtering to VirtualSystemType='Microsoft:Hyper-V:System:Realized'
        // returns exactly one row per VM (the current/active settings — snapshot
        // settings have a different VirtualSystemType, see below).
        struct VmRealisedSettings
        {
            std::wstring        subType;
            std::optional<bool> secureBoot;
            std::wstring        templateId;
            std::wstring        configRoot;
            std::wstring        snapshotRoot;
            std::wstring        swapRoot;
            std::wstring        biosGuid;
            uint16_t            autoStart = 0;
            uint16_t            autoStop  = 0;
            uint16_t            userSnapshotType = 0;
            bool                autoCheckpoints  = false;
            uint32_t            startDelaySeconds = 0;
        };
        std::unordered_map<std::wstring, VmRealisedSettings> settingsByGuid;
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT VirtualSystemIdentifier, VirtualSystemSubType, "
                L"SecureBootEnabled, SecureBootTemplateId, "
                L"ConfigurationDataRoot, SnapshotDataRoot, SwapFileDataRoot, UserSnapshotType, "
                L"AutomaticSnapshotsEnabled, AutomaticStartupActionDelay, "
                L"AutomaticStartupAction, AutomaticShutdownAction, "
                L"BIOSGUID "
                L"FROM Msvm_VirtualSystemSettingData "
                L"WHERE VirtualSystemType='Microsoft:Hyper-V:System:Realized'");
            settingsByGuid.reserve(rows.size());
            for (auto& r : rows)
            {
                hyprv::wmi::hyperv::Msvm_VirtualSystemSettingData sd(std::move(r));
                auto guid = sd.VirtualSystemIdentifier().value_or(L"");
                if (guid.empty()) continue;
                VmRealisedSettings s;
                s.subType      = sd.VirtualSystemSubType().value_or(L"");
                s.secureBoot   = sd.SecureBootEnabled();
                s.templateId   = sd.SecureBootTemplateId().value_or(L"");
                s.configRoot   = sd.ConfigurationDataRoot().value_or(L"");
                s.snapshotRoot = sd.SnapshotDataRoot().value_or(L"");
                s.swapRoot     = sd.GetString(L"SwapFileDataRoot").value_or(L"");
                s.autoStart    = sd.AutomaticStartupAction().value_or(0);
                s.autoStop     = sd.AutomaticShutdownAction().value_or(0);
                s.userSnapshotType = sd.UserSnapshotType().value_or(0);
                s.autoCheckpoints  = sd.GetBool(L"AutomaticSnapshotsEnabled").value_or(false);
                s.startDelaySeconds = CimIntervalToSeconds(
                    sd.GetString(L"AutomaticStartupActionDelay").value_or(L""));
                // BIOSGUID is a free-form String — keep it as-is (curly-braced).
                s.biosGuid     = sd.GetString(L"BIOSGUID").value_or(L"");
                settingsByGuid[guid] = std::move(s);
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] VSSD realised query threw: %s", e.whatW.c_str());
        }

        // Per-VM security settings (vTPM, state encryption). One
        // Msvm_SecuritySettingData per VM; its InstanceID is
        // "Microsoft:<VMGUID>\<SUBGUID>", so we parse the VM GUID from there —
        // cheaper than a per-VM association traversal and keyed to match the
        // VirtualSystemIdentifier (same bare uppercase GUID).
        struct VmSecuritySettings
        {
            std::optional<bool> tpmEnabled;
            std::optional<bool> encryptState;
        };
        std::unordered_map<std::wstring, VmSecuritySettings> securityByGuid;
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT InstanceID, TpmEnabled, "
                L"EncryptStateAndVmMigrationTraffic "
                L"FROM Msvm_SecuritySettingData");
            securityByGuid.reserve(rows.size());
            for (auto& r : rows)
            {
                auto iid = r.GetString(L"InstanceID").value_or(L"");
                auto colon = iid.find(L':');
                if (colon == std::wstring::npos) continue;
                auto slash = iid.find(L'\\', colon);
                if (slash == std::wstring::npos) continue;
                std::wstring guid = iid.substr(colon + 1, slash - colon - 1);
                if (guid.empty()) continue;
                VmSecuritySettings s;
                s.tpmEnabled   = r.GetBool(L"TpmEnabled");
                s.encryptState = r.GetBool(L"EncryptStateAndVmMigrationTraffic");
                securityByGuid[guid] = s;
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SecuritySettingData query threw: %s", e.whatW.c_str());
        }

        // Per-VM snapshots — one row per snapshot, matched back to the VM by
        // VirtualSystemIdentifier. The VirtualSystemType for snapshots ends in
        // ":Snapshot" (variants: ":Snapshot:Full", ":Snapshot:Recovery") which
        // we wildcard-match. Parent is a full WMI path; ExtractInstanceIdFromPath
        // pulls out just the parent's InstanceID so OrderSnapshotsAsTree can
        // link rows.
        // "Now" position: the snapshot each VM's live state currently descends
        // from. Msvm_MostCurrentSnapshotInBranch links a VM (Antecedent) to its
        // current snapshot (Dependent, a Msvm_VirtualSystemSettingData). The
        // Dependent InstanceIDs are globally unique, so a flat set is enough to
        // mark snapshots — no per-VM keying. Verified the association points at
        // the right node across linear / reverted / new-branch cases.
        std::unordered_set<std::wstring> currentSnapIds;
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT * FROM Msvm_MostCurrentSnapshotInBranch");
            for (auto& r : rows)
            {
                std::wstring id = ExtractInstanceIdFromPath(
                    r.GetString(L"Dependent").value_or(L""));
                if (!id.empty()) currentSnapIds.insert(id);
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] MostCurrentSnapshotInBranch query threw: %s",
                e.whatW.c_str());
        }

        // VMs whose guest Shutdown integration service is up — one query for
        // ALL VMs (Msvm_ShutdownComponent exists per running VM, keyed by
        // SystemName=GUID). Drives the "Shut down" action's enabled state.
        std::unordered_set<std::wstring> shutdownAvailGuids;
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT SystemName FROM Msvm_ShutdownComponent");
            for (auto& r : rows)
            {
                std::wstring sys = r.GetString(L"SystemName").value_or(L"");
                if (!sys.empty()) shutdownAvailGuids.insert(sys);
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] Msvm_ShutdownComponent query threw: %s",
                e.whatW.c_str());
        }

        std::unordered_map<std::wstring, std::vector<Snapshot>> snapsByGuid;
        try
        {
            // SELECT *  — NOT a curated column list. WQL doesn't include the
            // __PATH system property in the result row unless we either ask
            // for it explicitly or use SELECT *. WmiObject::Path() reads
            // __PATH, and our Snapshot needs it for ApplySnapshot /
            // DestroySnapshot lookups. With an explicit column list every
            // snapshot row landed with empty Path() and got dropped — the
            // entire snapshot-listing-empty bug.
            auto rows = vsms.Scope()->Query(
                L"SELECT * FROM Msvm_VirtualSystemSettingData "
                L"WHERE VirtualSystemType LIKE 'Microsoft:Hyper-V:Snapshot%'");
            for (auto& r : rows)
            {
                hyprv::wmi::hyperv::Msvm_VirtualSystemSettingData sd(std::move(r));
                auto guid = sd.VirtualSystemIdentifier().value_or(L"");
                if (guid.empty()) continue;
                Snapshot snap;
                snap.path             = sd.Path();
                if (snap.path.empty()) continue;
                snap.instanceId       = sd.InstanceID().value_or(L"");
                snap.parentInstanceId = ExtractInstanceIdFromPath(
                                            sd.Parent().value_or(L""));
                snap.elementName      = sd.ElementName().value_or(L"");
                snap.creationTime     = sd.CreationTime();
                snap.isCurrent        = currentSnapIds.count(snap.instanceId) > 0;
                snapsByGuid[guid].push_back(std::move(snap));
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] snapshot query threw: %s", e.whatW.c_str());
        }

        // Per-VM dynamic memory settings (Msvm_MemorySettingData). One row
        // per VM with the active settings; snapshot copies have different
        // InstanceID prefixes which we filter via the path-encoded GUID.
        // Units: VirtualQuantity / Reservation / Limit are in MB (per CIM
        // Hyper-V provider docs).
        struct MemorySettings
        {
            uint64_t startupMb = 0;
            uint64_t minMb     = 0;
            uint64_t maxMb     = 0;
            bool     dynamic   = false;
        };
        std::unordered_map<std::wstring, MemorySettings> memByGuid;
        try
        {
            // Match the VM's GUID from the InstanceID prefix
            // ("Microsoft:GUID\\..."). LIKE 'Microsoft:%' filters out the
            // resource pool template rows that don't belong to any VM.
            auto rows = vsms.Scope()->Query(
                L"SELECT InstanceID, VirtualQuantity, Reservation, Limit, "
                L"DynamicMemoryEnabled "
                L"FROM Msvm_MemorySettingData "
                L"WHERE InstanceID LIKE 'Microsoft:%'");
            for (auto& r : rows)
            {
                hyprv::wmi::hyperv::Msvm_MemorySettingData ms(std::move(r));
                auto iid = ms.InstanceID().value_or(L"");
                auto vmGuid = FindFirstGuidInInstanceId(iid);
                if (vmGuid.empty()) continue;  // skips "Microsoft:Definition\..." templates
                MemorySettings m;
                m.startupMb = ms.VirtualQuantity().value_or(0);
                m.minMb     = ms.Reservation().value_or(0);
                m.maxMb     = ms.Limit().value_or(0);
                m.dynamic   = ms.DynamicMemoryEnabled().value_or(false);
                // Guard against multiple matches (a VM's snapshots each have
                // their own MemorySettingData rows). Keep the first one
                // returned — should be the active config since we don't
                // restrict by VirtualSystemType (CIM doesn't expose that
                // here without an extra join). In practice we get one per
                // VM because non-active settings have a different InstanceID
                // structure that fails the LIKE pattern.
                memByGuid.try_emplace(vmGuid, m);
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] MemorySettingData query threw: %s", e.whatW.c_str());
        }

        // Guest IPs — Msvm_GuestNetworkAdapterConfiguration is the modern
        // source for guest-side IP addresses (per Microsoft, the KVP keys
        // NetworkAddressIPv4 / IPv6 are deprecated). One row per virtual NIC
        // on a running VM; we flatten by VM GUID.
        struct GuestIps
        {
            std::vector<std::wstring> v4;
            std::vector<std::wstring> v6;
        };
        std::unordered_map<std::wstring, GuestIps> ipsByGuid;
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT InstanceID, IPAddresses "
                L"FROM Msvm_GuestNetworkAdapterConfiguration");
            for (auto& r : rows)
            {
                hyprv::wmi::hyperv::Msvm_GuestNetworkAdapterConfiguration nc(std::move(r));
                auto iid = nc.InstanceID().value_or(L"");
                // Format: "Microsoft:GuestNetwork\<VM-GUID>\<NIC-GUID>".
                // FindFirstGuidInInstanceId handles the extra prefix segment.
                auto vmGuid = FindFirstGuidInInstanceId(iid);
                if (vmGuid.empty()) continue;
                auto& bucket = ipsByGuid[vmGuid];
                for (auto const& ip : nc.IPAddresses())
                {
                    // Quick v4/v6 split: colon = v6 (IPv6 addresses always
                    // have one), dot = v4. Anything else (e.g. "169.254.x.x"
                    // link-local) falls into v4.
                    if (ip.find(L':') != std::wstring::npos)
                        bucket.v6.push_back(ip);
                    else if (!ip.empty())
                        bucket.v4.push_back(ip);
                }
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GuestNetworkAdapterConfiguration query threw: %s",
                e.whatW.c_str());
        }

        // Per-NIC detail. We stitch three classes together, keyed by the
        // (VM-GUID, NIC-GUID) pair embedded in their InstanceIDs:
        //   Msvm_SyntheticEthernetPortSettingData       — name, MAC, dynamic flag
        //   Msvm_EthernetPortAllocationSettingData      — switch connection
        //   Msvm_VirtualEthernetSwitch                  — switch friendly name
        // The IPs come from the existing Msvm_GuestNetworkAdapterConfig query
        // (we re-walk it here too so we can attribute IPs to specific NICs,
        // not just aggregate across the whole VM).
        std::unordered_map<std::wstring,
            std::unordered_map<std::wstring, NetworkAdapter>> nicsByVm;
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT InstanceID, ElementName, Address, StaticMacAddress, "
                L"ClusterMonitored, DeviceNamingEnabled "
                L"FROM Msvm_SyntheticEthernetPortSettingData");
            for (auto& r : rows)
            {
                auto guids = FindAllGuidsInInstanceId(
                    r.GetString(L"InstanceID").value_or(L""));
                if (guids.size() < 2) continue;
                auto& adapter = nicsByVm[guids[0]][guids[1]];
                adapter.nicGuid    = guids[1];
                adapter.name       = r.GetString(L"ElementName").value_or(L"");
                adapter.macAddress = r.GetString(L"Address").value_or(L"");
                adapter.dynamicMac = !r.GetBool(L"StaticMacAddress").value_or(true);
                adapter.clusterMonitored = r.GetBool(L"ClusterMonitored").value_or(true);
                adapter.deviceNaming = r.GetBool(L"DeviceNamingEnabled").value_or(false);
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SyntheticEthernetPortSettingData query threw: %s",
                e.whatW.c_str());
        }

        // Switch GUID -> friendly name lookup. Switches live at the host
        // level so the query is cheap (one row per virtual switch).
        std::unordered_map<std::wstring, std::wstring> switchNameByGuid;
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT Name, ElementName FROM Msvm_VirtualEthernetSwitch");
            for (auto& r : rows)
            {
                hyprv::wmi::hyperv::Msvm_VirtualEthernetSwitch sw(std::move(r));
                auto name = sw.Name().value_or(L"");
                if (!name.empty())
                    switchNameByGuid[name] = sw.ElementName().value_or(L"");
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] VirtualEthernetSwitch query threw: %s",
                e.whatW.c_str());
        }

        // Walk allocation rows and stamp each adapter with its switch.
        // Parent is a full path to the SyntheticEthernetPortSettingData;
        // HostResource[0] is a path to the VirtualEthernetSwitch — we pull
        // Name="<GUID>" out of both and look the adapter / switch up.
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT Parent, HostResource, EnabledState "
                L"FROM Msvm_EthernetPortAllocationSettingData");
            for (auto& r : rows)
            {
                hyprv::wmi::hyperv::Msvm_EthernetPortAllocationSettingData ap(std::move(r));
                auto parentIid = ExtractInstanceIdFromPath(ap.Parent().value_or(L""));
                auto pGuids = FindAllGuidsInInstanceId(parentIid);
                if (pGuids.size() < 2) continue;
                auto& vmMap = nicsByVm[pGuids[0]];
                auto it = vmMap.find(pGuids[1]);
                if (it == vmMap.end()) continue;
                it->second.connected = (ap.EnabledState().value_or(0) == 2);
                auto hosts = ap.HostResource();
                if (!hosts.empty())
                {
                    auto swGuid = ExtractNamePropertyFromPath(hosts.front());
                    if (auto sit = switchNameByGuid.find(swGuid);
                        sit != switchNameByGuid.end())
                        it->second.switchName = sit->second;
                    else
                        it->second.switchName = swGuid;  // fall back to GUID
                }
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] EthernetPortAllocationSettingData query threw: %s",
                e.whatW.c_str());
        }

        // Access-VLAN per NIC. Msvm_EthernetSwitchPortVlanSettingData is a
        // feature setting layered on the port allocation; its InstanceID is
        // "Microsoft:<VM-GUID>\<NIC-GUID>\C\<def-guid>\<inst-guid>", so the
        // first two GUIDs identify the NIC. We surface only access-mode VLANs
        // (OperationMode 1) — trunk/private stay 0 (untagged) to match the
        // dialog's access-only UI. A NIC with no VLAN setting keeps vlanId 0.
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT * FROM Msvm_EthernetSwitchPortVlanSettingData");
            for (auto& r : rows)
            {
                auto guids = FindAllGuidsInInstanceId(
                    r.GetString(L"InstanceID").value_or(L""));
                if (guids.size() < 2) continue;
                auto& vmMap = nicsByVm[guids[0]];
                auto it = vmMap.find(guids[1]);
                if (it == vmMap.end()) continue;
                auto& n = it->second;
                uint16_t mode = static_cast<uint16_t>(
                    r.GetUInt32(L"OperationMode").value_or(0));
                n.vlanMode = mode;
                if (mode == 1)        // Access
                {
                    n.vlanId = r.GetUInt16(L"AccessVlanId").value_or(0);
                }
                else if (mode == 2)   // Trunk
                {
                    n.nativeVlanId = r.GetUInt16(L"NativeVlanId").value_or(0);
                    for (auto v : r.GetUInt32Array(L"TrunkVlanIdArray"))
                        n.trunkVlanList.push_back(static_cast<uint16_t>(v));
                }
                else if (mode == 3)   // Private
                {
                    n.primaryVlanId   = r.GetUInt16(L"PrimaryVlanId").value_or(0);
                    n.secondaryVlanId = r.GetUInt16(L"SecondaryVlanId").value_or(0);
                    n.pvlanMode = static_cast<uint8_t>(
                        r.GetUInt32(L"PvlanMode").value_or(0));
                    for (auto v : r.GetUInt32Array(L"SecondaryVlanIdArray"))
                        n.secondaryVlanList.push_back(static_cast<uint16_t>(v));
                }
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] EthernetSwitchPortVlanSettingData query threw: %s",
                e.whatW.c_str());
        }

        // Advanced features per NIC. Msvm_EthernetSwitchPortSecuritySettingData
        // is another feature setting on the port allocation; same InstanceID
        // shape (first two GUIDs = VM, NIC). Absent until the user sets one,
        // in which case all fields read default (off / MonitorMode 0).
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT InstanceID, AllowMacSpoofing, EnableDhcpGuard, "
                L"EnableRouterGuard, AllowTeaming, MonitorMode, "
                L"AllowIeeePriorityTag "
                L"FROM Msvm_EthernetSwitchPortSecuritySettingData");
            for (auto& r : rows)
            {
                auto guids = FindAllGuidsInInstanceId(
                    r.GetString(L"InstanceID").value_or(L""));
                if (guids.size() < 2) continue;
                auto& vmMap = nicsByVm[guids[0]];
                auto it = vmMap.find(guids[1]);
                if (it == vmMap.end()) continue;
                it->second.macSpoofing   = r.GetBool(L"AllowMacSpoofing").value_or(false);
                it->second.dhcpGuard     = r.GetBool(L"EnableDhcpGuard").value_or(false);
                it->second.routerGuard   = r.GetBool(L"EnableRouterGuard").value_or(false);
                it->second.nicTeaming    = r.GetBool(L"AllowTeaming").value_or(false);
                it->second.portMirroring =
                    static_cast<uint8_t>(r.GetUInt16(L"MonitorMode").value_or(0));
                it->second.ieeePriorityTag =
                    r.GetBool(L"AllowIeeePriorityTag").value_or(false);
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] EthernetSwitchPortSecuritySettingData query threw: %s",
                e.whatW.c_str());
        }

        // Bandwidth limits per NIC. Msvm_EthernetSwitchPortBandwidthSettingData
        // is another port-allocation feature setting (same InstanceID shape).
        // Limit/Reservation are bits/sec; absent until the user sets one.
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT InstanceID, Limit, Reservation "
                L"FROM Msvm_EthernetSwitchPortBandwidthSettingData");
            for (auto& r : rows)
            {
                auto guids = FindAllGuidsInInstanceId(
                    r.GetString(L"InstanceID").value_or(L""));
                if (guids.size() < 2) continue;
                auto& vmMap = nicsByVm[guids[0]];
                auto it = vmMap.find(guids[1]);
                if (it == vmMap.end()) continue;
                it->second.bandwidthMaxBps = r.GetUInt64(L"Limit").value_or(0);
                it->second.bandwidthMinBps = r.GetUInt64(L"Reservation").value_or(0);
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] EthernetSwitchPortBandwidthSettingData query threw: %s",
                e.whatW.c_str());
        }

        // Hardware acceleration per NIC. Msvm_EthernetSwitchPortOffloadSettingData
        // is a connection feature setting that ALWAYS exists by default (VMQ on).
        // VMQOffloadWeight/IOVOffloadWeight: 0=off / 100=on; IPSecOffloadLimit:
        // 0=off, else the max # of offloaded security associations.
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT InstanceID, VMQOffloadWeight, IOVOffloadWeight, "
                L"IPSecOffloadLimit FROM Msvm_EthernetSwitchPortOffloadSettingData");
            for (auto& r : rows)
            {
                auto guids = FindAllGuidsInInstanceId(
                    r.GetString(L"InstanceID").value_or(L""));
                if (guids.size() < 2) continue;
                auto& vmMap = nicsByVm[guids[0]];
                auto it = vmMap.find(guids[1]);
                if (it == vmMap.end()) continue;
                it->second.vmqEnabled   = r.GetUInt32(L"VMQOffloadWeight").value_or(0) != 0;
                it->second.sriovEnabled = r.GetUInt32(L"IOVOffloadWeight").value_or(0) != 0;
                uint32_t ipsec = r.GetUInt32(L"IPSecOffloadLimit").value_or(0);
                it->second.ipsecOffload      = (ipsec != 0);
                if (ipsec != 0) it->second.ipsecOffloadMaxSA = ipsec;
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] EthernetSwitchPortOffloadSettingData query threw: %s",
                e.whatW.c_str());
        }

        // Re-walk GuestNetworkAdapterConfiguration to map IPs to specific
        // NICs (the earlier pass aggregates across NICs into ipsByGuid). We
        // need both — the rail-row "guestIpv4" is a per-VM concept while the
        // flyout shows per-NIC detail.
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT InstanceID, IPAddresses "
                L"FROM Msvm_GuestNetworkAdapterConfiguration");
            for (auto& r : rows)
            {
                hyprv::wmi::hyperv::Msvm_GuestNetworkAdapterConfiguration nc(std::move(r));
                auto guids = FindAllGuidsInInstanceId(nc.InstanceID().value_or(L""));
                if (guids.size() < 2) continue;
                auto& vmMap = nicsByVm[guids[0]];
                auto it = vmMap.find(guids[1]);
                if (it == vmMap.end()) continue;
                for (auto const& ip : nc.IPAddresses())
                {
                    if (ip.empty()) continue;
                    if (ip.find(L':') != std::wstring::npos) it->second.ipv6.push_back(ip);
                    else                                      it->second.ipv4.push_back(ip);
                }
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GuestNetworkAdapterConfiguration (per-NIC) query threw: %s",
                e.whatW.c_str());
        }

        // Disk attachments — Msvm_StorageAllocationSettingData rows describe
        // each VHD/VHDX/ISO attached to a VM. HostResource[0] is the file
        // path on the host. ResourceSubType buckets them into HDD vs DVD.
        // We also stat the file to get the on-disk size (cheap — single
        // GetFileAttributesEx call). InstanceID is "Microsoft:<VM-GUID>\..."
        // so the existing FindFirstGuidInInstanceId helper works.
        std::unordered_map<std::wstring, std::vector<VirtualDisk>> disksByVm;
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT InstanceID, ResourceSubType, HostResource "
                L"FROM Msvm_StorageAllocationSettingData");
            for (auto& r : rows)
            {
                hyprv::wmi::hyperv::Msvm_StorageAllocationSettingData sa(std::move(r));
                auto vmGuid = FindFirstGuidInInstanceId(sa.InstanceID().value_or(L""));
                if (vmGuid.empty()) continue;
                auto hosts = sa.HostResource();
                if (hosts.empty() || hosts.front().empty()) continue;
                auto subType = sa.ResourceSubType().value_or(L"");

                VirtualDisk d;
                d.path = hosts.front();
                if (subType.find(L"Virtual Hard Disk") != std::wstring::npos)
                    d.kind = DiskKind::Hdd;
                else if (subType.find(L"Virtual CD/DVD") != std::wstring::npos ||
                         subType.find(L"Virtual DVD")    != std::wstring::npos)
                    d.kind = DiskKind::Dvd;
                else
                    d.kind = DiskKind::Other;

                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (GetFileAttributesExW(d.path.c_str(),
                                         GetFileExInfoStandard, &fad))
                {
                    d.fileSizeBytes = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32)
                                    | fad.nFileSizeLow;
                }
                disksByVm[vmGuid].push_back(std::move(d));
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] StorageAllocationSettingData query threw: %s",
                e.whatW.c_str());
        }

        // KVP data — what the guest OS reports about itself via integration
        // services. One Msvm_KvpExchangeComponent per VM, SystemName is the
        // VM's GUID. Only populated while the VM is running and has IS up;
        // a missing/empty row just means we keep the static fields blank.
        std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::wstring>> kvpByGuid;
        try
        {
            auto rows = vsms.Scope()->Query(
                L"SELECT SystemName, GuestIntrinsicExchangeItems "
                L"FROM Msvm_KvpExchangeComponent");
            for (auto& r : rows)
            {
                hyprv::wmi::hyperv::Msvm_KvpExchangeComponent kvp(std::move(r));
                auto guid = kvp.SystemName().value_or(L"");
                if (guid.empty()) continue;
                auto items = kvp.GuestIntrinsicExchangeItems();
                if (items.empty()) continue;
                auto& bucket = kvpByGuid[guid];
                for (auto const& xml : items) ParseKvpItem(xml, bucket);
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] KVP query threw: %s", e.whatW.c_str());
        }

        std::vector<hyprv::wmi::WmiObject> summaries;
        try
        {
            // Empty SettingData -> WMI applies the call to every active
            // virtual system's current settings (== all VMs on the host).
            // The codegen doesn't set the SettingData in-param, which is
            // exactly what we want — WMI treats it as null.
            auto r = vsms.GetSummaryInformation(
                kSummaryInfoRequest, /* SettingData */ {});
            if (r.ReturnValue != 0)
            {
                HyprvAppLog(L"[vmm] GetSummaryInformation ret=%u", r.ReturnValue);
                return;
            }
            summaries = std::move(r.SummaryInformation);
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GetSummaryInformation threw: %s", e.whatW.c_str());
            return;
        }

        std::vector<VirtualMachine> next;
        next.reserve(summaries.size());
        for (auto& s : summaries)
        {
            if (!s) continue;
            hyprv::wmi::hyperv::Msvm_SummaryInformation si(std::move(s));
            VirtualMachine vm;
            vm.guid             = si.Name().value_or(L"");
            if (vm.guid.empty()) continue;  // skip host / phantom rows
            vm.elementName      = si.ElementName().value_or(L"");
            // Prefer the Msvm_ComputerSystem state (always populated); fall
            // back to whatever SummaryInformation gave us if the side query
            // didn't reach this VM.
            if (auto it = stateByGuid.find(vm.guid); it != stateByGuid.end())
                vm.state        = static_cast<VmState>(it->second);
            else
                vm.state        = static_cast<VmState>(si.EnabledState().value_or(0));
            // EnhancedSessionModeState — true only for the explicit
            // AllowedAndAvailable (2) value. Anything else (off, transitioning,
            // disallowed, integration services not reporting) is treated as
            // "use basic session" so the rdphost child doesn't try to ride a
            // negotiation that will silently never produce pixels.
            if (auto it = enhancedByGuid.find(vm.guid); it != enhancedByGuid.end())
                vm.enhancedSessionAvailable = (it->second == 2);
            vm.numProcessors     = si.NumberOfProcessors().value_or(0);
            vm.processorLoadPct  = si.ProcessorLoad().value_or(0);
            vm.heartbeatState    = si.Heartbeat().value_or(0);
            vm.shutdownServiceAvailable = shutdownAvailGuids.count(vm.guid) > 0;
            vm.memoryAssignedMb  = si.MemoryUsage().value_or(0);
            vm.memoryAvailableMb = si.MemoryAvailable().value_or(0);
            // Memory Demand comes from the dynamic-memory balancer perf
            // counter, NOT from Msvm_SummaryInformation. Hyper-V Manager
            // and PowerShell Get-VM use the same source. CurrentPressure is
            // a percentage of PhysicalMemory; demand can exceed assigned
            // when the host hasn't caught up to the guest's request yet.
            //
            // VMs without dynamic memory enabled, stopped VMs, or VMs
            // missing from the perf counter (e.g. integration services
            // down) get no entry — demand falls back to assigned.
            if (auto it = perfByName.find(vm.elementName); it != perfByName.end())
            {
                vm.memoryPressurePct = it->second.pressurePct;
                uint64_t phys = it->second.physicalMemoryMb;
                if (phys == 0) phys = vm.memoryAssignedMb;
                // Demand = phys * pressure / 100, with overflow-safe math
                // (phys * 200 still fits comfortably in uint64).
                vm.memoryDemandMb = phys * vm.memoryPressurePct / 100;
            }
            else
            {
                vm.memoryPressurePct = 0;
                vm.memoryDemandMb    = vm.memoryAssignedMb;
            }
            vm.uptimeMs          = si.UpTime().value_or(0);
            vm.creationTime     = si.CreationTime();
            vm.guestOs          = si.GuestOperatingSystem().value_or(L"");
            vm.configVersion    = si.Version().value_or(L"");
            vm.notes            = si.Notes().value_or(L"");

            // Merge in the realised-settings fields (overrides anything
            // SummaryInformation tried to fill in for these properties).
            if (auto it = settingsByGuid.find(vm.guid); it != settingsByGuid.end())
            {
                auto const& setting = it->second;
                if (!setting.subType.empty())
                {
                    wchar_t last = setting.subType.back();
                    if      (last == L'1') vm.generation = L"Generation 1";
                    else if (last == L'2') vm.generation = L"Generation 2";
                    else                   vm.generation = setting.subType;
                }
                vm.secureBootEnabled = setting.secureBoot;
                vm.secureBootTemplateId = setting.templateId;
                vm.configDataRoot    = setting.configRoot;
                vm.snapshotDataRoot  = setting.snapshotRoot;
                vm.swapFileDataRoot  = setting.swapRoot;
                vm.biosGuid          = setting.biosGuid;
                vm.autoStartAction   = setting.autoStart;
                vm.autoStopAction    = setting.autoStop;
                vm.userSnapshotType  = setting.userSnapshotType;
                vm.automaticCheckpointsEnabled = setting.autoCheckpoints;
                vm.autoStartDelaySeconds = setting.startDelaySeconds;
            }
            if (auto it = securityByGuid.find(vm.guid); it != securityByGuid.end())
            {
                vm.tpmEnabled          = it->second.tpmEnabled;
                vm.encryptStateEnabled = it->second.encryptState;
            }

            auto tasks = si.AsynchronousTasks();
            vm.statusText = BuildStatusTextFromTasks(tasks);

            if (auto it = snapsByGuid.find(vm.guid); it != snapsByGuid.end())
                vm.snapshots = OrderSnapshotsAsTree(std::move(it->second));

            // Memory settings — startup/min/max + whether dynamic memory is on.
            if (auto it = memByGuid.find(vm.guid); it != memByGuid.end())
            {
                vm.memStartupMb         = it->second.startupMb;
                vm.memMinMb             = it->second.minMb;
                vm.memMaxMb             = it->second.maxMb;
                vm.dynamicMemoryEnabled = it->second.dynamic;
            }

            // Guest IPs — pulled from GuestNetworkAdapterConfiguration. The
            // deprecated KVP NetworkAddressIPv4 / IPv6 keys are intentionally
            // not read.
            if (auto it = ipsByGuid.find(vm.guid); it != ipsByGuid.end())
            {
                vm.guestIpv4 = std::move(it->second.v4);
                vm.guestIpv6 = std::move(it->second.v6);
            }

            // Per-NIC detail — flatten the (VM, NIC) inner map into a vector
            // on the VM. Sorted by name for stable display order across
            // refresh ticks (the unordered_map iteration order is otherwise
            // arbitrary and would cause rows to jiggle).
            if (auto it = nicsByVm.find(vm.guid); it != nicsByVm.end())
            {
                for (auto& [nicGuid, nic] : it->second)
                    vm.networkAdapters.push_back(std::move(nic));
                std::sort(vm.networkAdapters.begin(), vm.networkAdapters.end(),
                    [](NetworkAdapter const& a, NetworkAdapter const& b) {
                        if (a.name != b.name) return a.name < b.name;
                        return a.macAddress < b.macAddress;
                    });
            }

            // Disks — HDDs first (most important), then DVDs, then anything
            // else. Within each bucket sort by path so the rows are stable.
            if (auto it = disksByVm.find(vm.guid); it != disksByVm.end())
            {
                vm.disks = std::move(it->second);
                std::sort(vm.disks.begin(), vm.disks.end(),
                    [](VirtualDisk const& a, VirtualDisk const& b) {
                        if (a.kind != b.kind)
                            return static_cast<int>(a.kind) < static_cast<int>(b.kind);
                        return a.path < b.path;
                    });
            }

            // KVP — pull selected guest-reported keys we surface in the
            // flyout. The list is intentionally curated (there are ~20+
            // intrinsic items) so we don't bloat memory with stuff we never
            // show. IP addresses come from GuestNetworkAdapterConfiguration
            // above (the KVP IP keys are deprecated).
            if (auto it = kvpByGuid.find(vm.guid); it != kvpByGuid.end())
            {
                auto const& kv = it->second;
                auto get = [&](wchar_t const* k) -> std::wstring {
                    auto i = kv.find(k);
                    return i == kv.end() ? std::wstring{} : i->second;
                };
                vm.kvpOsName        = get(L"OSName");
                vm.kvpOsVersion     = get(L"OSVersion");
                vm.kvpOsBuildNumber = get(L"OSBuildNumber");
                vm.kvpFqdn          = get(L"FullyQualifiedDomainName");
                vm.kvpIntegrationServicesVersion = get(L"IntegrationServicesVersion");
            }

            next.push_back(std::move(vm));
        }

        std::sort(next.begin(), next.end(),
            [](VirtualMachine const& a, VirtualMachine const& b) {
                return _wcsicmp(a.elementName.c_str(), b.elementName.c_str()) < 0;
            });

        // Merge in history from the prior snapshot, then push a fresh sample
        // for any running VM. Stopped VMs get their history cleared so a
        // restart shows a clean sparkline.
        {
            std::lock_guard<std::mutex> lk(m_lock);
            std::unordered_map<std::wstring, VirtualMachine const*> prev;
            prev.reserve(m_vms.size());
            for (auto const& v : m_vms) prev[v.guid] = &v;

            const auto pollNow = std::chrono::steady_clock::now();
            for (auto& vm : next)
            {
                if (auto it = prev.find(vm.guid); it != prev.end())
                {
                    vm.cpuHistoryPct   = it->second->cpuHistoryPct;
                    vm.memoryHistoryMb = it->second->memoryHistoryMb;
                }
                // Optimistic pending-blink: keep blinking until the VM's state
                // actually moves off the request-time state, or the entry ages
                // out (the job-done clear is the primary signal; this is a
                // safety net for synchronous ops / missed clears).
                if (auto pit = m_pendingStateChange.find(vm.guid);
                    pit != m_pendingStateChange.end())
                {
                    const auto age     = pollNow - pit->second.requestedAt;
                    const bool moved   = (vm.state != pit->second.oldState);
                    const bool minDone = (age >= kMinPendingBlink);
                    const bool expired = (age > std::chrono::seconds(60));
                    // Clear once the state has actually moved (after the minimum
                    // visible blink), or the safety expiry. ShutdownVM relies on
                    // the "moved" path (no watchable completion job); the
                    // RequestStateChange watcher's clear covers the no-move
                    // failure case. LABELED ops (snapshots) skip the moved-clear
                    // — a revert's auto-stop moves the VM to Off mid-op, which
                    // would otherwise stop the blink before the apply finishes;
                    // their watcher/worker is the authoritative clear (60 s
                    // expiry stays as a safety net).
                    const bool movedClear = moved && minDone && pit->second.label.empty();
                    if (movedClear || expired)
                        m_pendingStateChange.erase(pit);
                    else
                    {
                        vm.pendingStateChange = true;
                        // Carry the optimistic label across the poll (UpdateAll
                        // rebuilt `vm` from fresh WMI, so it's blank by default),
                        // and surface it on statusText until Hyper-V's own job
                        // text (e.g. "Creating Checkpoint (35%)") shows up and
                        // takes precedence.
                        vm.pendingJobLabel = pit->second.label;
                        if (!pit->second.label.empty() && vm.statusText.empty())
                            vm.statusText = pit->second.label;
                    }
                }
                if (vm.IsRunning())
                {
                    if (vm.cpuHistoryPct.size() >= kHistoryDepth)
                        vm.cpuHistoryPct.erase(vm.cpuHistoryPct.begin());
                    vm.cpuHistoryPct.push_back(vm.processorLoadPct);

                    if (vm.memoryHistoryMb.size() >= kHistoryDepth)
                        vm.memoryHistoryMb.erase(vm.memoryHistoryMb.begin());
                    // Track demand (moves second-to-second), not assigned
                    // (static when the host isn't ballooning) — matches the
                    // value Hyper-V Manager graphs.
                    vm.memoryHistoryMb.push_back(
                        static_cast<uint32_t>(vm.memoryDemandMb));
                }
                else
                {
                    vm.cpuHistoryPct.clear();
                    vm.memoryHistoryMb.clear();
                }
            }

            m_vms = std::move(next);
        }
    }

    std::vector<VirtualMachine> VMManager::GetAll() const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        return m_vms;
    }

    std::optional<VirtualMachine> VMManager::GetByGuid(std::wstring const& guid) const
    {
        std::lock_guard<std::mutex> lk(m_lock);
        for (auto const& v : m_vms)
        {
            if (_wcsicmp(v.guid.c_str(), guid.c_str()) == 0) return v;
        }
        return std::nullopt;
    }

    // Defined further down (with the WMI helpers); WatchStateChangeJob above it
    // needs to wait on a job.
    static std::pair<bool, std::wstring> WaitForJob(
        hyprv::wmi::WmiScope& scope, hyprv::wmi::WmiObject const& job);

    VMManager::SubToken VMManager::AddOnChanged(OnChangedFn cb)
    {
        std::lock_guard<std::mutex> lk(m_lock);
        SubToken t = m_nextSubToken++;
        m_onChangedSubs.emplace_back(t, std::move(cb));
        return t;
    }

    void VMManager::RemoveOnChanged(SubToken token)
    {
        std::lock_guard<std::mutex> lk(m_lock);
        std::erase_if(m_onChangedSubs, [token](auto const& p) { return p.first == token; });
    }

    VMManager::SubToken VMManager::AddOnError(ErrorFn cb)
    {
        std::lock_guard<std::mutex> lk(m_lock);
        SubToken t = m_nextSubToken++;
        m_onErrorSubs.emplace_back(t, std::move(cb));
        return t;
    }

    void VMManager::RemoveOnError(SubToken token)
    {
        std::lock_guard<std::mutex> lk(m_lock);
        std::erase_if(m_onErrorSubs, [token](auto const& p) { return p.first == token; });
    }

    void VMManager::NotifyError(std::wstring const& vmName, std::wstring const& message)
    {
        std::vector<ErrorFn> subs;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            subs.reserve(m_onErrorSubs.size());
            for (auto const& p : m_onErrorSubs) subs.push_back(p.second);
        }
        for (auto const& cb : subs)
        {
            if (!cb) continue;
            try { cb(vmName, message); }
            catch (...) { /* never throw across the worker boundary */ }
        }
    }

    // Tidy a Hyper-V job ErrorDescription for display. Hyper-V appends
    // " (Virtual machine ID <guid>)" to EVERY line of the stacked message — the
    // error dialog's title already names the VM, so strip the repeated suffix
    // (no regex dependency — a simple scan). Leaves the actual error text
    // (including the actionable last line) intact.
    static std::wstring CleanJobError(std::wstring s)
    {
        const std::wstring marker = L"(Virtual machine ID ";
        size_t pos;
        while ((pos = s.find(marker)) != std::wstring::npos)
        {
            size_t end = s.find(L')', pos);
            if (end == std::wstring::npos) break;
            size_t start = pos;
            if (start > 0 && s[start - 1] == L' ') --start;   // eat the leading space
            s.erase(start, end - start + 1);
        }
        return s;
    }

    void VMManager::MarkPendingStateChange(std::wstring const& guid,
                                           std::wstring const& label)
    {
        if (guid.empty()) return;   // nothing to blink (e.g. snapshot guid unparsed)
        {
            std::lock_guard<std::mutex> lk(m_lock);
            VmState cur = VmState::Unknown;
            for (auto& v : m_vms)
                if (_wcsicmp(v.guid.c_str(), guid.c_str()) == 0)
                {
                    cur = v.state;
                    v.pendingStateChange = true;
                    v.pendingJobLabel = label;
                    // Show the label NOW (before the next poll) on every status
                    // surface — but never clobber a real Hyper-V job text.
                    if (!label.empty() && v.statusText.empty())
                        v.statusText = label;
                    break;
                }
            m_pendingStateChange[guid] = { cur, std::chrono::steady_clock::now(), label };
        }
        NotifyChanged();   // immediate render so the dot blinks on the click
    }

    void VMManager::ClearPendingStateChange(std::wstring const& guid)
    {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            // Capture the optimistic label before erasing so we can wipe its
            // echo from statusText (but only if it's still OUR text — leave a
            // real Hyper-V job string alone).
            std::wstring label;
            if (auto it = m_pendingStateChange.find(guid); it != m_pendingStateChange.end())
            {
                label = it->second.label;
                m_pendingStateChange.erase(it);
                changed = true;
            }
            for (auto& v : m_vms)
                if (_wcsicmp(v.guid.c_str(), guid.c_str()) == 0)
                {
                    if (v.pendingStateChange) { v.pendingStateChange = false; changed = true; }
                    if (!v.pendingJobLabel.empty()) { v.pendingJobLabel.clear(); changed = true; }
                    if (!label.empty() && v.statusText == label) { v.statusText.clear(); changed = true; }
                    break;
                }
        }
        if (changed) NotifyChanged();
    }

    void VMManager::WatchStateChangeJob(std::wstring jobPath, std::wstring guid,
                                        std::wstring vmName)
    {
        if (jobPath.empty()) { ClearPendingStateChange(guid); return; }
        // Detached worker — the UI WmiScope is STA-bound (gotcha #8), so this
        // thread builds its own MTA scope to wait on the job. VMManager is a
        // singleton (effectively immortal), so capturing `this` is safe.
        const auto started = std::chrono::steady_clock::now();
        std::thread([this, jobPath = std::move(jobPath),
                     guid = std::move(guid), vmName = std::move(vmName), started]
        {
            HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try
            {
                hyprv::wmi::WmiScope scope(L"root\\virtualization\\v2");
                auto job = scope.GetByPath(jobPath.c_str());
                if (!job)
                {
                    HyprvAppLog(L"[vmm] WatchStateChangeJob: job not found at %s",
                        jobPath.c_str());
                }
                else
                {
                    auto [ok, desc] = WaitForJob(scope, job);
                    HyprvAppLog(L"[vmm] WatchStateChangeJob vm=%s done ok=%d desc=%s",
                        vmName.c_str(), ok ? 1 : 0, desc.c_str());
                    if (!ok)
                    {
                        std::wstring msg = desc.empty()
                            ? std::wstring{ L"The operation failed." }
                            : CleanJobError(desc);
                        NotifyError(vmName, msg);
                    }
                }
            }
            catch (hyprv::wmi::WmiException const& e)
            {
                HyprvAppLog(L"[vmm] WatchStateChangeJob exception: %s", e.whatW.c_str());
            }
            catch (...) {}
            // The job finished (success OR failure) — stop the optimistic blink
            // even if the VM never left its original state (e.g. a failed start),
            // but hold it for the minimum visible duration so a fast op still
            // pulses noticeably.
            auto elapsed = std::chrono::steady_clock::now() - started;
            if (elapsed < kMinPendingBlink)
                std::this_thread::sleep_for(kMinPendingBlink - elapsed);
            ClearPendingStateChange(guid);
            if (SUCCEEDED(comHr)) CoUninitialize();
        }).detach();
    }

    // Defined later (near DestroyVM); forward-declared so WatchJob can delete
    // VHD files once a destroy job finishes.
    static void DeleteVhdFilesBestEffort(std::vector<std::wstring> const& files);

    void VMManager::WatchJob(std::wstring jobPath, std::wstring guid,
                             std::wstring vmName, std::wstring actionLabel,
                             std::vector<std::wstring> deleteFilesOnSuccess)
    {
        // No job to wait on (shouldn't happen on ret==4096, but be defensive) —
        // stop the optimistic blink immediately so it can't stick.
        if (jobPath.empty()) { ClearPendingStateChange(guid); return; }
        // Detached worker — the UI WmiScope is STA-bound (gotcha #8), so this
        // thread builds its own MTA scope to wait on the job. VMManager is a
        // singleton (effectively immortal), so capturing `this` is safe.
        const auto started = std::chrono::steady_clock::now();
        std::thread([this, jobPath = std::move(jobPath), guid = std::move(guid),
                     vmName = std::move(vmName), actionLabel = std::move(actionLabel),
                     deleteFilesOnSuccess = std::move(deleteFilesOnSuccess), started]
        {
            HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try
            {
                hyprv::wmi::WmiScope scope(L"root\\virtualization\\v2");
                auto job = scope.GetByPath(jobPath.c_str());
                if (!job)
                {
                    HyprvAppLog(L"[vmm] WatchJob: job not found at %s", jobPath.c_str());
                }
                else
                {
                    auto [ok, desc] = WaitForJob(scope, job);
                    HyprvAppLog(L"[vmm] WatchJob (%s) vm=%s done ok=%d desc=%s",
                        actionLabel.c_str(), vmName.c_str(), ok ? 1 : 0, desc.c_str());
                    if (!ok)
                    {
                        std::wstring msg = desc.empty()
                            ? (L"Couldn't " + actionLabel + L".")
                            : CleanJobError(desc);
                        NotifyError(vmName, msg);
                    }
                    // Delete the VM's VHD files only after a CLEAN destroy — the
                    // VM held them open until the job completed.
                    else if (!deleteFilesOnSuccess.empty())
                        DeleteVhdFilesBestEffort(deleteFilesOnSuccess);
                }
            }
            catch (hyprv::wmi::WmiException const& e)
            {
                HyprvAppLog(L"[vmm] WatchJob exception: %s", e.whatW.c_str());
            }
            catch (...) {}
            // Refresh the cache BEFORE clearing the status, so the new/removed
            // snapshot is already in the list when "Taking snapshot…" goes away
            // (otherwise the status clears and the list only catches up on the
            // next regular poll, seconds later). Blocks until a fresh cycle ran.
            KickPollAndWait(15000);
            // The job finished (success OR failure) — stop the optimistic blink,
            // but hold it for the minimum visible duration so a fast op still
            // pulses noticeably (mirrors WatchStateChangeJob). No-op for an empty
            // guid (a caller that didn't want blink handling).
            auto elapsed = std::chrono::steady_clock::now() - started;
            if (elapsed < kMinPendingBlink)
                std::this_thread::sleep_for(kMinPendingBlink - elapsed);
            ClearPendingStateChange(guid);
            if (SUCCEEDED(comHr)) CoUninitialize();
        }).detach();
    }

    // Resolve a VM's display name from the cache for an error-dialog title.
    // Falls back to the GUID — the title only names the VM, so a miss is cosmetic.
    std::wstring VMManager::VmDisplayName(std::wstring const& vmGuid) const
    {
        if (auto vm = GetByGuid(vmGuid); vm && !vm->elementName.empty())
            return vm->elementName;
        return vmGuid;
    }

    void VMManager::KickPoll()
    {
        m_kickPoll.store(true);
        m_stopCv.notify_all();
    }

    void VMManager::KickPollAndWait(int timeoutMs)
    {
        // Register a refresh request, kick the poller, then block until a
        // poll cycle that BEGAN AFTER this call has finished. We can't just
        // wait for m_pollGen to advance: a cycle already in-flight when we
        // kick (which read WMI before the edit landed) would complete first
        // and satisfy that, leaving the cache pre-edit — the dialog then
        // reopens stale until the *next* cycle. The poll loop snapshots
        // m_pollReq at each cycle's start and publishes it to m_pollServiced
        // at the end, so m_pollServiced >= our request proves a fully-fresh
        // cycle ran. Sleep granularity 20 ms; caps at timeoutMs so a stalled
        // poll thread can't hang the UI forever.
        uint64_t req = m_pollReq.fetch_add(1) + 1;
        KickPoll();
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(timeoutMs);
        while (m_pollServiced.load() < req
               && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    bool VMManager::RequestStateChange(std::wstring const& guid, VmStateChange state)
    {
        if (!m_scope) return false;
        try
        {
            // Look up the Msvm_ComputerSystem by GUID (Name is the GUID).
            std::wstring wql =
                L"SELECT * FROM Msvm_ComputerSystem WHERE Name='" + guid + L"'";
            auto rows = m_scope->Query(wql.c_str());
            if (rows.empty())
            {
                HyprvAppLog(L"[vmm] RequestStateChange: VM not found guid=%s",
                    guid.c_str());
                return false;
            }
            auto& cs = rows.front();
            std::wstring vmName = cs.GetString(L"ElementName").value_or(guid);
            // Call RequestStateChange via the raw WmiObject API. The generated
            // typed wrapper insists on a time_point for TimeoutPeriod (which is
            // CIM interval, not absolute time — our codegen maps DateTime to
            // time_point uniformly). Leaving TimeoutPeriod unset lets WMI use
            // the default (null = no timeout), which is what we want.
            auto in = cs.SpawnMethodIn(L"RequestStateChange");
            in.Set(L"RequestedState", static_cast<uint16_t>(state));
            auto out = cs.InvokeMethod(L"RequestStateChange", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(0);
            HyprvAppLog(L"[vmm] RequestStateChange guid=%s state=%u -> ret=%u",
                guid.c_str(), static_cast<uint16_t>(state), ret);
            if (ret == 0 || ret == 4096)
                MarkPendingStateChange(guid);   // optimistic blink, immediate
            if (ret == 4096)
            {
                // Async job started — watch it so a FAILED transition (e.g. a VM
                // that can't start) surfaces an error instead of silently
                // reverting. The `Job` out-param is a CIM REFERENCE (a __PATH
                // string), NOT an embedded object — read it with GetString, not
                // GetObject (which only handles VT_UNKNOWN and returns nullopt
                // for a reference, the bug that made this silently no-op).
                std::wstring jobPath = out.GetString(L"Job").value_or(std::wstring{});
                HyprvAppLog(L"[vmm] RequestStateChange job=%s", jobPath.c_str());
                WatchStateChangeJob(std::move(jobPath), guid, vmName);
                return true;
            }
            if (ret != 0)
            {
                // WMI rejected the request up front (e.g. 32775 invalid state).
                NotifyError(vmName,
                    L"The requested operation could not be started (Hyper-V "
                    L"returned error " + std::to_wstring(ret) + L").");
                return false;
            }
            return true;   // ret == 0: synchronous success
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] RequestStateChange exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::TakeSnapshot(std::wstring const& guid)
    {
        if (!m_snapSvc || !m_scope) return false;
        try
        {
            std::wstring wql =
                L"SELECT * FROM Msvm_ComputerSystem WHERE Name='" + guid + L"'";
            auto rows = m_scope->Query(wql.c_str());
            if (rows.empty()) return false;
            std::wstring vmName = rows.front().GetString(L"ElementName").value_or(guid);
            // SnapshotType 2 = Full (memory + disk). 3 = Disk-only. We always
            // pick Full to match the Hyper-V Manager UI default.
            //
            // Invoke manually (not the typed CreateSnapshot wrapper) for TWO
            // reasons, both codegen bugs around object-typed params:
            //   1. AffectedSystem is a CIM **Reference** — it must be set as the
            //      object's __PATH string (via .Path()), NOT as an embedded
            //      object. The generated wrapper does in.Set(name, WmiObject),
            //      which marshals VT_UNKNOWN and Hyper-V rejects with
            //      WBEM_E_TYPE_MISMATCH (0x80041005, "Put failed for
            //      AffectedSystem"). THIS is why snapshots never worked — the
            //      typed CreateSnapshot/ApplySnapshot/DestroySnapshot wrappers
            //      all have the same latent bug. (Same REF-vs-embedded trap as
            //      AddFeatureSettings' AffectedConfiguration, gotcha #28.)
            //   2. The async Job out-param is ALSO a reference (a __PATH string);
            //      read it with GetString, not GetObject (which only handles
            //      VT_UNKNOWN), then watch the job so a failed checkpoint
            //      surfaces an error instead of dying silently.
            auto in = m_snapSvc.SpawnMethodIn(L"CreateSnapshot");
            in.Set(L"AffectedSystem", rows.front().Path());
            in.Set(L"SnapshotSettings", std::wstring{ L"" });
            in.Set(L"SnapshotType", static_cast<uint16_t>(2));
            auto out = m_snapSvc.InvokeMethod(L"CreateSnapshot", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(0);
            HyprvAppLog(L"[vmm] TakeSnapshot guid=%s ret=%u", guid.c_str(), ret);
            if (ret == 4096)
            {
                std::wstring jobPath = out.GetString(L"Job").value_or(std::wstring{});
                MarkPendingStateChange(guid, L"Taking snapshot…");
                WatchJob(std::move(jobPath), guid, vmName, L"create the snapshot");
                return true;
            }
            if (ret != 0)
            {
                NotifyError(vmName,
                    L"Couldn't create the snapshot (Hyper-V returned error "
                    + std::to_wstring(ret) + L").");
                return false;
            }
            return true;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] TakeSnapshot exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::ApplySnapshot(std::wstring const& vmGuid, std::wstring const& snapshotPath)
    {
        if (!m_snapSvc || !m_scope) return false;
        std::wstring vmName = VmDisplayName(vmGuid);
        // ApplySnapshot needs the VM Off OR Saved (a Running apply returns 32775;
        // both Off and Saved are verified to accept it). When it's Running/Paused
        // we SAVE it first, apply, then START it back — matching the spirit of
        // Hyper-V Manager and letting a failed apply resume the original state
        // from the save (no data loss). The caller has already confirmed.
        auto vmOpt = GetByGuid(vmGuid);
        if (!vmOpt || !(vmOpt->IsOff() || vmOpt->IsSaved()))
        {
            MarkPendingStateChange(vmGuid, L"Reverting snapshot…");
            SaveThenApplySnapshot(vmGuid, snapshotPath, vmName);
            return true;
        }
        try
        {
            auto snap = m_scope->GetByPath(snapshotPath.c_str());
            if (!snap) return false;
            // Manual invoke — Snapshot is a REF (.Path(), not the embedded object)
            // and the Job out-param is a reference too (see TakeSnapshot).
            auto in = m_snapSvc.SpawnMethodIn(L"ApplySnapshot");
            in.Set(L"Snapshot", snap.Path());
            auto out = m_snapSvc.InvokeMethod(L"ApplySnapshot", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(0);
            HyprvAppLog(L"[vmm] ApplySnapshot vm=%s ret=%u", vmGuid.c_str(), ret);
            if (ret == 4096)
            {
                std::wstring jobPath = out.GetString(L"Job").value_or(std::wstring{});
                MarkPendingStateChange(vmGuid, L"Applying snapshot…");
                WatchJob(std::move(jobPath), vmGuid, vmName, L"apply the snapshot");
                return true;
            }
            if (ret != 0)
            {
                NotifyError(vmName,
                    L"Couldn't apply the snapshot (Hyper-V returned error "
                    + std::to_wstring(ret) + L").");
                return false;
            }
            return true;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] ApplySnapshot exception: %s", e.whatW.c_str());
            return false;
        }
    }

    void VMManager::SaveThenApplySnapshot(std::wstring vmGuid, std::wstring snapshotPath,
                                          std::wstring vmName)
    {
        const auto started = std::chrono::steady_clock::now();
        std::thread([this, vmGuid, snapshotPath, vmName, started]()
        {
            HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::wstring errMsg;
            bool resumeAfter = false;   // start the VM back when we're done
            std::wstring csWql =
                L"SELECT * FROM Msvm_ComputerSystem WHERE Name='" + vmGuid + L"'";
            // EnabledState: 2=Running, 3=Off, 6=Saved. RequestStateChange args:
            // 2=Enabled (start/resume), 6=Offline (save).
            auto requestState = [&](hyprv::wmi::WmiScope& sc, uint16_t reqState) -> bool
            {
                auto rows = sc.Query(csWql.c_str());
                if (rows.empty()) return false;
                auto& cs = rows.front();
                auto in = cs.SpawnMethodIn(L"RequestStateChange");
                in.Set(L"RequestedState", reqState);
                auto out = cs.InvokeMethod(L"RequestStateChange", in);
                uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(0);
                if (ret == 4096)
                {
                    std::wstring jp = out.GetString(L"Job").value_or(std::wstring{});
                    if (!jp.empty())
                        if (auto job = sc.GetByPath(jp.c_str())) WaitForJob(sc, job);
                    return true;
                }
                return ret == 0;
            };
            auto waitForState = [&](hyprv::wmi::WmiScope& sc, uint16_t want, int maxQuarterSec)
            {
                for (int i = 0; i < maxQuarterSec; ++i)
                {
                    auto r = sc.Query(csWql.c_str());
                    if (!r.empty() && r.front().GetUInt16(L"EnabledState").value_or(0) == want)
                        return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
                return false;
            };
            try
            {
                hyprv::wmi::WmiScope scope(L"root\\virtualization\\v2");
                auto rows = scope.Query(csWql.c_str());
                if (rows.empty())
                {
                    errMsg = L"The virtual machine was not found.";
                }
                else
                {
                    uint16_t st = rows.front().GetUInt16(L"EnabledState").value_or(0);
                    // 1) SAVE the VM if it's not already Off/Saved (Running/Paused).
                    //    Saving preserves the current state so a failed apply can
                    //    resume it.
                    if (st != 3 && st != 6)
                    {
                        resumeAfter = true;
                        HyprvAppLog(L"[vmm] SaveThenApply: saving vm=%s", vmName.c_str());
                        if (!requestState(scope, 6))
                            errMsg = L"Couldn't save the virtual machine.";
                        else if (!waitForState(scope, 6 /*Saved*/, 480 /*~120 s*/))
                            errMsg = L"The virtual machine didn't finish saving.";
                    }
                }
                // 2) Apply the snapshot (own MTA snapshot service — gotcha #8).
                if (errMsg.empty())
                {
                    auto snap = scope.GetByPath(snapshotPath.c_str());
                    if (!snap)
                    {
                        errMsg = L"The snapshot could not be found.";
                    }
                    else
                    {
                        hyprv::wmi::hyperv::Msvm_VirtualSystemSnapshotService svc(
                            scope.GetInstance(L"Msvm_VirtualSystemSnapshotService"));
                        auto in = svc.SpawnMethodIn(L"ApplySnapshot");
                        in.Set(L"Snapshot", snap.Path());
                        auto out = svc.InvokeMethod(L"ApplySnapshot", in);
                        uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(0);
                        HyprvAppLog(L"[vmm] SaveThenApply ApplySnapshot vm=%s ret=%u",
                            vmName.c_str(), ret);
                        if (ret == 4096)
                        {
                            std::wstring jp = out.GetString(L"Job").value_or(std::wstring{});
                            if (!jp.empty())
                                if (auto job = scope.GetByPath(jp.c_str()))
                                {
                                    auto [ok, desc] = WaitForJob(scope, job);
                                    if (!ok) errMsg = desc.empty()
                                        ? std::wstring{ L"Couldn't apply the snapshot." }
                                        : CleanJobError(desc);
                                }
                        }
                        else if (ret != 0)
                        {
                            errMsg = L"Couldn't apply the snapshot (Hyper-V returned error "
                                   + std::to_wstring(ret) + L").";
                        }
                    }
                }
                // 3) Resume: start the VM back. On success this enters the
                //    snapshot's state; on a failed apply the VM is still Saved, so
                //    Start resumes the ORIGINAL state (no data loss). Only start
                //    from Off/Saved (don't poke a still-running/transitional VM,
                //    e.g. if the save itself failed).
                if (resumeAfter)
                {
                    auto r = scope.Query(csWql.c_str());
                    uint16_t st2 = r.empty() ? 0 : r.front().GetUInt16(L"EnabledState").value_or(0);
                    if (st2 == 3 || st2 == 6)
                    {
                        HyprvAppLog(L"[vmm] SaveThenApply: resuming vm=%s (st=%u)",
                            vmName.c_str(), st2);
                        requestState(scope, 2 /*Enabled = start/resume*/);
                    }
                }
            }
            catch (hyprv::wmi::WmiException const& e)
            {
                HyprvAppLog(L"[vmm] SaveThenApply exception: %s", e.whatW.c_str());
                errMsg = L"The operation failed.";
            }
            catch (...) {}
            if (!errMsg.empty()) NotifyError(vmName, errMsg);
            // Refresh the cache (new state + post-revert snapshot/"Now" position)
            // BEFORE clearing the status, so the list is fresh when it goes away.
            KickPollAndWait(15000);
            auto elapsed = std::chrono::steady_clock::now() - started;
            if (elapsed < kMinPendingBlink)
                std::this_thread::sleep_for(kMinPendingBlink - elapsed);
            ClearPendingStateChange(vmGuid);
            if (SUCCEEDED(comHr)) CoUninitialize();
        }).detach();
    }

    bool VMManager::RenameSnapshot(std::wstring const& snapshotPath,
                                   std::wstring const& newName)
    {
        if (!m_scope) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto snapObj = m_scope->GetByPath(snapshotPath.c_str());
            if (!snapObj) return false;
            // A snapshot IS a Msvm_VirtualSystemSettingData; renaming it is the
            // same shape as RenameVM but on the snapshot VSSD — set ElementName +
            // ModifySystemSettings (DTD 2.0 XML via GetCimXml).
            hyprv::wmi::hyperv::Msvm_VirtualSystemSettingData vssd(std::move(snapObj));
            vssd.ElementName(newName);
            auto r = vsms.ModifySystemSettings(vssd.GetCimXml());
            HyprvAppLog(L"[vmm] RenameSnapshot name=%s ret=%u",
                newName.c_str(), r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok) HyprvAppLog(L"[vmm] RenameSnapshot job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] RenameSnapshot exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::DeleteSnapshot(std::wstring const& vmGuid, std::wstring const& snapshotPath,
                                   bool subtree)
    {
        if (!m_snapSvc || !m_scope) return false;
        try
        {
            auto snap = m_scope->GetByPath(snapshotPath.c_str());
            if (!snap) return false;
            std::wstring vmName = VmDisplayName(vmGuid);
            // Manual invoke — the snapshot param is a REF (.Path(), not the
            // embedded object) and the Job out-param is a reference too (see
            // TakeSnapshot). The two destroy methods differ only in the method
            // name + the (also-REF) in-param name.
            const wchar_t* method = subtree ? L"DestroySnapshotTree" : L"DestroySnapshot";
            const wchar_t* param  = subtree ? L"SnapshotSettingData" : L"AffectedSnapshot";
            auto in = m_snapSvc.SpawnMethodIn(method);
            in.Set(param, snap.Path());
            auto out = m_snapSvc.InvokeMethod(method, in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(0);
            HyprvAppLog(L"[vmm] DeleteSnapshot path=%s subtree=%d ret=%u",
                snapshotPath.c_str(), subtree ? 1 : 0, ret);
            if (ret == 4096)
            {
                std::wstring jobPath = out.GetString(L"Job").value_or(std::wstring{});
                MarkPendingStateChange(vmGuid,
                    subtree ? L"Deleting snapshot subtree…" : L"Deleting snapshot…");
                WatchJob(std::move(jobPath), vmGuid, vmName,
                         subtree ? L"delete the snapshot subtree"
                                 : L"delete the snapshot");
                return true;
            }
            if (ret != 0)
            {
                NotifyError(vmName,
                    L"Couldn't delete the snapshot (Hyper-V returned error "
                    + std::to_wstring(ret) + L").");
                return false;
            }
            return true;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] DeleteSnapshot exception: %s", e.whatW.c_str());
            return false;
        }
    }

    // Helper — find the Msvm_Keyboard associated to a VM's ComputerSystem.
    // Returns an empty WmiObject if the lookup fails (rare; mostly happens on
    // a VM that's mid-creation or in a weird state). Caller checks .Valid().
    static hyprv::wmi::WmiObject FindKeyboard(
        hyprv::wmi::WmiScope& scope, std::wstring const& guid)
    {
        std::wstring wql =
            L"SELECT * FROM Msvm_ComputerSystem WHERE Name='" + guid + L"'";
        auto rows = scope.Query(wql.c_str());
        if (rows.empty()) return {};
        // Msvm_Keyboard is associated to Msvm_ComputerSystem via Msvm_SystemDevice.
        // Mirrors VMPlex's `Msvm.GetAssociated<IMsvm_Keyboard>("Msvm_SystemDevice")`.
        // GetAssociated arg order: (assocClass, resultClass).
        auto kbs = rows.front().GetAssociated(
            L"Msvm_SystemDevice",   // assocClass
            L"Msvm_Keyboard");      // resultClass
        if (kbs.empty()) return {};
        return std::move(kbs.front());
    }

    bool VMManager::TypeCtrlAltDel(std::wstring const& guid)
    {
        if (!m_scope) return false;
        try
        {
            auto kbObj = FindKeyboard(*m_scope, guid);
            if (!kbObj)
            {
                HyprvAppLog(L"[vmm] TypeCtrlAltDel: Msvm_Keyboard not found for %s",
                    guid.c_str());
                return false;
            }
            hyprv::wmi::hyperv::Msvm_Keyboard kb(std::move(kbObj));
            uint32_t ret = kb.TypeCtrlAltDel();
            HyprvAppLog(L"[vmm] TypeCtrlAltDel guid=%s ret=%u", guid.c_str(), ret);
            return ret == 0 || ret == 4096;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] TypeCtrlAltDel exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::TypeText(std::wstring const& guid, std::wstring const& text)
    {
        if (!m_scope) return false;
        if (text.empty()) return true;   // nothing to type — trivially "success"
        try
        {
            auto kbObj = FindKeyboard(*m_scope, guid);
            if (!kbObj)
            {
                HyprvAppLog(L"[vmm] TypeText: Msvm_Keyboard not found for %s",
                    guid.c_str());
                return false;
            }
            hyprv::wmi::hyperv::Msvm_Keyboard kb(std::move(kbObj));
            uint32_t ret = kb.TypeText(text);
            HyprvAppLog(L"[vmm] TypeText guid=%s len=%zu ret=%u",
                guid.c_str(), text.size(), ret);
            return ret == 0 || ret == 4096;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] TypeText exception: %s", e.whatW.c_str());
            return false;
        }
    }

    // Best-effort delete of VHD files left behind after a VM is destroyed.
    // DeleteFileW only; logs each result. Used by DestroyVM's "also delete VHDs".
    static void DeleteVhdFilesBestEffort(std::vector<std::wstring> const& files)
    {
        for (auto const& f : files)
        {
            if (f.empty()) continue;
            BOOL ok = DeleteFileW(f.c_str());
            HyprvAppLog(L"[vmm] DestroyVM delete VHD '%s' ok=%d err=%lu",
                f.c_str(), ok ? 1 : 0, ok ? 0ul : GetLastError());
        }
    }

    bool VMManager::DestroyVM(std::wstring const& guid, bool deleteVhds)
    {
        if (!m_scope) return false;
        try
        {
            // Resolve VirtualSystemManagementService on the UI thread's scope.
            // We can't reuse the poller-thread VSMS because it lives in a
            // different COM apartment.
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms)
            {
                HyprvAppLog(L"[vmm] DestroyVM: VSMS instance not found");
                return false;
            }
            std::wstring wql =
                L"SELECT * FROM Msvm_ComputerSystem WHERE Name='" + guid + L"'";
            auto rows = m_scope->Query(wql.c_str());
            if (rows.empty())
            {
                HyprvAppLog(L"[vmm] DestroyVM: VM not found guid=%s", guid.c_str());
                return false;
            }
            std::wstring vmName = rows.front().GetString(L"ElementName").value_or(guid);

            // Capture the VHD file paths NOW (while the VM still exists) so we
            // can delete them once the destroy job releases them. Pass-through
            // disks have no file (path is a host-disk label) — skip them.
            std::vector<std::wstring> vhdFiles;
            if (deleteVhds)
            {
                for (auto const& d : GetHardDisks(guid))
                    if (!d.isPassthrough && !d.path.empty())
                        vhdFiles.push_back(d.path);
                HyprvAppLog(L"[vmm] DestroyVM: will delete %zu VHD file(s) after destroy",
                    vhdFiles.size());
            }
            // Manual invoke — AffectedSystem is a REF (.Path(), not the embedded
            // object; the typed DestroySystem wrapper had the same latent
            // type-mismatch bug as the snapshot methods) and the Job out-param is
            // a reference too (see TakeSnapshot). A delete can fail async
            // (locked/in-use files) and must surface, not vanish.
            auto in = vsms.SpawnMethodIn(L"DestroySystem");
            in.Set(L"AffectedSystem", rows.front().Path());
            auto out = vsms.InvokeMethod(L"DestroySystem", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(0);
            HyprvAppLog(L"[vmm] DestroyVM guid=%s ret=%u", guid.c_str(), ret);
            if (ret == 4096)
            {
                std::wstring jobPath = out.GetString(L"Job").value_or(std::wstring{});
                MarkPendingStateChange(guid);   // blink the dot while the job runs
                // The watcher deletes the VHD files once the destroy completes
                // cleanly (the VM holds them open until then).
                WatchJob(std::move(jobPath), guid, vmName, L"delete the virtual machine",
                         std::move(vhdFiles));
                return true;
            }
            if (ret != 0)
            {
                NotifyError(vmName,
                    L"Couldn't delete the virtual machine (Hyper-V returned error "
                    + std::to_wstring(ret) + L").");
                return false;
            }
            // Synchronous success — the VM is already gone, so the files are
            // free to delete now.
            DeleteVhdFilesBestEffort(vhdFiles);
            return true;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] DestroyVM exception: %s", e.whatW.c_str());
            return false;
        }
    }

    std::wstring VMManager::GetDefaultVhdDirectory() const
    {
        if (!m_scope) return {};
        try
        {
            // Host management-service settings singleton carries the default
            // VHD + config dirs the New-VM cmdlet / Hyper-V Manager use.
            auto rows = m_scope->Query(
                L"SELECT * FROM Msvm_VirtualSystemManagementServiceSettingData");
            if (rows.empty()) return {};
            return rows.front().GetString(L"DefaultVirtualHardDiskPath")
                .value_or(std::wstring{});
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GetDefaultVhdDirectory exception: %s", e.whatW.c_str());
            return {};
        }
    }

    std::wstring VMManager::GetDefaultVmDirectory() const
    {
        if (!m_scope) return {};
        try
        {
            auto rows = m_scope->Query(
                L"SELECT * FROM Msvm_VirtualSystemManagementServiceSettingData");
            if (rows.empty()) return {};
            return rows.front().GetString(L"DefaultExternalDataRoot")
                .value_or(std::wstring{});
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GetDefaultVmDirectory exception: %s", e.whatW.c_str());
            return {};
        }
    }

    std::wstring VMManager::CreateVM(NewVmConfig const& cfg)
    {
        if (!m_scope || cfg.name.empty()) return {};
        const std::wstring subType = (cfg.generation == 1)
            ? L"Microsoft:Hyper-V:SubType:1"
            : L"Microsoft:Hyper-V:SubType:2";
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms)
            {
                HyprvAppLog(L"[vmm] CreateVM: VSMS instance not found");
                return {};
            }

            // ---- DefineSystem: spawn a fresh Msvm_VirtualSystemSettingData
            // (ElementName + VirtualSystemSubType), serialize to embedded CIM
            // XML, and hand it to DefineSystem. ResourceSettings is an empty
            // array (memory/processor get their defaults, which we then tune
            // via the verified Set* helpers); ReferenceConfiguration is left
            // NULL (brand-new VM, not based on a reference). VERIFIED reversibly.
            auto cls = m_scope->GetClass(L"Msvm_VirtualSystemSettingData");
            if (!cls)
            {
                HyprvAppLog(L"[vmm] CreateVM: VSSD class not found");
                return {};
            }
            CComPtr<IWbemClassObject> inst;
            if (FAILED(cls.Raw()->SpawnInstance(0, &inst)) || !inst)
            {
                HyprvAppLog(L"[vmm] CreateVM: SpawnInstance failed");
                return {};
            }
            hyprv::wmi::WmiObject ssd(m_scope.get(), inst);
            ssd.Set(L"ElementName", cfg.name);
            ssd.Set(L"VirtualSystemSubType", subType);
            // Custom VM location ("store the virtual machine in a different
            // location"): config + checkpoints + smart-paging file all root
            // here. Verified — DefineSystem honors these and auto-creates the
            // directory. Empty leaves the host default (DefaultExternalDataRoot).
            if (!cfg.vmStoragePath.empty())
            {
                ssd.Set(L"ConfigurationDataRoot", cfg.vmStoragePath);
                ssd.Set(L"SnapshotDataRoot",      cfg.vmStoragePath);
                ssd.Set(L"SwapFileDataRoot",      cfg.vmStoragePath);
            }

            auto in = vsms.SpawnMethodIn(L"DefineSystem");
            in.Set(L"SystemSettings", ssd.GetCimXml());
            in.SetStringArray(L"ResourceSettings", std::vector<std::wstring>{});
            auto out = vsms.InvokeMethod(L"DefineSystem", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
            HyprvAppLog(L"[vmm] CreateVM DefineSystem name='%s' gen=%d ret=%u",
                cfg.name.c_str(), cfg.generation, ret);
            if (ret == 4096)
            {
                auto [ok, desc] = WaitForJob(*m_scope,
                    out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
                if (!ok)
                {
                    HyprvAppLog(L"[vmm] CreateVM DefineSystem job failed: %s", desc.c_str());
                    return {};
                }
            }
            else if (ret != 0)
            {
                NotifyError(cfg.name,
                    L"Couldn't create the virtual machine (Hyper-V returned error "
                    + std::to_wstring(ret) + L").");
                return {};
            }

            // Resolve the new VM's GUID from ResultingSystem (a CIM ref path).
            // Prefer GetByPath → Name; fall back to parsing Name="..." out of
            // the path so a GetByPath quirk doesn't lose a freshly-made VM.
            std::wstring resSysPath = out.GetString(L"ResultingSystem").value_or(std::wstring{});
            std::wstring guid;
            if (!resSysPath.empty())
            {
                if (auto sys = m_scope->GetByPath(resSysPath.c_str()))
                    guid = sys.GetString(L"Name").value_or(std::wstring{});
                if (guid.empty())
                {
                    // Parse Name="<guid>" from the ref path.
                    auto pos = resSysPath.find(L"Name=\"");
                    if (pos != std::wstring::npos)
                    {
                        pos += 6;
                        auto end = resSysPath.find(L'"', pos);
                        if (end != std::wstring::npos)
                            guid = resSysPath.substr(pos, end - pos);
                    }
                }
            }
            if (guid.empty())
            {
                HyprvAppLog(L"[vmm] CreateVM: couldn't resolve new VM GUID from '%s'",
                    resSysPath.c_str());
                return {};
            }
            HyprvAppLog(L"[vmm] CreateVM: new VM guid=%s", guid.c_str());

            // ---- Memory + processor. SetMemoryConfig ignores min/max when
            // dynamic is off; when on, use Hyper-V Manager's New-VM defaults
            // (min 512 MB capped at startup, max 1 TB). These are full
            // ModifySystemSettings writes and must succeed for a sane VM.
            {
                MemoryConfig mem{};
                mem.startupMb      = cfg.startupMemoryMb;
                mem.dynamicEnabled = cfg.dynamicMemory;
                mem.minMb          = std::min<uint64_t>(512, cfg.startupMemoryMb);
                mem.maxMb          = std::max<uint64_t>(cfg.startupMemoryMb, 1048576);
                if (!SetMemoryConfig(guid, mem))
                    HyprvAppLog(L"[vmm] CreateVM: SetMemoryConfig failed (continuing)");
            }
            {
                ProcessorConfig cpu{};
                cpu.count = static_cast<uint16_t>(cfg.cpuCount < 1 ? 1 : cfg.cpuCount);
                if (!SetProcessorConfig(guid, cpu))
                    HyprvAppLog(L"[vmm] CreateVM: SetProcessorConfig failed (continuing)");
            }

            // ---- The rest are best-effort: a failure leaves a usable VM the
            // user can finish in Settings, so we log and press on rather than
            // tearing the whole thing down.

            // Gen 2 has NO SCSI controller from DefineSystem (verified) — add
            // one so storage has somewhere to land. Gen 1 already has IDE.
            if (cfg.generation == 2)
            {
                if (!AddScsiController(guid))
                    HyprvAppLog(L"[vmm] CreateVM: AddScsiController failed (continuing)");
            }

            // Storage.
            switch (cfg.diskMode)
            {
            case NewVmConfig::Disk::CreateNew:
                if (cfg.newVhdSizeBytes > 0 && !cfg.vhdPath.empty())
                {
                    if (!CreateAndAttachVhd(guid, cfg.vhdPath, cfg.newVhdSizeBytes,
                                            cfg.dynamicVhd))
                        HyprvAppLog(L"[vmm] CreateVM: CreateAndAttachVhd failed (continuing)");
                }
                break;
            case NewVmConfig::Disk::UseExisting:
                if (!cfg.vhdPath.empty())
                {
                    if (!AttachVhd(guid, cfg.vhdPath))
                        HyprvAppLog(L"[vmm] CreateVM: AttachVhd failed (continuing)");
                }
                break;
            case NewVmConfig::Disk::None:
            default:
                break;
            }

            // Network: a fresh NIC connected to the chosen switch.
            if (!cfg.switchName.empty())
            {
                if (!AddNetworkAdapter(guid, cfg.switchName))
                    HyprvAppLog(L"[vmm] CreateVM: AddNetworkAdapter failed (continuing)");
            }

            // Install media: add a DVD drive, mount the ISO, and make the VM
            // boot from it first so the installer runs on first start.
            if (!cfg.isoPath.empty())
            {
                if (!AddDvdDrive(guid))
                    HyprvAppLog(L"[vmm] CreateVM: AddDvdDrive failed (continuing)");
                // Mount the ISO into the (only) drive — re-query for its ref.
                auto dvds = GetDvdDrives(guid);
                if (!dvds.empty())
                {
                    auto const& d = dvds.front();
                    if (!SetDvdMedia(guid, d.driveRef, d.mediaRef, cfg.isoPath))
                        HyprvAppLog(L"[vmm] CreateVM: SetDvdMedia failed (continuing)");
                    else
                        PromoteDvdToBootFront(guid, cfg.generation);
                }
            }

            KickPoll();   // surface the new VM in the next snapshot
            return guid;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] CreateVM exception: %s", e.whatW.c_str());
            return {};
        }
    }

    // Best-effort: move the DVD/CD entry to the front of the boot order so a
    // freshly-created VM with an install ISO boots the installer. Gen 2 uses
    // the BootSourceOrder ref list (match the entry whose description mentions
    // DVD/CD); Gen 1 uses the uint16 BootOrder codes (CD/DVD = 1). Both write
    // paths want the VM Off — it always is right after create.
    void VMManager::PromoteDvdToBootFront(std::wstring const& guid, int generation)
    {
        try
        {
            if (generation == 2)
            {
                auto order = GetBootOrder(guid);
                if (order.size() < 2) return;
                // Find the DVD by device KIND (the description is friendly now,
                // but matching the kind is robust regardless of label wording —
                // the old string match against the raw "EFI SCSI Device" never
                // fired, so a created VM didn't boot its install media).
                int dvdIdx = -1;
                for (size_t i = 0; i < order.size(); ++i)
                    if (order[i].kind == BootKind::Dvd) { dvdIdx = static_cast<int>(i); break; }
                if (dvdIdx <= 0) return;   // not found, or already first
                std::vector<std::wstring> refs;
                refs.push_back(order[dvdIdx].ref);
                for (size_t i = 0; i < order.size(); ++i)
                    if (static_cast<int>(i) != dvdIdx) refs.push_back(order[i].ref);
                if (!SetBootOrder(guid, refs))
                    HyprvAppLog(L"[vmm] CreateVM: boot-order promote (gen2) failed");
            }
            else
            {
                auto codes = GetBootOrderGen1(guid);
                if (codes.size() < 2) return;
                int dvdIdx = -1;
                for (size_t i = 0; i < codes.size(); ++i)
                    if (codes[i].code == 1) { dvdIdx = static_cast<int>(i); break; }  // 1 = CD/DVD
                if (dvdIdx <= 0) return;
                std::vector<uint16_t> ordered;
                ordered.push_back(codes[dvdIdx].code);
                for (size_t i = 0; i < codes.size(); ++i)
                    if (static_cast<int>(i) != dvdIdx) ordered.push_back(codes[i].code);
                if (!SetBootOrderGen1(guid, ordered))
                    HyprvAppLog(L"[vmm] CreateVM: boot-order promote (gen1) failed");
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] CreateVM PromoteDvdToBootFront exception: %s", e.whatW.c_str());
        }
    }

    // ---- VM edit operations (settings editor) -----------------------------

    // Private helper: fetch the active (Realized) Msvm_VirtualSystemSettingData
    // for a VM. Each VM has one Realized VSSD that holds the live config + N
    // snapshot VSSDs. The Realized one's InstanceID is "Microsoft:<GUID>".
    static hyprv::wmi::WmiObject FindRealizedVssd(
        hyprv::wmi::WmiScope& scope, std::wstring const& guid)
    {
        std::wstring wql =
            L"SELECT * FROM Msvm_VirtualSystemSettingData "
            L"WHERE VirtualSystemType='Microsoft:Hyper-V:System:Realized' "
            L"AND InstanceID='Microsoft:" + guid + L"'";
        auto rows = scope.Query(wql.c_str());
        if (rows.empty()) return {};
        return std::move(rows.front());
    }

    // Wait for an Msvm_ConcreteJob to reach a terminal state. Returns
    // {ok, message} — ok=false means the job failed (or is still running
    // after the timeout); message carries the ErrorDescription if any.
    //
    // Hyper-V's ModifySystemSettings / ModifyResourceSettings return
    // ReturnValue=4096 to mean "parameters validated, async job started".
    // The actual modification can still fail asynchronously — without
    // waiting on the job we'd report success but the change wouldn't
    // stick (caused the "saves but doesn't persist" bug).
    //
    // Polling loop on the UI thread; typical jobs complete in <1s so the
    // brief block is tolerable. If we ever see jobs that take longer
    // we'll move this to a background dispatcher + async UI feedback.
    static std::pair<bool, std::wstring> WaitForJob(
        hyprv::wmi::WmiScope& scope, hyprv::wmi::WmiObject const& job)
    {
        if (!job) return { true, {} };
        const std::wstring jobPath = job.Path();
        if (jobPath.empty()) return { true, {} };

        // CIM JobState — terminal values:
        //   7  = Completed         (success)
        //   8  = Terminated        (cancelled)
        //   9  = Killed
        //   10 = Exception         (job threw)
        //   11 = Service           (transient — keep polling)
        for (int i = 0; i < 600; ++i)   // up to 60s @ 100ms
        {
            auto fresh = scope.GetByPath(jobPath.c_str());
            if (!fresh) break;
            auto state = fresh.GetUInt16(L"JobState").value_or(0);
            if (state >= 7 && state != 11)
            {
                auto err  = fresh.GetUInt16(L"ErrorCode").value_or(0);
                auto desc = fresh.GetString(L"ErrorDescription").value_or(std::wstring{});
                bool ok = (state == 7 && err == 0);
                return { ok, std::move(desc) };
            }
            Sleep(100);
        }
        return { false, L"Job timed out" };
    }

    // Private helper: fetch the Msvm_MemorySettingData (or other resource
    // setting) associated to a VM's Realized VSSD. WMI association class is
    // Msvm_VirtualSystemSettingDataComponent.
    //
    // GetAssociated arg order: (assocClass, resultClass) — easy to invert.
    // Msvm_VirtualSystemSettingDataComponent is the relationship; the
    // memory/processor/etc. setting data class is the *target* we want
    // back, so it goes in `resultClass`.
    static hyprv::wmi::WmiObject FindResourceForVm(
        hyprv::wmi::WmiScope& scope, std::wstring const& guid,
        const wchar_t* resourceClass)
    {
        auto vssd = FindRealizedVssd(scope, guid);
        if (!vssd) return {};
        auto items = vssd.GetAssociated(
            L"Msvm_VirtualSystemSettingDataComponent",   // assocClass
            resourceClass);                              // resultClass
        if (items.empty()) return {};
        return std::move(items.front());
    }

    // The Msvm_SecuritySettingData for a VM associates to its realised VSSD,
    // but NOT via Msvm_VirtualSystemSettingDataComponent (which links resource
    // settings like memory/processor/NIC). Filter by ResultClass only — the
    // same traversal Get-CimAssociatedInstance -ResultClassName performs. The
    // returned object carries __PATH (needed for GetCimXml).
    static hyprv::wmi::WmiObject FindSecuritySettingData(
        hyprv::wmi::WmiScope& scope, std::wstring const& guid)
    {
        auto vssd = FindRealizedVssd(scope, guid);
        if (!vssd) return {};
        auto items = vssd.GetAssociated(nullptr, L"Msvm_SecuritySettingData");
        if (items.empty()) return {};
        return std::move(items.front());
    }

    // ---- vTPM key-protector provisioning (root\Microsoft\Windows\Hgs) -------
    // Enabling vTPM requires a key protector. For a host-local (non-shielded)
    // VM the convention — matching Enable-VMTPM — is a key protector owned by
    // a guardian named "UntrustedGuardian" with self-signed certs. Look it up,
    // creating it on first use via the static NewByGenerateCertificates method.
    // Returns an empty WmiObject on failure.
    static hyprv::wmi::WmiObject GetOrCreateUntrustedGuardian(
        hyprv::wmi::WmiScope& hgs)
    {
        auto rows = hgs.Query(
            L"SELECT * FROM MSFT_HgsGuardian WHERE Name='UntrustedGuardian'");
        if (!rows.empty()) return std::move(rows.front());

        // Create it. NewByGenerateCertificates is a class-level (static)
        // method — invoke on the class object; cmdletOutput is the new
        // guardian instance.
        auto cls = hgs.GetClass(L"MSFT_HgsGuardian");
        if (!cls) return {};
        auto in = cls.SpawnMethodIn(L"NewByGenerateCertificates");
        if (!in) return {};
        in.Set(L"Name", std::wstring{ L"UntrustedGuardian" });
        in.Set(L"GenerateCertificates", true);
        auto out = cls.InvokeMethod(L"NewByGenerateCertificates", in);
        if (auto g = out.GetObject(L"cmdletOutput"); g && *g)
            return std::move(*g);
        // Fall back to re-querying in case the provider only persists it.
        rows = hgs.Query(
            L"SELECT * FROM MSFT_HgsGuardian WHERE Name='UntrustedGuardian'");
        if (!rows.empty()) return std::move(rows.front());
        return {};
    }

    // Generate a fresh local-mode key protector owned by the UntrustedGuardian
    // (AllowUntrustedRoot — mirrors `New-HgsKeyProtector -AllowUntrustedRoot`).
    // NewByGuardians is a static method taking the guardian as an embedded
    // instance; cmdletOutput.RawData is the ~5 KB blob. Empty on failure.
    static std::vector<uint8_t> GenerateLocalKeyProtector(
        hyprv::wmi::WmiScope& hgs)
    {
        auto guardian = GetOrCreateUntrustedGuardian(hgs);
        if (!guardian) return {};
        auto cls = hgs.GetClass(L"MSFT_HgsKeyProtector");
        if (!cls) return {};
        auto in = cls.SpawnMethodIn(L"NewByGuardians");
        if (!in) return {};
        in.Set(L"Owner", guardian);            // embedded instance (VT_UNKNOWN)
        in.Set(L"AllowUntrustedRoot", true);
        auto out = cls.InvokeMethod(L"NewByGuardians", in);
        auto kp = out.GetObject(L"cmdletOutput");
        if (!kp || !*kp) return {};
        return kp->GetUInt8Array(L"RawData");
    }

    bool VMManager::RenameVM(std::wstring const& guid, std::wstring const& newName)
    {
        if (!m_scope) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms)
            {
                HyprvAppLog(L"[vmm] RenameVM: VSMS not found");
                return false;
            }
            auto vssdObj = FindRealizedVssd(*m_scope, guid);
            if (!vssdObj)
            {
                HyprvAppLog(L"[vmm] RenameVM: VSSD not found for %s", guid.c_str());
                return false;
            }
            hyprv::wmi::hyperv::Msvm_VirtualSystemSettingData vssd(std::move(vssdObj));
            vssd.ElementName(newName);
            // ModifySystemSettings takes the modified instance in WMI DTD
            // 2.0 XML (GetCimXml). ret 4096 = async job — wait + check.
            auto r = vsms.ModifySystemSettings(vssd.GetCimXml());
            HyprvAppLog(L"[vmm] RenameVM guid=%s name=%s ret=%u",
                guid.c_str(), newName.c_str(), r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok)
                HyprvAppLog(L"[vmm] RenameVM job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] RenameVM exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::SetNotes(std::wstring const& guid, std::wstring const& notes)
    {
        if (!m_scope) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto vssdObj = FindRealizedVssd(*m_scope, guid);
            if (!vssdObj) return false;
            hyprv::wmi::hyperv::Msvm_VirtualSystemSettingData vssd(std::move(vssdObj));
            // Notes is a StringArray in CIM. Hyper-V uses a single-element
            // array for the (single) notes blob; empty string clears.
            std::vector<std::wstring> notesArr;
            if (!notes.empty()) notesArr.push_back(notes);
            vssd.Notes(notesArr);
            auto r = vsms.ModifySystemSettings(vssd.GetCimXml());
            HyprvAppLog(L"[vmm] SetNotes guid=%s len=%zu ret=%u",
                guid.c_str(), notes.size(), r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok)
                HyprvAppLog(L"[vmm] SetNotes job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetNotes exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::SetSecureBoot(std::wstring const& guid, bool enabled,
                                  std::wstring const& templateId)
    {
        if (!m_scope) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto vssdObj = FindRealizedVssd(*m_scope, guid);
            if (!vssdObj) return false;
            hyprv::wmi::hyperv::Msvm_VirtualSystemSettingData vssd(std::move(vssdObj));
            vssd.SecureBootEnabled(enabled);
            // Always write the template too — even when disabling — so the
            // user's choice persists and a later re-enable keeps it. Hyper-V
            // ignores the template while Secure Boot is off. Skip only when
            // the caller passed an empty string (don't clobber a valid stored
            // template with "" when the UI couldn't resolve one).
            //
            // CRITICAL: Hyper-V stores SecureBootTemplateId as a BARE GUID
            // (no braces), e.g. "1734c6e8-3154-4dda-ba5f-a874cc483422".
            // Writing the braced form "{1734c6e8-...}" turns the whole
            // ModifySystemSettings into a silent no-op — it returns 4096
            // with a job that completes "successfully" but applies nothing,
            // so Secure Boot never actually toggles. Verified by capturing
            // the working .NET WMI call. Casing doesn't matter (Hyper-V
            // compares case-insensitively); only the braces break it. Strip
            // braces + whitespace before writing.
            if (!templateId.empty())
            {
                std::wstring bare;
                bare.reserve(templateId.size());
                for (wchar_t c : templateId)
                    if (c != L'{' && c != L'}' && c != L' ' && c != L'\t')
                        bare.push_back(c);
                vssd.SecureBootTemplateId(bare);
            }
            auto r = vsms.ModifySystemSettings(vssd.GetCimXml());
            HyprvAppLog(L"[vmm] SetSecureBoot guid=%s on=%d tmpl=%s ret=%u",
                guid.c_str(), enabled ? 1 : 0, templateId.c_str(), r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok)
                HyprvAppLog(L"[vmm] SetSecureBoot job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetSecureBoot exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::SetAutomaticActions(std::wstring const& guid,
                                        uint16_t startAction,
                                        uint16_t stopAction,
                                        uint32_t startDelaySeconds)
    {
        if (!m_scope) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto vssdObj = FindRealizedVssd(*m_scope, guid);
            if (!vssdObj) return false;
            hyprv::wmi::hyperv::Msvm_VirtualSystemSettingData vssd(std::move(vssdObj));
            // AutomaticStartupAction: 2=Nothing, 3=StartIfRunning, 4=Always.
            // AutomaticShutdownAction: 2=TurnOff, 3=Save, 4=ShutDown.
            // These are plain config prefs — no VM-state gate, unlike the
            // processor/memory modifies. The start-delay is a CIM interval we
            // intentionally leave untouched (the datetime setter can't express
            // it cleanly and the UI doesn't surface it yet).
            vssd.AutomaticStartupAction(startAction);
            vssd.AutomaticShutdownAction(stopAction);
            // Start delay is a CIM interval datetime; write it as the
            // "ddddddddHHMMSS.mmmmmm:000" string (WMI accepts a string for a
            // datetime property). Live in any state like the start action.
            vssd.Set(L"AutomaticStartupActionDelay",
                     SecondsToCimInterval(startDelaySeconds));
            auto r = vsms.ModifySystemSettings(vssd.GetCimXml());
            HyprvAppLog(L"[vmm] SetAutomaticActions guid=%s start=%u stop=%u delay=%us ret=%u",
                guid.c_str(), startAction, stopAction, startDelaySeconds, r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok)
                HyprvAppLog(L"[vmm] SetAutomaticActions job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetAutomaticActions exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::SetSmartPagingFileLocation(std::wstring const& guid,
                                               std::wstring const& path)
    {
        if (!m_scope || path.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto vssdObj = FindRealizedVssd(*m_scope, guid);
            if (!vssdObj) return false;
            hyprv::wmi::hyperv::Msvm_VirtualSystemSettingData vssd(std::move(vssdObj));
            vssd.Set(L"SwapFileDataRoot", path);
            auto r = vsms.ModifySystemSettings(vssd.GetCimXml());
            HyprvAppLog(L"[vmm] SetSmartPagingFileLocation guid=%s path=%s ret=%u",
                guid.c_str(), path.c_str(), r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok)
                HyprvAppLog(L"[vmm] SetSmartPagingFileLocation job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetSmartPagingFileLocation exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::SetCheckpointConfig(std::wstring const& guid,
                                        uint16_t snapshotType,
                                        bool automaticCheckpoints,
                                        std::wstring const& snapshotDataRoot)
    {
        if (!m_scope) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto vssdObj = FindRealizedVssd(*m_scope, guid);
            if (!vssdObj) return false;
            hyprv::wmi::hyperv::Msvm_VirtualSystemSettingData vssd(std::move(vssdObj));
            // UserSnapshotType: 2=Disabled, 3=Production, 4=ProductionOnly,
            // 5=Standard — changeable in any state. SnapshotDataRoot only
            // sticks when the VM has no checkpoints (the dialog gates the
            // location field accordingly); pass the current value to leave it.
            vssd.UserSnapshotType(snapshotType);
            vssd.Set(L"AutomaticSnapshotsEnabled", automaticCheckpoints);
            if (!snapshotDataRoot.empty())
                vssd.Set(L"SnapshotDataRoot", snapshotDataRoot);
            auto r = vsms.ModifySystemSettings(vssd.GetCimXml());
            HyprvAppLog(L"[vmm] SetCheckpointConfig guid=%s type=%u autoChk=%d root=%s ret=%u",
                guid.c_str(), snapshotType, automaticCheckpoints ? 1 : 0,
                snapshotDataRoot.c_str(), r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok)
                HyprvAppLog(L"[vmm] SetCheckpointConfig job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetCheckpointConfig exception: %s", e.whatW.c_str());
            return false;
        }
    }

    VMManager::SecurityInfo VMManager::GetVmSecurity(std::wstring const& guid) const
    {
        SecurityInfo info;
        if (!m_scope) return info;
        try
        {
            auto ssd = FindSecuritySettingData(*m_scope, guid);
            if (!ssd) return info;
            info.tpmEnabled   = ssd.GetBool(L"TpmEnabled");
            info.encryptState = ssd.GetBool(L"EncryptStateAndVmMigrationTraffic");
            info.shielded     = ssd.GetBool(L"ShieldingRequested");
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GetVmSecurity exception: %s", e.whatW.c_str());
        }
        return info;
    }

    bool VMManager::EnsureKeyProtector(hyprv::wmi::WmiObject& svc,
                                       hyprv::wmi::WmiObject& ssd,
                                       std::wstring const& guid)
    {
        // Hyper-V refuses to change ANY security setting (vTPM OR state
        // encryption) unless a valid key protector is configured — the
        // cmdlet error is "the selected security settings ... cannot be
        // changed without a valid key protector configured". So provision a
        // local one (owned by the host's UntrustedGuardian) before the modify
        // when the VM doesn't already have a real KP.
        //
        // CRITICAL: GetKeyProtector does NOT return an empty array when no KP
        // is set — it returns a 4-byte placeholder ("00 00 00 04"). A real KP
        // is ~5 KB. Treat anything tiny as "no key protector"; an `empty()`
        // check saw the placeholder as present, skipped provisioning, and the
        // subsequent modify silently no-opped (ret=4096, clean job, no change).
        std::vector<uint8_t> existing;
        try
        {
            auto gin = svc.SpawnMethodIn(L"GetKeyProtector");
            gin.Set(L"SecuritySettingData", ssd.GetCimXml());
            auto gout = svc.InvokeMethod(L"GetKeyProtector", gin);
            existing = gout.GetUInt8Array(L"KeyProtector");
        }
        catch (hyprv::wmi::WmiException const&) { /* treat as none */ }
        if (existing.size() >= 32) return true;   // real KP already present

        if (!m_hgsScope)
            m_hgsScope = std::make_unique<hyprv::wmi::WmiScope>(
                L"root\\Microsoft\\Windows\\Hgs");
        auto kp = GenerateLocalKeyProtector(*m_hgsScope);
        if (kp.empty())
        {
            HyprvAppLog(L"[vmm] EnsureKeyProtector: key-protector generation failed");
            return false;
        }
        auto sin = svc.SpawnMethodIn(L"SetKeyProtector");
        sin.SetUInt8Array(L"KeyProtector", kp);
        sin.Set(L"SecuritySettingData", ssd.GetCimXml());
        auto sout = svc.InvokeMethod(L"SetKeyProtector", sin);
        uint32_t sret = sout.GetUInt32(L"ReturnValue").value_or(~0u);
        HyprvAppLog(L"[vmm] EnsureKeyProtector SetKeyProtector guid=%s bytes=%zu ret=%u",
            guid.c_str(), kp.size(), sret);
        if (sret != 0 && sret != 4096) return false;
        auto [sok, sdesc] = WaitForJob(*m_scope,
            sout.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
        if (!sok)
        {
            HyprvAppLog(L"[vmm] EnsureKeyProtector job failed: %s", sdesc.c_str());
            return false;
        }
        // SetKeyProtector regenerates the SSD ref — re-fetch so the caller's
        // subsequent GetCimXml serializes the current instance.
        ssd = FindSecuritySettingData(*m_scope, guid);
        return static_cast<bool>(ssd);
    }

    bool VMManager::SetVmStateEncryption(std::wstring const& guid, bool enabled)
    {
        if (!m_scope) return false;
        try
        {
            hyprv::wmi::WmiObject svc = m_scope->GetInstance(L"Msvm_SecurityService");
            if (!svc)
            {
                HyprvAppLog(L"[vmm] SetVmStateEncryption: no Msvm_SecurityService");
                return false;
            }
            auto ssd = FindSecuritySettingData(*m_scope, guid);
            if (!ssd)
            {
                HyprvAppLog(L"[vmm] SetVmStateEncryption: no SSD for %s", guid.c_str());
                return false;
            }
            // Hyper-V rejects ANY security-setting change without a valid key
            // protector — enabling state encryption needs one just like vTPM.
            if (enabled && !EnsureKeyProtector(svc, ssd, guid)) return false;
            ssd.Set(L"EncryptStateAndVmMigrationTraffic", enabled);
            auto in = svc.SpawnMethodIn(L"ModifySecuritySettings");
            in.Set(L"SecuritySettingData", ssd.GetCimXml());
            auto out = svc.InvokeMethod(L"ModifySecuritySettings", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
            HyprvAppLog(L"[vmm] SetVmStateEncryption guid=%s on=%d ret=%u",
                guid.c_str(), enabled ? 1 : 0, ret);
            if (ret != 0 && ret != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope,
                out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
            if (!ok)
                HyprvAppLog(L"[vmm] SetVmStateEncryption job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetVmStateEncryption exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::SetVmTpm(std::wstring const& guid, bool enabled)
    {
        if (!m_scope) return false;
        try
        {
            hyprv::wmi::WmiObject svc = m_scope->GetInstance(L"Msvm_SecurityService");
            if (!svc)
            {
                HyprvAppLog(L"[vmm] SetVmTpm: no Msvm_SecurityService");
                return false;
            }
            auto ssd = FindSecuritySettingData(*m_scope, guid);
            if (!ssd)
            {
                HyprvAppLog(L"[vmm] SetVmTpm: no SSD for %s", guid.c_str());
                return false;
            }
            // A valid key protector is a prerequisite for enabling vTPM.
            if (enabled && !EnsureKeyProtector(svc, ssd, guid)) return false;
            ssd.Set(L"TpmEnabled", enabled);
            auto in = svc.SpawnMethodIn(L"ModifySecuritySettings");
            in.Set(L"SecuritySettingData", ssd.GetCimXml());
            auto out = svc.InvokeMethod(L"ModifySecuritySettings", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
            HyprvAppLog(L"[vmm] SetVmTpm guid=%s on=%d ret=%u",
                guid.c_str(), enabled ? 1 : 0, ret);
            if (ret != 0 && ret != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope,
                out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
            if (!ok)
                HyprvAppLog(L"[vmm] SetVmTpm job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetVmTpm exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::SetVmShielded(std::wstring const& guid, bool enabled)
    {
        if (!m_scope) return false;
        try
        {
            hyprv::wmi::WmiObject svc = m_scope->GetInstance(L"Msvm_SecurityService");
            if (!svc)
            {
                HyprvAppLog(L"[vmm] SetVmShielded: no Msvm_SecurityService");
                return false;
            }
            auto ssd = FindSecuritySettingData(*m_scope, guid);
            if (!ssd)
            {
                HyprvAppLog(L"[vmm] SetVmShielded: no SSD for %s", guid.c_str());
                return false;
            }
            // A valid key protector is the prerequisite for any security change.
            if (enabled && !EnsureKeyProtector(svc, ssd, guid)) return false;
            // Shielding is a COMPOSITE: a shielded VM requires vTPM + encrypted
            // state. Setting JUST ShieldingRequested is a silent no-op (the
            // gotcha-#27 family — ret=0 but Get-VMSecurity.Shielded stays
            // False). Hyper-V's own Set-VMSecurityPolicy flips ShieldingRequested
            // AND EncryptStateAndVmMigrationTraffic together (verified by diffing
            // the raw SSD); we also pin TpmEnabled so the shielded config is
            // complete. Disabling clears only ShieldingRequested — Hyper-V leaves
            // encryption on (the cmdlet does the same), so the user turns
            // encryption off separately if desired.
            ssd.Set(L"ShieldingRequested", enabled);
            if (enabled)
            {
                ssd.Set(L"TpmEnabled", true);
                ssd.Set(L"EncryptStateAndVmMigrationTraffic", true);
            }
            auto in = svc.SpawnMethodIn(L"ModifySecuritySettings");
            in.Set(L"SecuritySettingData", ssd.GetCimXml());
            auto out = svc.InvokeMethod(L"ModifySecuritySettings", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
            HyprvAppLog(L"[vmm] SetVmShielded guid=%s on=%d ret=%u",
                guid.c_str(), enabled ? 1 : 0, ret);
            if (ret != 0 && ret != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope,
                out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
            if (!ok)
                HyprvAppLog(L"[vmm] SetVmShielded job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetVmShielded exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::ShutdownVM(std::wstring const& guid, bool force)
    {
        if (!m_scope) return false;
        try
        {
            // The shutdown integration component is keyed by SystemName = the
            // VM GUID. Absent when the guest has no integration services up
            // (no LIS / pre-login / Linux without hyperv-daemons) — graceful
            // shutdown isn't possible there, so report failure and let the
            // caller fall back to Turn off.
            std::wstring wql =
                L"SELECT * FROM Msvm_ShutdownComponent WHERE SystemName='" +
                guid + L"'";
            auto rows = m_scope->Query(wql.c_str());
            if (rows.empty())
            {
                HyprvAppLog(L"[vmm] ShutdownVM: no Msvm_ShutdownComponent "
                            L"(integration services down?) guid=%s", guid.c_str());
                return false;
            }
            auto sc = std::move(rows.front());
            auto in = sc.SpawnMethodIn(L"InitiateShutdown");
            in.Set(L"Force", force);
            in.Set(L"Reason", std::wstring{ L"Shut down requested from hyprv" });
            auto out = sc.InvokeMethod(L"InitiateShutdown", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
            HyprvAppLog(L"[vmm] ShutdownVM guid=%s force=%d -> ret=%u",
                guid.c_str(), force ? 1 : 0, ret);
            if (ret != 0 && ret != 4096) return false;
            // Optimistic blink — Shut down uses InitiateShutdown (not
            // RequestStateChange), so it needs its own mark. The guest stays
            // Running through a graceful shutdown (gotcha #22), so the dot
            // pulses green until the VM actually reaches Off (poll clears it).
            MarkPendingStateChange(guid);
            auto [ok, desc] = WaitForJob(*m_scope,
                out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
            if (!ok)
                HyprvAppLog(L"[vmm] ShutdownVM job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] ShutdownVM exception: %s", e.whatW.c_str());
            return false;
        }
    }

    std::vector<VMManager::BootEntry>
    VMManager::GetBootOrder(std::wstring const& guid) const
    {
        std::vector<BootEntry> result;
        if (!m_scope) return result;
        try
        {
            auto vssdObj = FindRealizedVssd(*m_scope, guid);
            if (!vssdObj) return result;
            hyprv::wmi::hyperv::Msvm_VirtualSystemSettingData vssd(std::move(vssdObj));
            // BootSourceOrder is a string[] of WMI reference paths to
            // Msvm_BootSourceSettingData. Gen 1 VMs return empty here (they
            // populate the uint16 BootOrder instead).
            auto refs = vssd.BootSourceOrder();
            if (refs.empty()) return result;

            // Correlate each boot source to its backing device so we can show
            // friendly labels (Hyper-V Manager style) instead of the raw,
            // cryptic BootSourceDescription ("EFI SCSI Device"). The link: a
            // boot source's InstanceID is its device RASD's InstanceID + "\B"
            // (verified via Msvm_LogicalIdentity). Build the maps once:
            //   rasdType:      device RASD InstanceID -> ResourceType
            //   drivePathToId: drive RASD __PATH      -> its InstanceID
            //   driveFile:     drive InstanceID       -> attached file name
            std::unordered_map<std::wstring, uint16_t> rasdType;
            std::unordered_map<std::wstring, std::wstring> drivePathToId;
            for (auto& r : m_scope->Query(
                    (L"SELECT * FROM Msvm_ResourceAllocationSettingData "
                     L"WHERE InstanceID LIKE 'Microsoft:" + guid + L"%'").c_str()))
            {
                auto id = r.GetString(L"InstanceID").value_or(std::wstring{});
                if (id.empty()) continue;
                uint16_t rt = r.GetUInt16(L"ResourceType").value_or(0);
                rasdType[id] = rt;
                if (rt == 16 || rt == 17)   // DVD drive / disk drive
                    drivePathToId[r.Path()] = id;
            }
            std::unordered_map<std::wstring, std::wstring> driveFile;
            for (auto& s : m_scope->Query(
                    (L"SELECT Parent, HostResource FROM Msvm_StorageAllocationSettingData "
                     L"WHERE InstanceID LIKE 'Microsoft:" + guid + L"%'").c_str()))
            {
                auto parent = s.GetString(L"Parent").value_or(std::wstring{});
                auto it = drivePathToId.find(parent);
                if (it == drivePathToId.end()) continue;
                auto hosts = s.GetStringArray(L"HostResource");
                if (!hosts.empty() && !hosts.front().empty())
                {
                    auto const& p = hosts.front();
                    auto pos = p.find_last_of(L"\\/");
                    driveFile[it->second] =
                        (pos == std::wstring::npos) ? p : p.substr(pos + 1);
                }
            }

            result.reserve(refs.size());
            for (auto const& ref : refs)
            {
                BootEntry e;
                e.ref = ref;
                std::wstring rawDesc, deviceId;
                uint32_t btype = 0;
                if (auto bsd = m_scope->GetByPath(ref.c_str()))
                {
                    rawDesc  = bsd.GetString(L"BootSourceDescription").value_or(std::wstring{});
                    btype    = bsd.GetUInt32(L"BootSourceType").value_or(0);
                    deviceId = bsd.GetString(L"InstanceID").value_or(std::wstring{});
                    // Strip the trailing "\B" to get the device RASD InstanceID.
                    if (deviceId.size() >= 2 &&
                        deviceId.compare(deviceId.size() - 2, 2, L"\\B") == 0)
                        deviceId.erase(deviceId.size() - 2);
                }

                auto typeIt = rasdType.find(deviceId);
                uint16_t rt = (typeIt != rasdType.end()) ? typeIt->second : 0;
                auto fileIt = driveFile.find(deviceId);
                std::wstring file = (fileIt != driveFile.end()) ? fileIt->second : std::wstring{};

                if (rt == 16)        // DVD drive
                {
                    e.kind = BootKind::Dvd;
                    e.description = file.empty() ? L"DVD Drive (no media)"
                                                 : (L"DVD Drive (" + file + L")");
                }
                else if (rt == 17)   // disk drive
                {
                    e.kind = BootKind::HardDrive;
                    e.description = file.empty() ? L"Hard Drive"
                                                 : (L"Hard Drive (" + file + L")");
                }
                else if (btype == 2) // network (no backing storage RASD)
                {
                    e.kind = BootKind::Network;
                    e.description = L"Network Adapter";
                }
                else                 // file boot / unrecognized — keep the raw label
                {
                    e.kind = BootKind::Other;
                    e.description = rawDesc.empty() ? L"(unknown device)" : rawDesc;
                }
                result.push_back(std::move(e));
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GetBootOrder exception: %s", e.whatW.c_str());
        }
        return result;
    }

    bool VMManager::SetBootOrder(std::wstring const& guid,
                                 std::vector<std::wstring> const& orderedRefs)
    {
        if (!m_scope) return false;
        if (orderedRefs.empty()) return true;   // nothing to do
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto vssdObj = FindRealizedVssd(*m_scope, guid);
            if (!vssdObj) return false;
            hyprv::wmi::hyperv::Msvm_VirtualSystemSettingData vssd(std::move(vssdObj));
            // Write the permuted refs verbatim — see SetBootOrder header note
            // on why we never reconstruct them.
            vssd.BootSourceOrder(orderedRefs);
            auto r = vsms.ModifySystemSettings(vssd.GetCimXml());
            HyprvAppLog(L"[vmm] SetBootOrder guid=%s count=%zu ret=%u",
                guid.c_str(), orderedRefs.size(), r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok)
                HyprvAppLog(L"[vmm] SetBootOrder job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetBootOrder exception: %s", e.whatW.c_str());
            return false;
        }
    }

    // Friendly label for a Gen 1 BootOrder device-type code. Matches the
    // PowerShell BootDevice enum / Hyper-V Manager's BIOS tab. Codes verified
    // against live WMI (WIN7X64 reads 1,2,3,0 = CD, IDE, LegacyNic, Floppy).
    static std::wstring Gen1BootLabel(uint16_t code)
    {
        switch (code)
        {
        case 0: return L"Floppy Disk";
        case 1: return L"CD/DVD Drive";
        case 2: return L"Hard Drive (IDE)";
        case 3: return L"Legacy Network Adapter";
        default: return L"Device (code " + std::to_wstring(code) + L")";
        }
    }

    std::vector<VMManager::Gen1BootEntry>
    VMManager::GetBootOrderGen1(std::wstring const& guid) const
    {
        std::vector<Gen1BootEntry> result;
        if (!m_scope) return result;
        try
        {
            auto vssdObj = FindRealizedVssd(*m_scope, guid);
            if (!vssdObj) return result;
            hyprv::wmi::hyperv::Msvm_VirtualSystemSettingData vssd(std::move(vssdObj));
            // BootOrder is a uint16[] of device-type codes (Gen 1). The
            // generated getter reads it via GetUInt32Array + narrows. Gen 2
            // VMs leave this empty (they use BootSourceOrder instead).
            for (uint16_t code : vssd.BootOrder())
            {
                Gen1BootEntry e;
                e.code        = code;
                e.description = Gen1BootLabel(code);
                result.push_back(std::move(e));
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GetBootOrderGen1 exception: %s", e.whatW.c_str());
        }
        return result;
    }

    bool VMManager::SetBootOrderGen1(std::wstring const& guid,
                                     std::vector<uint16_t> const& orderedCodes)
    {
        if (!m_scope) return false;
        if (orderedCodes.empty()) return true;   // nothing to do
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto vssdObj = FindRealizedVssd(*m_scope, guid);
            if (!vssdObj) return false;
            hyprv::wmi::hyperv::Msvm_VirtualSystemSettingData vssd(std::move(vssdObj));
            // Write the permuted codes back (BootOrder uint16[]). The setter
            // now routes through WmiObject::SetUInt16Array (VT_I4 SAFEARRAY —
            // the wire form WMI uses for CIM uint16 arrays).
            vssd.BootOrder(orderedCodes);
            auto r = vsms.ModifySystemSettings(vssd.GetCimXml());
            HyprvAppLog(L"[vmm] SetBootOrderGen1 guid=%s count=%zu ret=%u",
                guid.c_str(), orderedCodes.size(), r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok)
                HyprvAppLog(L"[vmm] SetBootOrderGen1 job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetBootOrderGen1 exception: %s", e.whatW.c_str());
            return false;
        }
    }

    // Fetch the default Virtual CD/DVD Disk media template — the pre-filled
    // Msvm_StorageAllocationSettingData (Caption / AllocationUnits / ResourceType
    // 31 / ConsumerVisibility / …) that AddResourceSettings expects as the basis
    // for a new disc. Its InstanceID is "Microsoft:Definition\<guid>\Default";
    // mounted media instead carry the VM's GUID, so "Definition" uniquely picks
    // the template out of all CD/DVD allocation setting data on the host.
    static hyprv::wmi::WmiObject FindDefaultDvdMediaTemplate(hyprv::wmi::WmiScope& scope)
    {
        auto rows = scope.Query(
            L"SELECT * FROM Msvm_StorageAllocationSettingData "
            L"WHERE ResourceSubType='Microsoft:Hyper-V:Virtual CD/DVD Disk'");
        for (auto& r : rows)
        {
            auto iid = r.GetString(L"InstanceID").value_or(std::wstring{});
            if (iid.find(L"Definition") != std::wstring::npos)
                return std::move(r);
        }
        return {};
    }

    // Defined further down (with the storage helpers); GetDvdDrives /
    // GetStorageControllers above it need the slot reader.
    static int ReadAddressOnParent(hyprv::wmi::WmiObject const& o);

    std::vector<VMManager::DvdDrive>
    VMManager::GetDvdDrives(std::wstring const& guid) const
    {
        std::vector<DvdDrive> result;
        if (!m_scope) return result;
        try
        {
            // DVD drives are ResourceType 16 RASDs (both Gen 1 and Gen 2 use
            // "Microsoft:Hyper-V:Synthetic DVD Drive").
            auto drives = m_scope->Query(
                (L"SELECT * FROM Msvm_ResourceAllocationSettingData "
                 L"WHERE ResourceType=16 AND InstanceID LIKE 'Microsoft:"
                 + guid + L"%'").c_str());
            if (drives.empty()) return result;

            // All CD/DVD media SASDs for this VM — matched to their drive by
            // the Parent reference. (Empty drives contribute no media row.)
            auto media = m_scope->Query(
                (L"SELECT * FROM Msvm_StorageAllocationSettingData "
                 L"WHERE InstanceID LIKE 'Microsoft:" + guid + L"%'").c_str());

            // Numbered controller labels keyed by ref, like GetHardDisks.
            std::unordered_map<std::wstring, std::wstring> ctrlLabels;
            for (auto const& c : GetStorageControllers(guid))
                ctrlLabels.emplace(c.ref, c.label);

            int idx = 0;
            for (auto& drv : drives)
            {
                DvdDrive d;
                d.driveRef = drv.Path();
                d.label    = L"DVD Drive " + std::to_wstring(++idx);
                d.slot     = ReadAddressOnParent(drv);
                if (d.slot < 0) d.slot = 0;
                auto ctrlRef = drv.GetString(L"Parent").value_or(std::wstring{});
                auto cit = ctrlLabels.find(ctrlRef);
                if (cit != ctrlLabels.end()) d.controller = cit->second;
                for (auto& m : media)
                {
                    auto sub = m.GetString(L"ResourceSubType").value_or(std::wstring{});
                    if (sub.find(L"DVD") == std::wstring::npos &&
                        sub.find(L"CD")  == std::wstring::npos)
                        continue;
                    // Parent is the drive RASD __PATH — same provider, so it
                    // matches drv.Path() byte-for-byte.
                    if (m.GetString(L"Parent").value_or(std::wstring{}) != d.driveRef)
                        continue;
                    d.mediaRef = m.Path();
                    auto hr = m.GetStringArray(L"HostResource");
                    if (!hr.empty()) d.mediaPath = hr.front();
                    break;
                }
                result.push_back(std::move(d));
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GetDvdDrives exception: %s", e.whatW.c_str());
        }
        return result;
    }

    bool VMManager::SetDvdMedia(std::wstring const& guid,
                                std::wstring const& driveRef,
                                std::wstring const& mediaRef,
                                std::wstring const& newIsoPath)
    {
        if (!m_scope) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;

            const bool hasMedia  = !mediaRef.empty();
            const bool wantMedia = !newIsoPath.empty();

            // EJECT — remove the media object. The generated RemoveResourceSettings
            // wrapper is still a no-op stub (ReferenceArray in-param codegen TODO),
            // so invoke the method manually: the in-param is a REF[] of object
            // __PATHs, set via SetReferenceArray.
            if (!wantMedia && hasMedia)
            {
                auto in = vsms.SpawnMethodIn(L"RemoveResourceSettings");
                in.SetReferenceArray(L"ResourceSettings",
                                     std::vector<std::wstring>{ mediaRef });
                auto out = vsms.InvokeMethod(L"RemoveResourceSettings", in);
                uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
                HyprvAppLog(L"[vmm] SetDvdMedia EJECT ret=%u", ret);
                if (ret != 0 && ret != 4096) return false;
                auto [ok, desc] = WaitForJob(*m_scope,
                    out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
                if (!ok) HyprvAppLog(L"[vmm] SetDvdMedia eject job failed: %s", desc.c_str());
                if (ok) KickPoll();
                return ok;
            }

            // CHANGE — point the existing media object at a different ISO.
            if (wantMedia && hasMedia)
            {
                auto m = m_scope->GetByPath(mediaRef.c_str());
                if (!m) return false;
                m.SetStringArray(L"HostResource",
                                 std::vector<std::wstring>{ newIsoPath });
                std::vector<std::wstring> texts{ m.GetCimXml() };
                auto r = vsms.ModifyResourceSettings(texts);
                HyprvAppLog(L"[vmm] SetDvdMedia CHANGE ret=%u", r.ReturnValue);
                if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
                auto [ok, desc] = WaitForJob(*m_scope, r.Job);
                if (!ok) HyprvAppLog(L"[vmm] SetDvdMedia change job failed: %s", desc.c_str());
                if (ok) KickPoll();
                return ok;
            }

            // MOUNT — clone the default media template, point it at the drive
            // (Parent) + ISO (HostResource), and AddResourceSettings to the
            // realised VSSD. AffectedConfiguration is the VSSD __PATH (a REF
            // passed as a path string — unambiguous vs. an embedded object).
            if (wantMedia && !hasMedia)
            {
                auto tmpl = FindDefaultDvdMediaTemplate(*m_scope);
                if (!tmpl)
                {
                    HyprvAppLog(L"[vmm] SetDvdMedia: default DVD media template not found");
                    return false;
                }
                tmpl.Set(L"Parent", driveRef);
                tmpl.SetStringArray(L"HostResource",
                                    std::vector<std::wstring>{ newIsoPath });
                auto vssd = FindRealizedVssd(*m_scope, guid);
                if (!vssd) return false;

                auto in = vsms.SpawnMethodIn(L"AddResourceSettings");
                in.Set(L"AffectedConfiguration", vssd.Path());
                in.SetStringArray(L"ResourceSettings",
                                  std::vector<std::wstring>{ tmpl.GetCimXml() });
                auto out = vsms.InvokeMethod(L"AddResourceSettings", in);
                uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
                HyprvAppLog(L"[vmm] SetDvdMedia MOUNT ret=%u", ret);
                if (ret != 0 && ret != 4096) return false;
                auto [ok, desc] = WaitForJob(*m_scope,
                    out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
                if (!ok) HyprvAppLog(L"[vmm] SetDvdMedia mount job failed: %s", desc.c_str());
                if (ok) KickPoll();
                return ok;
            }

            // Empty -> empty: nothing to do.
            return true;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetDvdMedia exception: %s", e.whatW.c_str());
            return false;
        }
    }

    // True if `s` ends with "\Default" — used to pick the default resource
    // template out of the Definition namespace (which also exposes
    // \Minimum, \Maximum, \Increment variants we don't want).
    static bool EndsWithDefault(std::wstring const& s)
    {
        static const std::wstring kSuffix = L"\\Default";
        return s.size() >= kSuffix.size() &&
               s.compare(s.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
    }

    // Default disk-drive RASD template ("Synthetic Disk Drive", ResourceType
    // 17). There's also a "Physical Disk Drive" template — match the subtype
    // to avoid it, and the \Default variant (not Min/Max/Increment).
    static hyprv::wmi::WmiObject FindDefaultDiskDriveTemplate(hyprv::wmi::WmiScope& scope)
    {
        auto rows = scope.Query(
            L"SELECT * FROM Msvm_ResourceAllocationSettingData "
            L"WHERE ResourceType=17 "
            L"AND ResourceSubType='Microsoft:Hyper-V:Synthetic Disk Drive'");
        for (auto& r : rows)
            if (EndsWithDefault(r.GetString(L"InstanceID").value_or(std::wstring{})))
                return std::move(r);
        return {};
    }

    // Default VHD SASD template ("Virtual Hard Disk").
    static hyprv::wmi::WmiObject FindDefaultVhdTemplate(hyprv::wmi::WmiScope& scope)
    {
        auto rows = scope.Query(
            L"SELECT * FROM Msvm_StorageAllocationSettingData "
            L"WHERE ResourceSubType='Microsoft:Hyper-V:Virtual Hard Disk'");
        for (auto& r : rows)
            if (EndsWithDefault(r.GetString(L"InstanceID").value_or(std::wstring{})))
                return std::move(r);
        return {};
    }

    // Default "Synthetic SCSI Controller" RASD template (ResourceType 6).
    static hyprv::wmi::WmiObject FindDefaultScsiControllerTemplate(hyprv::wmi::WmiScope& scope)
    {
        auto rows = scope.Query(
            L"SELECT * FROM Msvm_ResourceAllocationSettingData "
            L"WHERE ResourceType=6 "
            L"AND ResourceSubType='Microsoft:Hyper-V:Synthetic SCSI Controller'");
        for (auto& r : rows)
            if (EndsWithDefault(r.GetString(L"InstanceID").value_or(std::wstring{})))
                return std::move(r);
        return {};
    }

    // CIM AddressOnParent is a string; -1 if absent/non-numeric.
    static int ReadAddressOnParent(hyprv::wmi::WmiObject const& o)
    {
        auto s = o.GetString(L"AddressOnParent").value_or(std::wstring{});
        return s.empty() ? -1 : _wtoi(s.c_str());
    }

    // Resolve the target controller + slot for a new disk/DVD drive. When
    // controllerRef is empty, auto-pick (SCSI=RT6 preferred, else IDE=RT5) —
    // this preserves the pre-picker behavior. When controllerRef is given, use
    // that exact controller. slot<0 picks the lowest free AddressOnParent on
    // the chosen controller (disk + DVD drives share the slot space); a given
    // slot is honored as-is (the caller is expected to have offered only free
    // slots). Returns false if no controller exists or no slot is free.
    static bool ResolveStorageTarget(hyprv::wmi::WmiScope& scope,
                                     std::wstring const& guid,
                                     std::wstring const& controllerRef,
                                     int slot,
                                     std::wstring& outCtrlPath,
                                     bool& outIsScsi,
                                     int& outSlot)
    {
        hyprv::wmi::WmiObject ctrl;
        if (!controllerRef.empty())
        {
            ctrl = scope.GetByPath(controllerRef.c_str());
            if (!ctrl)
            {
                HyprvAppLog(L"[vmm] ResolveStorageTarget: controller ref not found: %s",
                    controllerRef.c_str());
                return false;
            }
        }
        else
        {
            auto controllers = scope.Query(
                (L"SELECT * FROM Msvm_ResourceAllocationSettingData "
                 L"WHERE InstanceID LIKE 'Microsoft:" + guid +
                 L"%' AND (ResourceType=6 OR ResourceType=5)").c_str());
            if (controllers.empty()) return false;
            for (auto& c : controllers)
                if (c.GetUInt16(L"ResourceType").value_or(0) == 6) { ctrl = c; break; }
            if (!ctrl) ctrl = std::move(controllers.front());   // IDE fallback
        }
        outCtrlPath = ctrl.Path();
        outIsScsi   = ctrl.GetUInt16(L"ResourceType").value_or(0) == 6;
        const int maxSlot = outIsScsi ? 64 : 2;

        if (slot >= 0)
        {
            if (slot >= maxSlot) return false;
            outSlot = slot;
            return true;
        }
        // Lowest free slot among the controller's children.
        std::vector<int> used;
        for (auto& k : scope.Query(
                (L"SELECT * FROM Msvm_ResourceAllocationSettingData "
                 L"WHERE InstanceID LIKE 'Microsoft:" + guid + L"%'").c_str()))
            if (k.GetString(L"Parent").value_or(std::wstring{}) == outCtrlPath)
            {
                int ap = ReadAddressOnParent(k);
                if (ap >= 0) used.push_back(ap);
            }
        int s = 0;
        while (s < maxSlot && std::find(used.begin(), used.end(), s) != used.end())
            ++s;
        if (s >= maxSlot) return false;
        outSlot = s;
        return true;
    }

    std::vector<VMManager::HardDisk>
    VMManager::GetHardDisks(std::wstring const& guid) const
    {
        std::vector<HardDisk> result;
        if (!m_scope) return result;
        try
        {
            // Numbered controller labels ("SCSI Controller 0", ...) keyed by
            // ref, so the disk cards match the destination picker's wording.
            std::unordered_map<std::wstring, std::wstring> ctrlLabels;
            for (auto const& c : GetStorageControllers(guid))
                ctrlLabels.emplace(c.ref, c.label);

            auto sasd = m_scope->Query(
                (L"SELECT * FROM Msvm_StorageAllocationSettingData "
                 L"WHERE InstanceID LIKE 'Microsoft:" + guid + L"%'").c_str());
            for (auto& s : sasd)
            {
                if (s.GetString(L"ResourceSubType").value_or(std::wstring{})
                        .find(L"Virtual Hard Disk") == std::wstring::npos)
                    continue;
                HardDisk d;
                d.vhdRef = s.Path();
                auto hr = s.GetStringArray(L"HostResource");
                if (!hr.empty()) d.path = hr.front();
                d.driveRef = s.GetString(L"Parent").value_or(std::wstring{});
                // Resolve drive → slot, and controller → numbered label.
                if (!d.driveRef.empty())
                {
                    auto drive = m_scope->GetByPath(d.driveRef.c_str());
                    if (drive)
                    {
                        d.slot = ReadAddressOnParent(drive);
                        if (d.slot < 0) d.slot = 0;
                        auto ctrlRef = drive.GetString(L"Parent").value_or(std::wstring{});
                        auto it = ctrlLabels.find(ctrlRef);
                        if (it != ctrlLabels.end())
                            d.controller = it->second;
                    }
                }
                // Storage QoS (normalized 8 KB IOPS) lives on this same SASD.
                d.iopsMin = s.GetUInt64(L"IOPSReservation").value_or(0);
                d.iopsMax = s.GetUInt64(L"IOPSLimit").value_or(0);
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (!d.path.empty() &&
                    GetFileAttributesExW(d.path.c_str(), GetFileExInfoStandard, &fad))
                    d.fileSizeBytes = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32)
                                    | fad.nFileSizeLow;
                result.push_back(std::move(d));
            }

            // Pass-through physical disks: a single RT-17 RASD with
            // ResourceSubType "Physical Disk Drive" and HostResource pointing at
            // a host Msvm_DiskDrive (no VHD SASD layer — gotcha verified). They
            // don't appear in the SASD query above, so enumerate them here.
            auto rasd = m_scope->Query(
                (L"SELECT * FROM Msvm_ResourceAllocationSettingData "
                 L"WHERE InstanceID LIKE 'Microsoft:" + guid + L"%'").c_str());
            for (auto& r : rasd)
            {
                if (r.GetUInt16(L"ResourceType").value_or(0) != 17) continue;
                if (r.GetString(L"ResourceSubType").value_or(std::wstring{})
                        .find(L"Physical Disk Drive") == std::wstring::npos)
                    continue;
                HardDisk d;
                d.isPassthrough = true;
                d.driveRef = r.Path();            // detach target (no SASD)
                d.slot = ReadAddressOnParent(r);
                if (d.slot < 0) d.slot = 0;
                auto ctrlRef = r.GetString(L"Parent").value_or(std::wstring{});
                auto it = ctrlLabels.find(ctrlRef);
                if (it != ctrlLabels.end()) d.controller = it->second;
                // Resolve the host Msvm_DiskDrive (HostResource[0]) for a label
                // + size; fall back to a generic label if it can't be read.
                auto hr = r.GetStringArray(L"HostResource");
                if (!hr.empty() && !hr.front().empty())
                {
                    if (auto dd = m_scope->GetByPath(hr.front().c_str()))
                    {
                        d.path = dd.GetString(L"ElementName").value_or(std::wstring{});
                        d.fileSizeBytes = dd.GetUInt64(L"MaxMediaSize").value_or(0);
                    }
                }
                if (d.path.empty()) d.path = L"Physical disk";
                result.push_back(std::move(d));
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GetHardDisks exception: %s", e.whatW.c_str());
        }
        return result;
    }

    // Default "Physical Disk Drive" RT-17 RASD template — the pass-through
    // counterpart of FindDefaultDiskDriveTemplate (different ResourceSubType).
    // VERIFIED a dedicated \Default template exists for this subtype.
    static hyprv::wmi::WmiObject FindDefaultPhysicalDiskDriveTemplate(hyprv::wmi::WmiScope& scope)
    {
        auto rows = scope.Query(
            L"SELECT * FROM Msvm_ResourceAllocationSettingData "
            L"WHERE ResourceType=17 "
            L"AND ResourceSubType='Microsoft:Hyper-V:Physical Disk Drive'");
        for (auto& r : rows)
            if (EndsWithDefault(r.GetString(L"InstanceID").value_or(std::wstring{})))
                return std::move(r);
        return {};
    }

    std::vector<VMManager::PhysicalDisk>
    VMManager::GetAvailablePhysicalDisks() const
    {
        std::vector<PhysicalDisk> result;
        if (!m_scope) return result;
        try
        {
            // Offline host disks expose a DriveNumber on their Msvm_DiskDrive;
            // online/VM-attached synthetic drives don't (DriveNumber is NULL).
            for (auto& dd : m_scope->Query(L"SELECT * FROM Msvm_DiskDrive"))
            {
                auto num = dd.GetUInt32(L"DriveNumber");
                if (!num) continue;                 // not an offline host disk
                PhysicalDisk pd;
                pd.devicePath  = dd.Path();
                pd.driveNumber = *num;
                pd.label       = dd.GetString(L"ElementName").value_or(
                                     L"Disk " + std::to_wstring(*num));
                pd.sizeBytes   = dd.GetUInt64(L"MaxMediaSize").value_or(0);
                result.push_back(std::move(pd));
            }
            std::sort(result.begin(), result.end(),
                [](auto const& a, auto const& b) { return a.driveNumber < b.driveNumber; });
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GetAvailablePhysicalDisks exception: %s", e.whatW.c_str());
        }
        return result;
    }

    bool VMManager::AttachPhysicalDisk(std::wstring const& guid,
                                       std::wstring const& devicePath,
                                       std::wstring const& controllerRef, int reqSlot)
    {
        if (!m_scope || guid.empty() || devicePath.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto vssd = FindRealizedVssd(*m_scope, guid);
            if (!vssd) return false;

            std::wstring ctrlPath;
            bool isScsi = true;
            int slot = 0;
            if (!ResolveStorageTarget(*m_scope, guid, controllerRef, reqSlot,
                                      ctrlPath, isScsi, slot))
            {
                HyprvAppLog(L"[vmm] AttachPhysicalDisk: no controller/free slot for %s", guid.c_str());
                return false;
            }

            // Single-layer add: the pass-through disk drive RASD with its
            // HostResource pointing straight at the host Msvm_DiskDrive __PATH.
            auto tmpl = FindDefaultPhysicalDiskDriveTemplate(*m_scope);
            if (!tmpl)
            {
                HyprvAppLog(L"[vmm] AttachPhysicalDisk: physical disk drive template not found");
                return false;
            }
            tmpl.Set(L"Parent", ctrlPath);
            tmpl.Set(L"AddressOnParent", std::to_wstring(slot));
            tmpl.SetStringArray(L"HostResource",
                                std::vector<std::wstring>{ devicePath });
            auto in = vsms.SpawnMethodIn(L"AddResourceSettings");
            in.Set(L"AffectedConfiguration", vssd.Path());
            in.SetStringArray(L"ResourceSettings",
                              std::vector<std::wstring>{ tmpl.GetCimXml() });
            auto out = vsms.InvokeMethod(L"AddResourceSettings", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
            HyprvAppLog(L"[vmm] AttachPhysicalDisk slot=%d scsi=%d ret=%u",
                slot, isScsi ? 1 : 0, ret);
            if (ret != 0 && ret != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope,
                out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
            if (!ok) HyprvAppLog(L"[vmm] AttachPhysicalDisk job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] AttachPhysicalDisk exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::SetDiskQos(std::wstring const& guid,
                               std::wstring const& vhdRef,
                               uint64_t minIops, uint64_t maxIops)
    {
        static_cast<void>(guid);
        if (!m_scope || vhdRef.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;

            // Mirror SetDvdMedia's CHANGE path: fetch the VHD SASD, set the two
            // QoS scalars, ModifyResourceSettings. IOPSReservation/IOPSLimit are
            // already on the object (no Add-from-template path — the SASD always
            // exists for an attached disk). Values are verbatim normalized IOPS.
            auto s = m_scope->GetByPath(vhdRef.c_str());
            if (!s) return false;
            s.Set(L"IOPSReservation", minIops);
            s.Set(L"IOPSLimit",       maxIops);
            std::vector<std::wstring> texts{ s.GetCimXml() };
            auto r = vsms.ModifyResourceSettings(texts);
            HyprvAppLog(L"[vmm] SetDiskQos min=%llu max=%llu ret=%u",
                static_cast<unsigned long long>(minIops),
                static_cast<unsigned long long>(maxIops), r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok) HyprvAppLog(L"[vmm] SetDiskQos job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetDiskQos exception: %s", e.whatW.c_str());
            return false;
        }
    }

    std::vector<VMManager::SerialPort>
    VMManager::GetSerialPorts(std::wstring const& guid) const
    {
        std::vector<SerialPort> result;
        if (!m_scope) return result;
        try
        {
            for (auto& s : m_scope->Query(
                    (L"SELECT * FROM Msvm_SerialPortSettingData "
                     L"WHERE InstanceID LIKE 'Microsoft:" + guid + L"%'").c_str()))
            {
                if (s.GetUInt16(L"ResourceType").value_or(0) != 21) continue;
                SerialPort p;
                p.ref  = s.Path();
                p.name = s.GetString(L"ElementName").value_or(std::wstring{});
                auto conn = s.GetStringArray(L"Connection");
                if (!conn.empty()) p.path = conn.front();
                result.push_back(std::move(p));
            }
            std::sort(result.begin(), result.end(),
                [](auto const& a, auto const& b) { return a.name < b.name; });
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GetSerialPorts exception: %s", e.whatW.c_str());
        }
        return result;
    }

    bool VMManager::SetSerialPortConnection(std::wstring const& guid,
                                            std::wstring const& portRef,
                                            std::wstring const& pipePath)
    {
        static_cast<void>(guid);
        if (!m_scope || portRef.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto s = m_scope->GetByPath(portRef.c_str());
            if (!s) return false;
            // Connection is a single-element string array: [pipePath] to
            // connect, [""] to disconnect. CRITICAL: disconnect must be a
            // one-element EMPTY STRING, NOT an empty array — an empty
            // Connection=[] array is a silent no-op (ModifyResourceSettings
            // returns 4096 + a clean job but the value never changes; verified
            // by diffing the serialized WMI DTD 2.0 XML: <VALUE.ARRAY></VALUE.ARRAY>
            // is ignored, <VALUE.ARRAY><VALUE></VALUE></VALUE.ARRAY> clears it
            // and reads back as []). The silent-no-op family (gotchas #7/#16/#30).
            s.SetStringArray(L"Connection", std::vector<std::wstring>{ pipePath });
            std::vector<std::wstring> texts{ s.GetCimXml() };
            auto r = vsms.ModifyResourceSettings(texts);
            HyprvAppLog(L"[vmm] SetSerialPortConnection path='%s' ret=%u",
                pipePath.c_str(), r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok) HyprvAppLog(L"[vmm] SetSerialPortConnection job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetSerialPortConnection exception: %s", e.whatW.c_str());
            return false;
        }
    }

    std::vector<VMManager::StorageController>
    VMManager::GetStorageControllers(std::wstring const& guid) const
    {
        std::vector<StorageController> result;
        if (!m_scope) return result;
        try
        {
            // All RASDs for this VM, once — we need both the controllers and
            // every child's AddressOnParent (to mark used slots).
            auto rasd = m_scope->Query(
                (L"SELECT * FROM Msvm_ResourceAllocationSettingData "
                 L"WHERE InstanceID LIKE 'Microsoft:" + guid + L"%'").c_str());

            std::vector<StorageController> ide, scsi;
            for (auto& c : rasd)
            {
                uint16_t rt = c.GetUInt16(L"ResourceType").value_or(0);
                if (rt != 5 && rt != 6) continue;
                StorageController sc;
                sc.ref      = c.Path();
                sc.isScsi   = (rt == 6);
                sc.maxSlots = sc.isScsi ? 64 : 2;
                if (sc.isScsi)
                    scsi.push_back(std::move(sc));
                else
                {
                    // IDE controllers carry Address 0/1 — use it as the number.
                    auto a = c.GetString(L"Address").value_or(std::wstring{});
                    sc.number = a.empty() ? 0 : _wtoi(a.c_str());
                    ide.push_back(std::move(sc));
                }
            }
            // Stable SCSI numbering: sort by ref (== InstanceID order) and index.
            std::sort(scsi.begin(), scsi.end(),
                [](auto const& a, auto const& b) { return a.ref < b.ref; });
            for (size_t i = 0; i < scsi.size(); ++i) scsi[i].number = static_cast<int>(i);
            std::sort(ide.begin(), ide.end(),
                [](auto const& a, auto const& b) { return a.number < b.number; });

            auto finish = [&](StorageController& sc)
            {
                sc.label = (sc.isScsi ? L"SCSI Controller " : L"IDE Controller ")
                         + std::to_wstring(sc.number);
                for (auto& k : rasd)
                    if (k.GetString(L"Parent").value_or(std::wstring{}) == sc.ref)
                    {
                        int ap = ReadAddressOnParent(k);
                        if (ap >= 0) sc.usedSlots.push_back(ap);
                    }
                result.push_back(std::move(sc));
            };
            for (auto& c : ide)  finish(c);   // IDE first (Hyper-V Manager order)
            for (auto& c : scsi) finish(c);
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GetStorageControllers exception: %s", e.whatW.c_str());
        }
        return result;
    }

    bool VMManager::AddScsiController(std::wstring const& guid)
    {
        if (!m_scope || guid.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto vssd = FindRealizedVssd(*m_scope, guid);
            if (!vssd) return false;

            auto tmpl = FindDefaultScsiControllerTemplate(*m_scope);
            if (!tmpl)
            {
                HyprvAppLog(L"[vmm] AddScsiController: default SCSI template not found");
                return false;
            }
            auto in = vsms.SpawnMethodIn(L"AddResourceSettings");
            in.Set(L"AffectedConfiguration", vssd.Path());
            in.SetStringArray(L"ResourceSettings",
                              std::vector<std::wstring>{ tmpl.GetCimXml() });
            auto out = vsms.InvokeMethod(L"AddResourceSettings", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
            HyprvAppLog(L"[vmm] AddScsiController ret=%u", ret);
            if (ret != 0 && ret != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope,
                out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
            if (!ok) HyprvAppLog(L"[vmm] AddScsiController job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] AddScsiController exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::RemoveScsiController(std::wstring const& guid,
                                         std::wstring const& controllerRef)
    {
        if (!m_scope || controllerRef.empty()) return false;
        static_cast<void>(guid);
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;

            // Manual RemoveResourceSettings (generated wrapper is a stub). The
            // caller only removes EMPTY controllers (no child drives), so a
            // single remove of the controller RASD is all that's needed — the
            // exact reverse of AddScsiController (verified ret=0 on Off).
            auto in = vsms.SpawnMethodIn(L"RemoveResourceSettings");
            in.SetReferenceArray(L"ResourceSettings",
                                 std::vector<std::wstring>{ controllerRef });
            auto out = vsms.InvokeMethod(L"RemoveResourceSettings", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
            HyprvAppLog(L"[vmm] RemoveScsiController ret=%u", ret);
            if (ret != 0 && ret != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope,
                out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
            if (!ok) HyprvAppLog(L"[vmm] RemoveScsiController job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] RemoveScsiController exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::AttachVhd(std::wstring const& guid, std::wstring const& vhdxPath,
                              std::wstring const& controllerRef, int reqSlot)
    {
        if (!m_scope || vhdxPath.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto vssd = FindRealizedVssd(*m_scope, guid);
            if (!vssd) return false;
            const std::wstring vssdPath = vssd.Path();

            // Resolve target controller + slot (explicit picker, else auto-pick).
            std::wstring ctrlPath;
            bool isScsi = true;
            int slot = 0;
            if (!ResolveStorageTarget(*m_scope, guid, controllerRef, reqSlot,
                                      ctrlPath, isScsi, slot))
            {
                HyprvAppLog(L"[vmm] AttachVhd: no controller/free slot for %s", guid.c_str());
                return false;
            }

            // Step 1: add the disk-drive RASD (clone the default, set
            // Parent=controller + AddressOnParent=slot).
            auto drvTmpl = FindDefaultDiskDriveTemplate(*m_scope);
            if (!drvTmpl)
            {
                HyprvAppLog(L"[vmm] AttachVhd: default disk-drive template not found");
                return false;
            }
            drvTmpl.Set(L"Parent", ctrlPath);
            drvTmpl.Set(L"AddressOnParent", std::to_wstring(slot));
            {
                auto in = vsms.SpawnMethodIn(L"AddResourceSettings");
                in.Set(L"AffectedConfiguration", vssdPath);
                in.SetStringArray(L"ResourceSettings",
                                  std::vector<std::wstring>{ drvTmpl.GetCimXml() });
                auto out = vsms.InvokeMethod(L"AddResourceSettings", in);
                uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
                HyprvAppLog(L"[vmm] AttachVhd add-drive slot=%d scsi=%d ret=%u",
                    slot, isScsi ? 1 : 0, ret);
                if (ret != 0 && ret != 4096) return false;
                auto [ok, desc] = WaitForJob(*m_scope,
                    out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
                if (!ok) { HyprvAppLog(L"[vmm] AttachVhd add-drive job failed: %s", desc.c_str()); return false; }
            }

            // Step 2: re-query the new drive (its ResultingResourceSettings
            // comes back as embedded XML, not a usable object — gotcha #9 —
            // so find it by controller + slot instead).
            std::wstring newDriveRef;
            auto drives2 = m_scope->Query(
                (L"SELECT * FROM Msvm_ResourceAllocationSettingData "
                 L"WHERE InstanceID LIKE 'Microsoft:" + guid + L"%'").c_str());
            for (auto& d : drives2)
            {
                if (d.GetUInt16(L"ResourceType").value_or(0) != 17) continue;
                if (d.GetString(L"Parent").value_or(std::wstring{}) != ctrlPath) continue;
                if (ReadAddressOnParent(d) != slot) continue;
                newDriveRef = d.Path();
                break;
            }
            if (newDriveRef.empty())
            {
                HyprvAppLog(L"[vmm] AttachVhd: new disk drive not found after add");
                return false;
            }

            // Step 3: add the VHD SASD child of the new drive. When this add
            // returns 4096 (async) right after the drive add, the VHD often
            // fails to LINK — the just-added drive hasn't fully quiesced and
            // the async result is eventually consistent. So: let it settle,
            // re-resolve the drive ref (it can regenerate after a modify),
            // add the VHD, then POLL for the link to appear. Roll the orphan
            // drive back if it never links (no empty-drive left behind).
            auto rollbackDrive = [&]
            {
                auto in = vsms.SpawnMethodIn(L"RemoveResourceSettings");
                in.SetReferenceArray(L"ResourceSettings",
                                     std::vector<std::wstring>{ newDriveRef });
                vsms.InvokeMethod(L"RemoveResourceSettings", in);
                KickPoll();
            };
            // Settle, then re-resolve the new drive's ref by (controller, slot).
            ::Sleep(1000);
            for (auto& d : m_scope->Query(
                    (L"SELECT * FROM Msvm_ResourceAllocationSettingData "
                     L"WHERE InstanceID LIKE 'Microsoft:" + guid + L"%'").c_str()))
            {
                if (d.GetUInt16(L"ResourceType").value_or(0) != 17) continue;
                if (d.GetString(L"Parent").value_or(std::wstring{}) != ctrlPath) continue;
                if (ReadAddressOnParent(d) != slot) continue;
                newDriveRef = d.Path();
                break;
            }
            auto vhdLinked = [&]() -> bool
            {
                for (auto& s : m_scope->Query(
                        (L"SELECT * FROM Msvm_StorageAllocationSettingData "
                         L"WHERE InstanceID LIKE 'Microsoft:" + guid + L"%'").c_str()))
                {
                    if (s.GetString(L"Parent").value_or(std::wstring{}) != newDriveRef)
                        continue;
                    auto hr = s.GetStringArray(L"HostResource");
                    if (!hr.empty() && !hr.front().empty()) return true;
                }
                return false;
            };
            auto vhdTmpl = FindDefaultVhdTemplate(*m_scope);
            if (!vhdTmpl)
            {
                HyprvAppLog(L"[vmm] AttachVhd: default VHD template not found");
                rollbackDrive();
                return false;
            }
            vhdTmpl.Set(L"Parent", newDriveRef);
            vhdTmpl.SetStringArray(L"HostResource",
                                   std::vector<std::wstring>{ vhdxPath });
            {
                auto in = vsms.SpawnMethodIn(L"AddResourceSettings");
                in.Set(L"AffectedConfiguration", vssdPath);
                in.SetStringArray(L"ResourceSettings",
                                  std::vector<std::wstring>{ vhdTmpl.GetCimXml() });
                auto out = vsms.InvokeMethod(L"AddResourceSettings", in);
                uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
                HyprvAppLog(L"[vmm] AttachVhd add-vhd ret=%u path=%s", ret, vhdxPath.c_str());
                if (ret != 0 && ret != 4096) { rollbackDrive(); return false; }
                auto [ok, desc] = WaitForJob(*m_scope,
                    out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
                if (!ok)
                    HyprvAppLog(L"[vmm] AttachVhd add-vhd job failed: %s", desc.c_str());
            }
            // Poll up to ~3s for the link to appear (eventually consistent).
            bool attached = false;
            for (int i = 0; i < 12 && !attached; ++i)
            {
                if (vhdLinked()) { attached = true; break; }
                ::Sleep(250);
            }
            if (!attached)
            {
                HyprvAppLog(L"[vmm] AttachVhd: VHD did not link after settle+poll "
                            L"— rolling back the orphan drive");
                rollbackDrive();
                return false;
            }
            HyprvAppLog(L"[vmm] AttachVhd: VHD linked at slot=%d", slot);
            KickPoll();
            return true;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] AttachVhd exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::CreateAndAttachVhd(std::wstring const& guid,
                                       std::wstring const& path,
                                       uint64_t sizeBytes,
                                       bool dynamic,
                                       std::wstring const& controllerRef,
                                       int slot)
    {
        if (!m_scope || path.empty() || sizeBytes == 0) return false;
        try
        {
            // Build the VHD settings: spawn a fresh Msvm_VirtualHardDiskSettingData
            // (no generated factory — get the class def + SpawnInstance), set
            // Type/Format/Path/MaxInternalSize, serialize to embedded XML.
            auto cls = m_scope->GetClass(L"Msvm_VirtualHardDiskSettingData");
            if (!cls)
            {
                HyprvAppLog(L"[vmm] CreateVhd: VirtualHardDiskSettingData class not found");
                return false;
            }
            CComPtr<IWbemClassObject> inst;
            if (FAILED(cls.Raw()->SpawnInstance(0, &inst)) || !inst)
            {
                HyprvAppLog(L"[vmm] CreateVhd: SpawnInstance failed");
                return false;
            }
            hyprv::wmi::hyperv::Msvm_VirtualHardDiskSettingData sd(
                hyprv::wmi::WmiObject(m_scope.get(), inst));
            sd.Type(static_cast<uint16_t>(dynamic ? 3 : 2));   // 3=dynamic, 2=fixed
            sd.Format(static_cast<uint16_t>(3));               // 3=VHDX
            sd.Path(path);
            sd.MaxInternalSize(sizeBytes);

            auto ims = m_scope->GetInstance(L"Msvm_ImageManagementService");
            if (!ims)
            {
                HyprvAppLog(L"[vmm] CreateVhd: ImageManagementService not found");
                return false;
            }
            auto in = ims.SpawnMethodIn(L"CreateVirtualHardDisk");
            in.Set(L"VirtualDiskSettingData", sd.GetCimXml());
            auto out = ims.InvokeMethod(L"CreateVirtualHardDisk", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
            HyprvAppLog(L"[vmm] CreateVhd path=%s size=%llu dynamic=%d ret=%u",
                path.c_str(), static_cast<unsigned long long>(sizeBytes),
                dynamic ? 1 : 0, ret);
            if (ret != 0 && ret != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope,
                out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
            if (!ok)
            {
                HyprvAppLog(L"[vmm] CreateVhd job failed: %s", desc.c_str());
                return false;
            }
            // Created — now attach it to the VM (honoring the picked target).
            return AttachVhd(guid, path, controllerRef, slot);
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] CreateAndAttachVhd exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::DetachVhd(std::wstring const& guid,
                              std::wstring const& vhdRef,
                              std::wstring const& driveRef)
    {
        if (!m_scope) return false;
        static_cast<void>(guid);
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;

            // Manual RemoveResourceSettings (generated wrapper is a stub).
            // Remove the VHD (child) first, then its disk drive (parent).
            auto removeOne = [&](std::wstring const& ref, const wchar_t* what) -> bool
            {
                if (ref.empty()) return true;
                auto in = vsms.SpawnMethodIn(L"RemoveResourceSettings");
                in.SetReferenceArray(L"ResourceSettings",
                                     std::vector<std::wstring>{ ref });
                auto out = vsms.InvokeMethod(L"RemoveResourceSettings", in);
                uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
                HyprvAppLog(L"[vmm] DetachVhd remove %s ret=%u", what, ret);
                if (ret != 0 && ret != 4096) return false;
                auto [ok, desc] = WaitForJob(*m_scope,
                    out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
                if (!ok) HyprvAppLog(L"[vmm] DetachVhd %s job failed: %s", what, desc.c_str());
                return ok;
            };
            bool okVhd   = removeOne(vhdRef,   L"vhd");
            bool okDrive = removeOne(driveRef, L"drive");
            if (okVhd || okDrive) KickPoll();
            return okVhd && okDrive;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] DetachVhd exception: %s", e.whatW.c_str());
            return false;
        }
    }

    // Default "Synthetic DVD Drive" RASD template (ResourceType 16). Same
    // shape as FindDefaultDiskDriveTemplate but a different ResourceSubType;
    // realized drives carry the VM GUID, so "Definition\...\Default" uniquely
    // picks the template.
    static hyprv::wmi::WmiObject FindDefaultDvdDriveTemplate(hyprv::wmi::WmiScope& scope)
    {
        auto rows = scope.Query(
            L"SELECT * FROM Msvm_ResourceAllocationSettingData "
            L"WHERE ResourceSubType='Microsoft:Hyper-V:Synthetic DVD Drive' "
            L"AND InstanceID LIKE 'Microsoft:Definition%Default'");
        if (!rows.empty()) return std::move(rows.front());
        return {};
    }

    bool VMManager::AddDvdDrive(std::wstring const& guid,
                                std::wstring const& controllerRef, int reqSlot)
    {
        if (!m_scope || guid.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto vssd = FindRealizedVssd(*m_scope, guid);
            if (!vssd) return false;
            const std::wstring vssdPath = vssd.Path();

            // Resolve target controller + slot (explicit picker, else auto-pick:
            // SCSI Gen 2 / IDE Gen 1, lowest free slot — disk + DVD share it).
            std::wstring ctrlPath;
            bool isScsi = true;
            int slot = 0;
            if (!ResolveStorageTarget(*m_scope, guid, controllerRef, reqSlot,
                                      ctrlPath, isScsi, slot))
            {
                HyprvAppLog(L"[vmm] AddDvdDrive: no controller/free slot for %s", guid.c_str());
                return false;
            }

            auto tmpl = FindDefaultDvdDriveTemplate(*m_scope);
            if (!tmpl)
            {
                HyprvAppLog(L"[vmm] AddDvdDrive: default DVD-drive template not found");
                return false;
            }
            tmpl.Set(L"Parent", ctrlPath);
            tmpl.Set(L"AddressOnParent", std::to_wstring(slot));
            auto in = vsms.SpawnMethodIn(L"AddResourceSettings");
            in.Set(L"AffectedConfiguration", vssdPath);
            in.SetStringArray(L"ResourceSettings",
                              std::vector<std::wstring>{ tmpl.GetCimXml() });
            auto out = vsms.InvokeMethod(L"AddResourceSettings", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
            HyprvAppLog(L"[vmm] AddDvdDrive scsi=%d slot=%d ret=%u",
                isScsi ? 1 : 0, slot, ret);
            if (ret != 0 && ret != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope,
                out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
            if (!ok) HyprvAppLog(L"[vmm] AddDvdDrive job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] AddDvdDrive exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::RemoveDvdDrive(std::wstring const& guid,
                                   std::wstring const& driveRef,
                                   std::wstring const& mediaRef)
    {
        if (!m_scope || driveRef.empty()) return false;
        static_cast<void>(guid);
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;

            // Manual RemoveResourceSettings (generated wrapper is a stub).
            // Eject the disc (media SASD) first if present, then the drive
            // RASD — mirrors DetachVhd (child before parent).
            auto removeOne = [&](std::wstring const& ref, const wchar_t* what) -> bool
            {
                if (ref.empty()) return true;
                auto in = vsms.SpawnMethodIn(L"RemoveResourceSettings");
                in.SetReferenceArray(L"ResourceSettings",
                                     std::vector<std::wstring>{ ref });
                auto out = vsms.InvokeMethod(L"RemoveResourceSettings", in);
                uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
                HyprvAppLog(L"[vmm] RemoveDvdDrive remove %s ret=%u", what, ret);
                if (ret != 0 && ret != 4096) return false;
                auto [ok, desc] = WaitForJob(*m_scope,
                    out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
                if (!ok) HyprvAppLog(L"[vmm] RemoveDvdDrive %s job failed: %s", what, desc.c_str());
                return ok;
            };
            bool okMedia = removeOne(mediaRef, L"media");
            bool okDrive = removeOne(driveRef, L"drive");
            if (okMedia || okDrive) KickPoll();
            return okMedia && okDrive;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] RemoveDvdDrive exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::SetMemoryConfig(std::wstring const& guid, MemoryConfig const& cfg)
    {
        if (!m_scope) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto memObj = FindResourceForVm(*m_scope, guid, L"Msvm_MemorySettingData");
            if (!memObj)
            {
                HyprvAppLog(L"[vmm] SetMemoryConfig: MemorySettingData not found for %s",
                    guid.c_str());
                return false;
            }
            hyprv::wmi::hyperv::Msvm_MemorySettingData mem(std::move(memObj));
            mem.VirtualQuantity      (cfg.startupMb);
            mem.DynamicMemoryEnabled (cfg.dynamicEnabled);
            mem.Reservation          (cfg.minMb);
            mem.Limit                (cfg.maxMb);
            mem.TargetMemoryBuffer   (cfg.targetBufferPct);
            mem.Weight               (cfg.priority);
            if (cfg.maxMemoryPerNumaNodeMb > 0)
                mem.MaxMemoryBlocksPerNumaNode(cfg.maxMemoryPerNumaNodeMb);   // NUMA, MB

            // GetCimXml returns the WMI-DTD-2.0 XML embedded-instance
            // form Hyper-V expects (verified against PowerShell's
            // Set-VMMemory wire format). MOF is rejected with CIM 32773.
            std::vector<std::wstring> texts{ mem.GetCimXml() };
            auto r = vsms.ModifyResourceSettings(texts);
            HyprvAppLog(L"[vmm] SetMemoryConfig guid=%s startup=%llu dyn=%d ret=%u",
                guid.c_str(),
                static_cast<unsigned long long>(cfg.startupMb),
                cfg.dynamicEnabled ? 1 : 0,
                r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            // 4096 = async job; wait + check actual outcome. Without this
            // the caller sees "success" but the job may have failed.
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok)
                HyprvAppLog(L"[vmm] SetMemoryConfig job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetMemoryConfig WmiException: %s", e.whatW.c_str());
            return false;
        }
        catch (std::exception const& e)
        {
            HyprvAppLog(L"[vmm] SetMemoryConfig std::exception: %hs", e.what());
            return false;
        }
        catch (...)
        {
            HyprvAppLog(L"[vmm] SetMemoryConfig: unknown exception");
            return false;
        }
    }

    bool VMManager::SetProcessorConfig(std::wstring const& guid, ProcessorConfig const& cfg)
    {
        if (!m_scope) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto procObj = FindResourceForVm(*m_scope, guid, L"Msvm_ProcessorSettingData");
            if (!procObj)
            {
                HyprvAppLog(L"[vmm] SetProcessorConfig: ProcessorSettingData not found for %s",
                    guid.c_str());
                return false;
            }
            hyprv::wmi::hyperv::Msvm_ProcessorSettingData proc(std::move(procObj));
            proc.VirtualQuantity        (cfg.count);
            proc.Reservation            (cfg.reservationPct);
            proc.Limit                  (cfg.limitPct);
            proc.Weight                 (cfg.weight);
            proc.LimitProcessorFeatures (cfg.limitProcessorFeatures);
            if (cfg.maxProcessorsPerNumaNode > 0)
                proc.MaxProcessorsPerNumaNode(cfg.maxProcessorsPerNumaNode);
            if (cfg.maxNumaNodesPerSocket > 0)
                proc.MaxNumaNodesPerSocket(cfg.maxNumaNodesPerSocket);
            proc.HwThreadsPerCore(cfg.hwThreadsPerCore);   // 0 = inherit host

            std::vector<std::wstring> texts{ proc.GetCimXml() };
            auto r = vsms.ModifyResourceSettings(texts);
            HyprvAppLog(L"[vmm] SetProcessorConfig guid=%s count=%u ret=%u",
                guid.c_str(), cfg.count, r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok)
                HyprvAppLog(L"[vmm] SetProcessorConfig job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetProcessorConfig WmiException: %s", e.whatW.c_str());
            return false;
        }
    }

    VMManager::MemoryConfig VMManager::GetMemoryConfig(std::wstring const& guid) const
    {
        MemoryConfig cfg;
        if (!m_scope) return cfg;
        try
        {
            auto mem = FindResourceForVm(*m_scope, guid, L"Msvm_MemorySettingData");
            if (!mem) return cfg;
            cfg.startupMb       = mem.GetUInt64(L"VirtualQuantity").value_or(0);
            cfg.dynamicEnabled  = mem.GetBool(L"DynamicMemoryEnabled").value_or(false);
            cfg.minMb           = mem.GetUInt64(L"Reservation").value_or(0);
            cfg.maxMb           = mem.GetUInt64(L"Limit").value_or(0);
            cfg.targetBufferPct = mem.GetUInt32(L"TargetMemoryBuffer").value_or(20);
            cfg.priority        = mem.GetUInt32(L"Weight").value_or(5000);
            cfg.maxMemoryPerNumaNodeMb = mem.GetUInt64(L"MaxMemoryBlocksPerNumaNode").value_or(0);
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GetMemoryConfig exception: %s", e.whatW.c_str());
        }
        return cfg;
    }

    VMManager::ProcessorConfig VMManager::GetProcessorConfig(std::wstring const& guid) const
    {
        ProcessorConfig cfg;
        if (!m_scope) return cfg;
        try
        {
            auto proc = FindResourceForVm(*m_scope, guid, L"Msvm_ProcessorSettingData");
            if (!proc) return cfg;
            cfg.count          = static_cast<uint16_t>(
                                     proc.GetUInt64(L"VirtualQuantity").value_or(1));
            cfg.reservationPct = proc.GetUInt64(L"Reservation").value_or(0);       // raw (×1000)
            cfg.limitPct       = proc.GetUInt64(L"Limit").value_or(100000);        // raw (×1000)
            cfg.weight         = proc.GetUInt32(L"Weight").value_or(100);
            cfg.limitProcessorFeatures   = proc.GetBool(L"LimitProcessorFeatures").value_or(false);
            cfg.maxProcessorsPerNumaNode = proc.GetUInt64(L"MaxProcessorsPerNumaNode").value_or(0);
            cfg.maxNumaNodesPerSocket    = proc.GetUInt64(L"MaxNumaNodesPerSocket").value_or(0);
            cfg.hwThreadsPerCore         = proc.GetUInt64(L"HwThreadsPerCore").value_or(0);
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GetProcessorConfig exception: %s", e.whatW.c_str());
        }
        return cfg;
    }

    // Class names of every integration-component setting data class. Order
    // matches Hyper-V Manager's display order. Each has the same shape:
    // { ElementName (display label), Enabled (bool) }, so generic WmiObject
    // get/set works — no typed wrappers needed.
    static const wchar_t* const kIntegrationServiceClasses[] = {
        L"Msvm_HeartbeatComponentSettingData",
        L"Msvm_KvpExchangeComponentSettingData",
        L"Msvm_ShutdownComponentSettingData",
        L"Msvm_TimeSyncComponentSettingData",
        L"Msvm_VssComponentSettingData",
        L"Msvm_GuestServiceInterfaceComponentSettingData",
    };

    std::vector<VMManager::IntegrationService>
    VMManager::GetIntegrationServices(std::wstring const& guid) const
    {
        std::vector<IntegrationService> result;
        if (!m_scope) return result;
        try
        {
            auto vssd = FindRealizedVssd(*m_scope, guid);
            if (!vssd) return result;
            for (auto* cls : kIntegrationServiceClasses)
            {
                auto rows = vssd.GetAssociated(
                    L"Msvm_VirtualSystemSettingDataComponent",   // assocClass
                    cls);                                        // resultClass
                if (rows.empty()) continue;
                auto const& obj = rows.front();
                IntegrationService is;
                is.className   = cls;
                is.displayName = obj.GetString(L"ElementName").value_or(std::wstring{});
                // Integration component setting data classes use the
                // CIM-standard EnabledState uint16 (2=Enabled, 3=Disabled,
                // others=transitional/unknown), NOT a bool "Enabled".
                auto state = obj.GetUInt16(L"EnabledState").value_or(0);
                is.enabled     = (state == 2);
                result.push_back(std::move(is));
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GetIntegrationServices exception: %s",
                e.whatW.c_str());
        }
        return result;
    }

    bool VMManager::SetIntegrationServiceEnabled(std::wstring const& guid,
                                                  std::wstring const& className,
                                                  bool enabled)
    {
        if (!m_scope) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto vssd = FindRealizedVssd(*m_scope, guid);
            if (!vssd) return false;
            auto rows = vssd.GetAssociated(
                L"Msvm_VirtualSystemSettingDataComponent",   // assocClass
                className.c_str());                          // resultClass
            if (rows.empty())
            {
                HyprvAppLog(L"[vmm] SetIntegrationServiceEnabled: %s not found for %s",
                    className.c_str(), guid.c_str());
                return false;
            }
            auto obj = std::move(rows.front());
            // CIM EnabledState: 2 = Enabled, 3 = Disabled.
            obj.Set(L"EnabledState", static_cast<uint16_t>(enabled ? 2 : 3));
            // Same XML format as SetMemoryConfig — Hyper-V rejects MOF
            // (GetText) with CIM 32773; it wants WMI DTD 2.0 XML.
            std::vector<std::wstring> texts{ obj.GetCimXml() };
            auto r = vsms.ModifyResourceSettings(texts);
            HyprvAppLog(L"[vmm] SetIntegrationServiceEnabled guid=%s class=%s enabled=%d ret=%u",
                guid.c_str(), className.c_str(), enabled ? 1 : 0, r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok)
                HyprvAppLog(L"[vmm] SetIntegrationServiceEnabled job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetIntegrationServiceEnabled WmiException: %s",
                e.whatW.c_str());
            return false;
        }
        catch (std::exception const& e)
        {
            HyprvAppLog(L"[vmm] SetIntegrationServiceEnabled std::exception: %hs", e.what());
            return false;
        }
        catch (...)
        {
            HyprvAppLog(L"[vmm] SetIntegrationServiceEnabled: unknown exception");
            return false;
        }
    }

    std::vector<std::wstring> VMManager::GetVirtualSwitches() const
    {
        std::vector<std::wstring> out;
        if (!m_scope) return out;
        try
        {
            auto rows = m_scope->Query(
                L"SELECT * FROM Msvm_VirtualEthernetSwitch");
            for (auto& r : rows)
            {
                hyprv::wmi::hyperv::Msvm_VirtualEthernetSwitch sw(std::move(r));
                if (auto name = sw.ElementName())
                    out.push_back(*name);
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] GetVirtualSwitches exception: %s", e.whatW.c_str());
        }
        return out;
    }

    bool VMManager::SetNetworkAdapterSwitch(std::wstring const& vmGuid,
                                             std::wstring const& nicGuid,
                                             std::wstring const& switchName)
    {
        if (!m_scope || vmGuid.empty() || nicGuid.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;

            // The allocation row's InstanceID embeds both the VM-GUID and
            // the NIC-GUID. Matching by both keeps us scoped to the one row
            // even if multiple VMs share a NIC name. WQL LIKE is the only
            // operator that handles the embedded backslash without escape
            // games.
            std::wstring wql =
                L"SELECT * FROM Msvm_EthernetPortAllocationSettingData "
                L"WHERE InstanceID LIKE 'Microsoft:" + vmGuid +
                L"%" + nicGuid + L"%'";
            auto rows = m_scope->Query(wql.c_str());
            if (rows.empty())
            {
                HyprvAppLog(L"[vmm] SetNetworkAdapterSwitch: allocation not found "
                            L"vm=%s nic=%s", vmGuid.c_str(), nicGuid.c_str());
                return false;
            }
            hyprv::wmi::hyperv::Msvm_EthernetPortAllocationSettingData alloc(
                std::move(rows.front()));

            // Disconnect path: HostResource cleared, EnabledState=3
            // (Disabled). Matches Hyper-V Manager's "Not connected" wire
            // shape (verified against Disconnect-VMNetworkAdapter output).
            // Connect path: resolve the switch by ElementName -> __PATH,
            // set HostResource=[<switch_path>], EnabledState=2 (Enabled).
            if (switchName.empty())
            {
                alloc.HostResource({});
                alloc.EnabledState(static_cast<uint16_t>(3));
            }
            else
            {
                std::wstring swWql =
                    L"SELECT * FROM Msvm_VirtualEthernetSwitch "
                    L"WHERE ElementName='" + switchName + L"'";
                auto swRows = m_scope->Query(swWql.c_str());
                if (swRows.empty())
                {
                    HyprvAppLog(L"[vmm] SetNetworkAdapterSwitch: switch not found name=%s",
                        switchName.c_str());
                    return false;
                }
                std::wstring swPath = swRows.front().Path();
                if (swPath.empty())
                {
                    HyprvAppLog(L"[vmm] SetNetworkAdapterSwitch: switch path empty name=%s",
                        switchName.c_str());
                    return false;
                }
                alloc.HostResource(std::vector<std::wstring>{ swPath });
                alloc.EnabledState(static_cast<uint16_t>(2));
            }

            std::vector<std::wstring> texts{ alloc.GetCimXml() };
            auto r = vsms.ModifyResourceSettings(texts);
            HyprvAppLog(L"[vmm] SetNetworkAdapterSwitch vm=%s nic=%s switch=%s ret=%u",
                vmGuid.c_str(), nicGuid.c_str(),
                switchName.empty() ? L"(disconnect)" : switchName.c_str(),
                r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok)
                HyprvAppLog(L"[vmm] SetNetworkAdapterSwitch job failed: %s",
                    desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetNetworkAdapterSwitch WmiException: %s",
                e.whatW.c_str());
            return false;
        }
        catch (std::exception const& e)
        {
            HyprvAppLog(L"[vmm] SetNetworkAdapterSwitch std::exception: %hs", e.what());
            return false;
        }
        catch (...)
        {
            HyprvAppLog(L"[vmm] SetNetworkAdapterSwitch: unknown exception");
            return false;
        }
    }

    bool VMManager::SetNetworkAdapterMac(std::wstring const& vmGuid,
                                         std::wstring const& nicGuid,
                                         bool dynamic,
                                         std::wstring const& staticMac)
    {
        if (!m_scope || vmGuid.empty() || nicGuid.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;

            // The synthetic-port setting data InstanceID embeds both GUIDs:
            // "Microsoft:<VM-GUID>\<NIC-GUID>". Match both to scope to the one
            // NIC (same approach as SetNetworkAdapterSwitch).
            std::wstring wql =
                L"SELECT * FROM Msvm_SyntheticEthernetPortSettingData "
                L"WHERE InstanceID LIKE 'Microsoft:" + vmGuid +
                L"%" + nicGuid + L"%'";
            auto rows = m_scope->Query(wql.c_str());
            if (rows.empty())
            {
                HyprvAppLog(L"[vmm] SetNetworkAdapterMac: NIC setting data not found "
                            L"vm=%s nic=%s", vmGuid.c_str(), nicGuid.c_str());
                return false;
            }
            auto nic = std::move(rows.front());

            if (dynamic)
            {
                // Dynamic = empty Address + StaticMacAddress false. Hyper-V
                // assigns from the host pool. Empty (not zeros) is the marker
                // — verified against existing dynamic NICs, which carry an
                // empty Address.
                nic.Set(L"Address", L"");
                nic.Set(L"StaticMacAddress", false);
            }
            else
            {
                if (staticMac.size() != 12)
                {
                    HyprvAppLog(L"[vmm] SetNetworkAdapterMac: bad static MAC len=%zu",
                                staticMac.size());
                    return false;
                }
                nic.Set(L"Address", staticMac);
                nic.Set(L"StaticMacAddress", true);
            }

            std::vector<std::wstring> texts{ nic.GetCimXml() };
            auto r = vsms.ModifyResourceSettings(texts);
            HyprvAppLog(L"[vmm] SetNetworkAdapterMac vm=%s nic=%s dynamic=%d mac=%s ret=%u",
                vmGuid.c_str(), nicGuid.c_str(), dynamic ? 1 : 0,
                dynamic ? L"(pool)" : staticMac.c_str(), r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok)
                HyprvAppLog(L"[vmm] SetNetworkAdapterMac job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetNetworkAdapterMac WmiException: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::SetNetworkAdapterProtectedNetwork(std::wstring const& vmGuid,
                                                      std::wstring const& nicGuid,
                                                      bool clusterMonitored)
    {
        if (!m_scope || vmGuid.empty() || nicGuid.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            std::wstring wql =
                L"SELECT * FROM Msvm_SyntheticEthernetPortSettingData "
                L"WHERE InstanceID LIKE 'Microsoft:" + vmGuid +
                L"%" + nicGuid + L"%'";
            auto rows = m_scope->Query(wql.c_str());
            if (rows.empty())
            {
                HyprvAppLog(L"[vmm] SetNetworkAdapterProtectedNetwork: NIC not found "
                            L"vm=%s nic=%s", vmGuid.c_str(), nicGuid.c_str());
                return false;
            }
            auto nic = std::move(rows.front());
            nic.Set(L"ClusterMonitored", clusterMonitored);
            std::vector<std::wstring> texts{ nic.GetCimXml() };
            auto r = vsms.ModifyResourceSettings(texts);
            HyprvAppLog(L"[vmm] SetNetworkAdapterProtectedNetwork vm=%s nic=%s on=%d ret=%u",
                vmGuid.c_str(), nicGuid.c_str(), clusterMonitored ? 1 : 0, r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok)
                HyprvAppLog(L"[vmm] SetNetworkAdapterProtectedNetwork job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetNetworkAdapterProtectedNetwork WmiException: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::SetNetworkAdapterDeviceNaming(std::wstring const& vmGuid,
                                                  std::wstring const& nicGuid,
                                                  bool enabled)
    {
        if (!m_scope || vmGuid.empty() || nicGuid.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            // DeviceNamingEnabled is a bool on the synthetic port — same shape /
            // write path as ClusterMonitored (ModifyResourceSettings).
            std::wstring wql =
                L"SELECT * FROM Msvm_SyntheticEthernetPortSettingData "
                L"WHERE InstanceID LIKE 'Microsoft:" + vmGuid +
                L"%" + nicGuid + L"%'";
            auto rows = m_scope->Query(wql.c_str());
            if (rows.empty())
            {
                HyprvAppLog(L"[vmm] SetNetworkAdapterDeviceNaming: NIC not found "
                            L"vm=%s nic=%s", vmGuid.c_str(), nicGuid.c_str());
                return false;
            }
            auto nic = std::move(rows.front());
            nic.Set(L"DeviceNamingEnabled", enabled);
            std::vector<std::wstring> texts{ nic.GetCimXml() };
            auto r = vsms.ModifyResourceSettings(texts);
            HyprvAppLog(L"[vmm] SetNetworkAdapterDeviceNaming vm=%s nic=%s on=%d ret=%u",
                vmGuid.c_str(), nicGuid.c_str(), enabled ? 1 : 0, r.ReturnValue);
            if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope, r.Job);
            if (!ok)
                HyprvAppLog(L"[vmm] SetNetworkAdapterDeviceNaming job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetNetworkAdapterDeviceNaming WmiException: %s", e.whatW.c_str());
            return false;
        }
    }

    // Default offload feature-setting template (for the rare Add path; the
    // offload setting normally already exists on every connection).
    static hyprv::wmi::WmiObject FindDefaultNicOffloadTemplate(hyprv::wmi::WmiScope& scope)
    {
        auto rows = scope.Query(
            L"SELECT * FROM Msvm_EthernetSwitchPortOffloadSettingData "
            L"WHERE InstanceID LIKE 'Microsoft:Definition%Default'");
        if (!rows.empty()) return std::move(rows.front());
        return {};
    }

    bool VMManager::SetNetworkAdapterOffload(std::wstring const& vmGuid,
                                             std::wstring const& nicGuid,
                                             NicOffloadFeatures const& f)
    {
        if (!m_scope || vmGuid.empty() || nicGuid.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;

            // The offload setting (a connection feature setting) ALWAYS exists
            // by default, so this is virtually always a Modify; the Add path is
            // kept as a defensive mirror of SetNetworkAdapterAdvanced.
            std::wstring existingRef, allocRef;
            {
                auto rows = m_scope->Query(
                    (L"SELECT * FROM Msvm_EthernetSwitchPortOffloadSettingData "
                     L"WHERE InstanceID LIKE 'Microsoft:" + vmGuid + L"%"
                     + nicGuid + L"%'").c_str());
                if (!rows.empty()) existingRef = rows.front().Path();
            }
            {
                auto rows = m_scope->Query(
                    (L"SELECT * FROM Msvm_EthernetPortAllocationSettingData "
                     L"WHERE InstanceID LIKE 'Microsoft:" + vmGuid + L"%"
                     + nicGuid + L"%'").c_str());
                if (!rows.empty()) allocRef = rows.front().Path();
            }

            auto fill = [&](hyprv::wmi::WmiObject& s)
            {
                s.Set(L"VMQOffloadWeight", static_cast<uint32_t>(f.vmq ? 100 : 0));
                s.Set(L"IOVOffloadWeight", static_cast<uint32_t>(f.sriov ? 100 : 0));
                s.Set(L"IPSecOffloadLimit",
                      static_cast<uint32_t>(f.ipsecOffload ? f.ipsecOffloadMaxSA : 0));
            };

            if (!existingRef.empty())
            {
                auto s = m_scope->GetByPath(existingRef.c_str());
                if (!s) return false;
                fill(s);
                auto r = vsms.ModifyFeatureSettings(
                    std::vector<std::wstring>{ s.GetCimXml() });
                HyprvAppLog(L"[vmm] SetNetworkAdapterOffload MODIFY nic=%s "
                            L"vmq=%d sriov=%d ipsec=%d maxSA=%u ret=%u",
                    nicGuid.c_str(), f.vmq, f.sriov, f.ipsecOffload,
                    f.ipsecOffloadMaxSA, r.ReturnValue);
                if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
                auto [ok, desc] = WaitForJob(*m_scope, r.Job);
                if (!ok)
                    HyprvAppLog(L"[vmm] SetNetworkAdapterOffload modify job failed: %s", desc.c_str());
                if (ok) KickPoll();
                return ok;
            }

            if (allocRef.empty())
            {
                HyprvAppLog(L"[vmm] SetNetworkAdapterOffload: NIC connection not found "
                            L"vm=%s nic=%s", vmGuid.c_str(), nicGuid.c_str());
                return false;
            }
            auto tmpl = FindDefaultNicOffloadTemplate(*m_scope);
            if (!tmpl)
            {
                HyprvAppLog(L"[vmm] SetNetworkAdapterOffload: offload template not found");
                return false;
            }
            fill(tmpl);
            auto in = vsms.SpawnMethodIn(L"AddFeatureSettings");
            in.Set(L"AffectedConfiguration", allocRef);
            in.SetStringArray(L"FeatureSettings",
                              std::vector<std::wstring>{ tmpl.GetCimXml() });
            auto out = vsms.InvokeMethod(L"AddFeatureSettings", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
            HyprvAppLog(L"[vmm] SetNetworkAdapterOffload ADD nic=%s ret=%u", nicGuid.c_str(), ret);
            if (ret != 0 && ret != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope,
                out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
            if (!ok)
                HyprvAppLog(L"[vmm] SetNetworkAdapterOffload add job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetNetworkAdapterOffload exception: %s", e.whatW.c_str());
            return false;
        }
    }

    // ---- NIC add/remove + VLAN helpers ------------------------------------
    // The default "Definition\<guid>\Default" templates that AddResourceSettings
    // / AddFeatureSettings expect as the basis for a new object. Realized
    // objects carry the VM GUID instead, so "Definition" + Default uniquely
    // picks the template (same pattern as FindDefaultDvdMediaTemplate).
    static hyprv::wmi::WmiObject FindDefaultSyntheticEthernetPortTemplate(
        hyprv::wmi::WmiScope& scope)
    {
        auto rows = scope.Query(
            L"SELECT * FROM Msvm_SyntheticEthernetPortSettingData "
            L"WHERE InstanceID LIKE 'Microsoft:Definition%Default'");
        if (!rows.empty()) return std::move(rows.front());
        return {};
    }
    static hyprv::wmi::WmiObject FindDefaultEthernetConnectionTemplate(
        hyprv::wmi::WmiScope& scope)
    {
        auto rows = scope.Query(
            L"SELECT * FROM Msvm_EthernetPortAllocationSettingData "
            L"WHERE InstanceID LIKE 'Microsoft:Definition%Default'");
        if (!rows.empty()) return std::move(rows.front());
        return {};
    }
    static hyprv::wmi::WmiObject FindDefaultVlanTemplate(hyprv::wmi::WmiScope& scope)
    {
        auto rows = scope.Query(
            L"SELECT * FROM Msvm_EthernetSwitchPortVlanSettingData "
            L"WHERE InstanceID LIKE 'Microsoft:Definition%Default'");
        if (!rows.empty()) return std::move(rows.front());
        return {};
    }

    // A fresh braced GUID for VirtualSystemIdentifiers, e.g.
    // "{A83AB18D-2A91-4CE0-AD16-9978BD51A9CF}". Hyper-V assigns a second
    // identifier itself after the add — one fresh GUID is all we set. Casing
    // is irrelevant (Hyper-V compares GUIDs case-insensitively).
    static std::wstring MakeBracedGuid()
    {
        GUID g{};
        if (FAILED(CoCreateGuid(&g))) return {};
        wchar_t buf[64]{};
        if (StringFromGUID2(g, buf, ARRAYSIZE(buf)) == 0) return {};
        return std::wstring{ buf };
    }

    bool VMManager::AddNetworkAdapter(std::wstring const& vmGuid,
                                      std::wstring const& switchName)
    {
        if (!m_scope || vmGuid.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;
            auto vssd = FindRealizedVssd(*m_scope, vmGuid);
            if (!vssd) return false;
            const std::wstring vssdPath = vssd.Path();

            // Step 1: add the synthetic port — clone the default template, set
            // a display name + a fresh VSI GUID (so we can find the realized
            // port afterwards; its ResultingResourceSettings comes back as
            // embedded XML, gotcha #9). MAC is left to the pool.
            auto portTmpl = FindDefaultSyntheticEthernetPortTemplate(*m_scope);
            if (!portTmpl)
            {
                HyprvAppLog(L"[vmm] AddNetworkAdapter: synthetic port template not found");
                return false;
            }
            std::wstring vsi = MakeBracedGuid();
            if (vsi.empty()) return false;
            portTmpl.Set(L"ElementName", L"Network Adapter");
            portTmpl.SetStringArray(L"VirtualSystemIdentifiers",
                                    std::vector<std::wstring>{ vsi });
            {
                auto in = vsms.SpawnMethodIn(L"AddResourceSettings");
                in.Set(L"AffectedConfiguration", vssdPath);
                in.SetStringArray(L"ResourceSettings",
                                  std::vector<std::wstring>{ portTmpl.GetCimXml() });
                auto out = vsms.InvokeMethod(L"AddResourceSettings", in);
                uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
                HyprvAppLog(L"[vmm] AddNetworkAdapter add-port vm=%s ret=%u",
                    vmGuid.c_str(), ret);
                if (ret != 0 && ret != 4096) return false;
                auto [ok, desc] = WaitForJob(*m_scope,
                    out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
                if (!ok)
                {
                    HyprvAppLog(L"[vmm] AddNetworkAdapter add-port job failed: %s", desc.c_str());
                    return false;
                }
            }

            // Step 2: find the new port by the VSI GUID, then add its Ethernet
            // Connection (connected to switchName, or disconnected if empty).
            std::wstring newPortRef;
            for (auto& p : m_scope->Query(
                    (L"SELECT * FROM Msvm_SyntheticEthernetPortSettingData "
                     L"WHERE InstanceID LIKE 'Microsoft:" + vmGuid + L"%'").c_str()))
            {
                bool match = false;
                for (auto const& id : p.GetStringArray(L"VirtualSystemIdentifiers"))
                    if (_wcsicmp(id.c_str(), vsi.c_str()) == 0) { match = true; break; }
                if (match) { newPortRef = p.Path(); break; }
            }
            if (newPortRef.empty())
            {
                HyprvAppLog(L"[vmm] AddNetworkAdapter: new port not found after add");
                return false;
            }

            auto connTmpl = FindDefaultEthernetConnectionTemplate(*m_scope);
            if (!connTmpl)
            {
                HyprvAppLog(L"[vmm] AddNetworkAdapter: ethernet connection template not found");
                return false;
            }
            connTmpl.Set(L"Parent", newPortRef);
            if (switchName.empty())
            {
                // Disconnected: empty HostResource + EnabledState 3 (Disabled)
                // — same wire shape SetNetworkAdapterSwitch uses to disconnect.
                connTmpl.SetStringArray(L"HostResource", {});
                connTmpl.Set(L"EnabledState", static_cast<uint16_t>(3));
            }
            else
            {
                auto swRows = m_scope->Query(
                    (L"SELECT * FROM Msvm_VirtualEthernetSwitch "
                     L"WHERE ElementName='" + switchName + L"'").c_str());
                if (swRows.empty())
                {
                    HyprvAppLog(L"[vmm] AddNetworkAdapter: switch not found name=%s",
                        switchName.c_str());
                    return false;
                }
                connTmpl.SetStringArray(L"HostResource",
                    std::vector<std::wstring>{ swRows.front().Path() });
                connTmpl.Set(L"EnabledState", static_cast<uint16_t>(2));
            }
            {
                auto in = vsms.SpawnMethodIn(L"AddResourceSettings");
                in.Set(L"AffectedConfiguration", vssdPath);
                in.SetStringArray(L"ResourceSettings",
                                  std::vector<std::wstring>{ connTmpl.GetCimXml() });
                auto out = vsms.InvokeMethod(L"AddResourceSettings", in);
                uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
                HyprvAppLog(L"[vmm] AddNetworkAdapter add-conn switch=%s ret=%u",
                    switchName.empty() ? L"(disconnect)" : switchName.c_str(), ret);
                if (ret != 0 && ret != 4096) return false;
                auto [ok, desc] = WaitForJob(*m_scope,
                    out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
                if (!ok)
                    HyprvAppLog(L"[vmm] AddNetworkAdapter add-conn job failed: %s", desc.c_str());
                if (ok) KickPoll();
                return ok;
            }
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] AddNetworkAdapter exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::RemoveNetworkAdapter(std::wstring const& vmGuid,
                                         std::wstring const& nicGuid)
    {
        if (!m_scope || vmGuid.empty() || nicGuid.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;

            // Resolve the synthetic port + its Ethernet Connection allocation.
            // Both InstanceIDs embed "Microsoft:<VM-GUID>...<NIC-GUID>"; the
            // class filter keeps each query to the right object.
            std::wstring portRef, allocRef;
            {
                auto rows = m_scope->Query(
                    (L"SELECT * FROM Msvm_SyntheticEthernetPortSettingData "
                     L"WHERE InstanceID LIKE 'Microsoft:" + vmGuid + L"%"
                     + nicGuid + L"%'").c_str());
                if (!rows.empty()) portRef = rows.front().Path();
            }
            {
                auto rows = m_scope->Query(
                    (L"SELECT * FROM Msvm_EthernetPortAllocationSettingData "
                     L"WHERE InstanceID LIKE 'Microsoft:" + vmGuid + L"%"
                     + nicGuid + L"%'").c_str());
                if (!rows.empty()) allocRef = rows.front().Path();
            }
            if (portRef.empty())
            {
                HyprvAppLog(L"[vmm] RemoveNetworkAdapter: port not found vm=%s nic=%s",
                    vmGuid.c_str(), nicGuid.c_str());
                return false;
            }

            // Manual RemoveResourceSettings (generated wrapper is a stub).
            // Connection (child) first, then the port (parent) — mirrors
            // DetachVhd. Removing the child first is safe regardless of
            // whether removing the port would have cascaded it.
            auto removeOne = [&](std::wstring const& ref, const wchar_t* what) -> bool
            {
                if (ref.empty()) return true;
                auto in = vsms.SpawnMethodIn(L"RemoveResourceSettings");
                in.SetReferenceArray(L"ResourceSettings",
                                     std::vector<std::wstring>{ ref });
                auto out = vsms.InvokeMethod(L"RemoveResourceSettings", in);
                uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
                HyprvAppLog(L"[vmm] RemoveNetworkAdapter remove %s ret=%u", what, ret);
                if (ret != 0 && ret != 4096) return false;
                auto [ok, desc] = WaitForJob(*m_scope,
                    out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
                if (!ok)
                    HyprvAppLog(L"[vmm] RemoveNetworkAdapter %s job failed: %s", what, desc.c_str());
                return ok;
            };
            bool okAlloc = removeOne(allocRef, L"connection");
            bool okPort  = removeOne(portRef,  L"port");
            if (okAlloc || okPort) KickPoll();
            return okAlloc && okPort;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] RemoveNetworkAdapter exception: %s", e.whatW.c_str());
            return false;
        }
    }

    bool VMManager::SetNetworkAdapterVlan(std::wstring const& vmGuid,
                                          std::wstring const& nicGuid,
                                          uint16_t vlanId)
    {
        // Back-compat thin wrapper: untag (0) / access (1..4094).
        VlanConfig c;
        c.mode = vlanId ? 1 : 0;
        c.accessVlanId = vlanId;
        return SetNetworkAdapterVlan(vmGuid, nicGuid, c);
    }

    bool VMManager::SetNetworkAdapterVlan(std::wstring const& vmGuid,
                                          std::wstring const& nicGuid,
                                          VlanConfig const& cfg)
    {
        if (!m_scope || vmGuid.empty() || nicGuid.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;

            // Set every VLAN field explicitly, zeroing the ones the active mode
            // doesn't use, so a mode switch (e.g. access→trunk) leaves no stale
            // value behind. OperationMode gates which Hyper-V honours; the empty
            // arrays for inactive modes are simply ignored.
            auto applyFields = [&](hyprv::wmi::WmiObject& v) {
                v.Set(L"OperationMode",   static_cast<uint32_t>(cfg.mode));
                v.Set(L"AccessVlanId",    static_cast<uint16_t>(cfg.mode == 1 ? cfg.accessVlanId : 0));
                v.Set(L"NativeVlanId",    static_cast<uint16_t>(cfg.mode == 2 ? cfg.nativeVlanId : 0));
                v.SetUInt16Array(L"TrunkVlanIdArray",
                                 cfg.mode == 2 ? cfg.trunkVlanList : std::vector<uint16_t>{});
                v.Set(L"PrimaryVlanId",   static_cast<uint16_t>(cfg.mode == 3 ? cfg.primaryVlanId : 0));
                v.Set(L"SecondaryVlanId", static_cast<uint16_t>(cfg.mode == 3 ? cfg.secondaryVlanId : 0));
                v.Set(L"PvlanMode",       static_cast<uint32_t>(cfg.mode == 3 ? cfg.pvlanMode : 0));
                v.SetUInt16Array(L"SecondaryVlanIdArray",
                                 cfg.mode == 3 ? cfg.secondaryVlanList : std::vector<uint16_t>{});
            };

            // Existing VLAN feature setting (if any) + the NIC's Ethernet
            // Connection allocation (the AddFeatureSettings target). Both
            // InstanceIDs embed "Microsoft:<VM-GUID>...<NIC-GUID>".
            std::wstring existingVlanRef, allocRef;
            {
                auto rows = m_scope->Query(
                    (L"SELECT * FROM Msvm_EthernetSwitchPortVlanSettingData "
                     L"WHERE InstanceID LIKE 'Microsoft:" + vmGuid + L"%"
                     + nicGuid + L"%'").c_str());
                if (!rows.empty()) existingVlanRef = rows.front().Path();
            }
            {
                auto rows = m_scope->Query(
                    (L"SELECT * FROM Msvm_EthernetPortAllocationSettingData "
                     L"WHERE InstanceID LIKE 'Microsoft:" + vmGuid + L"%"
                     + nicGuid + L"%'").c_str());
                if (!rows.empty()) allocRef = rows.front().Path();
            }

            // Untag — remove the VLAN feature setting (manual RemoveFeatureSettings,
            // ReferenceArray in-param; generated wrapper is a stub).
            if (cfg.mode == 0)
            {
                if (existingVlanRef.empty()) return true;   // already untagged
                auto in = vsms.SpawnMethodIn(L"RemoveFeatureSettings");
                in.SetReferenceArray(L"FeatureSettings",
                                     std::vector<std::wstring>{ existingVlanRef });
                auto out = vsms.InvokeMethod(L"RemoveFeatureSettings", in);
                uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
                HyprvAppLog(L"[vmm] SetNetworkAdapterVlan UNTAG nic=%s ret=%u",
                    nicGuid.c_str(), ret);
                if (ret != 0 && ret != 4096) return false;
                auto [ok, desc] = WaitForJob(*m_scope,
                    out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
                if (!ok)
                    HyprvAppLog(L"[vmm] SetNetworkAdapterVlan untag job failed: %s", desc.c_str());
                if (ok) KickPoll();
                return ok;
            }

            // Set the VLAN — modify the existing feature setting, or add one.
            if (!existingVlanRef.empty())
            {
                auto v = m_scope->GetByPath(existingVlanRef.c_str());
                if (!v) return false;
                applyFields(v);
                auto r = vsms.ModifyFeatureSettings(
                    std::vector<std::wstring>{ v.GetCimXml() });
                HyprvAppLog(L"[vmm] SetNetworkAdapterVlan MODIFY nic=%s mode=%u ret=%u",
                    nicGuid.c_str(), cfg.mode, r.ReturnValue);
                if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
                auto [ok, desc] = WaitForJob(*m_scope, r.Job);
                if (!ok)
                    HyprvAppLog(L"[vmm] SetNetworkAdapterVlan modify job failed: %s", desc.c_str());
                if (ok) KickPoll();
                return ok;
            }

            if (allocRef.empty())
            {
                HyprvAppLog(L"[vmm] SetNetworkAdapterVlan: NIC connection not found "
                            L"vm=%s nic=%s", vmGuid.c_str(), nicGuid.c_str());
                return false;
            }
            auto tmpl = FindDefaultVlanTemplate(*m_scope);
            if (!tmpl)
            {
                HyprvAppLog(L"[vmm] SetNetworkAdapterVlan: VLAN template not found");
                return false;
            }
            applyFields(tmpl);
            // AddFeatureSettings.AffectedConfiguration is a REF — pass the
            // allocation __PATH string (like AddResourceSettings). Invoke
            // manually: the generated wrapper marshals AffectedConfiguration
            // as an embedded instance, which is wrong for a reference param.
            auto in = vsms.SpawnMethodIn(L"AddFeatureSettings");
            in.Set(L"AffectedConfiguration", allocRef);
            in.SetStringArray(L"FeatureSettings",
                              std::vector<std::wstring>{ tmpl.GetCimXml() });
            auto out = vsms.InvokeMethod(L"AddFeatureSettings", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
            HyprvAppLog(L"[vmm] SetNetworkAdapterVlan ADD nic=%s mode=%u ret=%u",
                nicGuid.c_str(), cfg.mode, ret);
            if (ret != 0 && ret != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope,
                out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
            if (!ok)
                HyprvAppLog(L"[vmm] SetNetworkAdapterVlan add job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetNetworkAdapterVlan exception: %s", e.whatW.c_str());
            return false;
        }
    }

    static hyprv::wmi::WmiObject FindDefaultNicSecurityTemplate(hyprv::wmi::WmiScope& scope)
    {
        auto rows = scope.Query(
            L"SELECT * FROM Msvm_EthernetSwitchPortSecuritySettingData "
            L"WHERE InstanceID LIKE 'Microsoft:Definition%Default'");
        if (!rows.empty()) return std::move(rows.front());
        return {};
    }

    bool VMManager::SetNetworkAdapterAdvanced(std::wstring const& vmGuid,
                                              std::wstring const& nicGuid,
                                              NicAdvancedFeatures const& f)
    {
        if (!m_scope || vmGuid.empty() || nicGuid.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;

            // Existing security feature setting (if any) + the NIC's Ethernet
            // Connection allocation (the AddFeatureSettings target). Both
            // InstanceIDs embed "Microsoft:<VM-GUID>...<NIC-GUID>".
            std::wstring existingRef, allocRef;
            {
                auto rows = m_scope->Query(
                    (L"SELECT * FROM Msvm_EthernetSwitchPortSecuritySettingData "
                     L"WHERE InstanceID LIKE 'Microsoft:" + vmGuid + L"%"
                     + nicGuid + L"%'").c_str());
                if (!rows.empty()) existingRef = rows.front().Path();
            }
            {
                auto rows = m_scope->Query(
                    (L"SELECT * FROM Msvm_EthernetPortAllocationSettingData "
                     L"WHERE InstanceID LIKE 'Microsoft:" + vmGuid + L"%"
                     + nicGuid + L"%'").c_str());
                if (!rows.empty()) allocRef = rows.front().Path();
            }

            // Fill the security object's properties (existing or template).
            auto fill = [&](hyprv::wmi::WmiObject& s)
            {
                s.Set(L"AllowMacSpoofing",  f.macSpoofing);
                s.Set(L"EnableDhcpGuard",   f.dhcpGuard);
                s.Set(L"EnableRouterGuard", f.routerGuard);
                s.Set(L"AllowTeaming",      f.nicTeaming);
                s.Set(L"MonitorMode",       static_cast<uint16_t>(f.portMirroring));
                s.Set(L"AllowIeeePriorityTag", f.ieeePriorityTag);
            };

            if (!existingRef.empty())
            {
                auto s = m_scope->GetByPath(existingRef.c_str());
                if (!s) return false;
                fill(s);
                auto r = vsms.ModifyFeatureSettings(
                    std::vector<std::wstring>{ s.GetCimXml() });
                HyprvAppLog(L"[vmm] SetNetworkAdapterAdvanced MODIFY nic=%s "
                            L"spoof=%d dhcp=%d router=%d team=%d mirror=%u ieee=%d ret=%u",
                    nicGuid.c_str(), f.macSpoofing, f.dhcpGuard, f.routerGuard,
                    f.nicTeaming, f.portMirroring, f.ieeePriorityTag, r.ReturnValue);
                if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
                auto [ok, desc] = WaitForJob(*m_scope, r.Job);
                if (!ok)
                    HyprvAppLog(L"[vmm] SetNetworkAdapterAdvanced modify job failed: %s", desc.c_str());
                if (ok) KickPoll();
                return ok;
            }

            if (allocRef.empty())
            {
                HyprvAppLog(L"[vmm] SetNetworkAdapterAdvanced: NIC connection not found "
                            L"vm=%s nic=%s", vmGuid.c_str(), nicGuid.c_str());
                return false;
            }
            auto tmpl = FindDefaultNicSecurityTemplate(*m_scope);
            if (!tmpl)
            {
                HyprvAppLog(L"[vmm] SetNetworkAdapterAdvanced: security template not found");
                return false;
            }
            fill(tmpl);
            // AddFeatureSettings.AffectedConfiguration is a REF (allocation
            // __PATH string); invoke manually like the VLAN add.
            auto in = vsms.SpawnMethodIn(L"AddFeatureSettings");
            in.Set(L"AffectedConfiguration", allocRef);
            in.SetStringArray(L"FeatureSettings",
                              std::vector<std::wstring>{ tmpl.GetCimXml() });
            auto out = vsms.InvokeMethod(L"AddFeatureSettings", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
            HyprvAppLog(L"[vmm] SetNetworkAdapterAdvanced ADD nic=%s "
                        L"spoof=%d dhcp=%d router=%d team=%d mirror=%u ieee=%d ret=%u",
                nicGuid.c_str(), f.macSpoofing, f.dhcpGuard, f.routerGuard,
                f.nicTeaming, f.portMirroring, f.ieeePriorityTag, ret);
            if (ret != 0 && ret != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope,
                out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
            if (!ok)
                HyprvAppLog(L"[vmm] SetNetworkAdapterAdvanced add job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetNetworkAdapterAdvanced exception: %s", e.whatW.c_str());
            return false;
        }
    }

    static hyprv::wmi::WmiObject FindDefaultNicBandwidthTemplate(hyprv::wmi::WmiScope& scope)
    {
        auto rows = scope.Query(
            L"SELECT * FROM Msvm_EthernetSwitchPortBandwidthSettingData "
            L"WHERE InstanceID LIKE 'Microsoft:Definition%Default'");
        if (!rows.empty()) return std::move(rows.front());
        return {};
    }

    bool VMManager::SetNetworkAdapterBandwidth(std::wstring const& vmGuid,
                                               std::wstring const& nicGuid,
                                               uint64_t maxBps,
                                               uint64_t minBps)
    {
        if (!m_scope || vmGuid.empty() || nicGuid.empty()) return false;
        try
        {
            hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService vsms(
                m_scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms) return false;

            std::wstring existingRef, allocRef;
            {
                auto rows = m_scope->Query(
                    (L"SELECT * FROM Msvm_EthernetSwitchPortBandwidthSettingData "
                     L"WHERE InstanceID LIKE 'Microsoft:" + vmGuid + L"%"
                     + nicGuid + L"%'").c_str());
                if (!rows.empty()) existingRef = rows.front().Path();
            }
            {
                auto rows = m_scope->Query(
                    (L"SELECT * FROM Msvm_EthernetPortAllocationSettingData "
                     L"WHERE InstanceID LIKE 'Microsoft:" + vmGuid + L"%"
                     + nicGuid + L"%'").c_str());
                if (!rows.empty()) allocRef = rows.front().Path();
            }

            auto fill = [&](hyprv::wmi::WmiObject& s)
            {
                s.Set(L"Limit",       maxBps);
                s.Set(L"Reservation", minBps);
            };

            if (!existingRef.empty())
            {
                auto s = m_scope->GetByPath(existingRef.c_str());
                if (!s) return false;
                fill(s);
                auto r = vsms.ModifyFeatureSettings(
                    std::vector<std::wstring>{ s.GetCimXml() });
                HyprvAppLog(L"[vmm] SetNetworkAdapterBandwidth MODIFY nic=%s "
                            L"max=%llu min=%llu ret=%u", nicGuid.c_str(),
                    static_cast<unsigned long long>(maxBps),
                    static_cast<unsigned long long>(minBps), r.ReturnValue);
                if (r.ReturnValue != 0 && r.ReturnValue != 4096) return false;
                auto [ok, desc] = WaitForJob(*m_scope, r.Job);
                if (!ok)
                    HyprvAppLog(L"[vmm] SetNetworkAdapterBandwidth modify job failed: %s", desc.c_str());
                if (ok) KickPoll();
                return ok;
            }

            if (allocRef.empty())
            {
                HyprvAppLog(L"[vmm] SetNetworkAdapterBandwidth: NIC connection not found "
                            L"vm=%s nic=%s", vmGuid.c_str(), nicGuid.c_str());
                return false;
            }
            auto tmpl = FindDefaultNicBandwidthTemplate(*m_scope);
            if (!tmpl)
            {
                HyprvAppLog(L"[vmm] SetNetworkAdapterBandwidth: bandwidth template not found");
                return false;
            }
            fill(tmpl);
            auto in = vsms.SpawnMethodIn(L"AddFeatureSettings");
            in.Set(L"AffectedConfiguration", allocRef);
            in.SetStringArray(L"FeatureSettings",
                              std::vector<std::wstring>{ tmpl.GetCimXml() });
            auto out = vsms.InvokeMethod(L"AddFeatureSettings", in);
            uint32_t ret = out.GetUInt32(L"ReturnValue").value_or(~0u);
            HyprvAppLog(L"[vmm] SetNetworkAdapterBandwidth ADD nic=%s max=%llu min=%llu ret=%u",
                nicGuid.c_str(), static_cast<unsigned long long>(maxBps),
                static_cast<unsigned long long>(minBps), ret);
            if (ret != 0 && ret != 4096) return false;
            auto [ok, desc] = WaitForJob(*m_scope,
                out.GetObject(L"Job").value_or(hyprv::wmi::WmiObject{}));
            if (!ok)
                HyprvAppLog(L"[vmm] SetNetworkAdapterBandwidth add job failed: %s", desc.c_str());
            if (ok) KickPoll();
            return ok;
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] SetNetworkAdapterBandwidth exception: %s", e.whatW.c_str());
            return false;
        }
    }

    void VMManager::PollLoop()
    {
        // The main-thread WmiScope proxy can't be invoked from this thread
        // (its IWbemServices is STA-bound and cross-apartment ExecMethod
        // calls return RPC_E_WRONG_THREAD). So this thread builds its own
        // scope + management-service references and uses them for every
        // GetSummaryInformation call.
        HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        std::unique_ptr<hyprv::wmi::WmiScope>                          scope;
        // Second scope for the dynamic-memory perf counter — that class
        // lives in root\cimv2, not root\virtualization\v2. Best-effort —
        // if cimv2 fails to open we just lose Memory Demand fidelity, the
        // rest of the manager still works.
        std::unique_ptr<hyprv::wmi::WmiScope>                          perfScope;
        hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService        vsms;
        try
        {
            scope = std::make_unique<hyprv::wmi::WmiScope>(
                L"root\\virtualization\\v2");
            vsms = hyprv::wmi::hyperv::Msvm_VirtualSystemManagementService(
                scope->GetInstance(L"Msvm_VirtualSystemManagementService"));
            if (!vsms)
                HyprvAppLog(L"[vmm] poller: Msvm_VirtualSystemManagementService missing");
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] poller setup failed: %s", e.whatW.c_str());
            if (SUCCEEDED(comHr)) CoUninitialize();
            return;
        }
        try
        {
            perfScope = std::make_unique<hyprv::wmi::WmiScope>(L"root\\cimv2");
        }
        catch (hyprv::wmi::WmiException const& e)
        {
            HyprvAppLog(L"[vmm] perf scope setup failed (continuing): %s",
                e.whatW.c_str());
        }

        // Initial refresh — kicks the first OnChanged before the 1 s wait.
        {
            uint64_t servicing = m_pollReq.load();
            try { UpdateAll(vsms, perfScope.get()); NotifyChanged(); } catch (...) {}
            m_pollGen.fetch_add(1);
            m_pollServiced.store(servicing);
        }

        // Each iteration: sleep up to kPollIntervalMs OR until a
        // subscription kicks us OR until the dtor signals stop.
        while (!m_stop.load())
        {
            {
                std::unique_lock<std::mutex> lk(m_stopLock);
                m_stopCv.wait_for(lk, std::chrono::milliseconds(kPollIntervalMs),
                    [this] { return m_stop.load() || m_kickPoll.load(); });
                if (m_stop.load()) break;
                m_kickPoll.store(false);
            }

            // Snapshot the request counter at the START of the cycle (after
            // consuming the kick), so this cycle only "services" requests
            // registered before its WMI reads begin. See KickPollAndWait.
            uint64_t servicing = m_pollReq.load();
            try   { UpdateAll(vsms, perfScope.get()); NotifyChanged(); }
            catch (...) { /* never escape the poll thread */ }
            // Bump AFTER the refresh+notify so KickPollAndWait waiters
            // see fresh m_vms by the time the counters advance.
            m_pollGen.fetch_add(1);
            m_pollServiced.store(servicing);
        }

        // Release the WMI interfaces before CoUninitialize — they hold COM
        // pointers that must be freed inside the apartment that created them.
        vsms = {};
        perfScope.reset();
        scope.reset();
        if (SUCCEEDED(comHr)) CoUninitialize();
    }

    void VMManager::NotifyChanged()
    {
        std::vector<OnChangedFn> subs;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            subs.reserve(m_onChangedSubs.size());
            for (auto const& p : m_onChangedSubs) subs.push_back(p.second);
        }
        for (auto const& cb : subs)
        {
            if (!cb) continue;
            try { cb(); }
            catch (...) { /* never throw across WMI worker boundary */ }
        }
    }
}
