#!/usr/bin/env python3
# -*- coding: utf-8 -*-
from __future__ import annotations
import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path


TESTS = Path(__file__).resolve().parent
GALLT = TESTS.parent
RESULTS = GALLT / "results"
LLVM_BIN = Path(os.environ.get("SGC_LLVM_BIN"))
STDLIB = Path(os.environ.get("SGC_STDLIB"))

COMPILERS = {
    "clang": Path(os.environ.get(
        "SGC_CLANG")),
    "msvc": Path(os.environ.get(
        "SGC_MSVC")),
}

ERROR_RE = re.compile(r"\bER\s*(\d{4})\b", re.IGNORECASE)
CLIB_RE = re.compile(r'(?m)^\s*clib\s*\(\s*"([^"]+)"\s*\)')
FILENAME_RE = re.compile(r"^[a-z]+__[a-z0-9_]+__(?:pass|error)__\d{3}\.glt$")


@dataclass
class Expectation:
    kind: str
    value: str = ""
    error_code: str = ""
    output: str = ""


@dataclass
class Result:
    compiler: str
    group: str
    test: str
    outcome: str
    duration_ms: int
    compile_rc: int | None = None
    run_rc: int | None = None
    compiled: bool = False
    detail: str = ""
    stdout: str = ""
    stderr: str = ""
    exe: str = ""


def ensure_allowed(path: Path) -> None:
    def normalized(candidate: Path) -> str:
        value = os.path.normcase(os.path.abspath(str(candidate.resolve())))
        if value.startswith("\\\\?\\"):
            value = value[4:]
        return value

    resolved = normalized(path)
    gallt = normalized(GALLT)
    temp = normalized(Path(tempfile.gettempdir()))
    allowed = (
        resolved == gallt
        or resolved.startswith(gallt + os.sep)
        or resolved == temp
        or resolved.startswith(temp + os.sep)
    )
    if not allowed:
        raise RuntimeError(f"write outside allowed roots: {resolved}")


def environment() -> dict[str, str]:
    env = os.environ.copy()
    env["PATH"] = str(LLVM_BIN) + os.pathsep + env.get("PATH", "")
    env["SGC_STDLIB"] = str(STDLIB)
    env["SGC_SL"] = str(STDLIB)
    return env


def normalize_text(data: bytes) -> str:
    return data.decode("utf-8", errors="replace").replace("\r\n", "\n").replace("\r", "\n")


