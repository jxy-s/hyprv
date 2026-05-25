#include "WmiObject.h"
#include "WmiScope.h"

#include <cstring>
#include <stdexcept>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "ole32.lib")

namespace hyprv::wmi
{
    // ---- WmiException -----------------------------------------------------------
    WmiException::WmiException(HRESULT h, std::wstring msg)
        : hr(h), whatW(std::move(msg))
    {
        // Best-effort UTF-16 → UTF-8 (or CP_ACP) for what().
        if (!whatW.empty())
        {
            int n = WideCharToMultiByte(CP_UTF8, 0, whatW.data(), (int)whatW.size(),
                                        nullptr, 0, nullptr, nullptr);
            whatA.resize(static_cast<size_t>(n));
            WideCharToMultiByte(CP_UTF8, 0, whatW.data(), (int)whatW.size(),
                                whatA.data(), n, nullptr, nullptr);
        }
    }

    [[noreturn]] static void Throw(HRESULT hr, std::wstring msg)
    {
        // Append hex hr for diagnostics.
        wchar_t suffix[32];
        swprintf_s(suffix, L" (hr=0x%08lX)", hr);
        msg += suffix;
        throw WmiException(hr, std::move(msg));
    }

    // ---- CIM DateTime parsing (yyyymmddHHMMSS.mmmmmmsUUU) ----------------------
    static std::optional<std::chrono::system_clock::time_point> ParseCimDateTime(const wchar_t* s)
    {
        if (!s || wcslen(s) < 21) return std::nullopt;
        // Format: YYYYMMDDHHMMSS.mmmmmm±UUU
        SYSTEMTIME st{};
        wchar_t buf[5];
        auto take = [&](int off, int len, WORD& out) {
            wcsncpy_s(buf, &s[off], len);
            buf[len] = 0;
            out = static_cast<WORD>(_wtoi(buf));
        };
        take(0, 4, st.wYear);
        take(4, 2, st.wMonth);
        take(6, 2, st.wDay);
        take(8, 2, st.wHour);
        take(10, 2, st.wMinute);
        take(12, 2, st.wSecond);
        // 15..20 = microseconds (ignored, we lose precision past ms)
        WORD ms = 0;
        take(15, 3, ms);
        st.wMilliseconds = ms;
        FILETIME ft{};
        if (!SystemTimeToFileTime(&st, &ft)) return std::nullopt;
        // FILETIME is 100ns ticks since 1601; system_clock epoch is 1970.
        const uint64_t kFt1970 = 116444736000000000ULL;
        uint64_t ticks = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        if (ticks < kFt1970) return std::nullopt;
        uint64_t since1970_100ns = ticks - kFt1970;
        return std::chrono::system_clock::time_point{
            std::chrono::microseconds{ since1970_100ns / 10 }
        };
    }

    // ---- Basic property accessors -----------------------------------------------
    std::wstring WmiObject::ClassName() const
    {
        auto v = GetString(L"__CLASS");
        return v ? *v : std::wstring{};
    }

    std::wstring WmiObject::Path() const
    {
        auto v = GetString(L"__PATH");
        return v ? *v : std::wstring{};
    }

    std::wstring WmiObject::GetText(LONG fmt) const
    {
        if (!m_obj) return {};
        BSTR text = nullptr;
        HRESULT hr = m_obj->GetObjectText(fmt, &text);
        if (FAILED(hr) || !text) return {};
        std::wstring out(text, SysStringLen(text));
        SysFreeString(text);
        return out;
    }

