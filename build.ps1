param(
    [ValidateSet('both', 'msvc', 'clang')]
    [string]$Only = 'both'
)

$ErrorActionPreference = 'Stop'

$Root    = Split-Path -Parent $PSScriptRoot
$SrcDir  = Join-Path $Root 'SGC'
$OutDir  = $SrcDir
$ObjRoot = Join-Path $Root 'build\obj'

$Sources = @(
    './SGC.cpp',
    './include/gallt/codegen/codegen.cpp',
    './include/gallt/codegen/codegen_lifetime.cpp',
    './include/gallt/common/diagnostics.cpp',
    './include/gallt/common/token.cpp',
    './include/gallt/driver/command_line.cpp',
    './include/gallt/driver/compiler.cpp',
    './include/gallt/lexer/lexer.cpp',
    './include/gallt/parser/ast.cpp',
    './include/gallt/parser/parser.cpp',
    './include/gallt/semantic/generic_expander.cpp',
    './include/gallt/semantic/constant_folding.cpp',
    './include/gallt/semantic/lifecycle.cpp',
    './include/gallt/semantic/namespace_lowering.cpp',
    './include/gallt/semantic/type_checker.cpp'
)

function New-CleanDir([string]$Path) {
    if (Test-Path $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Find-Tool([string]$Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $cmd) {
        throw "required tool '$Name' not found in PATH"
    }
    return $cmd.Source
}

function Invoke-Build([string]$Name, [string]$Compiler) {
    Write-Host "==> building $Name with $Compiler"
    $objDir = Join-Path $ObjRoot $Name
    New-CleanDir $objDir
    $exe = Join-Path $OutDir ("sgc_" + $Name + ".exe")
    if (Test-Path $exe) { Remove-Item -LiteralPath $exe -Force }

    $exeFwd    = $exe.Replace('\', '/')
    $objDirFwd = ($objDir.Replace('\', '/')) + '/'

    $common = @(
        '/nologo', '/std:c++20', '/utf-8', '/EHsc', '/O2', '/DNDEBUG', '/Z7', '/FS',
        '/I./include'
    )
    $link = @(
        ('/Fe:' + $exeFwd),
        ('/Fo:' + $objDirFwd),
        '/link', '/STACK:33554432', '/ENTRY:wmainCRTStartup', '/SUBSYSTEM:CONSOLE'
    )

    $lines = @()
    $lines += 'cd /d "' + $SrcDir + '"'
    $quoted = ($Sources | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $lines += (('"' + $Compiler + '" ' + ($common -join ' ') + ' ' + $quoted + ' ' + ($link -join ' ')))

    $batch = Join-Path $objDir 'build.cmd'
    Set-Content -LiteralPath $batch -Value $lines -Encoding ASCII
    & cmd.exe /c $batch
    if ($LASTEXITCODE -ne 0) {
        throw "$Name build failed with exit code $LASTEXITCODE"
    }
    Write-Host "    -> $exe"
    $stdlibSrc = Join-Path $SrcDir 'sl'
    $stdlibDst = Join-Path $Root 'sl'
    if (Test-Path $stdlibSrc) {
        if (-not (Test-Path $stdlibDst)) {
            New-Item -ItemType Directory -Path $stdlibDst -Force | Out-Null
        }
        Copy-Item -Path (Join-Path $stdlibSrc '*') -Destination $stdlibDst -Recurse -Force
        Write-Host "    -> stdlib mirrored to $stdlibDst"
    }
}

if ($Only -eq 'both' -or $Only -eq 'msvc') {
    Invoke-Build 'msvc' (Find-Tool 'cl.exe')
}
if ($Only -eq 'both' -or $Only -eq 'clang') {
    Invoke-Build 'clang' (Find-Tool 'clang-cl.exe')
}

Write-Host 'build finished.'
