# gen-hyperv-h.ps1 — generate C++ wrappers for Msvm_* WMI classes.
#
# Mirrors EasyWMI\interfacegen.ps1 but emits a single C++ header instead of a
# C# file. Each CIM class becomes a `struct` inheriting from hyprv::wmi::WmiObject,
# with typed property accessors and method invokers that call into the runtime
# layer (WmiObject::GetXxx / Set / SpawnMethodIn / InvokeMethod).
#
# Usage:
#     pwsh .\gen-hyperv-h.ps1 .\hyperv.json
#
# Run this on a host that has the target WMI namespace (root\virtualization\v2
# implies Hyper-V is installed). Output is plain text + UTF-8; commit the
# generated header into the tree so other dev machines don't need to rerun.

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$JsonConfig
)

# CIM type → C++ type. Scalar types are wrapped in std::optional<T> on read,
# arrays in std::vector<T>. Embedded instances + references become WmiObject.
$CppScalarType = @{
    'Boolean'   = 'bool'
    'UInt8'     = 'uint8_t'
    'SInt8'     = 'int8_t'
    'UInt16'    = 'uint16_t'
    'SInt16'    = 'int16_t'
    'UInt32'    = 'uint32_t'
    'SInt32'    = 'int32_t'
    'UInt64'    = 'uint64_t'
    'SInt64'    = 'int64_t'
    'Real32'    = 'float'
    'Real64'    = 'double'
    'Char16'    = 'wchar_t'
    'DateTime'  = 'std::chrono::system_clock::time_point'
    'String'    = 'std::wstring'
    'Instance'  = 'WmiObject'
    'Reference' = 'WmiObject'
}

# Which WmiObject getter to call for each CIM scalar type.
$CppGetter = @{
    'Boolean'  = 'GetBool'
    'UInt8'    = 'GetUInt32'   # promote 8/16/32 → use GetUInt32 + cast
    'SInt8'    = 'GetInt32'
    'UInt16'   = 'GetUInt16'
    'SInt16'   = 'GetInt32'
    'UInt32'   = 'GetUInt32'
    'SInt32'   = 'GetInt32'
    'UInt64'   = 'GetUInt64'
    'SInt64'   = 'GetInt64'
    'Real32'   = 'GetDouble'
    'Real64'   = 'GetDouble'
    'Char16'   = 'GetUInt16'
    'DateTime' = 'GetDateTime'
    'String'   = 'GetString'
}

# C++ scalar type each $CppGetter actually returns inside its std::optional<T>.
# When this differs from $CppScalarType (the public accessor's element type),
# the emitted body wraps with ::hyprv::wmi::detail::NarrowOpt<T> so the
# narrowing static_cast is explicit instead of an implicit C4244.
$CppGetterReturns = @{
    'Boolean'  = 'bool'
    'UInt8'    = 'uint32_t'
    'SInt8'    = 'int32_t'
    'UInt16'   = 'uint16_t'
    'SInt16'   = 'int32_t'
    'UInt32'   = 'uint32_t'
    'SInt32'   = 'int32_t'
    'UInt64'   = 'uint64_t'
    'SInt64'   = 'int64_t'
    'Real32'   = 'double'
    'Real64'   = 'double'
    'Char16'   = 'uint16_t'
    'DateTime' = 'std::chrono::system_clock::time_point'
    'String'   = 'std::wstring'
}

function Is-ArrayCimType { param([string]$t); return $t.EndsWith('Array') }
function Strip-Array     { param([string]$t); return $t -replace 'Array$','' }

function GetQualifier { param($CimParam, $QualName); return $CimParam.Qualifiers[$QualName] }
function IsKey         { param($Prop); return $(GetQualifier $Prop 'KEY') -ne $null }
function IsOutParam    { param($CimParam); return $(GetQualifier $CimParam 'OUT') -ne $null }
function IsStaticMethod { param($Method); return $(GetQualifier $Method 'Static') -ne $null }

# Returns the C++ return-type string for a get-property.
function CppGetReturnType {
    param([string]$cimType)
    if (Is-ArrayCimType $cimType) {
        $scalar = Strip-Array $cimType
        if ($scalar -eq 'Instance' -or $scalar -eq 'Reference') {
            return 'std::vector<WmiObject>'
        }
        $cpp = $CppScalarType[$scalar]
        if (-not $cpp) { $cpp = 'WmiObject' }
        return ('std::vector<{0}>' -f $cpp)
    }
    $cpp = $CppScalarType[$cimType]
    if (-not $cpp) { return 'WmiObject' }
    if ($cpp -eq 'WmiObject') { return 'std::optional<WmiObject>' }
    return ('std::optional<{0}>' -f $cpp)
}

