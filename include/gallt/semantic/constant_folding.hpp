// semantic/constant_folding.hpp
// Gallt 0.3.txt §19：编译期常量表达式的求值
// Gallt 0.3.txt §19: evaluation of compile-time constant expressions
//
// 允许的组成：整数字面量、浮点字面量、字符字面量、布尔字面量；已实例化的编译期常量参数；
// 括号；算术 + - * / % **；比较 > < ==；逻辑 && || !；类型转换 [类型]([表达式])；
// 编译期内建函数 size、align（参数可在编译期确定时）。
// Allowed: integer/float/char/bool literals; bound compile-time constant parameters;
// parentheses; arithmetic; comparisons; logical operators; type conversions; size/align.

#ifndef GALLT_SEMANTIC_CONSTANT_FOLDING_HPP
#define GALLT_SEMANTIC_CONSTANT_FOLDING_HPP

#include "../parser/ast.hpp"

#include <cstddef>
#include <functional>
#include <string>

namespace gallt {

    // 求值上下文：类型大小/对齐解析 + 编译期常量参数绑定
    // Evaluation context: type layout resolution and compile-time constant bindings
    struct ConstantEvaluationContext {
        // 解析类型名的大小与对齐（用于 size/align）；未设置时 size/align 无法求值
        // Resolve a type name's size/alignment for size/align; unset means not evaluable
        std::function<bool(const std::string& type_name, std::size_t& size, std::size_t& align)>
            type_layout;
        // 解析编译期常量参数名（用于泛型体内的 N、N*2、int(N) 等）
        // Resolve a compile-time constant parameter name (N, N*2, int(N), ...)
        std::function<bool(const std::string& name, long long& int_value, double& float_value,
            bool& is_float)> lookup_constant;
    };

    // 求值编译期常量表达式；成功返回 true
    // Evaluate a compile-time constant expression; returns true on success
    bool evaluate_constant_expression(const AST::Expression* expr, long long& int_out,
        double& float_out, bool& is_float_out, const ConstantEvaluationContext& context);

} // namespace gallt

#endif // GALLT_SEMANTIC_CONSTANT_FOLDING_HPP
