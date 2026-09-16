#include "include/gallt/driver/command_line.hpp"
#include "include/gallt/driver/compiler.hpp"
#include <iostream>

const std::string VERSION = "0.4.1 Preview";
const std::string BUILD_DATE = "2026-09-16";

int wmain(int argc, wchar_t* argv[]) {
    std::cout << "sgc Standard Gallt Compiler " << VERSION << "\n";
    std::cout << "Gallt Lang Standard Version 26.09 (Preview)\n";
    std::cout << "Project repository address: https://github.com/yimingfeishou/SGC\n";
    std::cout << "Build date: " << BUILD_DATE << "\n";
    std::cout << "Copyright (c) Yimingfeishou.\n\n";
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