    std::wstring WmiObject::GetCimXml() const
    {
        // Hyper-V's Modify{System,Resource}Settings expects WMI DTD 2.0
        // XML for its embedded-instance string params — verified by
        // capturing what PowerShell's Set-VMProcessor sends. MOF with
        // WBEM_FLAG_NONSYSTEM_ONLY (which we briefly tried) is rejected
        // with CIM 32773 InvalidParameter.
        //
        // The output contains __PATH/__CLASS/__NAMESPACE system properties
        // and full qualifier metadata — keep them. Stripping them was a
        // false lead from an earlier debugging session; PowerShell sends
        // them too and the modify succeeds. The earlier process crash
        // attributed to the XML route was actually std::regex catastrophic
        // backtracking on the 15 KB output, not the WMI call.
        //
        // WMI_OBJ_TEXT_WMI_DTD_2_0 = 2 isn't always exposed by older SDK
        // headers, hence the magic number. VMPlex's TextFormat.WmiDtd20
        // maps to the same value.
        if (!m_obj) return {};
        CComPtr<IWbemObjectTextSrc> textSrc;
        HRESULT hr = textSrc.CoCreateInstance(
            CLSID_WbemObjectTextSrc, nullptr, CLSCTX_INPROC_SERVER);
        if (FAILED(hr) || !textSrc)
            Throw(hr, L"CoCreateInstance(WbemObjectTextSrc)");

        constexpr ULONG kWmiDtd20 = 2;
        BSTR text = nullptr;
        hr = textSrc->GetText(
            0,                       // lFlags (reserved, must be 0)
            m_obj,                   // pObj
            kWmiDtd20,               // uObjTextFormat
            nullptr,                 // pCtx — default context is fine
            &text);
        if (FAILED(hr) || !text)
            Throw(hr, L"IWbemObjectTextSrc::GetText");
        std::wstring out(text, SysStringLen(text));
        SysFreeString(text);
        return out;
    }

    bool WmiObject::GetVariant(const wchar_t* name, VARIANT* outVal, CIMTYPE* outType) const
    {
        if (!m_obj) return false;
        VariantInit(outVal);
        CIMTYPE type = 0;
        HRESULT hr = m_obj->Get(name, 0, outVal, &type, nullptr);
        if (outType) *outType = type;
        if (FAILED(hr)) return false;
        if (outVal->vt == VT_NULL || outVal->vt == VT_EMPTY) return false;
        return true;
    }

    std::optional<std::wstring> WmiObject::GetString(const wchar_t* name) const
    {
        CComVariant v;
        if (!GetVariant(name, &v)) return std::nullopt;
        if (v.vt != VT_BSTR || !v.bstrVal) return std::nullopt;
        return std::wstring(v.bstrVal, SysStringLen(v.bstrVal));
    }

    std::optional<bool> WmiObject::GetBool(const wchar_t* name) const
    {
        CComVariant v;
        if (!GetVariant(name, &v)) return std::nullopt;
        if (v.vt != VT_BOOL) return std::nullopt;
        return v.boolVal != VARIANT_FALSE;
    }

    std::optional<int32_t> WmiObject::GetInt32(const wchar_t* name) const
    {
        CComVariant v;
        if (!GetVariant(name, &v)) return std::nullopt;
        if (FAILED(v.ChangeType(VT_I4))) return std::nullopt;
        return v.lVal;
    }

    std::optional<uint32_t> WmiObject::GetUInt32(const wchar_t* name) const
    {
        CComVariant v;
        if (!GetVariant(name, &v)) return std::nullopt;
        // CIM uint32 often comes back as VT_I4 — accept both.
        if (v.vt == VT_I4) return static_cast<uint32_t>(v.lVal);
        if (FAILED(v.ChangeType(VT_UI4))) return std::nullopt;
        return v.ulVal;
    }

    std::optional<int64_t> WmiObject::GetInt64(const wchar_t* name) const
    {
        CComVariant v;
        if (!GetVariant(name, &v)) return std::nullopt;
        // CIM sint64 typically comes back as VT_BSTR.
        if (v.vt == VT_BSTR && v.bstrVal) return _wtoi64(v.bstrVal);
        if (FAILED(v.ChangeType(VT_I8))) return std::nullopt;
        return v.llVal;
    }