# Returns the bare C++ value type (no optional, no reference) — used for
# result-struct fields and other contexts that need a stored value.
function CppValueType {
    param([string]$cimType)
    if (Is-ArrayCimType $cimType) {
        $scalar = Strip-Array $cimType
        if ($scalar -eq 'Instance' -or $scalar -eq 'Reference') {
            return 'std::vector<WmiObject>'
        }
        $cpp = $CppScalarType[$scalar]
        if (-not $cpp) { $cpp = 'WmiObject' }
        return ('std::vector<{0}>' -f $cpp)
    }
    $cpp = $CppScalarType[$cimType]
    if (-not $cpp) { return 'WmiObject' }
    return $cpp
}

# Returns the C++ parameter type for a method input parameter.
function CppParamType {
    param([string]$cimType)
    if (Is-ArrayCimType $cimType) {
        $scalar = Strip-Array $cimType
        $cpp = $CppScalarType[$scalar]
        if (-not $cpp) { $cpp = 'WmiObject' }
        return ('std::vector<{0}> const&' -f $cpp)
    }
    $cpp = $CppScalarType[$cimType]
    if (-not $cpp) { return 'WmiObject const&' }
    if ($cpp -eq 'std::wstring' -or $cpp -eq 'WmiObject') {
        return ('{0} const&' -f $cpp)
    }
    return $cpp
}

# Emit the get-property accessor body. For narrow integer arrays we read via
# GetUInt32Array and narrow-cast element-by-element to preserve the public
# signature's element type. Scalars whose public type is narrower than what
# the underlying getter returns (UInt8, SInt8, SInt16, Char16, Real32) are
# wrapped in NarrowOpt so the static_cast is explicit (no C4244).
#
# $receiver lets the same helper emit code that calls on `this` (default empty
# prefix) or on a different object — pass `'out.'` for method out-param
# extraction inside InvokeMethod result struct construction.
function EmitGetterBody {
    param([string]$cimType, [string]$propName, [string]$receiver = '')
    if (Is-ArrayCimType $cimType) {
        $scalar = Strip-Array $cimType
        if ($scalar -eq 'String') { return ('{0}GetStringArray(L"{1}")' -f $receiver, $propName) }
        if ($scalar -eq 'UInt32' -or $scalar -eq 'SInt32') {
            return ('{0}GetUInt32Array(L"{1}")' -f $receiver, $propName)
        }
        if ($scalar -eq 'UInt16' -or $scalar -eq 'SInt16' -or $scalar -eq 'UInt8' -or $scalar -eq 'SInt8') {
            $cpp = $CppScalarType[$scalar]
            if (-not $cpp) { $cpp = 'uint32_t' }
            return ('::hyprv::wmi::detail::NarrowVec<{0}>({1}GetUInt32Array(L"{2}"))' -f $cpp, $receiver, $propName)
        }
        if ($scalar -eq 'Instance' -or $scalar -eq 'Reference') {
            return ('{0}GetObjectArray(L"{1}")' -f $receiver, $propName)
        }
        # fallback — extend GetXxxArray helpers as needed
        return ('{0}GetObjectArray(L"{1}") /* TODO: {2} */' -f $receiver, $propName, $cimType)
    }
    if ($cimType -eq 'Instance' -or $cimType -eq 'Reference') {
        return ('{0}GetObject(L"{1}")' -f $receiver, $propName)
    }
    $getter = $CppGetter[$cimType]
    if (-not $getter) { return ('{0}GetObject(L"{1}") /* TODO: {2} */' -f $receiver, $propName, $cimType) }

    $base = '{0}{1}(L"{2}")' -f $receiver, $getter, $propName
    $publicCpp = $CppScalarType[$cimType]
    $getterCpp = $CppGetterReturns[$cimType]
    if ($publicCpp -and $getterCpp -and $publicCpp -ne $getterCpp) {
        return ('::hyprv::wmi::detail::NarrowOpt<{0}>({1})' -f $publicCpp, $base)
    }
    return $base
}

# Emit the set-property call body (param value is named 'v').
function EmitSetterBody {
    param([string]$cimType, [string]$propName)
    if (Is-ArrayCimType $cimType) {
        $scalar = Strip-Array $cimType
        if ($scalar -eq 'UInt32' -or $scalar -eq 'SInt32') {
            return ('SetUInt32Array(L"{0}", v);' -f $propName)
        }
        if ($scalar -eq 'String') {
            return ('SetStringArray(L"{0}", v);' -f $propName)
        }
        if ($scalar -eq 'UInt16' -or $scalar -eq 'SInt16') {
            return ('SetUInt16Array(L"{0}", v);' -f $propName)
        }
        # Reference arrays still need codegen support — extend WmiObject
        # (e.g. SetReferenceArray) and add cases here as needed.
        return ('static_cast<void>(v); /* TODO: array setter for {0} */' -f $cimType)
    }
    return ('Set(L"{0}", v);' -f $propName)
}

