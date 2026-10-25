# SGC / Standard Gallt Compiler

## English

### Overview

SGC is a compiler front-end for a brand-new language called Gallt. Its backend reuses LLVM's clang/lld and emits PE/ELF executables directly on Windows 11 x64 / Linux x64. This version is experimental.

Current version: `0.5.0-1023 Preview`.

### Layout

- `SGC/include/gallt/lexer`: lexer
- `SGC/include/gallt/parser`: parser and AST
- `SGC/include/gallt/semantic`: symbol table and type checking
- `SGC/include/gallt/codegen`: LLVM IR generation
- `SGC/include/gallt/driver`: command-line driver

### Build

The built `sgc` locates `clang.exe` through `SGC_LLVM_BIN` or `LLVM_BIN`. Running `sgc` also requires LLVM's `clang.exe` and `lld`.

### Usage examples

```text
sgc --compile --input "file.glt" --output "program.exe"
sgc --compile --input "file.glt" --output "program.exe" --optimization-level <0-4>
sgc --compile --input "file.glt" --output "program.exe" -OL <0-4>
sgc --compile --input "file.glt" --output "program.exe" -DS <0-2>
sgc --compile --input "file.glt" --output "program.exe" --debug
sgc --compile --input "file.glt" --output "program.exe" --release
sgc --help
sgc --version
```

For more usage instructions, please enter `--help` within SGC to view them.

### Tests

- The test directory is `./tests`. After entering this directory, you can run `python run_tests.py` for full testing, and the final results will be output to the `./results` directory (if the directory does not exist, the script will automatically create it).
- This script depends on the following environment variables to work:

```text
SGC_LLVM_BIN # Set the LLVM backend location.
SGC_STDLIB # Set the Gallt standard library location.
SGC_CLANG # Set the location of the SGC compiled with Clang.
SGC_MSVC # Set the location of the SGC compiled with MSVC.
```

- Running this script requires Python 3.10 or later, and the above environment variables to be set.

### Notes

- This is an early preview release. Some syntax and edge cases may still be incomplete or incorrect.
- Temporary backend files are written to `%TEMP%` by default. Set `SGC_KEEP_TEMP=1` to keep the generated `.ll` and `.c` files.
- If no extension is specified for the output file, `.exe` is appended automatically.

---

## 简体中文

### 简介

SGC 是一个全新语言 Gallt 编译器前端。后端复用 LLVM 的 clang/lld，在 Windows 11 x64 / Linux x64 上直接生成 PE/ELF 可执行文件，该版本是实验性的。

当前版本：`0.5.0-1023 Preview`。

### 目录结构

- `SGC/include/gallt/lexer`：词法分析
- `SGC/include/gallt/parser`：语法分析与 AST
- `SGC/include/gallt/semantic`：符号表与类型检查
- `SGC/include/gallt/codegen`：LLVM IR 生成
- `SGC/include/gallt/driver`：命令行驱动

### 构建

编译生成的 `sgc` 需要通过 `SGC_LLVM_BIN` 或 `LLVM_BIN` 找到 `clang.exe`。运行 `sgc` 时还需要 LLVM 的 `clang.exe` 与 `lld`。

### 用法示例

```text
sgc --compile --input "file.glt" --output "program.exe"
sgc --compile --input "file.glt" --output "program.exe" --optimization-level <0-4>
sgc --compile --input "file.glt" --output "program.exe" -OL <0-4>
sgc --compile --input "file.glt" --output "program.exe" -DS <0-2>
sgc --compile --input "file.glt" --output "program.exe" --debug
sgc --compile --input "file.glt" --output "program.exe" --release
sgc --help
sgc --version
```

更多用法，请在 SGC 内输入 `--help` 以查看。

### 测试

- 测试目录为 `./tests`，进入该目录后，可以使用 `python run_tests.py` 进行完整测试，最终结果将会输出到 `./results` 目录中（若不存在该目录，脚本则会自动创建）。
- 该脚本依赖以下环境变量才能工作：

```text
SGC_LLVM_BIN # 设置 LLVM 后端位置。
SGC_STDLIB # 设置 Gallt 标准库位置。
SGC_CLANG # 设置使用 Clang 编译的 SGC 的位置。
SGC_MSVC # 设置使用了 MSVC 编译的 SGC 的位置。
```

- 运行该脚本需要 Python 3.10 及以上版本，且设置了如上环境变量。

### 注意事项

- 当前为早期预览版本，部分语法和边缘情况可能仍有问题。
- 临时后端文件默认写入 `%TEMP%`。设置 `SGC_KEEP_TEMP=1` 可保留生成的 `.ll` 与 `.c` 文件。
- 输出文件未指定扩展名时，会自动追加 `.exe`。