    std::optional<uint64_t> WmiObject::GetUInt64(const wchar_t* name) const
    {
        CComVariant v;
        if (!GetVariant(name, &v)) return std::nullopt;
        if (v.vt == VT_BSTR && v.bstrVal) return _wcstoui64(v.bstrVal, nullptr, 10);
        if (FAILED(v.ChangeType(VT_UI8))) return std::nullopt;
        return v.ullVal;
    }

    std::optional<uint16_t> WmiObject::GetUInt16(const wchar_t* name) const
    {
        CComVariant v;
        if (!GetVariant(name, &v)) return std::nullopt;
        if (FAILED(v.ChangeType(VT_UI2))) return std::nullopt;
        return v.uiVal;
    }

    std::optional<double> WmiObject::GetDouble(const wchar_t* name) const
    {
        CComVariant v;
        if (!GetVariant(name, &v)) return std::nullopt;
        if (FAILED(v.ChangeType(VT_R8))) return std::nullopt;
        return v.dblVal;
    }

    std::optional<std::chrono::system_clock::time_point>
    WmiObject::GetDateTime(const wchar_t* name) const
    {
        auto s = GetString(name);
        if (!s) return std::nullopt;
        return ParseCimDateTime(s->c_str());
    }

    std::optional<WmiObject> WmiObject::GetObject(const wchar_t* name) const
    {
        CComVariant v;
        if (!GetVariant(name, &v)) return std::nullopt;
        if (v.vt != VT_UNKNOWN || !v.punkVal) return std::nullopt;
        CComPtr<IWbemClassObject> obj;
        if (FAILED(v.punkVal->QueryInterface(IID_PPV_ARGS(&obj))) || !obj) return std::nullopt;
        return WmiObject(m_scope, std::move(obj));
    }

    // ---- Array accessors --------------------------------------------------------
    static std::vector<std::wstring> ReadStringArray(const VARIANT& v)
    {
        std::vector<std::wstring> out;
        if (!(v.vt & VT_ARRAY) || !v.parray) return out;
        SAFEARRAY* sa = v.parray;
        LONG lb = 0, ub = -1;
        SafeArrayGetLBound(sa, 1, &lb);
        SafeArrayGetUBound(sa, 1, &ub);
        for (LONG i = lb; i <= ub; ++i)
        {
            BSTR s = nullptr;
            if (SUCCEEDED(SafeArrayGetElement(sa, &i, &s)) && s)
            {
                out.emplace_back(s, SysStringLen(s));
                SysFreeString(s);
            }
        }
        return out;
    }

    static std::vector<uint32_t> ReadUInt32Array(const VARIANT& v)
    {
        std::vector<uint32_t> out;
        if (!(v.vt & VT_ARRAY) || !v.parray) return out;
        SAFEARRAY* sa = v.parray;
        LONG lb = 0, ub = -1;
        SafeArrayGetLBound(sa, 1, &lb);
        SafeArrayGetUBound(sa, 1, &ub);
        for (LONG i = lb; i <= ub; ++i)
        {
            // SAFEARRAY of CIM uint32 is typically VT_I4 elements (WMI quirk).
            LONG val = 0;
            if (SUCCEEDED(SafeArrayGetElement(sa, &i, &val)))
                out.push_back(static_cast<uint32_t>(val));
        }
        return out;
    }

    std::vector<std::wstring> WmiObject::GetStringArray(const wchar_t* name) const
    {
        CComVariant v;
        if (!GetVariant(name, &v)) return {};
        return ReadStringArray(v);
    }

    std::vector<uint32_t> WmiObject::GetUInt32Array(const wchar_t* name) const
    {
        CComVariant v;
        if (!GetVariant(name, &v)) return {};
        return ReadUInt32Array(v);
    }

