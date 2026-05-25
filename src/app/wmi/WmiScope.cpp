#include "WmiScope.h"
#include "WmiSubscription.h"

#include <mutex>

namespace hyprv::wmi
{
    [[noreturn]] static void Throw(HRESULT hr, std::wstring msg)
    {
        wchar_t suffix[32];
        swprintf_s(suffix, L" (hr=0x%08lX)", hr);
        msg += suffix;
        throw WmiException(hr, std::move(msg));
    }

    WmiScope::WmiScope(const wchar_t* nameSpace, const wchar_t* machine)
        : m_namespace(nameSpace ? nameSpace : L"")
    {
        // Each thread that calls into WMI needs COM init. We don't init here —
        // assume the host (WinUI 3) already did APARTMENTTHREADED, or that the
        // caller (e.g. a background worker thread) inits before constructing.

        // Process-wide CoInitializeSecurity must be called exactly once. We try
        // it here; if the host (or another scope) already did, RPC_E_TOO_LATE
        // is fine.
        static std::once_flag s_secOnce;
        std::call_once(s_secOnce, [] {
            HRESULT hr = CoInitializeSecurity(
                nullptr, -1, nullptr, nullptr,
                RPC_C_AUTHN_LEVEL_DEFAULT,
                RPC_C_IMP_LEVEL_IMPERSONATE,
                nullptr, EOAC_NONE, nullptr);
            (void)hr;  // RPC_E_TOO_LATE is OK; the host may have set its own.
        });

        HRESULT hr = m_loc.CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER);
        if (FAILED(hr)) Throw(hr, L"CoCreateInstance(WbemLocator)");

        // Build "\\<machine>\<namespace>" path.
        std::wstring path = L"\\\\";
        path += (machine && *machine) ? machine : L".";
        path += L"\\";
        path += m_namespace;

        CComBSTR resource(path.c_str());
        hr = m_loc->ConnectServer(
            resource,
            nullptr, nullptr,    // current user
            nullptr, 0,
            nullptr, nullptr,
            &m_svc);
        if (FAILED(hr) || !m_svc) Throw(hr, std::wstring(L"ConnectServer ") + path);

        // Apply the proxy blanket so subsequent calls impersonate properly —
        // required for Hyper-V virtualization namespace.
        hr = CoSetProxyBlanket(
            m_svc,
            RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
            nullptr,
            RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr, EOAC_NONE);
        if (FAILED(hr)) Throw(hr, L"CoSetProxyBlanket(WbemServices)");
    }

    WmiScope::~WmiScope() = default;

    static std::vector<WmiObject> EnumerateAll(IEnumWbemClassObject* en, WmiScope* scope)
    {
        std::vector<WmiObject> out;
        if (!en) return out;
        while (true)
        {
            CComPtr<IWbemClassObject> obj;
            ULONG ret = 0;
            HRESULT hr = en->Next(WBEM_INFINITE, 1, &obj, &ret);
            if (FAILED(hr) || ret == 0 || !obj) break;
            out.emplace_back(scope, std::move(obj));
        }
        return out;
    }

    std::vector<WmiObject> WmiScope::Query(const wchar_t* wql) const
    {
        CComPtr<IEnumWbemClassObject> en;
        CComBSTR lang(L"WQL");
        CComBSTR query(wql);
        HRESULT hr = m_svc->ExecQuery(
            lang, query,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &en);
        if (FAILED(hr) || !en) Throw(hr, std::wstring(L"ExecQuery: ") + wql);
        return EnumerateAll(en, const_cast<WmiScope*>(this));
    }

    std::vector<WmiObject> WmiScope::GetInstances(const wchar_t* className) const
    {
        CComPtr<IEnumWbemClassObject> en;
        CComBSTR cls(className);
        HRESULT hr = m_svc->CreateInstanceEnum(
            cls,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &en);
        if (FAILED(hr) || !en) Throw(hr, std::wstring(L"CreateInstanceEnum: ") + className);
        return EnumerateAll(en, const_cast<WmiScope*>(this));
    }

    WmiObject WmiScope::GetInstance(const wchar_t* className) const
    {
        auto all = GetInstances(className);
        return all.empty() ? WmiObject{} : std::move(all.front());
    }

    WmiObject WmiScope::GetByPath(const wchar_t* path) const
    {
        CComPtr<IWbemClassObject> obj;
        CComBSTR bp(path);
        HRESULT hr = m_svc->GetObject(bp, 0, nullptr, &obj, nullptr);
        if (FAILED(hr) || !obj) return {};
        return WmiObject(const_cast<WmiScope*>(this), std::move(obj));
    }

    WmiObject WmiScope::GetClass(const wchar_t* className) const
    {
        CComPtr<IWbemClassObject> obj;
        CComBSTR bp(className);
        HRESULT hr = m_svc->GetObject(bp, 0, nullptr, &obj, nullptr);
        if (FAILED(hr) || !obj) Throw(hr, std::wstring(L"GetObject (class): ") + className);
        return WmiObject(const_cast<WmiScope*>(this), std::move(obj));
    }

    void WmiScope::PutInstance(WmiObject const& obj)
    {
        // IWbemServices::PutInstance commits an instance's current property
        // values to the WMI store. The class must already exist; the object
        // must be a real instance (not a class definition). WBEM_FLAG_UPDATE_ONLY
        // tells the provider "this is an update, not a create" — the
        // alternative (WBEM_FLAG_CREATE_ONLY) is for new-instance creation
        // which we don't use here.
        if (!obj) Throw(E_INVALIDARG, L"PutInstance: null object");
        HRESULT hr = m_svc->PutInstance(obj.Raw(), WBEM_FLAG_UPDATE_ONLY,
                                        nullptr, nullptr);
        if (FAILED(hr))
            Throw(hr, std::wstring(L"PutInstance failed for ") + obj.Path());
    }

    std::unique_ptr<WmiSubscription> WmiScope::Subscribe(
        const wchar_t* wql,
        std::function<void(WmiObject, WmiObject)> onEvent)
    {
        return std::make_unique<WmiSubscription>(*this, wql, std::move(onEvent));
    }
}
