param(
    [ValidateSet('both', 'msvc', 'clang')]
    [string]$Only = 'both'
)

$ErrorActionPreference = 'Stop'

$Root    = $PSScriptRoot
$OutDir  = $Root
$ObjRoot = Join-Path $Root 'build\obj'

$Sources = @(
    'SGC.cpp',
    'include\gallt\codegen\codegen.cpp',
    'include\gallt\codegen\codegen_constant.cpp',
    'include\gallt\codegen\codegen_aggregate.cpp',
    'include\gallt\codegen\codegen_expression.cpp',
    'include\gallt\codegen\codegen_builtin.cpp',
    'include\gallt\codegen\codegen_lifetime.cpp',
    'include\gallt\codegen\codegen_variadic.cpp',
    'include\gallt\common\diagnostics.cpp',
    'include\gallt\common\token.cpp',
    'include\gallt\driver\command_line.cpp',
    'include\gallt\driver\compiler.cpp',
    'include\gallt\lexer\lexer.cpp',
    'include\gallt\pal\platform_paths.cpp',
    'include\gallt\pal\platform_windows.cpp',
    'include\gallt\pal\platform_linux.cpp',
    'include\gallt\parser\ast.cpp',
    'include\gallt\parser\parser.cpp',
    'include\gallt\parser\parser_expression.cpp',
    'include\gallt\parser\parser_declaration.cpp',
    'include\gallt\parser\parser_generic.cpp',
    'include\gallt\parser\parser_statement.cpp',
    'include\gallt\semantic\generic_expander.cpp',
    'include\gallt\semantic\generic_expander_pattern.cpp',
    'include\gallt\semantic\generic_expander_clone.cpp',
    'include\gallt\semantic\generic_expander_property.cpp',
    'include\gallt\semantic\generic_expander_pack.cpp',
    'include\gallt\semantic\condition_compiler.cpp',
    'include\gallt\semantic\constant_folding.cpp',
    'include\gallt\semantic\lifecycle.cpp',
    'include\gallt\semantic\lifecycle_rewrite.cpp',
    'include\gallt\semantic\namespace_lowering.cpp',
    'include\gallt\semantic\type_checker.cpp',
    'include\gallt\semantic\type_checker_expression.cpp',
    'include\gallt\semantic\type_checker_overload.cpp',
    'include\gallt\semantic\type_checker_declaration.cpp',
    'include\gallt\semantic\type_checker_control.cpp',
    'include\gallt\semantic\type_checker_layout.cpp',
    'include\gallt\semantic\type_checker_variadic.cpp'
)

function Find-VcVars {
    if ($env:SGC_VCVARS -and (Test-Path -LiteralPath $env:SGC_VCVARS)) {
        return (Resolve-Path -LiteralPath $env:SGC_VCVARS).Path
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $vsPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($vsPath) {
            $candidate = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $candidate) {
                return (Resolve-Path -LiteralPath $candidate).Path
            }
        }
    }

    foreach ($vs in '2026', '2022', '2019') {
        foreach ($drive in 'C', 'D', 'E', 'F') {
            $candidate = "${drive}:\VS${vs}\VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path -LiteralPath $candidate) {
                return (Resolve-Path -LiteralPath $candidate).Path
            }
        }
    }
    return $null
}

function Find-ClangCl {
    if ($env:SGC_CLANG_CL -and (Test-Path -LiteralPath $env:SGC_CLANG_CL)) {
        return (Resolve-Path -LiteralPath $env:SGC_CLANG_CL).Path
    }
    if ($env:SGC_LLVM_BIN) {
        $candidate = Join-Path $env:SGC_LLVM_BIN 'clang-cl.exe'
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    $onPath = Get-Command clang-cl.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    foreach ($v in 'LLVM_BIN', 'LLVM_HOME') {
        $val = [Environment]::GetEnvironmentVariable($v)
        if (-not $val) { continue }
        $dir = if ($v -eq 'LLVM_HOME') { Join-Path $val 'bin' } else { $val }
        $candidate = Join-Path $dir 'clang-cl.exe'
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw ("clang-cl.exe not found. Set SGC_CLANG_CL to its full path, " +
           "or set SGC_LLVM_BIN to the LLVM bin directory, or add LLVM to PATH.")
}

function Invoke-EmbedRuntime {
    $generator = Join-Path $Root 'embed_runtime.ps1'
    if (-not (Test-Path -LiteralPath $generator)) {
        throw "runtime embedding script not found: $generator"
    }
    Write-Host '==> embedding runtime (crt_core.c + crt_file.c + crt_string.c -> crt_embedded.hpp)'
    & $generator
    $header = Join-Path $Root 'include\gallt\runtime\crt_embedded.hpp'
    if (-not (Test-Path -LiteralPath $header)) {
        throw "runtime embedding failed: crt_embedded.hpp was not generated"
    }
}

function New-CleanDir([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Invoke-Build([string]$Name, [string]$Compiler, [string]$VcVars) {
    Write-Host "==> building $Name with $Compiler"
    $objDir = Join-Path $ObjRoot $Name
    New-CleanDir $objDir
    $exe = Join-Path $OutDir ("sgc_" + $Name + ".exe")
    if (Test-Path -LiteralPath $exe) { Remove-Item -LiteralPath $exe -Force }

    $srcList = New-Object System.Collections.Generic.List[string]
    foreach ($s in $Sources) {
        $srcList.Add((Join-Path $Root $s))
    }

    $common = @(
        '/nologo', '/std:c++20', '/utf-8', '/EHsc', '/O2', '/DNDEBUG', '/FS',
        ('/I' + (Join-Path $Root 'include'))
    )
    $link = @(
        ('/Fe:' + $exe),
        ('/Fo:' + $objDir + '\'),
        '/link', '/STACK:33554432', '/ENTRY:wmainCRTStartup', '/SUBSYSTEM:CONSOLE'
    )

    $lines = @()
    if ($VcVars) {
        $lines += 'call "' + $VcVars + '" >nul 2>&1'
    }
    $lines += 'cd /d "' + $Root + '"'
    $quoted = ($srcList | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $lines += (('"' + $Compiler + '" ' + ($common -join ' ') + ' ' + $quoted + ' ' + ($link -join ' ')))

    $batch = Join-Path $objDir 'build.cmd'
    Set-Content -LiteralPath $batch -Value $lines -Encoding ASCII
    & cmd.exe /c $batch
    if ($LASTEXITCODE -ne 0) {
        throw "$Name build failed with exit code $LASTEXITCODE"
    }
    Write-Host "    -> $exe"
}

Invoke-EmbedRuntime

$vcvars = Find-VcVars
if (-not $vcvars) {
    Write-Warning ("vcvars64.bat not found; relying on the current environment. " +
                   "Set SGC_VCVARS to the full path if the build fails.")
}

if ($Only -eq 'both' -or $Only -eq 'msvc') {
    Invoke-Build 'msvc' 'cl.exe' $vcvars
}
if ($Only -eq 'both' -or $Only -eq 'clang') {
    $clangCl = Find-ClangCl
    Invoke-Build 'clang' $clangCl $vcvars
}

Write-Host 'build finished.'