    std::vector<uint8_t> WmiObject::GetUInt8Array(const wchar_t* name) const
    {
        std::vector<uint8_t> out;
        CComVariant v;
        if (!GetVariant(name, &v)) return out;
        if (!(v.vt & VT_ARRAY) || !v.parray) return out;
        SAFEARRAY* sa = v.parray;
        VARTYPE elemType = VT_EMPTY;
        if (FAILED(SafeArrayGetVartype(sa, &elemType))) return out;
        LONG lb = 0, ub = -1;
        SafeArrayGetLBound(sa, 1, &lb);
        SafeArrayGetUBound(sa, 1, &ub);
        out.reserve(static_cast<size_t>(ub - lb + 1));
        for (LONG i = lb; i <= ub; ++i)
        {
            // OctetString/UInt8Array marshals as VT_UI1 elements; tolerate
            // VT_I2/VT_I4 just in case a provider widens them.
            if (elemType == VT_UI1)
            {
                BYTE b = 0;
                if (SUCCEEDED(SafeArrayGetElement(sa, &i, &b))) out.push_back(b);
            }
            else
            {
                LONG val = 0;
                if (SUCCEEDED(SafeArrayGetElement(sa, &i, &val)))
                    out.push_back(static_cast<uint8_t>(val));
            }
        }
        return out;
    }

    std::vector<WmiObject> WmiObject::GetObjectArray(const wchar_t* name) const
    {
        std::vector<WmiObject> out;
        CComVariant v;
        if (!GetVariant(name, &v)) return out;
        if (!(v.vt & VT_ARRAY) || !v.parray) return out;
        SAFEARRAY* sa = v.parray;
        // Hyper-V's Modify{System,Resource}Settings out-params are declared
        // Instance[] in the MOF but returned as VT_BSTR (embedded-instance
        // XML strings), not VT_UNKNOWN. Treating a BSTR as IUnknown* and
        // dereferencing it produced an AV (read at 0xffffffffffffffff).
        // Guard by checking the element type and silently returning empty
        // for non-object arrays — the alternative would be a separate
        // GetEmbeddedInstanceXmlArray helper, which no caller needs yet.
        VARTYPE elemType = VT_EMPTY;
        if (FAILED(SafeArrayGetVartype(sa, &elemType))) return out;
        if (elemType != VT_UNKNOWN && elemType != VT_DISPATCH && elemType != VT_VARIANT)
            return out;
        LONG lb = 0, ub = -1;
        SafeArrayGetLBound(sa, 1, &lb);
        SafeArrayGetUBound(sa, 1, &ub);
        for (LONG i = lb; i <= ub; ++i)
        {
            IUnknown* punk = nullptr;
            if (SUCCEEDED(SafeArrayGetElement(sa, &i, &punk)) && punk)
            {
                CComPtr<IWbemClassObject> wco;
                if (SUCCEEDED(punk->QueryInterface(IID_PPV_ARGS(&wco))) && wco)
                    out.emplace_back(m_scope, std::move(wco));
                punk->Release();
            }
        }
        return out;
    }

    // ---- Setters ----------------------------------------------------------------
    void WmiObject::SetVariant(const wchar_t* name, const VARIANT& v)
    {
        if (!m_obj) Throw(E_POINTER, std::wstring(L"Set on null WmiObject ") + name);
        HRESULT hr = m_obj->Put(name, 0, const_cast<VARIANT*>(&v), 0);
        if (FAILED(hr)) Throw(hr, std::wstring(L"Put failed for ") + name);
    }

