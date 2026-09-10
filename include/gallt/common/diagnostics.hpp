// common/diagnostics.hpp
// Gallt 编译器诊断系统 —— 错误/警告报告、源码位置追踪
// Gallt Compiler Diagnostic System — Error/Warning Reporting & Source Location Tracking

#ifndef GALLT_COMMON_DIAGNOSTICS_HPP
#define GALLT_COMMON_DIAGNOSTICS_HPP

#include "source_location.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <ostream>
#include <optional>
#include <cstdint>

namespace gallt {

    // ============================================================================
    // 诊断严重性级别
    // Diagnostic severity levels
    // ============================================================================

    enum class DiagnosticSeverity {
        Error,   // 编译错误 (导致编译停止)
        Warning, // 警告 (不停止编译)
        Note,    // 附加信息 (辅助诊断)
    };

    // ============================================================================
    // 错误码定义 —— 完全对应 Gallt 标准文档错误表 (ER 0001 ~ ER 0059, RTER 0001 ~ RTER 0002)
    // Error codes — exactly match Gallt Standard Document error table
    // 参照: Gallt 0.2.txt § 错误表
    // Reference: Gallt 0.2.txt § Error Table
    // ============================================================================

    // 编译时错误 (ER 0001 ~ ER 0059)
    enum class ErrorCode : std::uint16_t {
        // ER 0001: 未定义标识符
        UndefinedIdentifier = 1,                 // "未定义标识符 '[identifier]'"
        // ER 0002: 标识符重复声明
        RedefinedIdentifier = 2,                 // "标识符 '[identifier]' 重复声明"
        // ER 0003: 赋值类型不匹配
        AssignmentTypeMismatch = 3,              // "赋值类型不匹配：左值类型 '[type1]' 与右值类型 '[type2]' 不兼容"
        // ER 0004: 二元运算符操作数类型不匹配
        BinaryOperatorTypeMismatch = 4,          // "二元运算符 '[operator]' 操作数类型不匹配：'[type1]' 和 '[type2]' 不支持"
        // ER 0005: 一元运算符操作数类型不支持
        UnaryOperatorTypeMismatch = 5,           // "一元运算符 '[operator]' 操作数类型 '[type]' 不支持"
        // ER 0006: 函数返回类型不匹配
        FunctionReturnTypeMismatch = 6,          // "函数返回类型不匹配：声明返回 '[type]'，实际返回 '[type2]'"
        // ER 0007: 非 void 函数缺少 return 语句
        MissingReturnStatement = 7,              // "非 void 函数 '[function]' 缺少 return 语句"
        // ER 0008: void 函数不能返回带有值的表达式
        VoidFunctionReturnsValue = 8,            // "void 函数 '[function]' 不能返回带有值的表达式"
        // ER 0009: 函数未定义
        UndefinedFunction = 9,                   // "函数 '[function]' 未定义"
        // ER 0010: 函数重复定义
        RedefinedFunction = 10,                  // "函数 '[function]' 重复定义"
        // ER 0011: 函数调用参数数量不匹配
        FunctionArgCountMismatch = 11,           // "函数调用参数数量不匹配：需要 '[num]' 个，提供 '[num2]' 个"
        // ER 0012: 函数调用参数类型不匹配
        FunctionArgTypeMismatch = 12,            // "函数调用参数类型不匹配：参数 '[index]' 期望 '[type1]'，得到 '[type2]'"
        // ER 0013: 数组下标必须为整数类型
        SubscriptNotInteger = 13,                // "数组下标必须为整数类型，得到 '[type]'"
        // ER 0014: 数组下标越界
        SubscriptOutOfBounds = 14,               // "数组下标越界：下标 '[value]' 超出范围 [0, '[size]')"
        // ER 0015: 数组初始化列表为空，无法推断数组长度
        EmptyArrayInitializer = 15,              // "数组初始化列表为空，无法推断数组长度"
        // ER 0016: 数组初始化长度与声明长度不匹配
        ArrayLengthMismatch = 16,                // "数组初始化长度与声明长度不匹配：声明 '[size]'，初始化 '[size2]'"
        // ER 0017: 标识符不在当前作用域中
        IdentifierNotInScope = 17,               // "标识符 '[identifier]' 不在当前作用域中"
        // ER 0018: 控制语句后缺少大括号 '{' 界定作用域
        MissingBraces = 18,                      // "控制语句后缺少大括号 '{' 界定作用域"
        // ER 0019: break 语句只能出现在循环体内部
        BreakOutsideLoop = 19,                   // "break 语句只能出现在循环体内部"
        // ER 0020: 表达式语法错误：意外的标记
        ExpressionSyntaxError = 20,              // "表达式语法错误：意外的标记 '[token]'"
        // ER 0021: 主函数签名错误
        MainSignatureError = 21,                 // "主函数签名错误：必须为 'int main()' 或 'int main(int count, char *array[])'"
        // ER 0022: 主函数返回值类型必须为 int
        MainReturnTypeError = 22,                // "主函数返回值类型必须为 int，当前为 '[type]'"
        // ER 0023: 字符常量格式错误
        InvalidCharLiteral = 23,                 // "字符常量格式错误：空字符或无效转义序列"
        // ER 0024: 字符串常量格式错误：未闭合的字符串字面量
        UnclosedStringLiteral = 24,              // "字符串常量格式错误：未闭合的字符串字面量"
        // ER 0025: 数值字面量后缀无效
        InvalidNumericSuffix = 25,               // "数值字面量后缀无效：仅允许 'f' 表示 float 类型，得到 '[suffix]'"
        // ER 0026: else 语句缺少匹配的 if
        ElseWithoutIf = 26,                      // "else 语句缺少匹配的 if"
        // ER 0027: return 语句出现在函数体之外
        ReturnOutsideFunction = 27,              // "return 语句出现在函数体之外"
        // ER 0028: 全局作用域中不允许执行语句
        StatementInGlobalScope = 28,             // "全局作用域中不允许执行语句"
        // ER 0029: 条件表达式必须为布尔类型
        ConditionNotBoolean = 29,                // "条件表达式必须为布尔类型，得到 '[type]'"
        // ER 0030: 数组类型不支持该运算符
        ArrayOperatorNotSupported = 30,          // "数组类型不支持该运算符"
        // ER 0031: 数组声明时下标必须为常量整数表达式
        ArraySizeNotConstant = 31,               // "数组声明时下标必须为常量整数表达式，得到 '[expr]'"
        // ER 0032: 形参列表中不允许使用 void 类型
        VoidParameter = 32,                      // "形参列表中不允许使用 void 类型"
        // ER 0033: 非法的类型转换
        InvalidTypeCast = 33,                    // "非法的类型转换：无法将 '[type1]' 转换为 '[type2]'"
        // ER 0034: 输入文件不符合规定，编码或换行符错误
        InvalidInputFile = 34,                   // "输入文件不符合规定，编码为 '[coding]'，换行符为 '[linebreak]'"
        // ER 0035: 指针类型不兼容
        PointerTypeMismatch = 35,                // "指针类型不兼容：不能将 '[type1]*' 赋值给 '[type2]*'"
        // ER 0036: 解引用操作要求操作数为指针类型
        DerefNonPointer = 36,                    // "解引用操作要求操作数为指针类型，得到 '[type]'"
        // ER 0037: 取地址操作要求操作数为左值
        AddressOfNonLValue = 37,                 // "取地址操作要求操作数为左值，得到非左值表达式"
        // ER 0038: 指针算术操作数类型无效
        PointerArithmeticInvalid = 38,           // "指针算术操作数类型无效：期望指针和整数，得到 '[type1]' 和 '[type2]'"
        // ER 0039: 空指针解引用
        NullPointerDereference = 39,             // "空指针解引用"
        // ER 0040: free() 操作数必须为指针类型
        FreeNonPointer = 40,                     // "free() 操作数必须为指针类型，得到 '[type]'"
        // ER 0041: heap() 的第一个参数必须是类型名
        HeapFirstArgNotType = 41,                // "heap() 的第一个参数必须是类型名，得到 '[expr]'"
        // ER 0042: heap() 的第二个参数必须是整数类型
        HeapSecondArgNotInteger = 42,            // "heap() 的第二个参数必须是整数类型，得到 '[type]'"
        // ER 0043: 函数指针类型不匹配
        FuncPtrTypeMismatch = 43,                // "函数指针类型不匹配：期望 '[signature]'，得到 '[signature2]'"
        // ER 0044: 结构体成员不存在
        StructMemberNotFound = 44,               // "结构体成员 '[member]' 在类型 '[struct]' 中不存在"
        // ER 0045: 结构体初始化列表长度与成员数量不匹配
        StructInitLengthMismatch = 45,           // "结构体初始化列表长度与成员数量不匹配：需要 '[num]' 个，提供 '[num2]' 个"
        // ER 0046: 结构体成员类型不兼容
        StructMemberTypeMismatch = 46,           // "结构体成员类型不兼容：成员 '[member]' 期望类型 '[type1]'，初始化得到 '[type2]'"
        // ER 0047: 结构体中不允许定义函数/方法
        StructMethodNotAllowed = 47,             // "结构体中不允许定义函数/方法，成员 '[member]' 为函数定义"
        // ER 0048: 箭头运算符 -> 左操作数必须为结构体指针类型
        ArrowOnNonStructPtr = 48,                // "箭头运算符 -> 左操作数必须为结构体指针类型，得到 '[type]'"
        // ER 0049: 点运算符 . 左操作数必须为结构体类型
        DotOnNonStruct = 49,                     // "点运算符 . 左操作数必须为结构体类型，得到 '[type]'"
        // ER 0050: 结构体自引用必须通过指针
        StructSelfRefNonPtr = 50,                // "结构体自引用必须通过指针，不允许直接包含自身"
        // ER 0051: guide / clib() 找不到库
        LibraryNotFound = 51,                    // "guide / clib() 找不到库"
        // ---- Gallt 0.2.txt §17 文件操作新增错误码 ----
        // ---- New file-operation error codes (Gallt 0.2.txt §17) ----
        // ER 0052: 文件操作函数参数数量不匹配
        FileArgCountMismatch = 52,               // "文件操作函数 '[function]' 参数数量不匹配：需要 '[num]' 个，提供 '[num2]' 个"
        // ER 0053: 文件操作函数参数类型不匹配
        FileArgTypeMismatch = 53,                // "文件操作函数 '[function]' 参数 '[index]' 类型不匹配：期望 '[type1]'，得到 '[type2]'"
        // ER 0054: 文件打开模式无效
        FileOpenModeInvalid = 54,                // "文件打开模式无效：'[mode]'"
        // ER 0055: 文件操作要求 file* 类型
        FileHandleRequired = 55,                 // "文件操作要求 file* 类型，得到 '[type]'"
        // ER 0056: fileseek() 的起始位置必须为 0、1 或 2
        FileSeekOriginInvalid = 56,              // "fileseek() 的起始位置必须为 0、1 或 2，得到 '[value]'"
        // ER 0057: 文件读写缓冲区必须为指针类型
        FileBufferNotPointer = 57,               // "文件读写缓冲区必须为指针类型，得到 '[type]'"
        // ER 0058: 文件读写大小必须为 int 类型
        FileSizeNotInt = 58,                     // "文件读写大小必须为 int 类型，得到 '[type]'"
        // ER 0059: 文件路径或模式必须为 string 类型
        FilePathOrModeNotString = 59,            // "文件路径或模式必须为 string 类型，得到 '[type]'"
    };

