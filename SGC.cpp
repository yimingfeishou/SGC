#include "include/gallt/driver/command_line.hpp"
#include "include/gallt/driver/compiler.hpp"
#include "include/gallt/pal/platform.hpp"
#include <iostream>

const std::string VERSION = "0.4.2-0925 Preview";

int wmain(int argc, wchar_t* argv[]) {
    gallt::pal::enable_utf8_console();
    std::cout << "sgc Standard Gallt Compiler " << VERSION << "\n";
    std::cout << "Project repository address: https://github.com/yimingfeishou/SGC\n";
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
        return gallt::pal::exit_success_code();
    case gallt::CommandMode::Version:
        std::cout << gallt::version_text();
        return gallt::pal::exit_success_code();
    case gallt::CommandMode::Compile:
        return gallt::run_compiler(options);
    case gallt::CommandMode::Invalid:
        break;
    }
    return 2;
}