    void WmiObject::Set(const wchar_t* name, const std::wstring& value)
    {
        CComVariant v(value.c_str());
        SetVariant(name, v);
    }
    void WmiObject::Set(const wchar_t* name, const wchar_t* value)
    {
        CComVariant v(value ? value : L"");
        SetVariant(name, v);
    }
    void WmiObject::Set(const wchar_t* name, bool value)
    {
        CComVariant v; v.vt = VT_BOOL; v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        SetVariant(name, v);
    }
    void WmiObject::Set(const wchar_t* name, int32_t value)
    {
        CComVariant v(value);  // VT_I4
        SetVariant(name, v);
    }
    void WmiObject::Set(const wchar_t* name, uint32_t value)
    {
        CComVariant v; v.vt = VT_I4; v.lVal = static_cast<LONG>(value); // WMI prefers I4 over UI4
        SetVariant(name, v);
    }
    void WmiObject::Set(const wchar_t* name, int64_t value)
    {
        // CIM sint64 wants VT_BSTR per WMI conventions.
        wchar_t buf[32]; swprintf_s(buf, L"%lld", static_cast<long long>(value));
        Set(name, buf);
    }
    void WmiObject::Set(const wchar_t* name, uint64_t value)
    {
        wchar_t buf[32]; swprintf_s(buf, L"%llu", static_cast<unsigned long long>(value));
        Set(name, buf);
    }
    void WmiObject::Set(const wchar_t* name, uint16_t value)
    {
        CComVariant v; v.vt = VT_I4; v.lVal = static_cast<LONG>(value);
        SetVariant(name, v);
    }
    void WmiObject::Set(const wchar_t* name, double value)
    {
        CComVariant v(value);  // VT_R8
        SetVariant(name, v);
    }
    void WmiObject::Set(const wchar_t* name, const WmiObject& nested)
    {
        if (!nested) { SetVariant(name, CComVariant()); return; }
        CComVariant v; v.vt = VT_UNKNOWN; v.punkVal = nested.Raw(); v.punkVal->AddRef();
        SetVariant(name, v);
    }

    void WmiObject::SetUInt32Array(const wchar_t* name, std::vector<uint32_t> const& values)
    {
        // CIM uint32 in SAFEARRAYs is conventionally VT_I4 element type (matches
        // ReadUInt32Array's read side). Build one, copy in, and Put.
        SAFEARRAYBOUND b{};
        b.lLbound   = 0;
        b.cElements = static_cast<ULONG>(values.size());
        SAFEARRAY* sa = SafeArrayCreate(VT_I4, 1, &b);
        if (!sa) Throw(E_OUTOFMEMORY, std::wstring(L"SafeArrayCreate failed for ") + name);
        for (LONG i = 0; i < static_cast<LONG>(values.size()); ++i)
        {
            LONG v = static_cast<LONG>(values[static_cast<size_t>(i)]);
            HRESULT hr = SafeArrayPutElement(sa, &i, &v);
            if (FAILED(hr))
            {
                SafeArrayDestroy(sa);
                Throw(hr, std::wstring(L"SafeArrayPutElement failed for ") + name);
            }
        }
        CComVariant v;
        v.vt     = VT_ARRAY | VT_I4;
        v.parray = sa;
        SetVariant(name, v);
        // CComVariant's destructor calls VariantClear -> SafeArrayDestroy. Good.
    }

    void WmiObject::SetUInt16Array(const wchar_t* name, std::vector<uint16_t> const& values)
    {
        // CIM uint16 arrays are carried as VT_I4 elements (same convention as
        // uint32 — see SetUInt32Array / the GetUInt32Array read path that the
        // BootOrder getter narrows from). WMI range-checks on Put, so the
        // wider element type is fine for the 0..65535 domain.
        SAFEARRAYBOUND b{};
        b.lLbound   = 0;
        b.cElements = static_cast<ULONG>(values.size());
        SAFEARRAY* sa = SafeArrayCreate(VT_I4, 1, &b);
        if (!sa) Throw(E_OUTOFMEMORY, std::wstring(L"SafeArrayCreate failed for ") + name);
        for (LONG i = 0; i < static_cast<LONG>(values.size()); ++i)
        {
            LONG v = static_cast<LONG>(values[static_cast<size_t>(i)]);
            HRESULT hr = SafeArrayPutElement(sa, &i, &v);
            if (FAILED(hr))
            {
                SafeArrayDestroy(sa);
                Throw(hr, std::wstring(L"SafeArrayPutElement failed for ") + name);
            }
        }
        CComVariant v;
        v.vt     = VT_ARRAY | VT_I4;
        v.parray = sa;
        SetVariant(name, v);
    }