    // 运行时错误 (RTER 0001 ~ RTER 0002)
    enum class RuntimeErrorCode : std::uint16_t {
        // RTER 0001: 内存分配失败
        HeapAllocFailure = 1,                    // "内存分配失败"
        // RTER 0002: 函数指针调用时指针为空
        NullFuncPtrCall = 2,                     // "函数指针调用时指针为空"
    };

    // ============================================================================
    // 诊断消息 (Diagnostic)
    // 单个诊断信息，包含严重性、位置、错误码、格式化消息
    // A single diagnostic, including severity, location, code, formatted message
    // ============================================================================

    struct Diagnostic {
        DiagnosticSeverity severity;
        SourceLocation location;
        ErrorCode code;                          // 仅当 severity 为 Error/Warning 时有效
        std::string message;                     // 已格式化的完整消息

        // 将错误码转换为字符串 "ER XXXX" 格式
        static std::string error_code_string(ErrorCode code);
        // 将运行时错误码转换为字符串 "RTER XXXX"
        static std::string runtime_error_code_string(RuntimeErrorCode code);
    };

    // ============================================================================
    // 诊断引擎 (DiagnosticEngine)
    // 收集、存储和输出所有诊断信息，控制编译是否继续
    // Collects, stores, and outputs all diagnostics; controls compilation continuation
    // ============================================================================

