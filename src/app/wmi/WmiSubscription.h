// WmiSubscription — keep-alive handle for a WMI async notification query.
//
// Lifetime: created by WmiScope::Subscribe; destroying it calls Stop()
// (idempotent) which CancelAsyncCall's the underlying notification, then
// releases the sink. Always wrap in std::unique_ptr or similar.
//
// Threading: the user-supplied callback fires on a WMI-owned worker thread.
// Don't touch UI from inside it — marshal to the UI dispatcher.

#pragma once

#include "WmiObject.h"

#include <functional>

namespace hyprv::wmi
{
    class WmiScope;
    class WmiEventSink;  // impl detail

    class WmiSubscription
    {
    public:
        using Callback = std::function<void(WmiObject /*target*/, WmiObject /*previous*/)>;

        // Constructed only by WmiScope::Subscribe.
        WmiSubscription(WmiScope& scope, const wchar_t* wql, Callback cb);
        ~WmiSubscription();

        WmiSubscription(const WmiSubscription&) = delete;
        WmiSubscription& operator=(const WmiSubscription&) = delete;

        // Idempotent. After Stop returns, no more callbacks will fire.
        void Stop();

    private:
        WmiScope&                     m_scope;
        std::wstring                  m_wql;
        CComPtr<IUnsecuredApartment>  m_apt;
        CComPtr<IUnknown>             m_stubUnk;
        CComPtr<IWbemObjectSink>      m_stubSink;
        CComPtr<WmiEventSink>         m_sink;  // our impl
        bool                          m_stopped = false;
    };
}