function GenClass {
    param ([ref]$ClassQueue, $Config, $Class)

    $cn = $Class.CimClassName

    '// ' + ('-' * 76)
    '// ' + $cn
    '// ' + ('-' * 76)
    'struct {0} : public ::hyprv::wmi::WmiObject' -f $cn
    '{'
    # JSON-decoded cim_namespace has single backslashes; escape for C++ string literal.
    $cimNsEscaped = $Config.cim_namespace -replace '\\', '\\'
    '    static constexpr wchar_t const* kClassName = L"{0}";' -f $cn
    '    static constexpr wchar_t const* kNamespace = L"{0}";' -f $cimNsEscaped
    ''
    '    {0}() = default;' -f $cn
    '    explicit {0}(::hyprv::wmi::WmiObject const& o) : ::hyprv::wmi::WmiObject(o) {{}}' -f $cn
    '    explicit {0}(::hyprv::wmi::WmiObject&& o) : ::hyprv::wmi::WmiObject(std::move(o)) {{}}' -f $cn
    ''

    # Properties
    $enumerator = $Class.CimClassProperties.GetEnumerator()
    while ($enumerator.MoveNext()) {
        $prop = $enumerator.Current
        $propName = $prop.Name
        $cimType  = $prop.CimType.ToString()

        $retType = CppGetReturnType $cimType
        $getBody = EmitGetterBody $cimType $propName
        $paramTy = CppParamType   $cimType
        $setBody = EmitSetterBody $cimType $propName
        $keyMark = if (IsKey $prop) { ' /* KEY */' } else { '' }

        '    // {0}{1}' -f $cimType, $keyMark
        '    {0} {1}() const {{ return {2}; }}' -f $retType, $propName, $getBody
        '    void {0}({1} v) {{ {2} }}' -f $propName, $paramTy, $setBody
        ''
    }

    # Methods
    $menum = $Class.CimClassMethods.GetEnumerator()
    while ($menum.MoveNext()) {
        $method  = $menum.Current
        $mName   = $method.Name
        $retType = $method.ReturnType.ToString()
        if (-not $retType) { $retType = 'UInt32' }
        $retCpp  = $CppScalarType[$retType]
        if (-not $retCpp) { $retCpp = 'uint32_t' }

        # Collect in/out params
        $inParams = @()
        $outParams = @()
        $penum = $method.Parameters.GetEnumerator()
        while ($penum.MoveNext()) {
            $p = $penum.Current
            $entry = @{ Name = $p.Name; CimType = $p.CimType.ToString() }
            if (IsOutParam $p) { $outParams += $entry } else { $inParams += $entry }
        }

        # Build the C++ result struct name if there are outs (other than ReturnValue)
        $hasOuts = $outParams.Count -gt 0
        $resultStructName = '{0}Result' -f $mName

        if ($hasOuts) {
            '    struct {0}' -f $resultStructName
            '    {'
            '        {0} ReturnValue;' -f $retCpp
            foreach ($o in $outParams) {
                $cpp = CppValueType $o.CimType
                '        {0} {1};' -f $cpp, $o.Name
            }
            '    };'
        }

        # Method signature: take in-params, return ReturnValue or Result struct
        $sig = ''
        foreach ($i in $inParams) {
            $pt = CppParamType $i.CimType
            if ($sig.Length -gt 0) { $sig += ', ' }
            $sig += '{0} {1}' -f $pt, $i.Name
        }
        $declRet = if ($hasOuts) { $resultStructName } else { $retCpp }

        '    {0} {1}({2})' -f $declRet, $mName, $sig
        '    {'
        if ($inParams.Count -gt 0 -or $hasOuts) {
            '        auto in = SpawnMethodIn(L"{0}");' -f $mName
            foreach ($i in $inParams) {
                if (Is-ArrayCimType $i.CimType) {
                    $scalar = Strip-Array $i.CimType
                    if ($scalar -eq 'UInt32' -or $scalar -eq 'SInt32') {
                        '        in.SetUInt32Array(L"{0}", {0});' -f $i.Name
                    } elseif ($scalar -eq 'String') {
                        '        in.SetStringArray(L"{0}", {0});' -f $i.Name
                    } else {
                        # Remaining array in-params (narrow-int, Reference,
                        # Instance arrays) still need codegen support —
                        # extend WmiObject and this branch as needed.
                        '        // TODO: array in-param "{0}" ({1}) — set via in.SetVariant manually' -f $i.Name, $i.CimType
                        '        static_cast<void>({0});' -f $i.Name
                    }
                } else {
                    '        in.Set(L"{0}", {0});' -f $i.Name
                }
            }
            '        auto out = InvokeMethod(L"{0}", in);' -f $mName
        } else {
            '        auto out = InvokeMethod(L"{0}");' -f $mName
        }
        $retGet = 'out.GetUInt32(L"ReturnValue").value_or(0)'
        if ($retType -eq 'UInt64') { $retGet = 'out.GetUInt64(L"ReturnValue").value_or(0)' }
        if ($retType -eq 'SInt32') { $retGet = 'out.GetInt32(L"ReturnValue").value_or(0)' }
        if ($hasOuts) {
            '        {0} r{{}};' -f $resultStructName
            '        r.ReturnValue = {0};' -f $retGet
            foreach ($o in $outParams) {
                # EmitGetterBody with receiver='out.' handles both array and
                # scalar cases — including NarrowVec/NarrowOpt wrapping when
                # the public type is narrower than the underlying WmiObject
                # getter's return type.
                $body = EmitGetterBody $o.CimType $o.Name 'out.'
                if (Is-ArrayCimType $o.CimType) {
                    # Array getter returns std::vector directly (no optional).
                    '        r.{0} = {1};' -f $o.Name, $body
                }
                elseif ($o.CimType -eq 'Instance' -or $o.CimType -eq 'Reference') {
                    '        r.{0} = {1}.value_or(WmiObject{{}});' -f $o.Name, $body
                } else {
                    $scal = $CppScalarType[$o.CimType]
                    if (-not $scal) { $scal = 'WmiObject' }
                    if ($scal -eq 'WmiObject') {
                        '        r.{0} = {1}.value_or(WmiObject{{}});' -f $o.Name, $body
                    } else {
                        '        r.{0} = {1}.value_or({2}{{}});' -f $o.Name, $body, $scal
                    }
                }
            }
            '        return r;'
        } else {
            '        return {0};' -f $retGet
        }
        '    }'
        ''

        # Queue referenced classes
        foreach ($p in $inParams + $outParams) {
            if ($p.CimType -eq 'Reference' -and $p.ContainsKey('ReferenceClassName')) {
                # PowerShell CimMethodParameter doesn't expose ReferenceClassName the
                # way the C# version does — skip queuing here; the generated header
                # uses WmiObject directly for references.
            }
        }
    }

    '};'
}

