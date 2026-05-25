// WmiObject — RAII wrapper around IWbemClassObject.
//
// Typed accessors (Get<T>/Set<T>) handle the VARIANT marshaling for the common
// CIM primitive types — string, bool, signed/unsigned integers, double, DateTime,
// nested IWbemClassObject, and SAFEARRAY-of-anything. Code-generated per-class
// wrappers (task #10) call these accessors by string property name; this base
// is also usable directly when no generated wrapper exists yet.
//
// Lifetime: holds a CComPtr<IWbemClassObject>. Cheap to copy (refcounted).
// Thread safety: matches IWbemClassObject — read-only access is fine from
// multiple threads, mutation isn't.

#pragma once

#include <windows.h>
#include <wbemidl.h>
#include <comdef.h>
#include <atlbase.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hyprv::wmi
{
    class WmiScope;  // fwd

    namespace detail
    {
        // Element-cast helper used by generated narrow-int array getters
        // (UInt16Array, UInt8Array, ...). WmiObject only exposes the widest
        // typed array accessors; generated code narrows to the declared element
        // type to preserve the public signature.
        template <typename Narrow, typename Wide>
        std::vector<Narrow> NarrowVec(std::vector<Wide> const& src)
        {
            std::vector<Narrow> out;
            out.reserve(src.size());
            for (auto const& v : src) out.push_back(static_cast<Narrow>(v));
            return out;
        }

        // Scalar equivalent of NarrowVec — used by generated UInt8/SInt8/SInt16/
        // Char16/Real32 accessors. WmiObject only exposes the widest typed
        // getters; emitting `return GetInt32(...)` into a return slot of
        // std::optional<int8_t> tripped C4244 (narrowing). Wrapping with a
        // static_cast on the unwrapped value silences the warning at the only
        // place the narrowing is actually intentional.
        template <typename Narrow, typename Wide>
        std::optional<Narrow> NarrowOpt(std::optional<Wide> const& src)
        {
            if (!src) return std::nullopt;
            return std::optional<Narrow>{ static_cast<Narrow>(*src) };
        }
    }

    // Thrown when a WMI call returns a failing HRESULT or a CIM ErrorObject.
    struct WmiException : public std::exception
    {
        HRESULT      hr;
        std::wstring whatW;
        std::string  whatA;  // ASCII copy so what() can return it
        WmiException(HRESULT h, std::wstring msg);
        const char* what() const noexcept override { return whatA.c_str(); }
    };

    // Wraps a single IWbemClassObject (an instance OR a class definition OR a
    // method's in/out param set — they all share IWbemClassObject). Holds a
    // back-pointer to the WmiScope so association traversal / method invoke
    // can reach IWbemServices.
    class WmiObject
    {
    public:
        WmiObject() = default;
        WmiObject(WmiScope* scope, CComPtr<IWbemClassObject> obj)
            : m_scope(scope), m_obj(std::move(obj)) {}

        // True if we wrap a real object.
        explicit operator bool() const noexcept { return m_obj != nullptr; }

        // Raw access for advanced callers / generated wrappers.
        IWbemClassObject* Raw() const noexcept { return m_obj.p; }
        WmiScope*         Scope() const noexcept { return m_scope; }

        // The WMI class name (e.g. "Msvm_ComputerSystem") of this instance.
        std::wstring ClassName() const;

        // The full WMI path (__PATH) — used for association queries and as the
        // key for ModifySystemSettings etc.
        std::wstring Path() const;

        // Get the MOF-style text of the object. fmt:
        //   WBEM_FLAG_NO_FLAVORS (0)   — minimal
        //   WBEM_FLAG_NONSYSTEM_ONLY (0x40) — skip __* system props
        // For Hyper-V's ModifySystemSettings / ModifyResourceSettings,
        // use GetCimXml() instead — those endpoints want WMI DTD 2.0 XML,
        // not MOF, and reject MOF with CIM 32773 ("invalid parameter").
        std::wstring GetText(LONG fmt = 0) const;

        // WMI DTD 2.0 XML representation — the embedded-instance format
        // Hyper-V's Modify{System,Resource}Settings expects. Uses
        // IWbemObjectTextSrc::GetText with format ID 2 (matches
        // VMPlex's `TextFormat.WmiDtd20`). MOF text via GetText is
        // rejected with CIM 32773.
        std::wstring GetCimXml() const;

        // ---- Typed property accessors -----------------------------------------
        // Overloads return std::optional so missing/null properties are distinguishable
        // from zero/empty. Throws WmiException only on truly unexpected errors (wrong
        // type, COM failure) — not on "property missing".
        std::optional<std::wstring> GetString(const wchar_t* name) const;
        std::optional<bool>         GetBool(const wchar_t* name) const;
        std::optional<int32_t>      GetInt32(const wchar_t* name) const;
        std::optional<uint32_t>     GetUInt32(const wchar_t* name) const;
        std::optional<int64_t>      GetInt64(const wchar_t* name) const;
        std::optional<uint64_t>     GetUInt64(const wchar_t* name) const;
        std::optional<uint16_t>     GetUInt16(const wchar_t* name) const;
        std::optional<double>       GetDouble(const wchar_t* name) const;

        // CIM DateTime — returns system_clock::time_point in UTC.
        std::optional<std::chrono::system_clock::time_point> GetDateTime(const wchar_t* name) const;

        // Nested objects (e.g. embedded instances). The returned WmiObject shares
        // the same scope.
        std::optional<WmiObject>    GetObject(const wchar_t* name) const;

        // Arrays — returns empty vector if absent. Throws on type mismatch.
        std::vector<std::wstring>   GetStringArray(const wchar_t* name) const;
        std::vector<uint32_t>       GetUInt32Array(const wchar_t* name) const;
        std::vector<WmiObject>      GetObjectArray(const wchar_t* name) const;
        // CIM UInt8Array / OctetString (e.g. MSFT_HgsKeyProtector.RawData,
        // Msvm_SecurityService::SetKeyProtector's KeyProtector). SAFEARRAY of
        // VT_UI1.
        std::vector<uint8_t>        GetUInt8Array(const wchar_t* name) const;

        // Generic raw VARIANT — for the (rare) cases not covered above. Caller
        // owns the VARIANT and must VariantClear it.
        bool GetVariant(const wchar_t* name, VARIANT* outVal,
                        CIMTYPE* outType = nullptr) const;

        // ---- Typed property mutators ------------------------------------------
        void Set(const wchar_t* name, const std::wstring& value);
        void Set(const wchar_t* name, const wchar_t* value);
        void Set(const wchar_t* name, bool value);
        void Set(const wchar_t* name, int32_t value);
        void Set(const wchar_t* name, uint32_t value);
        void Set(const wchar_t* name, int64_t value);
        void Set(const wchar_t* name, uint64_t value);
        void Set(const wchar_t* name, uint16_t value);
        void Set(const wchar_t* name, double value);
        void Set(const wchar_t* name, const WmiObject& nested);
        void Set(const wchar_t* name, std::chrono::system_clock::time_point value);
        void SetVariant(const wchar_t* name, const VARIANT& v);

        // SAFEARRAY-of-uint32 setter — mirror of GetUInt32Array. Generated method
        // in-params of CIM type UInt32Array call into this (and so does
        // hand-written code that needs to pass property-id arrays to
        // GetSummaryInformation etc).
        void SetUInt32Array(const wchar_t* name, std::vector<uint32_t> const& values);

        // SAFEARRAY setter for CIM UInt16Array properties (e.g. the Gen 1
        // Msvm_VirtualSystemSettingData.BootOrder). WMI represents CIM uint16
        // arrays as VT_I4 elements on the wire (the GetUInt32Array read side
        // confirms this), so this builds the same VT_ARRAY|VT_I4 SAFEARRAY as
        // SetUInt32Array — the distinct name just documents the CIM type.
        void SetUInt16Array(const wchar_t* name, std::vector<uint16_t> const& values);

        // SAFEARRAY-of-uint8 setter — mirror of GetUInt8Array. For CIM
        // UInt8Array / OctetString method in-params (e.g. SetKeyProtector's
        // KeyProtector blob). SAFEARRAY of VT_UI1.
        void SetUInt8Array(const wchar_t* name, std::vector<uint8_t> const& bytes);

        // SAFEARRAY-of-BSTR setter for CIM StringArray fields. Used by
        // generated property setters (Notes, HostResource, Connection,
        // etc.) and by generated method-in-param fillers for methods like
        // ModifyResourceSettings(StringArray ResourceSettings).
        void SetStringArray(const wchar_t* name, std::vector<std::wstring> const& values);

        // SAFEARRAY-of-BSTR setter for CIM REFERENCE-array method in-params
        // (e.g. RemoveResourceSettings' ResourceSettings, which is a
        // CIM_ResourceAllocationSettingData REF[]). On the wire a REF array
        // marshals identically to a string array — each element is an object
        // __PATH — so the implementation matches SetStringArray; the separate
        // name documents intent and gives the generator a target for
        // ReferenceArray in-params (currently emitted as no-op TODO stubs).
        void SetReferenceArray(const wchar_t* name, std::vector<std::wstring> const& paths);

        // Commit in-memory property changes back to the WMI store via
        // IWbemServices::PutInstance. The shorter path for scalar-only
        // edits — avoids the XML-embedded-instance dance required by
        // Modify{System,Resource}Settings. Equivalent to scope().PutInstance(*this).
        void Commit();

        // ---- Associations -----------------------------------------------------
        // Returns instances associated with this one via the named association
        // class (e.g. GetAssociated(L"Msvm_MostCurrentSnapshotInBranch")). For
        // tighter queries pass the result class + role qualifiers.
        std::vector<WmiObject> GetAssociated(
            const wchar_t* assocClass = nullptr,
            const wchar_t* resultClass = nullptr,
            const wchar_t* role = nullptr,
            const wchar_t* resultRole = nullptr) const;

        // ---- Method invocation -----------------------------------------------
        // Fetches the input-parameter template for `methodName`. Populate it
        // via Set() then pass to InvokeMethod.
        WmiObject SpawnMethodIn(const wchar_t* methodName) const;

        // Invoke. Returns the out-params object (which always contains
        // "ReturnValue" — a uint32 CIM status, 0 = success, 4096 = async job).
        // If inParams is empty WmiObject{}, no in-params are sent.
        WmiObject InvokeMethod(const wchar_t* methodName,
                               const WmiObject& inParams = {}) const;

    private:
        WmiScope*                  m_scope = nullptr;
        CComPtr<IWbemClassObject>  m_obj;
    };
}
