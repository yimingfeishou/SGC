// driver/compiler.hpp
// 编译器主驱动 —— 文件加载、guide 展开、语义检查、LLVM IR 生成与链接
// Main compiler driver — loading, guide expansion, checking, IR generation, linking

#ifndef GALLT_DRIVER_COMPILER_HPP
#define GALLT_DRIVER_COMPILER_HPP

#include "command_line.hpp"

namespace gallt {

    // 返回进程退出码（0 表示成功）
    // Returns the process exit code (0 means success)
    int run_compiler(const CommandOptions& options);

} // namespace gallt

#endif // GALLT_DRIVER_COMPILER_HPP
