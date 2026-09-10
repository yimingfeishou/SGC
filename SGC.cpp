// SGC.cpp
// Gallt 编译器命令行入口
// Command-line entry point of the Gallt compiler

#include "include/gallt/driver/command_line.hpp"
#include "include/gallt/driver/compiler.hpp"

#include <iostream>

const std::string VERSION = "0.2.0 Preview";

int wmain(int argc, wchar_t* argv[]) {
    std::cout << "sgc Standard Gallt Compiler " << VERSION << "\n";
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
