#ifndef GALLT_DRIVER_COMMAND_LINE_HPP
#define GALLT_DRIVER_COMMAND_LINE_HPP

#include <string>
#include <vector>

namespace gallt {

    enum class CommandMode {
        Compile,
        Help,
        Version,
        Invalid,
    };

    struct CommandOptions {
        CommandMode mode = CommandMode::Compile;
        std::string input;       
        std::string output;      
        int optimization_level = 2;
        int debug_symbols_level = 0;
        bool debug_mode = false;
        bool release_mode = false;
        bool optimization_level_explicit = false;
        bool debug_symbols_explicit = false;
        bool debug_mode_explicit = false;
        bool release_mode_explicit = false;
        std::vector<std::string> positional;
        std::string error_message;
    };

    bool parse_command_line(int argc, const wchar_t* const* argv, CommandOptions& out);

    std::string help_text();
    std::string version_text();

} 

#endif 