    void WmiObject::SetUInt8Array(const wchar_t* name, std::vector<uint8_t> const& bytes)
    {
        // CIM UInt8Array / OctetString → SAFEARRAY of VT_UI1 (matches the read
        // side in GetUInt8Array). Used for SetKeyProtector's KeyProtector blob.
        SAFEARRAYBOUND b{};
        b.lLbound   = 0;
        b.cElements = static_cast<ULONG>(bytes.size());
        SAFEARRAY* sa = SafeArrayCreate(VT_UI1, 1, &b);
        if (!sa) Throw(E_OUTOFMEMORY, std::wstring(L"SafeArrayCreate failed for ") + name);
        for (LONG i = 0; i < static_cast<LONG>(bytes.size()); ++i)
        {
            BYTE v = bytes[static_cast<size_t>(i)];
            HRESULT hr = SafeArrayPutElement(sa, &i, &v);
            if (FAILED(hr))
            {
                SafeArrayDestroy(sa);
                Throw(hr, std::wstring(L"SafeArrayPutElement failed for ") + name);
            }
        }
        CComVariant v;
        v.vt     = VT_ARRAY | VT_UI1;
        v.parray = sa;
        SetVariant(name, v);
    }

    void WmiObject::SetStringArray(const wchar_t* name, std::vector<std::wstring> const& values)
    {
        // CIM StringArray → SAFEARRAY of VT_BSTR. Each element is a BSTR
        // owned by the SAFEARRAY (SafeArrayPutElement copies via SysAllocString
        // internally for VT_BSTR), so we don't need to free element BSTRs
        // ourselves.
        SAFEARRAYBOUND b{};
        b.lLbound   = 0;
        b.cElements = static_cast<ULONG>(values.size());
        SAFEARRAY* sa = SafeArrayCreate(VT_BSTR, 1, &b);
        if (!sa) Throw(E_OUTOFMEMORY, std::wstring(L"SafeArrayCreate failed for ") + name);
        for (LONG i = 0; i < static_cast<LONG>(values.size()); ++i)
        {
            BSTR bs = SysAllocStringLen(values[static_cast<size_t>(i)].c_str(),
                static_cast<UINT>(values[static_cast<size_t>(i)].size()));
            if (!bs)
            {
                SafeArrayDestroy(sa);
                Throw(E_OUTOFMEMORY,
                    std::wstring(L"SysAllocStringLen failed for ") + name);
            }
            HRESULT hr = SafeArrayPutElement(sa, &i, bs);
            // SafeArrayPutElement copies the BSTR — we free the local copy
            // either way (success or failure).
            SysFreeString(bs);
            if (FAILED(hr))
            {
                SafeArrayDestroy(sa);
                Throw(hr, std::wstring(L"SafeArrayPutElement failed for ") + name);
            }
        }
        CComVariant v;
        v.vt     = VT_ARRAY | VT_BSTR;
        v.parray = sa;
        SetVariant(name, v);
    }

    void WmiObject::SetReferenceArray(const wchar_t* name,
                                      std::vector<std::wstring> const& paths)
    {
        // A CIM REFERENCE array (e.g. RemoveResourceSettings'
        // ResourceSettings) is passed as a SAFEARRAY of BSTR object __PATHs —
        // the same wire marshaling as a StringArray. Delegate so there's one
        // implementation; the distinct entry point documents the CIM type and
        // is what generated ReferenceArray in-param setters should call.
        SetStringArray(name, paths);
    }

    void WmiObject::Commit()
    {
        // Thin wrapper around WmiScope::PutInstance so callers can do:
        //   obj.Set(L"ElementName", L"new"); obj.Commit();
        // instead of routing every scalar tweak through the XML-embedded-
        // instance protocol of ModifySystemSettings / ModifyResourceSettings.
        // The scope pointer is set at WmiObject ctor time and remains valid
        // for the lifetime of the WmiScope (which outlives any object
        // borrowed from it).
        if (!m_scope) Throw(E_FAIL, L"WmiObject::Commit on a scope-less object");
        if (!m_obj)   Throw(E_FAIL, L"WmiObject::Commit on a null object");
        m_scope->PutInstance(*this);
    }