def normalize_expected_text(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def normalize_actual_text(text: str) -> str:
    text = normalize_expected_text(text)
    return text[:-1] if text.endswith("\n") else text


def parse_expectation(path: Path) -> Expectation:
    data = path.read_bytes()
    lines = data.decode("utf-8", errors="replace").split("\n")
    if len(lines) < 3:
        raise ValueError("missing filename/expect header")
    expected_name = f"// {path.name}"
    if lines[0] != expected_name:
        raise ValueError(f"first line must be {expected_name!r}, got {lines[0]!r}")
    if not lines[1].startswith("// expect "):
        raise ValueError(f"second line must start with '// expect ', got {lines[1]!r}")

    directive = lines[1][len("// ") :]
    if directive == "expect output":
        output_lines: list[str] = []
        index = 2
        while index < len(lines) and lines[index] != "":
            if not lines[index].startswith("//"):
                raise ValueError(f"invalid expectation line: {lines[index]!r}")
            line = lines[index]
            if line == "//":
                output_lines.append("")
            elif line.startswith("// "):
                output_lines.append(line[3:])
            else:
                output_lines.append(line[2:])
            index += 1
        return Expectation(kind="output", output="\n".join(output_lines))
    if directive.startswith("expect output "):
        return Expectation(kind="output", output=directive[len("expect output ") :])
    if directive.startswith("expect error "):
        code = directive[len("expect error ") :].strip()
        if not ERROR_RE.fullmatch(code):
            raise ValueError(f"invalid error code: {code!r}")
        return Expectation(kind="error", error_code=code)
    if directive == "expect exit nonzero":
        return Expectation(kind="nonzero")
    if directive == "expect exit 0":
        return Expectation(kind="exit0")
    raise ValueError(f"unknown expectation: {directive!r}")


def compiler_files() -> list[Path]:
    groups = ["pass", "error"]
    if "--include-ub" in sys.argv:
        groups.append("ub")
    return [
        path
        for group in groups
        for path in sorted((TESTS / group).glob("*.glt"))
    ]


def find_provider(library_name: str) -> Path | None:
    stem = Path(library_name).stem
    matches = list((TESTS / "support").rglob(stem + ".glt"))
    return matches[0] if len(matches) == 1 else None


def compile_program(
    test: Path,
    group: str,
    compiler_name: str,
    compiler: Path,
    compile_timeout: float,
    run_timeout: float,
) -> Result:
    started = time.monotonic()
    stem = test.stem
    exe = RESULTS / compiler_name / group / f"{stem}.exe"
    work = RESULTS / "run" / compiler_name / group / stem
    ensure_allowed(exe)
    ensure_allowed(work)
    exe.parent.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)
    (work / "tests" / "work").mkdir(parents=True, exist_ok=True)
    if exe.exists():
        exe.unlink()

    env = environment()
    source_text = test.read_text(encoding="utf-8", errors="replace")
    libraries = CLIB_RE.findall(source_text)
    local_libraries = [name for name in libraries if Path(name).stem not in {"string", "libucrt", "libc", "kernel32"}]
    compile_input = test

    try:
        expectation = parse_expectation(test)
    except ValueError as exc:
        return Result(
            compiler=compiler_name,
            group=group,
            test=str(test.relative_to(TESTS)),
            outcome="fail",
            duration_ms=int((time.monotonic() - started) * 1000),
            detail=str(exc),
        )

    if local_libraries:
        local_test = work / test.name
        shutil.copy2(test, local_test)
        compile_input = local_test
        for library in local_libraries:
            provider = find_provider(library)
            if provider is None:
                continue
            provider_project = work / provider.name
            shutil.copy2(provider, provider_project)
            provider_command = [
                str(compiler),
                "--compile",
                "--input",
                str(provider_project),
                "--output",
                str(work / library),
                "--no-runtime",
            ]
            provider_run = subprocess.run(
                provider_command,
                cwd=work,
                env=env,
                capture_output=True,
                timeout=compile_timeout,
                check=False,
            )
            if provider_run.returncode != 0 or not (work / library).is_file():
                return Result(
                    compiler=compiler_name,
                    group=group,
                    test=str(test.relative_to(TESTS)),
                    outcome="fail",
                    duration_ms=int((time.monotonic() - started) * 1000),
                    compile_rc=provider_run.returncode,
                    detail=f"provider build failed: {library}",
                    stdout=normalize_text(provider_run.stdout),
                    stderr=normalize_text(provider_run.stderr),
                )

    flags: list[str] = []
    if group != "error" and any("gallt_abi" in name for name in local_libraries):
        flags.append("--gallt-abi")

    command = [
        str(compiler),
        "--compile",
        "--input",
        str(compile_input),
        "--output",
        str(exe),
        *flags,
    ]
    try:
        compiled = subprocess.run(
            command,
            cwd=work if local_libraries else test.parent,
            env=env,
            input=b"n\n",
            capture_output=True,
            timeout=compile_timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        return Result(
            compiler=compiler_name,
            group=group,
            test=str(test.relative_to(TESTS)),
            outcome="fail",
            duration_ms=int((time.monotonic() - started) * 1000),
            detail="compiler timed out",
            stdout=normalize_text(exc.stdout or b""),
            stderr=normalize_text(exc.stderr or b""),
        )

    compile_stdout = normalize_text(compiled.stdout)
    compile_stderr = normalize_text(compiled.stderr)
    compiled_ok = compiled.returncode == 0 and exe.is_file()

    if expectation.kind == "error":
        if compiled_ok:
            return Result(
                compiler=compiler_name,
                group=group,
                test=str(test.relative_to(TESTS)),
                outcome="fail",
                duration_ms=int((time.monotonic() - started) * 1000),
                compile_rc=compiled.returncode,
                compiled=True,
                detail=f"expected compile error {expectation.error_code}, compilation succeeded",
                stdout=compile_stdout,
                stderr=compile_stderr,
                exe=str(exe),
            )
        feedback = compile_stdout + "\n" + compile_stderr
        if not ERROR_RE.search(feedback) or expectation.error_code not in feedback.upper():
            return Result(
                compiler=compiler_name,
                group=group,
                test=str(test.relative_to(TESTS)),
                outcome="fail",
                duration_ms=int((time.monotonic() - started) * 1000),
                compile_rc=compiled.returncode,
                compiled=False,
                detail=f"expected diagnostic {expectation.error_code}",
                stdout=compile_stdout,
                stderr=compile_stderr,
            )
        return Result(
            compiler=compiler_name,
            group=group,
            test=str(test.relative_to(TESTS)),
            outcome="pass",
            duration_ms=int((time.monotonic() - started) * 1000),
            compile_rc=compiled.returncode,
            compiled=False,
            stdout=compile_stdout,
            stderr=compile_stderr,
        )

    if not compiled_ok:
        if expectation.kind == "nonzero":
            return Result(
                compiler=compiler_name,
                group=group,
                test=str(test.relative_to(TESTS)),
                outcome="pass",
                duration_ms=int((time.monotonic() - started) * 1000),
                compile_rc=compiled.returncode,
                compiled=False,
                stdout=compile_stdout,
                stderr=compile_stderr,
            )
        return Result(
            compiler=compiler_name,
            group=group,
            test=str(test.relative_to(TESTS)),
            outcome="fail",
            duration_ms=int((time.monotonic() - started) * 1000),
            compile_rc=compiled.returncode,
            compiled=False,
            detail="compilation failed",
            stdout=compile_stdout,
            stderr=compile_stderr,
        )

    stdin_fixture = test.with_suffix(".stdin")
    stdin_bytes = stdin_fixture.read_bytes() if stdin_fixture.is_file() else None
    try:
        run = subprocess.run(
            [str(exe)],
            cwd=work,
            env=env,
            input=stdin_bytes,
            capture_output=True,
            timeout=run_timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        return Result(
            compiler=compiler_name,
            group=group,
            test=str(test.relative_to(TESTS)),
            outcome="pass" if group == "ub" else "fail",
            duration_ms=int((time.monotonic() - started) * 1000),
            compile_rc=compiled.returncode,
            compiled=True,
            detail="program timed out (undefined behavior)" if group == "ub" else "program timed out",
            stdout=normalize_text(exc.stdout or b""),
            stderr=normalize_text(exc.stderr or b""),
            exe=str(exe),
        )

    run_stdout = normalize_text(run.stdout)
    run_stderr = normalize_text(run.stderr)
    if group == "ub":
        outcome = "pass"
        detail = f"undefined behavior completed with exit {run.returncode}"
    elif expectation.kind == "nonzero":
        if run.returncode != 0:
            outcome = "pass"
            detail = ""
        else:
            outcome = "fail"
            detail = "expected nonzero exit, got 0"
    elif run.returncode != 0:
        outcome = "fail"
        detail = f"expected exit 0, got {run.returncode}"
    elif expectation.kind == "output":
        actual = normalize_actual_text(run_stdout)
        expected = normalize_expected_text(expectation.output)
        if actual == expected:
            outcome = "pass"
            detail = ""
        else:
            outcome = "fail"
            detail = f"stdout mismatch: expected {len(expected)} chars, got {len(actual)}"
    else:
        outcome = "pass"
        detail = ""

    return Result(
        compiler=compiler_name,
        group=group,
        test=str(test.relative_to(TESTS)),
        outcome=outcome,
        duration_ms=int((time.monotonic() - started) * 1000),
        compile_rc=compiled.returncode,
        run_rc=run.returncode,
        compiled=True,
        detail=detail,
        stdout=run_stdout,
        stderr=run_stderr,
        exe=str(exe),
    )


def select_tests(filter_text: str, include_ub: bool) -> list[tuple[str, Path]]:
    groups = ["pass", "error"]
    if include_ub:
        groups.append("ub")
    selected: list[tuple[str, Path]] = []
    for group in groups:
        for test in sorted((TESTS / group).glob("*.glt")):
            if filter_text and filter_text.lower() not in test.name.lower():
                continue
            if not FILENAME_RE.fullmatch(test.name):
                raise ValueError(f"invalid normalized test name: {test}")
            selected.append((group, test))
    return selected


def run_all(args: argparse.Namespace) -> int:
    ensure_allowed(RESULTS)
    RESULTS.mkdir(parents=True, exist_ok=True)
    compilers = list(COMPILERS) if args.compiler == "all" else [args.compiler]
    for name in compilers:
        if not COMPILERS[name].is_file():
            raise FileNotFoundError(COMPILERS[name])

    tests = select_tests(args.filter, args.include_ub)
    if not tests:
        print("no tests selected")
        return 2

    results: list[Result] = []
    total = len(tests) * len(compilers)
    completed = 0
    print(f"running {len(tests)} tests with {len(compilers)} compiler(s), {args.jobs} workers")
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {
            pool.submit(
                compile_program,
                test,
                group,
                compiler_name,
                COMPILERS[compiler_name],
                args.compile_timeout,
                args.run_timeout,
            ): (group, test, compiler_name)
            for compiler_name in compilers
            for group, test in tests
        }
        for future in concurrent.futures.as_completed(futures):
            group, test, compiler_name = futures[future]
            try:
                result = future.result()
            except Exception as exc:
                result = Result(
                    compiler=compiler_name,
                    group=group,
                    test=str(test.relative_to(TESTS)),
                    outcome="fail",
                    duration_ms=0,
                    detail=f"runner exception: {exc}",
                )
            results.append(result)
            completed += 1
            if completed % 50 == 0 or completed == total:
                print(f"  completed {completed}/{total}")

    results.sort(key=lambda item: (item.compiler, item.group, item.test))
    report = RESULTS / "latest-results.json"
    ensure_allowed(report)
    report.write_text(
        json.dumps([asdict(item) for item in results], ensure_ascii=False, indent=2),
        encoding="utf-8",
        newline="\n",
    )

    failed = [item for item in results if item.outcome != "pass"]
    for item in failed:
        print(
            f"FAIL [{item.compiler}] {item.group}/{item.test}: {item.detail}",
            file=sys.stderr,
        )
        if item.stderr:
            print(item.stderr.rstrip(), file=sys.stderr)

    summary: dict[str, dict[str, int]] = {}
    for item in results:
        compiler_summary = summary.setdefault(item.compiler, {})
        compiler_summary[item.outcome] = compiler_summary.get(item.outcome, 0) + 1
        key = f"{item.group}:{item.outcome}"
        compiler_summary[key] = compiler_summary.get(key, 0) + 1
    print("summary:", json.dumps(summary, ensure_ascii=False, sort_keys=True))
    print("report:", report)
    return 0 if not failed else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", choices=("clang", "msvc", "all"), default="all")
    parser.add_argument("--jobs", type=int, default=max(2, min(8, os.cpu_count() or 4)))
    parser.add_argument("--compile-timeout", type=float, default=30.0)
    parser.add_argument("--run-timeout", type=float, default=8.0)
    parser.add_argument("--filter", default="")
    parser.add_argument("--include-ub", action="store_true")
    return run_all(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