    class DiagnosticEngine {
    public:
        DiagnosticEngine() = default;

        // 报告错误 (编译错误)
        void report_error(SourceLocation loc, ErrorCode code, std::string_view message);
        // 报告警告
        void report_warning(SourceLocation loc, ErrorCode code, std::string_view message);
        // 报告通知/备注 (通常用于附加信息)
        void report_note(SourceLocation loc, std::string_view message);

        // 报告运行时错误 (不存储，直接输出)
        static void report_runtime_error(RuntimeErrorCode code, std::string_view message);

        // 检查是否有任何错误 (包括已报告的错误)
        bool has_errors() const noexcept { return error_count_ > 0; }
        // 获取错误数量
        std::size_t error_count() const noexcept { return error_count_; }
        // 获取警告数量
        std::size_t warning_count() const noexcept { return warning_count_; }

        // 清空所有诊断
        void clear();

        // 将所有诊断输出到流 (格式: "filename:line:col: severity: code: message")
        void print_all(std::ostream& os) const;

        // 将诊断输出到流 (按严重性过滤)
        void print_all(std::ostream& os, DiagnosticSeverity min_severity) const;

    private:
        std::vector<Diagnostic> diagnostics_;
        std::size_t error_count_ = 0;
        std::size_t warning_count_ = 0;

        // 内部报告函数
        void report(DiagnosticSeverity sev, SourceLocation loc, ErrorCode code, std::string_view msg);
        void report(DiagnosticSeverity sev, SourceLocation loc, std::string_view msg); // 用于 note

        // 格式化消息辅助函数
        static std::string format_message(ErrorCode code, std::string_view user_msg);
        static std::string severity_prefix(DiagnosticSeverity sev);
    };

} // namespace gallt

#endif // GALLT_COMMON_DIAGNOSTICS_HPP
