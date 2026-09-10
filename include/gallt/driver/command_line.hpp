// driver/command_line.hpp
// 命令行参数解析 —— sgc 编译驱动的参数接口
// Command-line parsing — argument interface of the sgc compiler driver

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
        std::string input;       // 输入 .glt 文件
        std::string output;      // 输出 .exe 文件
        std::vector<std::string> positional;
        std::string error_message;
    };

    // 从宽字符参数解析选项；返回 false 并填充 error_message 表示格式错误
    // Parse wide-character arguments; returns false and fills error_message on failure
    bool parse_command_line(int argc, const wchar_t* const* argv, CommandOptions& out);

    std::string help_text();
    std::string version_text();

} // namespace gallt

#endif // GALLT_DRIVER_COMMAND_LINE_HPP
