# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# Lists the fonts of a fresh Windows 10 / 11 install for
# molt_font_inventory.py, using only what Windows ships (Windows PowerShell
# 5.1; a fresh Windows has no Python):
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File collect_windows.ps1
#
# Writes molt-fonts-raw.json (or -Out FILE) in the current folder: per font
# file its path (the profile folder as "~"), where it is (system, user), its
# names in the Fonts registry keys, size, SHA-256 and, per face, the raw
# name, OS/2, head and fvar tables. Copy it to a machine with Python 3 and run
#   python3 molt_font_inventory.py import molt-fonts-raw.json
# Nothing is installed or changed; -ExecutionPolicy Bypass applies to this
# one run only.

[CmdletBinding()]
param([string]$Out = "molt-fonts-raw.json")

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$MaxFile = 256MB
$MaxFiles = 20000
$MaxFaces = 256
$MaxTables = 512
$Want = @("name", "OS/2", "head", "fvar")
$OpenType = '\.(ttf|otf|ttc|otc)$'
$OtherFont = '\.(fon|fnt|pfb|pfm|pfa|woff2?|t1)$'

function ConvertTo-JsonText([string]$s) {
    # Fast path: base64 tables and most names need no escaping.
    if ($s -notmatch '["\\\x00-\x1f]') { return '"' + $s + '"' }
    $sb = New-Object System.Text.StringBuilder ($s.Length + 2)
    [void]$sb.Append('"')
    foreach ($c in $s.ToCharArray()) {
        $code = [int]$c
        if ($c -eq '"') { [void]$sb.Append('\"') }
        elseif ($c -eq '\') { [void]$sb.Append('\\') }
        elseif ($code -lt 0x20) { [void]$sb.Append(('\u{0:x4}' -f $code)) }
        else { [void]$sb.Append($c) }
    }
    [void]$sb.Append('"')
    return $sb.ToString()
}

function ConvertTo-JsonValue($v) {
    if ($null -eq $v) { return "null" }
    if ($v -is [bool]) { if ($v) { return "true" } else { return "false" } }
    if ($v -is [int] -or $v -is [long]) { return [string]$v }
    if ($v -is [System.Collections.IDictionary]) {
        $parts = foreach ($k in $v.Keys) {
            (ConvertTo-JsonText ([string]$k)) + ":" + (ConvertTo-JsonValue $v[$k])
        }
        return "{" + ($parts -join ",") + "}"
    }
    if ($v -is [System.Collections.IEnumerable] -and -not ($v -is [string])) {
        $parts = foreach ($x in $v) { ConvertTo-JsonValue $x }
        return "[" + ($parts -join ",") + "]"
    }
    return ConvertTo-JsonText ([string]$v)
}

function Read-U16([byte[]]$d, [long]$o) {
    if ($o + 2 -gt $d.Length) { throw "truncated" }
    return ([int]$d[$o] -shl 8) -bor [int]$d[$o + 1]
}

function Read-U32([byte[]]$d, [long]$o) {
    if ($o + 4 -gt $d.Length) { throw "truncated" }
    return ([long]$d[$o] -shl 24) -bor ([long]$d[$o + 1] -shl 16) -bor `
           ([long]$d[$o + 2] -shl 8) -bor [long]$d[$o + 3]
}

function Get-Tag([byte[]]$d, [long]$o) {
    if ($o + 4 -gt $d.Length) { throw "truncated" }
    return [System.Text.Encoding]::GetEncoding(28591).GetString($d, [int]$o, 4)
}

# The faces of a font file: @(@{tag = hex; tables = @{tag = base64}}).
function Get-Faces([byte[]]$d) {
    if ($d.Length -lt 12) { throw "truncated" }
    $offsets = @([long]0)
    if ((Get-Tag $d 0) -ceq "ttcf") {
        $n = Read-U32 $d 8
        if ($n -lt 1 -or $n -gt $MaxFaces) { throw "$n faces" }
        $offsets = @(for ($i = 0; $i -lt $n; $i++) { Read-U32 $d (12 + 4 * $i) })
    }
    $faces = New-Object System.Collections.ArrayList
    foreach ($offset in $offsets) {
        $tag = Get-Tag $d $offset
        if (-not ($tag -ceq "OTTO" -or $tag -ceq "true" -or
                  $tag -ceq ([string][char]0 + [char]1 + [char]0 + [char]0))) {
            throw "not an sfnt face"
        }
        $count = Read-U16 $d ($offset + 4)
        if ($count -gt $MaxTables) { throw "$count tables" }
        $tables = [ordered]@{}
        for ($i = 0; $i -lt $count; $i++) {
            $record = $offset + 12 + 16 * $i
            $name = Get-Tag $d $record
            if ($Want -cnotcontains $name) { continue }
            $start = Read-U32 $d ($record + 8)
            $length = Read-U32 $d ($record + 12)
            if ($start + $length -gt $d.Length) { throw "table $name outside the file" }
            $tables[$name] = [Convert]::ToBase64String($d, [int]$start, [int]$length)
        }
        $first = [int]$offset
        $hex = -join ($d[$first..($first + 3)] | ForEach-Object { $_.ToString("x2") })
        [void]$faces.Add([ordered]@{ tag = $hex; tables = $tables })
    }
    return ,$faces
}

$profileDir = $env:USERPROFILE
function Hide-Profile([string]$path) {
    if ($profileDir -and $path.StartsWith($profileDir + "\", [StringComparison]::OrdinalIgnoreCase)) {
        return "~" + $path.Substring($profileDir.Length)
    }
    return $path
}

# path -> location; path -> registry names.
$found = [ordered]@{}
$registry = @{}
$windir = $env:WINDIR
$roots = @(,@((Join-Path $windir "Fonts"), "system"))
if ($env:LOCALAPPDATA) {
    $roots += ,@((Join-Path $env:LOCALAPPDATA "Microsoft\Windows\Fonts"), "user")
}
foreach ($root in $roots) {
    if (-not (Test-Path -LiteralPath $root[0] -PathType Container)) { continue }
    Get-ChildItem -LiteralPath $root[0] -File -Recurse -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match $OpenType -or $_.Name -match $OtherFont } |
        Sort-Object FullName | ForEach-Object {
            if (-not $found.Contains($_.FullName)) { $found[$_.FullName] = $root[1] }
        }
}
foreach ($key in @(@("HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts", "system"),
                   @("HKCU:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts", "user"))) {
    if (-not (Test-Path -LiteralPath $key[0])) { continue }
    $item = Get-Item -LiteralPath $key[0]
    foreach ($valueName in $item.GetValueNames()) {
        $value = [string]$item.GetValue($valueName)
        if (-not $value) { continue }
        if ([System.IO.Path]::IsPathRooted($value)) { $path = $value }
        else { $path = Join-Path (Join-Path $windir "Fonts") $value }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $path = (Get-Item -LiteralPath $path -Force).FullName
        if (-not $found.Contains($path)) { $found[$path] = $key[1] }
        if (-not $registry.ContainsKey($path)) {
            $registry[$path] = New-Object System.Collections.ArrayList
        }
        [void]$registry[$path].Add($valueName)
    }
}
if ($found.Count -gt $MaxFiles) { throw "more than $MaxFiles font files" }

$sha = [System.Security.Cryptography.SHA256]::Create()
$fonts = New-Object System.Collections.ArrayList
foreach ($path in $found.Keys) {
    $entry = [ordered]@{ path = (Hide-Profile $path); location = $found[$path] }
    if ($registry.ContainsKey($path)) { $entry["registry_names"] = $registry[$path] }
    try {
        $size = (Get-Item -LiteralPath $path -Force).Length
        $entry["size"] = [long]$size
        if ($size -gt $MaxFile) { throw "larger than $MaxFile bytes" }
        $data = [System.IO.File]::ReadAllBytes($path)
    } catch {
        $entry["error"] = "unreadable: " + $_.Exception.Message
        [void]$fonts.Add($entry)
        continue
    }
    $entry["sha256"] = -join ($sha.ComputeHash($data) | ForEach-Object { $_.ToString("x2") })
    if ($path -notmatch $OpenType) {
        $entry["error"] = "not an OpenType font (not compared)"
    } else {
        try {
            $entry["faces"] = Get-Faces $data
        } catch {
            $entry["error"] = "unreadable font: " + $_.Exception.Message
        }
    }
    [void]$fonts.Add($entry)
}

$os = [ordered]@{ platform = "windows"; machine = $env:PROCESSOR_ARCHITECTURE }
$cv = Get-ItemProperty -LiteralPath "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
foreach ($name in @("ProductName", "DisplayVersion", "ReleaseId", "CurrentBuild",
                    "UBR", "EditionID", "InstallationType")) {
    if ($cv.PSObject.Properties[$name]) { $os[$name] = [string]$cv.$name }
}

$doc = [ordered]@{
    format = "molt-font-raw-1"
    collector = "collect_windows.ps1 1"
    collected = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    os = $os
    fonts = $fonts
}
$target = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
[System.IO.File]::WriteAllText($target, (ConvertTo-JsonValue $doc) + "`n",
                               (New-Object System.Text.UTF8Encoding($false)))
Write-Output ("wrote {0} ({1} font files)" -f $target, $fonts.Count)
