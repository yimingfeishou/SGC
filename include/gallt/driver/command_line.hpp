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
        // 优化等级（Doc/编译器参数.txt）：0 不优化、1 基本、2 中等、3 激进、4 代码体积
        // Optimization level (Doc/compiler-options.txt): 0 none, 1 basic, 2 medium,
        // 3 aggressive, 4 size-oriented；直接转发给 clang（-O0/-O1/-O2/-O3/-Os）
        int optimization_level = 2;
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
