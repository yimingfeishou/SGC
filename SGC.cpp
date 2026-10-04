#include "include/gallt/driver/command_line.hpp"
#include "include/gallt/driver/compiler.hpp"
#include "include/gallt/driver/entry.hpp"
#include "include/gallt/pal/platform.hpp"
#include <iostream>
#include <string>
#include <vector>

const std::string VERSION = "0.5.0-1004 Preview";

namespace gallt {

    int sgc_main(const std::vector<std::wstring>& arguments) {
        pal::enable_utf8_console();
        std::cout << "Standard Gallt Compiler (sgc) " << VERSION << "\n";
        std::cout << "Project repository address: https://github.com/yimingfeishou/SGC\n";
        std::cout << "Copyright (c) Yimingfeishou\n\n";

        std::vector<const wchar_t*> argv;
        argv.reserve(arguments.size());

        for (const std::wstring& argument : arguments) {
            argv.push_back(argument.c_str());
        }

        CommandOptions options;

        if (!parse_command_line(static_cast<int>(argv.size()), argv.data(),
            options)) {
            std::cerr << "sgc: " << options.error_message << "\n\n"
                      << help_text();
            return 2;
        }

        switch (options.mode) {
        case CommandMode::Help:
            std::cout << help_text();
            return pal::exit_success_code();
        case CommandMode::Version:
            std::cout << version_text();
            return pal::exit_success_code();
        case CommandMode::Compile:
            return run_compiler(options);
        case CommandMode::Invalid:
            break;
        }

        return 2;
    }

}
