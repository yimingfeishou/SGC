// SGC.cpp
// Gallt 编译器命令行入口
// Command-line entry point of the Gallt compiler

#include "include/gallt/driver/command_line.hpp"
#include "include/gallt/driver/compiler.hpp"

#include <iostream>

// Gallt 语言标准 26.09 (Preview) 对应的编译器版本（Gallt 0.4 预览版本）
// Compiler version matching Gallt Lang Standard 26.09 (Preview), per Gallt 0.4
const std::string VERSION = "0.4.0 Preview";
// 编译日期 / build date (Gallt 0.4 preview, 2026-09-13)
const std::string BUILD_DATE = "2026-09-13";

int wmain(int argc, wchar_t* argv[]) {
    std::cout << "sgc Standard Gallt Compiler " << VERSION << "\n";
    std::cout << "Gallt Lang Standard Version 26.09 (Preview)\n";
    std::cout << "Build date: " << BUILD_DATE << "\n";
    std::cout << "Copyright (c) Yimingfeishou.\n\n";
    // 解析参数并分派帮助/版本/编译模式
    // Parse options and dispatch help, version, or compile mode
    gallt::CommandOptions options;
    if (!gallt::parse_command_line(argc, argv, options)) {
        std::cerr << "sgc: " << options.error_message << "\n\n"
                  << gallt::help_text();
        return 2;
    }
    switch (options.mode) {
    case gallt::CommandMode::Help:
        std::cout << gallt::help_text();
        return 0;
    case gallt::CommandMode::Version:
        std::cout << gallt::version_text();
        return 0;
    case gallt::CommandMode::Compile:
        return gallt::run_compiler(options);
    case gallt::CommandMode::Invalid:
        break;
    }
    return 2;
}
