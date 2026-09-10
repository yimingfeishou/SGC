# SGC / Gallt 编译器

## 中文文档

### 简介

SGC 是一个 Gallt 语言编译器前端。后端复用 LLVM 的 clang/lld，在 Windows 11 x64 上直接生成 PE 可执行文件。

当前版本：`0.2.0 Preview`。

### 目录结构

- `SGC/include/gallt/lexer`：词法分析
- `SGC/include/gallt/parser`：语法分析与 AST
- `SGC/include/gallt/semantic`：符号表与类型检查
- `SGC/include/gallt/codegen`：LLVM IR 生成
- `SGC/include/gallt/driver`：命令行驱动
- `SGC/include/gallt/...`：源码注释为中英双语

### 构建

用 Visual Studio 打开 `SGC.slnx`，选择 x64 后编译；或使用 CMake。

编译生成的 `sgc` 需要通过 `SGC_LLVM_BIN` 或 `LLVM_BIN` 找到 `clang.exe`。若未设置环境变量，还会探测常见 LLVM 构建目录，例如：

- `C:/LLVM/build/Release/bin`
- `D:/LLVM/build/Release/bin`
- `E:/LLVM/build/Release/bin`

运行 `sgc` 时还需要 LLVM 的 `clang.exe` 与 `lld`。

### 用法

```text
sgc --compile --input file.glt --output file.exe
sgc --help
sgc --version
```

### 注意事项

- 当前为早期预览版本，部分语法和边缘情况可能仍有问题。
- 临时后端文件默认写入 `%TEMP%`。设置 `SGC_KEEP_TEMP=1` 可保留生成的 `.ll` 与 `.c` 文件。
- 输出文件未指定扩展名时，会自动追加 `.exe`。

---

## English Documentation

### Overview

SGC is a Gallt language compiler frontend. Its backend reuses LLVM's clang/lld and emits PE executables directly on Windows 11 x64.

Current version: `0.2.0 Preview`.

### Layout

- `SGC/include/gallt/lexer`: lexer
- `SGC/include/gallt/parser`: parser and AST
- `SGC/include/gallt/semantic`: symbol table and type checking
- `SGC/include/gallt/codegen`: LLVM IR generation
- `SGC/include/gallt/driver`: command-line driver
- `SGC/include/gallt/...`: all source comments are bilingual, Chinese and English

### Build

Open `SGC.slnx` in Visual Studio, select x64, and build. Alternatively, use CMake.

The built `sgc` locates `clang.exe` through `SGC_LLVM_BIN` or `LLVM_BIN`. If neither is set, it also probes common LLVM build directories such as:

- `C:/LLVM/build/Release/bin`
- `D:/LLVM/build/Release/bin`
- `E:/LLVM/build/Release/bin`

Running `sgc` also requires LLVM's `clang.exe` and `lld`.

### Usage

```text
sgc --compile --input file.glt --output file.exe
sgc --help
sgc --version
```

### Notes

- This is an early preview release. Some syntax and edge cases may still be incomplete or incorrect.
- Temporary backend files are written to `%TEMP%` by default. Set `SGC_KEEP_TEMP=1` to keep the generated `.ll` and `.c` files.
- If no extension is specified for the output file, `.exe` is appended automatically.
