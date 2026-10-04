#!/bin/sh
set -eu

root=$(cd "$(dirname "$0")" && pwd)
cd "$root"

compiler=${CXX:-}

if [ -z "$compiler" ]; then
    for candidate in c++ g++ clang++; do
        if command -v "$candidate" >/dev/null 2>&1; then
            compiler=$candidate
            break
        fi
    done
fi

if [ -z "$compiler" ]; then
    echo "build.sh: no C++ compiler found; set CXX to a C++20 compiler" >&2
    exit 1
fi

if command -v pwsh >/dev/null 2>&1; then
    pwsh -ExecutionPolicy Bypass -File ./embed_runtime.ps1
elif command -v powershell >/dev/null 2>&1; then
    powershell -ExecutionPolicy Bypass -File ./embed_runtime.ps1
else
    echo "build.sh: pwsh not found; skipping the runtime embedding step" >&2
fi

sources=$(sed -n "s/^[[:space:]]*'\\([^']*\\.cpp\\)',*[[:space:]]*$/\\1/p" build.ps1 \
    | tr '\\\\' '/')

if [ -z "$sources" ]; then
    echo "build.sh: could not read the source list from build.ps1" >&2
    exit 1
fi

output=${SGC_LINUX_OUTPUT:-$root/sgc_linux}

echo "==> building linux with $compiler"

set -- $sources
"$compiler" -std=c++20 -O2 -I include "$@" -o "$output" -ldl

echo "    -> $output"
