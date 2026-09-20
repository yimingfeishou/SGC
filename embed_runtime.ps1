param(
    [string]$RuntimeC = '',
    [string]$OutHeader = ''
)

$ErrorActionPreference = 'Stop'

$Root = $PSScriptRoot

if ([string]::IsNullOrEmpty($RuntimeC)) {
    $RuntimeC = Join-Path $Root 'include\gallt\runtime\crt.c'
}
if ([string]::IsNullOrEmpty($OutHeader)) {
    $OutHeader = Join-Path $Root 'include\gallt\runtime\crt_embedded.hpp'
}

if (-not (Test-Path -LiteralPath $RuntimeC)) {
    throw "runtime source not found: $RuntimeC"
}

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$Lf = [string][char]10

$content = [System.IO.File]::ReadAllText($RuntimeC, $Utf8NoBom)

$content = $content.Replace([string][char]13 + $Lf, $Lf)

$delimiter = [char]41 + 'GALLT_CRT' + [char]34
if ($content.Contains($delimiter)) {
    throw "crt.c contains the raw-string delimiter $delimiter; use another delimiter"
}

$sha = [System.Security.Cryptography.SHA256]::Create()
$hash = (($sha.ComputeHash([System.IO.File]::ReadAllBytes($RuntimeC))) |
    ForEach-Object { $_.ToString('x2') }) -join ''

$relativeSource = $RuntimeC.Substring($Root.Length).TrimStart('\', '/')

$prologue = @'
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

$text = $prologue.Replace('<SOURCE>', $relativeSource).Replace('<SHA>', $hash) +
        $content + $epilogue

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
