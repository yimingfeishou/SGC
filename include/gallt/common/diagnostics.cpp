// common/diagnostics.cpp
// 诊断引擎实现 —— 收集、格式化并输出诊断
// Diagnostic engine implementation — collects, formats, and prints diagnostics

#include "diagnostics.hpp"

#include <cctype>
#include <cstdio>
#include <sstream>

namespace gallt {

    namespace {
        // Gallt 标准文档（0.4 预览版本）错误表的消息模板。
        // 错误编号与语义严格对应标准文档错误表；**消息文本统一为英文**
        // （占位符名称与文档一致，按出现顺序填充实际值）。
        // Message templates of the Gallt Standard Document (0.4 preview) error table.
        // Codes and semantics follow the standard error table exactly; the message text
        // is unified in **English** (placeholder names match the document and are filled
        // positionally with the actual values).
        struct ErrorTemplateEntry {
            ErrorCode code;
            const char* text;
        };

        const ErrorTemplateEntry kErrorTemplates[] = {
            { ErrorCode::UndefinedIdentifier, "undefined identifier '[identifier]'" },
            { ErrorCode::RedefinedIdentifier, "redefinition of identifier '[identifier]'" },
            { ErrorCode::AssignmentTypeMismatch, "assignment type mismatch: left-hand type '[type1]' is not compatible with right-hand type '[type2]'" },
            { ErrorCode::BinaryOperatorTypeMismatch, "binary operator '[operator]' operand type mismatch: '[type1]' and '[type2]' are not supported" },
            { ErrorCode::UnaryOperatorTypeMismatch, "unary operator '[operator]' operand type '[type]' is not supported" },
            { ErrorCode::FunctionReturnTypeMismatch, "function return type mismatch: declared '[type]', returned '[type2]'" },
            { ErrorCode::MissingReturnStatement, "non-void function '[function]' is missing a return statement" },
            { ErrorCode::VoidFunctionReturnsValue, "void function '[function]' cannot return a value" },
            { ErrorCode::UndefinedFunction, "function '[function]' is undefined" },
            { ErrorCode::RedefinedFunction, "redefinition of function '[function]'" },
            { ErrorCode::FunctionArgCountMismatch, "function call argument count mismatch: expected '[num]', provided '[num2]'" },
            { ErrorCode::FunctionArgTypeMismatch, "function call argument type mismatch: argument '[index]' expects '[type1]', got '[type2]'" },
            { ErrorCode::SubscriptNotInteger, "array index must be of integer type, got '[type]'" },
            { ErrorCode::SubscriptOutOfBounds, "array index out of bounds: index '[value]' is outside [0, '[size]')" },
            { ErrorCode::EmptyArrayInitializer, "empty array initializer; the array length cannot be inferred" },
            { ErrorCode::ArrayLengthMismatch, "array initializer length does not match the declaration: declared '[size]', initialized '[size2]'" },
            { ErrorCode::IdentifierNotInScope, "identifier '[identifier]' is not in the current scope" },
            { ErrorCode::MissingBraces, "control statement requires a '{' block to introduce a scope" },
            { ErrorCode::BreakOutsideLoop, "'break' may only appear inside a loop body" },
            { ErrorCode::ExpressionSyntaxError, "expression syntax error: unexpected token '[token]'" },
            { ErrorCode::MainSignatureError, "invalid main signature: must be 'int main()' or 'int main(int count, char *array[])'" },
            { ErrorCode::MainReturnTypeError, "main must return int, but returns '[type]'" },
            { ErrorCode::InvalidCharLiteral, "invalid character constant: empty character or invalid escape sequence" },
            { ErrorCode::UnclosedStringLiteral, "invalid string constant: unterminated string literal" },
            { ErrorCode::InvalidNumericSuffix, "invalid numeric literal suffix: only 'f' for float is allowed, got '[suffix]'" },
            { ErrorCode::ElseWithoutIf, "'else' without a matching 'if'" },
            { ErrorCode::ReturnOutsideFunction, "'return' outside of a function body" },
            { ErrorCode::StatementInGlobalScope, "executable statements are not allowed at global scope" },
            { ErrorCode::ConditionNotBoolean, "condition must be of boolean type, got '[type]'" },
            { ErrorCode::ArrayOperatorNotSupported, "this operator is not supported for array types" },
            { ErrorCode::ArraySizeNotConstant, "array size in a declaration must be a constant integer expression, got '[expr]'" },
            { ErrorCode::VoidParameter, "parameter list may not use type void" },
            { ErrorCode::InvalidTypeCast, "invalid type conversion: cannot convert '[type1]' to '[type2]'" },
            { ErrorCode::InvalidInputFile, "input file does not meet the requirements: encoding is '[coding]', line ending is '[linebreak]'" },
            { ErrorCode::PointerTypeMismatch, "incompatible pointer types: cannot assign '[type1]*' to '[type2]*'" },
            { ErrorCode::DerefNonPointer, "dereference requires a pointer operand, got '[type]'" },
            { ErrorCode::AddressOfNonLValue, "address-of requires an lvalue operand, got a non-lvalue expression" },
            { ErrorCode::PointerArithmeticInvalid, "invalid pointer arithmetic operands: expected a pointer and an integer, got '[type1]' and '[type2]'" },
            { ErrorCode::NullPointerDereference, "dereference of a null pointer" },
            { ErrorCode::FreeNonPointer, "free() operand must be of pointer type, got '[type]'" },
            { ErrorCode::HeapFirstArgNotType, "heap() first argument must be a type name, got '[expr]'" },
            { ErrorCode::HeapSecondArgNotInteger, "heap() second argument must be of integer type, got '[type]'" },
            { ErrorCode::FuncPtrTypeMismatch, "function pointer type mismatch: expected '[signature]', got '[signature2]'" },
            { ErrorCode::StructMemberNotFound, "struct member '[member]' does not exist in type '[struct]'" },
            { ErrorCode::StructInitLengthMismatch, "struct initializer length does not match the member count: expected '[num]', provided '[num2]'" },
            { ErrorCode::StructMemberTypeMismatch, "struct member type mismatch: member '[member]' expects '[type1]', initialized with '[type2]'" },
            { ErrorCode::StructMethodNotAllowed, "functions/methods are not allowed inside a struct; member '[member]' is a function definition" },
            { ErrorCode::ArrowOnNonStructPtr, "left operand of '->' must be a struct pointer, got '[type]'" },
            { ErrorCode::DotOnNonStruct, "left operand of '.' must be a struct type, got '[type]'" },
            { ErrorCode::StructSelfRefNonPtr, "a struct may not contain itself directly; self-reference requires a pointer" },
            { ErrorCode::LibraryNotFound, "guide / clib() cannot find the library" },
            { ErrorCode::FileArgCountMismatch, "file function '[function]' argument count mismatch: expected '[num]', provided '[num2]'" },
            { ErrorCode::FileArgTypeMismatch, "file function '[function]' argument '[index]' type mismatch: expected '[type1]', got '[type2]'" },
            { ErrorCode::FileOpenModeInvalid, "invalid file open mode: '[mode]'" },
            { ErrorCode::FileHandleRequired, "file operation requires a file* value, got '[type]'" },
            { ErrorCode::FileSeekOriginInvalid, "fileseek() origin must be 0, 1 or 2, got '[value]'" },
            { ErrorCode::FileBufferNotPointer, "file read/write buffer must be of pointer type, got '[type]'" },
            { ErrorCode::FileSizeNotInt, "file read/write size must be of type int, got '[type]'" },
            { ErrorCode::FilePathOrModeNotString, "file path or mode must be of type string, got '[type]'" },
            { ErrorCode::GenericStatementNotAllowed, "executable statements are not allowed at the top of a generic block, outside all member definitions" },
            { ErrorCode::GenericUndefined, "generic '[generic]' is undefined" },
            { ErrorCode::GenericPrimaryRedefined, "generic '[generic]' is redefined as a primary generic" },
            { ErrorCode::GenericParameterRedefined, "generic parameter '[parameter]' is redeclared" },
            { ErrorCode::GenericArgCountMismatch, "generic instantiation '[generic]<[arguments]>' argument count mismatch: expected '[num]', provided '[num2]'" },
            { ErrorCode::GenericConstraintViolated, "generic instantiation '[generic]<[arguments]>' argument '[index]' does not satisfy constraint '[constraint]', got '[type]'" },
            { ErrorCode::GenericNoMatch, "generic instantiation '[generic]<[arguments]>' matches no generic or specialization" },
            { ErrorCode::GenericAmbiguousSpecialization, "generic instantiation '[generic]<[arguments]>' has multiple incomparable most-specialized candidates (ambiguous)" },
            { ErrorCode::GenericConstantPatternNotConstant, "generic constant pattern cannot be evaluated at compile time: '[expr]'" },
            { ErrorCode::GenericNonTypeArgNotConstant, "generic non-type argument must be a compile-time constant expression, got '[expr]'" },
            { ErrorCode::GenericNonTypeArgTypeMismatch, "generic non-type argument type cannot be converted to constant parameter type '[type]', got '[type2]'" },
            { ErrorCode::GenericNonTypeArgOutOfRange, "generic non-type argument value is out of range for constant parameter type '[type]'" },
            { ErrorCode::GenericFreeIdentifierConflict, "partial specialization free identifier '[identifier]' conflicts with another free identifier in the same block" },
            { ErrorCode::GenericShortNameAmbiguous, "generic member short name '[identifier]' is ambiguous; use a qualified name" },
            { ErrorCode::GenericShortNameConflict, "generic member short name '[identifier]' conflicts with an existing declaration in the current scope" },
            { ErrorCode::GenericSpecializationMissingMember, "specialization '[specialization]' is missing member '[member]' that must exist after instantiation" },
            { ErrorCode::GenericSpecializationWithoutPrimary, "specialization of generic '[generic]' has no primary generic declaration" },
            { ErrorCode::GenericConstraintInvalid, "invalid generic constraint '[constraint]'" },
            { ErrorCode::GenericConstantParameterRuntimeUse, "generic compile-time constant parameter '[parameter]' may not be an lvalue or take part in runtime operations" },
            { ErrorCode::GenericNormalizationConflict, "generic argument normalization conflict: the same argument list produced different normalized results" },
            { ErrorCode::GenericMemberNotInInstantiation, "generic member '[member]' does not exist in instantiation '[instantiation]'" },
            { ErrorCode::GenericSpecializationBeforePrimary, "specialization '[specialization]' must appear after the primary generic '[generic]'" },
            { ErrorCode::GenericPatternDeductionMismatch, "partial specialization free identifier '[identifier]' appears multiple times in the pattern but deduced different types" },
            { ErrorCode::GenericMemberNameConflictsParameter, "generic block member name '[member]' conflicts with generic parameter '[parameter]'" },
            { ErrorCode::OverloadAmbiguous, "'[function]' has an ambiguous overload" },
            { ErrorCode::SpecialMemberRedefined, "special member function '[function]' is redefined" },
            { ErrorCode::SpecialMemberParamMismatch, "special member function '[function]' parameter type mismatch: expected '[expected]', got '[actual]'" },
            { ErrorCode::DestructorWithParameters, "a destructor may not have parameters" },
            { ErrorCode::ConstructorWithReturnType, "a constructor may not declare a return type" },
            { ErrorCode::SpecialMemberReturnValue, "a special member function may not return a value with 'return'" },
            { ErrorCode::NoCopyViolation, "type '[type]' is marked [nocopy]; copy construction and copy assignment are not allowed" },
            { ErrorCode::NoMoveViolation, "type '[type]' is marked [nomove]; move construction and move assignment are not allowed" },
            { ErrorCode::DestructNonConstructed, "destruct called on an object that was not allocated by construct" },
            { ErrorCode::DestructTwice, "destruct called twice on the same object" },
            { ErrorCode::PlacementTargetInvalid, "placement construction target memory is too small or misaligned" },
            { ErrorCode::SpecialMemberDirectCall, "special member function '[function]' cannot be called directly through '.' or '->' (explicit destructor excepted)" },
            { ErrorCode::SpecialMemberAmbiguous, "special member function '[function]' overload resolution is ambiguous" },
            { ErrorCode::SpecialMemberOnNonStruct, "type '[type]' is not a struct; special member functions cannot be defined" },
            { ErrorCode::SpecialMemberNameConflict, "special member function '[function]' conflicts with an ordinary function of the same name" },
            // Gallt 0.4.txt §19/§21 命名空间与编译期代码生成
            // Gallt 0.4.txt §19/§21 namespaces and compile-time code generation
            { ErrorCode::KeywordAsIdentifier, "compiler keyword '[identifier]' was defined as an identifier" },
            { ErrorCode::NamespaceUndefined, "namespace '[namespace]' is undefined" },
            { ErrorCode::NamespaceRedefined, "namespace '[namespace]' is redefined" },
            { ErrorCode::NamespaceMemberNotFound, "namespace member '[member]' does not exist in namespace '[namespace]'" },
            { ErrorCode::AccessNamespaceTargetInvalid, "access namespace target '[target]' does not exist or cannot be imported" },
            { ErrorCode::AdditionNamespaceTargetMissing, "addition namespace '[namespace]' has no namespace to append to" },
            { ErrorCode::ScopeOperatorOperandInvalid, "left operand of '::' must be a namespace or generic instance, got '[type]'" },
            { ErrorCode::EmitOutsideGenericBlock, "emit statements may only appear inside a generic block" },
            { ErrorCode::EmitStringNotConstant, "emit string expression is not a compile-time string constant: '[expr]'" },
            { ErrorCode::EmitBlockNotExpandable, "emit block contains content that cannot be expanded at compile time" },
            { ErrorCode::CompileTimePropertyNotApplicable, "compile-time property '[property]' does not apply to parameter '[parameter]'" },
            { ErrorCode::CompileTimePropertyArgCountMismatch, "compile-time property '[property]' argument count mismatch: expected '[num]', provided '[num2]'" },
            { ErrorCode::CompileTimePropertyArgNotString, "argument '[index]' of compile-time property '[property]' must be a string literal" },
            { ErrorCode::CompileTimeConditionNotBoolean, "compile-time condition must be a compile-time boolean constant, got '[expr]'" },
            { ErrorCode::AccessNamespaceNameConflict, "name '[name]' imported by access namespace conflicts with an existing declaration in the current scope" },
            { ErrorCode::AdditionNamespaceMemberConflict, "member '[member]' merged by addition namespace conflicts with an existing declaration" },
        };
    } // anonymous namespace

