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
    // 错误码定义 —— 完全对应 Gallt 标准文档错误表 (ER 0001 ~ ER 0098, RTER 0001 ~ RTER 0002)
    // Error codes — exactly match Gallt Standard Document error table
    // 参照: Gallt 0.3.txt § 错误表
    // Reference: Gallt 0.3.txt § Error Table
    // ============================================================================

    // 编译时错误 (ER 0001 ~ ER 0098)
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

        // ---- Gallt 0.3.txt §19 编译期泛型新增错误码 ----
        // ---- New compile-time-generics error codes (Gallt 0.3.txt §19) ----
        // ER 0060: 泛型块顶部、所有成员定义之外不允许可执行语句
        GenericStatementNotAllowed = 60,         // "泛型块顶部、所有成员定义之外不允许可执行语句"
        // ER 0061: 泛型未定义
        GenericUndefined = 61,                   // "泛型 '[generic]' 未定义"
        // ER 0062: 泛型重复定义为主泛型
        GenericPrimaryRedefined = 62,            // "泛型 '[generic]' 重复定义为主泛型"
        // ER 0063: 泛型参数重复声明
        GenericParameterRedefined = 63,          // "泛型参数 '[parameter]' 重复声明"
        // ER 0064: 泛型实例化实参数量不匹配
        GenericArgCountMismatch = 64,            // "泛型实例化 '[generic]<[arguments]>' 实参数量不匹配：需要 '[num]' 个，提供 '[num2]' 个"
        // ER 0065: 泛型实参不满足约束
        GenericConstraintViolated = 65,          // "泛型实例化 '[generic]<[arguments]>' 实参 '[index]' 不满足约束 '[constraint]'，得到 '[type]'"
        // ER 0066: 泛型实例化无法匹配任何泛型或特化
        GenericNoMatch = 66,                     // "泛型实例化 '[generic]<[arguments]>' 无法匹配任何泛型或特化"
        // ER 0067: 泛型实例化存在多个互不可比较的最特化候选
        GenericAmbiguousSpecialization = 67,     // "泛型实例化 '[generic]<[arguments]>' 存在多个互不可比较的最特化候选，产生二义性"
        // ER 0068: 泛型常量模式无法在编译期求值
        GenericConstantPatternNotConstant = 68,  // "泛型常量模式无法在编译期求值：'[expr]'"
        // ER 0069: 泛型非类型实参必须是编译期常量表达式
        GenericNonTypeArgNotConstant = 69,       // "泛型非类型实参必须是编译期常量表达式，得到 '[expr]'"
        // ER 0070: 泛型非类型实参类型无法转换为常量参数类型
        GenericNonTypeArgTypeMismatch = 70,      // "泛型非类型实参类型无法转换为常量参数类型 '[type]'，得到 '[type2]'"
        // ER 0071: 泛型非类型实参值超出常量参数类型可表示范围
        GenericNonTypeArgOutOfRange = 71,        // "泛型非类型实参值超出常量参数类型 '[type]' 的可表示范围"
        // ER 0072: 偏特化自由标识符与同一偏特化块内其他自由标识符冲突
        GenericFreeIdentifierConflict = 72,      // "偏特化自由标识符 '[identifier]' 与同一偏特化块内其他自由标识符冲突"
        // ER 0073: 泛型成员短名产生二义性
        GenericShortNameAmbiguous = 73,          // "泛型成员短名 '[identifier]' 产生二义性，必须使用限定名"
        // ER 0074: 泛型成员短名与当前作用域中已有声明冲突
        GenericShortNameConflict = 74,           // "泛型成员短名 '[identifier]' 与当前作用域中已有声明冲突"
        // ER 0075: 特化中缺少实例化后必须存在的成员
        GenericSpecializationMissingMember = 75, // "特化 '[specialization]' 中缺少实例化后必须存在的成员 '[member]'"
        // ER 0076: 泛型的特化缺少主泛型声明
        GenericSpecializationWithoutPrimary = 76, // "泛型 '[generic]' 的特化缺少主泛型声明"
        // ER 0077: 泛型约束无效
        GenericConstraintInvalid = 77,           // "泛型约束 '[constraint]' 无效"
        // ER 0078: 泛型编译期常量参数不允许为左值或参与运行时操作
        GenericConstantParameterRuntimeUse = 78, // "泛型编译期常量参数 '[parameter]' 不允许为左值或参与运行时操作"
        // ER 0079: 泛型实参规范化冲突
        GenericNormalizationConflict = 79,       // "泛型实参规范化冲突：相同实参组合产生不同规范化结果"
        // ER 0080: 泛型成员在实例化中不存在
        GenericMemberNotInInstantiation = 80,    // "泛型成员 '[member]' 在实例化 '[instantiation]' 中不存在"
        // ER 0081: 特化必须出现在主泛型之后
        GenericSpecializationBeforePrimary = 81, // "特化 '[specialization]' 必须出现在主泛型 '[generic]' 之后"
        // ER 0082: 偏特化自由标识符在模式中多次出现但推导类型不一致
        GenericPatternDeductionMismatch = 82,    // "偏特化自由标识符 '[identifier]' 在模式中多次出现，但推导类型不一致"
        // ER 0083: 泛型块内成员名与泛型参数冲突
        GenericMemberNameConflictsParameter = 83, // "泛型块内成员名 '[member]' 与泛型参数 '[parameter]' 冲突"

        // ---- Gallt 0.3.txt §18/§20 重载与对象生命周期新增错误码 ----
        // ---- New overload/lifetime error codes (Gallt 0.3.txt §18/§20) ----
        // ER 0084: 重载二义性
        OverloadAmbiguous = 84,                  // "'[function]' 重载二义性"
        // ER 0085: 特殊成员函数重复定义
        SpecialMemberRedefined = 85,             // "特殊成员函数 '[function]' 重复定义"
        // ER 0086: 特殊成员函数参数类型不匹配
        SpecialMemberParamMismatch = 86,         // "特殊成员函数 '[function]' 参数类型不匹配：期望 '[expected]'，得到 '[actual]'"
        // ER 0087: 析构函数不允许有参数
        DestructorWithParameters = 87,           // "析构函数不允许有参数"
        // ER 0088: 构造函数不允许声明返回值类型
        ConstructorWithReturnType = 88,          // "构造函数不允许声明返回值类型"
        // ER 0089: 特殊成员函数不允许使用 return 返回表达式
        SpecialMemberReturnValue = 89,           // "特殊成员函数不允许使用 return 返回表达式"
        // ER 0090: 类型标记为 [nocopy]
        NoCopyViolation = 90,                    // "类型 '[type]' 标记为 [nocopy]，不允许拷贝构造或拷贝赋值"
        // ER 0091: 类型标记为 [nomove]
        NoMoveViolation = 91,                    // "类型 '[type]' 标记为 [nomove]，不允许移动构造或移动赋值"
        // ER 0092: 对非 construct 分配的对象调用 destruct
        DestructNonConstructed = 92,             // "对非 construct 分配的对象调用 destruct"
        // ER 0093: 对同一对象重复调用 destruct
        DestructTwice = 93,                      // "对同一对象重复调用 destruct"
        // ER 0094: Placement 构造目标内存不足或未对齐
        PlacementTargetInvalid = 94,             // "Placement 构造目标内存不足或未对齐"
        // ER 0095: 特殊成员函数不能通过点运算符或箭头运算符直接调用
        SpecialMemberDirectCall = 95,            // "特殊成员函数 '[function]' 不能通过点运算符或箭头运算符直接调用 (显式析构例外)"
        // ER 0096: 特殊成员函数重载决议二义性
        SpecialMemberAmbiguous = 96,             // "特殊成员函数 '[function]' 重载决议二义性"
        // ER 0097: 类型不是结构体，不能定义特殊成员函数
        SpecialMemberOnNonStruct = 97,           // "类型 '[type]' 不是结构体，不能定义特殊成员函数"
        // ER 0098: 特殊成员函数与普通函数同名冲突
        SpecialMemberNameConflict = 98,          // "特殊成员函数 '[function]' 与普通函数同名冲突"

        // ---- Gallt 0.4.txt §19/§21 命名空间与编译期代码生成新增错误码 ----
        // ---- New namespace / compile-time code-generation error codes
        //      (Gallt 0.4.txt §19/§21) ----
        // ER 0099: 定义了编译器关键字
        KeywordAsIdentifier = 99,                // "定义了编译器关键字 '[identifier]'"
        // ER 0100: 命名空间未定义
        NamespaceUndefined = 100,                // "命名空间 '[namespace]' 未定义"
        // ER 0101: 命名空间重复定义
        NamespaceRedefined = 101,                // "命名空间 '[namespace]' 重复定义"
        // ER 0102: 命名空间成员不存在
        NamespaceMemberNotFound = 102,           // "命名空间成员 '[member]' 在命名空间 '[namespace]' 中不存在"
        // ER 0103: access namespace 目标不存在或不可引入
        AccessNamespaceTargetInvalid = 103,      // "access namespace 目标 '[target]' 不存在或不可引入"
        // ER 0104: addition namespace 未找到可追加的命名空间
        AdditionNamespaceTargetMissing = 104,    // "addition namespace '[namespace]' 未找到可追加的命名空间"
        // ER 0105: '::' 左操作数必须为命名空间或泛型实例
        ScopeOperatorOperandInvalid = 105,       // "'::' 左操作数必须为命名空间或泛型实例，得到 '[type]'"
        // ER 0106: emit 语句只能出现在泛型块内
        EmitOutsideGenericBlock = 106,           // "emit 语句只能出现在泛型块内"
        // ER 0107: emit 字符串表达式不是编译期字符串常量
        EmitStringNotConstant = 107,             // "emit 字符串表达式不是编译期字符串常量：'[expr]'"
        // ER 0108: emit 块中包含无法在编译期展开的内容
        EmitBlockNotExpandable = 108,            // "emit 块中包含无法在编译期展开的内容"
        // ER 0109: 编译期属性不适用于参数
        CompileTimePropertyNotApplicable = 109,  // "编译期属性 '[property]' 不适用于参数 '[parameter]'"
        // ER 0110: 编译期属性参数数量不匹配
        CompileTimePropertyArgCountMismatch = 110, // "编译期属性 '[property]' 参数数量不匹配：需要 '[num]' 个，提供 '[num2]' 个"
        // ER 0111: 编译期属性参数必须为字符串字面量
        CompileTimePropertyArgNotString = 111,   // "编译期属性 '[property]' 的参数 '[index]' 必须为字符串字面量"
        // ER 0112: 编译期条件表达式必须为编译期布尔常量
        CompileTimeConditionNotBoolean = 112,    // "编译期条件表达式必须为编译期布尔常量，得到 '[expr]'"
        // ER 0113: access namespace 引入的名字与当前作用域已有声明冲突
        AccessNamespaceNameConflict = 113,       // "access namespace 引入 '[name]' 与当前作用域已有声明冲突"
        // ER 0114: addition namespace 合并后成员与已有声明冲突
        AdditionNamespaceMemberConflict = 114,   // "addition namespace 合并后成员 '[member]' 与已有声明冲突"
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

        // 返回标准文档中该错误码的消息模板（含 [占位符]）
        // Return the message template of the error code (with [placeholders])
        static std::string error_template(ErrorCode code);
        // 将模板中的 [占位符] 按出现顺序依次替换为 values
        // Substitute every [placeholder] in the template, in order, with values
        static std::string substitute_placeholders(std::string_view tmpl,
            const std::vector<std::string>& values);
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
        // 按标准文档模板报告错误并按顺序填充占位符
        // Report an error using the standard-document template, filling placeholders in order
        void report_error_template(SourceLocation loc, ErrorCode code,
            const std::vector<std::string>& values);
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