    void WmiObject::Set(const wchar_t* name, std::chrono::system_clock::time_point tp)
    {
        // CIM DateTime: YYYYMMDDHHMMSS.mmmmmm+UUU (UUU = offset from UTC in min).
        // We always emit UTC ("+000").
        using namespace std::chrono;
        auto since = tp.time_since_epoch();
        auto us = duration_cast<microseconds>(since).count();
        const int64_t kFt1970_us = 11644473600000000LL;  // 1601→1970 in microseconds
        uint64_t ft_us = static_cast<uint64_t>(us + kFt1970_us);
        FILETIME ft;
        uint64_t ticks = ft_us * 10;  // 100ns units
        ft.dwLowDateTime  = static_cast<DWORD>(ticks & 0xFFFFFFFF);
        ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
        SYSTEMTIME st;
        FileTimeToSystemTime(&ft, &st);
        wchar_t buf[32];
        swprintf_s(buf, L"%04u%02u%02u%02u%02u%02u.%03u000+000",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        Set(name, buf);
    }

    // ---- Associations -----------------------------------------------------------
    std::vector<WmiObject> WmiObject::GetAssociated(
        const wchar_t* assocClass, const wchar_t* resultClass,
        const wchar_t* role, const wchar_t* resultRole) const
    {
        std::vector<WmiObject> out;
        if (!m_scope || !m_obj) return out;

        // Build "ASSOCIATORS OF {path} WHERE ..." query — IWbemServices doesn't
        // expose ManagementObject.GetRelated directly.
        const std::wstring path = Path();
        if (path.empty()) return out;

        std::wstring wql = L"ASSOCIATORS OF {" + path + L"}";
        bool first = true;
        auto addClause = [&](const wchar_t* key, const wchar_t* val) {
            if (!val || !*val) return;
            wql += first ? L" WHERE " : L" ";
            wql += key;
            wql += L"=";
            wql += val;
            first = false;
        };
        addClause(L"AssocClass", assocClass);
        addClause(L"ResultClass", resultClass);
        addClause(L"Role", role);
        addClause(L"ResultRole", resultRole);

        return m_scope->Query(wql.c_str());
    }

    // ---- Method invocation ------------------------------------------------------
    WmiObject WmiObject::SpawnMethodIn(const wchar_t* methodName) const
    {
        if (!m_obj || !m_scope) return {};
        // Need the CLASS object's method definitions, not this instance.
        CComPtr<IWbemClassObject> classObj;
        {
            auto cls = m_scope->GetClass(ClassName().c_str());
            classObj = cls.Raw();
        }
        if (!classObj) return {};

        CComPtr<IWbemClassObject> inSig, outSig;
        HRESULT hr = classObj->GetMethod(methodName, 0, &inSig, &outSig);
        if (FAILED(hr) || !inSig) return {};
        CComPtr<IWbemClassObject> inst;
        hr = inSig->SpawnInstance(0, &inst);
        if (FAILED(hr)) return {};
        return WmiObject(m_scope, std::move(inst));
    }

    WmiObject WmiObject::InvokeMethod(const wchar_t* methodName, const WmiObject& inParams) const
    {
        if (!m_obj || !m_scope)
            Throw(E_POINTER, std::wstring(L"InvokeMethod on null WmiObject ") + methodName);

        const auto path = Path();
        if (path.empty())
            Throw(E_INVALIDARG, std::wstring(L"InvokeMethod: no __PATH for ") + methodName);

        CComPtr<IWbemClassObject> outParams;
        CComBSTR bPath(path.c_str());
        CComBSTR bMethod(methodName);
        HRESULT hr = m_scope->Raw()->ExecMethod(
            bPath, bMethod, 0, nullptr,
            inParams ? inParams.Raw() : nullptr, &outParams, nullptr);
        if (FAILED(hr))
            Throw(hr, std::wstring(L"ExecMethod failed: ") + methodName);
        return WmiObject(m_scope, std::move(outParams));
    }
}
