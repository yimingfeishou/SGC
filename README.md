# SGC / Standard Gallt Compile

## 中文

### 简介

SGC 是一个全新语言 Gallt 编译器前端。后端复用 LLVM 的 clang/lld，在 Windows 11 x64 上直接生成 PE 可执行文件。

当前版本：`0.4.2-0922 Preview`。

### 目录结构

- `SGC/include/gallt/lexer`：词法分析
- `SGC/include/gallt/parser`：语法分析与 AST
- `SGC/include/gallt/semantic`：符号表与类型检查
- `SGC/include/gallt/codegen`：LLVM IR 生成
- `SGC/include/gallt/driver`：命令行驱动

### 构建

编译生成的 `sgc` 需要通过 `SGC_LLVM_BIN` 或 `LLVM_BIN` 找到 `clang.exe`。若未设置环境变量，还会探测常见 LLVM 构建目录，例如：

- `C:/LLVM/build/Release/bin`
- `D:/LLVM/build/Release/bin`
- `E:/LLVM/build/Release/bin`

运行 `sgc` 时还需要 LLVM 的 `clang.exe` 与 `lld`。

### 用法

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

### 注意事项

- 当前为早期预览版本，部分语法和边缘情况可能仍有问题。
- 临时后端文件默认写入 `%TEMP%`。设置 `SGC_KEEP_TEMP=1` 可保留生成的 `.ll` 与 `.c` 文件。
- 输出文件未指定扩展名时，会自动追加 `.exe`。
- SGC 预计将在 0.5.x-0.6.x 版本之后完成对 Linux 版本的基本适配。

---

## English

### Overview

SGC is a compiler front-end for a brand-new language called Gallt. Its backend reuses LLVM's clang/lld and emits PE executables directly on Windows 11 x64.

Current version: `0.4.2-0922 Preview`.

### Layout

- `SGC/include/gallt/lexer`: lexer
- `SGC/include/gallt/parser`: parser and AST
- `SGC/include/gallt/semantic`: symbol table and type checking
- `SGC/include/gallt/codegen`: LLVM IR generation
- `SGC/include/gallt/driver`: command-line driver

### Build

The built `sgc` locates `clang.exe` through `SGC_LLVM_BIN` or `LLVM_BIN`. If neither is set, it also probes common LLVM build directories such as:

- `C:/LLVM/build/Release/bin`
- `D:/LLVM/build/Release/bin`
- `E:/LLVM/build/Release/bin`

Running `sgc` also requires LLVM's `clang.exe` and `lld`.

### Usage

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

### Notes

- This is an early preview release. Some syntax and edge cases may still be incomplete or incorrect.
- Temporary backend files are written to `%TEMP%` by default. Set `SGC_KEEP_TEMP=1` to keep the generated `.ll` and `.c` files.
- If no extension is specified for the output file, `.exe` is appended automatically.
- SGC is expected to complete the basic adaptation for the Linux version after versions 0.5.x-0.6.x.
