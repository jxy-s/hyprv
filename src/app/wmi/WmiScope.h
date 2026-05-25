// WmiScope — RAII connection to a single CIM namespace via IWbemServices.
//
// One scope per namespace per process is the typical usage (the EasyWMI port
// has VMManager hold one scope for root\virtualization\v2 for the app's
// lifetime). Connection is synchronous and happens in the ctor; on failure
// the ctor throws WmiException.
//
// COM apartment: the underlying IWbemServices is set up with CloakingOn so
// it can be safely called from any COM-initialized thread in the process.
// CoInitializeEx(COINIT_MULTITHREADED) or APARTMENTTHREADED is fine; this
// class doesn't enforce a model.

#pragma once

#include "WmiObject.h"

#include <functional>
#include <memory>
#include <string>

namespace hyprv::wmi
{
    class WmiSubscription;

    class WmiScope
    {
    public:
        // Connect to e.g. L"root\\virtualization\\v2" or L"root\\cimv2". Throws
        // WmiException on connect failure. Optional remote machine: L"."
        // (default) for local, hostname for remote.
        explicit WmiScope(const wchar_t* nameSpace, const wchar_t* machine = L".");
        ~WmiScope();

        WmiScope(const WmiScope&) = delete;
        WmiScope& operator=(const WmiScope&) = delete;

        // Underlying IWbemServices for advanced callers (subscription/object-
        // path queries that need the raw COM pointer).
        IWbemServices* Raw() const noexcept { return m_svc.p; }

        // First instance of `className`. Returns an empty WmiObject if none.
        WmiObject GetInstance(const wchar_t* className) const;

        // All instances of `className`. Equivalent to "SELECT * FROM className".
        std::vector<WmiObject> GetInstances(const wchar_t* className) const;

        // Run an arbitrary WQL query. Returned objects share this scope.
        std::vector<WmiObject> Query(const wchar_t* wql) const;

        // Look up an instance by its full WMI __PATH (e.g. the value returned
        // by WmiObject::Path()). Useful for resolving out-params that are
        // returned as object references.
        WmiObject GetByPath(const wchar_t* path) const;

        // Fetch the class definition itself (not an instance) — needed by
        // SpawnInstance / SpawnMethodIn callers that want to construct a
        // brand-new instance to pass to ExecMethod.
        WmiObject GetClass(const wchar_t* className) const;

        // Commit in-memory property changes on `obj` back to the WMI store
        // via IWbemServices::PutInstance. Used by WmiObject::Commit() for
        // scalar-only edits where the heavyweight ModifySystemSettings /
        // ModifyResourceSettings XML-embedded-instance protocol is overkill.
        //
        // The caller's `obj` must wrap an instance fetched from this scope
        // (i.e. obj.Scope() == this). Throws WmiException on failure.
        void PutInstance(WmiObject const& obj);

        // Subscribe to a WMI event query. The callback fires on a WMI worker
        // thread; marshal to the UI thread yourself if needed. The returned
        // subscription must be kept alive — destroying it cancels.
        //
        // Common event class names: __InstanceCreationEvent, __InstanceModificationEvent,
        // __InstanceDeletionEvent. The "within" value is polling interval in
        // seconds (WMI samples target classes that aren't event-providers at
        // that interval).
        std::unique_ptr<WmiSubscription> Subscribe(
            const wchar_t* wql,
            std::function<void(WmiObject /*target*/, WmiObject /*previous*/)> onEvent);

    private:
        CComPtr<IWbemLocator>   m_loc;
        CComPtr<IWbemServices>  m_svc;
        std::wstring            m_namespace;
    };
}
