param(
    [string[]]$RuntimeC = @(),
    [string]$OutHeader = ''
)

$ErrorActionPreference = 'Stop'

$Root = $PSScriptRoot

if ([string]::IsNullOrEmpty($RuntimeC)) {
    $RuntimeC = @(
        (Join-Path $Root 'include\gallt\runtime\crt_core.c'),
        (Join-Path $Root 'include\gallt\runtime\crt_file.c'),
        (Join-Path $Root 'include\gallt\runtime\crt_string.c')
    )
}
if ([string]::IsNullOrEmpty($OutHeader)) {
    $OutHeader = Join-Path $Root 'include\gallt\runtime\crt_embedded.hpp'
}

foreach ($Part in $RuntimeC) {
    if (-not (Test-Path -LiteralPath $Part)) {
        throw "runtime source not found: $Part"
    }
}

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$Lf = [string][char]10

$content = ''
$rawBytes = New-Object System.Collections.Generic.List[byte]
foreach ($Part in $RuntimeC) {
    $content += [System.IO.File]::ReadAllText($Part, $Utf8NoBom)
    $rawBytes.AddRange([System.IO.File]::ReadAllBytes($Part))
}

$content = $content.Replace([string][char]13 + $Lf, $Lf)

$delimiter = [char]41 + 'GALLT_CRT' + [char]34
if ($content.Contains($delimiter)) {
    throw "crt.c contains the raw-string delimiter $delimiter; use another delimiter"
}

$sha = [System.Security.Cryptography.SHA256]::Create()
$hash = (($sha.ComputeHash($rawBytes.ToArray())) |
    ForEach-Object { $_.ToString('x2') }) -join ''

$relativeSource = ($RuntimeC | ForEach-Object {
        $_.Substring($Root.Length).TrimStart('\', '/') }) -join ', '

$prologue = @'
// WARNING: embed_runtime.ps1 AUTO-GENERATED FILE - DO NOT EDIT BY HAND

#ifndef GALLT_RUNTIME_CRT_EMBEDDED_HPP
#define GALLT_RUNTIME_CRT_EMBEDDED_HPP

namespace gallt {

    inline constexpr const char* kGalltRuntimeCSource = R"GALLT_CRT(
'@

$epilogue = @'
)GALLT_CRT";

}

#endif
'@

$prologue = $prologue.TrimEnd([char]13, [char]10) + $Lf
$epilogue = $epilogue.TrimEnd([char]13, [char]10) + $Lf

$text = $prologue + $content + $epilogue

$existing = ''
if (Test-Path -LiteralPath $OutHeader) {
    $existing = [System.IO.File]::ReadAllText($OutHeader, $Utf8NoBom)
}

if ($existing -eq $text) {
    Write-Host "    runtime already embedded: $relativeSource (sha256 $hash)"
}
else {
    $parent = Split-Path -Parent $OutHeader
    if (-not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    [System.IO.File]::WriteAllText($OutHeader, $text, $Utf8NoBom)
    Write-Host "    embedded $relativeSource -> crt_embedded.hpp (sha256 $hash)"
}