    std::string Diagnostic::error_template(ErrorCode code) {
        for (const ErrorTemplateEntry& entry : kErrorTemplates) {
            if (entry.code == code) {
                return std::string(entry.text);
            }
        }
        return std::string();
    }

    std::string Diagnostic::substitute_placeholders(std::string_view tmpl,
        const std::vector<std::string>& values) {
        // 依次把 [占位符] 替换为 values；未提供对应值时原样保留
        // Replace each [placeholder] with the matching value; keep it verbatim when absent
        std::string out;
        out.reserve(tmpl.size() + 32);
        std::size_t index = 0;
        std::size_t i = 0;
        while (i < tmpl.size()) {
            if (tmpl[i] == '[') {
                std::size_t close = tmpl.find(']', i);
                // 仅当 [..] 内是标识符形态（占位符）时才替换，避免误伤
                // 形如 "[0, '[size]')" 的区间写法
                // Only substitute identifier-shaped placeholders so ranges such as
                // "[0, '[size]')" stay intact
                bool placeholder_like = close != std::string_view::npos && close > i + 1;
                for (std::size_t k = i + 1; placeholder_like && k < close; ++k) {
                    char c = tmpl[k];
                    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
                        placeholder_like = false;
                    }
                }
                if (placeholder_like && index < values.size()) {
                    out.append(values[index]);
                    ++index;
                    i = close + 1;
                    continue;
                }
            }
            out.push_back(tmpl[i]);
            ++i;
        }
        return out;
    }

    // ============================================================================
    // 错误码格式化为 "ER XXXX" / "RTER XXXX"
    // Format an error code as "ER XXXX" / "RTER XXXX"
    // ============================================================================

    std::string Diagnostic::error_code_string(ErrorCode code) {
        std::string s = std::to_string(static_cast<std::uint16_t>(code));
        while (s.size() < 4u) {
            s.insert(s.begin(), '0');
        }
        return "ER " + s;
    }

    std::string Diagnostic::runtime_error_code_string(RuntimeErrorCode code) {
        std::string s = std::to_string(static_cast<std::uint16_t>(code));
        while (s.size() < 4u) {
            s.insert(s.begin(), '0');
        }
        return "RTER " + s;
    }

    // ============================================================================
    // 诊断引擎实现
    // Diagnostic engine implementation
    // ============================================================================

    std::string DiagnosticEngine::severity_prefix(DiagnosticSeverity sev) {
        // 按严重级别返回短标签；Note 不携带错误码
        // Return the short label by severity; notes carry no error code
        switch (sev) {
        case DiagnosticSeverity::Error:   return "error";
        case DiagnosticSeverity::Warning: return "warning";
        case DiagnosticSeverity::Note:    return "note";
        }
        return "note";
    }

    std::string DiagnosticEngine::format_message(ErrorCode code, std::string_view user_msg) {
        // 当前诊断消息已经由调用者完成格式化，这里原样保留
        // Callers already format the message; keep it verbatim
        (void)code;
        return std::string(user_msg);
    }

    void DiagnosticEngine::report(DiagnosticSeverity sev, SourceLocation loc, ErrorCode code, std::string_view msg) {
        Diagnostic d;
        d.severity = sev;
        d.location = loc;
        d.code = code;
        d.message = format_message(code, msg);
        diagnostics_.push_back(std::move(d));
        if (sev == DiagnosticSeverity::Error) {
            ++error_count_;
        } else if (sev == DiagnosticSeverity::Warning) {
            ++warning_count_;
        }
    }

    void DiagnosticEngine::report(DiagnosticSeverity sev, SourceLocation loc, std::string_view msg) {
        Diagnostic d;
        d.severity = sev;
        d.location = loc;
        d.code = ErrorCode::ExpressionSyntaxError; // note 不使用错误码 / notes carry no error code
        d.message = std::string(msg);
        diagnostics_.push_back(std::move(d));
        if (sev == DiagnosticSeverity::Error) {
            ++error_count_;
        } else if (sev == DiagnosticSeverity::Warning) {
            ++warning_count_;
        }
    }

    void DiagnosticEngine::report_error(SourceLocation loc, ErrorCode code, std::string_view message) {
        report(DiagnosticSeverity::Error, loc, code, message);
    }

    void DiagnosticEngine::report_error_template(SourceLocation loc, ErrorCode code,
        const std::vector<std::string>& values) {
        // 采用标准文档模板并填充占位符，保证错误信息与文档一致
        // Use the standard-document template and fill placeholders so messages match the spec
        std::string message = Diagnostic::substitute_placeholders(
            Diagnostic::error_template(code), values);
        report(DiagnosticSeverity::Error, loc, code, message);
    }

    void DiagnosticEngine::report_warning(SourceLocation loc, ErrorCode code, std::string_view message) {
        report(DiagnosticSeverity::Warning, loc, code, message);
    }

    void DiagnosticEngine::report_note(SourceLocation loc, std::string_view message) {
        report(DiagnosticSeverity::Note, loc, message);
    }

    void DiagnosticEngine::report_runtime_error(RuntimeErrorCode code, std::string_view message) {
        // 运行时错误直接打印到标准错误；编译器中仅为将来嵌入运行库保留接口
        // Runtime errors go directly to stderr; retained for future runtime embedding
        std::fprintf(stderr, "%s: %.*s\n",
            Diagnostic::runtime_error_code_string(code).c_str(),
            static_cast<int>(message.size()), message.data());
    }

    void DiagnosticEngine::clear() {
        diagnostics_.clear();
        error_count_ = 0;
        warning_count_ = 0;
    }

    void DiagnosticEngine::print_all(std::ostream& os) const {
        for (const Diagnostic& d : diagnostics_) {
            // filename:line:col: severity: ER XXXX: message
            if (d.location.is_valid() && !d.location.filename.empty()) {
                os << d.location.filename << ':'
                   << d.location.line << ':'
                   << d.location.column << ": ";
            } else if (d.location.is_valid()) {
                os << "<input>:" << d.location.line << ':' << d.location.column << ": ";
            }
            os << severity_prefix(d.severity) << ": ";
            if (d.severity != DiagnosticSeverity::Note) {
                os << Diagnostic::error_code_string(d.code) << ": ";
            }
            os << d.message << '\n';
        }
    }

    void DiagnosticEngine::print_all(std::ostream& os, DiagnosticSeverity min_severity) const {
        for (const Diagnostic& d : diagnostics_) {
            auto rank = [](DiagnosticSeverity s) -> int {
                // Error(0) < Warning(1) < Note(2)
                switch (s) {
                case DiagnosticSeverity::Error: return 0;
                case DiagnosticSeverity::Warning: return 1;
                case DiagnosticSeverity::Note: return 2;
                }
                return 2;
            };
            auto min_rank = [&]() -> int {
                switch (min_severity) {
                case DiagnosticSeverity::Error: return 0;
                case DiagnosticSeverity::Warning: return 1;
                case DiagnosticSeverity::Note: return 2;
                }
                return 0;
            };
            if (rank(d.severity) >= min_rank()) {
                if (d.location.is_valid() && !d.location.filename.empty()) {
                    os << d.location.filename << ':'
                       << d.location.line << ':'
                       << d.location.column << ": ";
                } else if (d.location.is_valid()) {
                    os << "<input>:" << d.location.line << ':' << d.location.column << ": ";
                }
                os << severity_prefix(d.severity) << ": ";
                if (d.severity != DiagnosticSeverity::Note) {
                    os << Diagnostic::error_code_string(d.code) << ": ";
                }
                os << d.message << '\n';
            }
        }
    }

} // namespace gallt
