#include "WmiSubscription.h"
#include "WmiScope.h"

namespace hyprv::wmi
{
    // ---- WmiEventSink: our IWbemObjectSink implementation -----------------------
    // Routes Indicate() callbacks to the user-supplied std::function. The sink
    // is wrapped by an IUnsecuredApartment stub so WMI worker threads can call
    // us without process-wide CoInitializeSecurity reconfiguration.
    class WmiEventSink : public IWbemObjectSink
    {
    public:
        WmiEventSink(WmiScope* scope, WmiSubscription::Callback cb)
            : m_scope(scope), m_cb(std::move(cb)) {}

        STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override
        {
            if (iid == IID_IUnknown || iid == IID_IWbemObjectSink)
            {
                *ppv = static_cast<IWbemObjectSink*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
        STDMETHODIMP_(ULONG) Release() override
        {
            LONG r = InterlockedDecrement(&m_ref);
            if (r == 0) delete this;
            return r;
        }

        STDMETHODIMP Indicate(LONG count, IWbemClassObject** items) override
        {
            if (!m_cb || !m_scope) return WBEM_S_NO_ERROR;
            for (LONG i = 0; i < count; ++i)
            {
                IWbemClassObject* eventObj = items[i];
                if (!eventObj) continue;

                // The event object is an instance of __InstanceCreationEvent /
                // ModificationEvent / DeletionEvent. The actual subject is in
                // the TargetInstance embedded property; PreviousInstance is
                // available for modification events.
                WmiObject ev(m_scope, CComPtr<IWbemClassObject>(eventObj));
                auto tgt = ev.GetObject(L"TargetInstance").value_or(WmiObject{});
                auto prv = ev.GetObject(L"PreviousInstance").value_or(WmiObject{});
                try { m_cb(std::move(tgt), std::move(prv)); }
                catch (...) { /* swallow — never throw across COM boundary */ }
            }
            return WBEM_S_NO_ERROR;
        }

        STDMETHODIMP SetStatus(LONG /*flags*/, HRESULT /*hr*/, BSTR /*param*/,
                               IWbemClassObject* /*objectParam*/) override
        {
            // Fires on subscription tear-down or an error. We don't need to
            // surface this to the callback for now — Subscribe() callers
            // assume "fire-and-forget until destruction".
            return WBEM_S_NO_ERROR;
        }

    private:
        LONG                          m_ref = 1;
        WmiScope*                     m_scope;
        WmiSubscription::Callback     m_cb;
    };

    // ---- WmiSubscription --------------------------------------------------------
    WmiSubscription::WmiSubscription(WmiScope& scope, const wchar_t* wql, Callback cb)
        : m_scope(scope), m_wql(wql ? wql : L"")
    {
        // Create the sink (refcount 1).
        m_sink.Attach(new WmiEventSink(&scope, std::move(cb)));

        // Wrap it through IUnsecuredApartment so WMI can call back without
        // process-wide impersonation security tweaks. This is the documented
        // safe pattern for IWbemServices::ExecNotificationQueryAsync.
        HRESULT hr = CoCreateInstance(
            CLSID_UnsecuredApartment, nullptr, CLSCTX_LOCAL_SERVER,
            IID_PPV_ARGS(&m_apt));
        if (FAILED(hr) || !m_apt)
        {
            throw WmiException(hr, L"CoCreateInstance(UnsecuredApartment) failed");
        }

        hr = m_apt->CreateObjectStub(m_sink, &m_stubUnk);
        if (FAILED(hr) || !m_stubUnk)
        {
            throw WmiException(hr, L"UnsecuredApartment::CreateObjectStub failed");
        }
        hr = m_stubUnk->QueryInterface(IID_PPV_ARGS(&m_stubSink));
        if (FAILED(hr) || !m_stubSink)
        {
            throw WmiException(hr, L"stub QI IWbemObjectSink failed");
        }

        CComBSTR lang(L"WQL");
        CComBSTR query(m_wql.c_str());
        hr = scope.Raw()->ExecNotificationQueryAsync(
            lang, query,
            WBEM_FLAG_SEND_STATUS, nullptr,
            m_stubSink);
        if (FAILED(hr))
        {
            throw WmiException(hr, std::wstring(L"ExecNotificationQueryAsync: ") + m_wql);
        }
    }

    WmiSubscription::~WmiSubscription()
    {
        Stop();
    }

    void WmiSubscription::Stop()
    {
        if (m_stopped) return;
        m_stopped = true;
        if (m_stubSink && m_scope.Raw())
            m_scope.Raw()->CancelAsyncCall(m_stubSink);
        m_stubSink.Release();
        m_stubUnk.Release();
        m_apt.Release();
        m_sink.Release();
    }
}