function ProcessClasses {
    param ($classQueue, $Config)

    '// AUTO-GENERATED by src/app/wmi/gen/gen-hyperv-h.ps1. DO NOT HAND-EDIT.'
    '// Source: ' + $Config.cim_namespace
    '#pragma once'
    ''
    '#include "../WmiObject.h"'
    '#include "../WmiScope.h"'
    ''
    '#include <chrono>'
    '#include <optional>'
    '#include <string>'
    '#include <vector>'
    '#include <cstdint>'
    ''
    'namespace {0}' -f $Config.namespace
    '{'

    $done = @{}
    while ($classQueue.Count) {
        $entry = $classQueue.Dequeue()
        if ($done.Item($entry.CimClassName) -ne $null) { continue }
        $done[$entry.CimClassName] = $true
        GenClass ([ref]$classQueue) $Config $entry
        ''
    }

    '} // namespace'
}

function ProcessConfig {
    param ([string]$JsonPath)

    $config = Get-Content -Raw -Path $JsonPath | ConvertFrom-Json

    foreach ($entry in $config) {
        $classQueue = [System.Collections.Queue]::new()

        foreach ($nameGlob in $entry.classes) {
            $classes = Get-CimClass -Namespace $entry.cim_namespace -ClassName $nameGlob
            if ($classes.Count -eq 1) {
                $classQueue.Enqueue($classes)
            } else {
                $en = $classes.GetEnumerator()
                while ($en.MoveNext()) { $classQueue.Enqueue($en.Current) }
            }
        }

        $cs = ProcessClasses $classQueue $entry

        # Resolve output path relative to the script's directory.
        $here = $PSScriptRoot
        $outAbs = Join-Path $here $entry.path
        $outDir = Split-Path -Parent $outAbs
        if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }

        # Write UTF-8 (no BOM) so MSVC is happy. Out-File default for pwsh 7+ is utf8NoBOM.
        Out-File -Encoding utf8NoBOM -FilePath $outAbs -InputObject $cs
        Write-Host "Wrote $outAbs"
    }
}

ProcessConfig $JsonConfig
