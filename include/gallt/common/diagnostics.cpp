#include "diagnostics.hpp"
#include <cctype>
#include <cstdio>
#include <sstream>

namespace gallt {

    namespace {
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
            { ErrorCode::MainSignatureError, "invalid main signature: must be 'int main()' or 'int main(int count, char* array[])'" },
            { ErrorCode::MainReturnTypeError, "main must return int, but returns '[type]'" },
            { ErrorCode::InvalidCharLiteral, "invalid character constant: empty character or invalid escape sequence" },
            { ErrorCode::UnclosedStringLiteral, "invalid string constant: unterminated string literal" },
            { ErrorCode::InvalidNumericSuffix, "invalid numeric literal suffix: only 'f', 'l', 'u' and 'lu' are allowed, got '[suffix]'" },
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
            { ErrorCode::ConstModification, "cannot modify constant '[identifier]'" },
            { ErrorCode::ConstCannotStoreVariable, "constant '[identifier]' cannot store a variable" },
            { ErrorCode::ConditionUsedAsValue, "condition '[condition]' cannot be used as a constant or variable" },
            { ErrorCode::OperatorCannotBeOverloaded, "operator '[op]' cannot be overloaded" },
            { ErrorCode::OperatorOverloadOperandCountMismatch, "operator overload '[op]' operand count mismatch: expected '[num]', provided '[num2]'" },
            { ErrorCode::OperatorOverloadRequiresCustomType, "operator overload '[op]' requires at least one operand of a custom type" },
            { ErrorCode::ModifyingOperatorFirstParameterNotPointer, "first parameter of modifying operator '[op]' must be of pointer type" },
            { ErrorCode::SubscriptOperatorMustReturnPointer, "operator[] must return a pointer type" },
            { ErrorCode::ArrowOperatorMustReturnPointer, "operator-> must return a pointer type" },
            { ErrorCode::OperatorOverloadDefaultArgumentNotAllowed, "operator overload '[op]' may not declare default arguments" },
            { ErrorCode::OperatorOverloadInsideStruct, "operator overload '[op]' may not be declared inside a struct" },
            { ErrorCode::ConversionOperatorTargetInvalid, "invalid conversion operator target type '[type]'" },
            { ErrorCode::OperatorOverloadAmbiguous, "operator overload '[op]' is ambiguous with an existing overload" },
            { ErrorCode::OperatorOverloadParameterMismatch, "operator overload '[op]' parameter type mismatch" },
            { ErrorCode::OperatorOverloadRedefined, "operator overload '[op]' is redefined" },
            { ErrorCode::ExprParameterUndefined, "expression parameter '[name]' is undefined" },
            { ErrorCode::ExprParameterSignatureMismatch, "expression parameter '[name]' signature mismatch: expected '[sig1]', got '[sig2]'" },
            { ErrorCode::ExprParameterReturnTypeMismatch, "expression parameter '[name]' return type mismatch: expected '[type1]', got '[type2]'" },
            { ErrorCode::ExprParameterCannotBeUsedAsValue, "expression parameter '[name]' cannot be addressed, assigned or used as a value" },
            { ErrorCode::ExprParameterCallArgCountMismatch, "expression parameter '[name]' call argument count mismatch: expected '[num]', provided '[num2]'" },
            { ErrorCode::ExprParameterCallArgTypeMismatch, "expression parameter '[name]' call argument '[index]' type mismatch: expected '[type1]', got '[type2]'" },
            { ErrorCode::ExprParameterFreeIdentifierUndefined, "expression parameter definition contains free identifier '[id]' that is undefined at the instantiation point" },
            { ErrorCode::ExprParameterDisallowedSyntax, "expression parameter definition contains a disallowed syntactic construct" },
            { ErrorCode::ExprParameterNormalizationConflict, "expression parameter instantiation normalization conflict" },
            { ErrorCode::ExprParameterInSpecializationPattern, "expression parameter '[name]' cannot take part in specialization matching or be used as a specialization pattern" },
            { ErrorCode::ExprParameterRecursionLimitExceeded, "expression parameter recursive expansion exceeded the compiler limit" },
            { ErrorCode::ExprParameterExpansionTypeError, "type checking failed after expression parameter expansion" },
            { ErrorCode::ExprParameterNameConflict, "expression parameter '[name]' conflicts with a generic parameter or member name" },
            { ErrorCode::ExprParameterBlockMissingReturn, "the last statement of an expression parameter block must be 'return'" },
            { ErrorCode::ExprParameterBlockReturnTypeMismatch, "expression parameter block return type mismatch: expected '[type1]', got '[type2]'" },
            { ErrorCode::ExprParameterBlockLocalConflictsParameter, "local variable '[id]' in an expression parameter block conflicts with parameter '[param]'" },
            { ErrorCode::ExprParameterShorthandRequiresParameterNames, "the shorthand expression parameter form requires the declared parameter list to be empty, or every parameter to have a name" },
            { ErrorCode::ExprParameterBlockDisallowedDeclaration, "function, struct, generic, namespace or special member definitions are not allowed inside an expression parameter block" },
            { ErrorCode::ExprParameterBlockDisallowedConstruct, "emit, guide, clib, extern and conditional compilation are not allowed inside an expression parameter block" },
        };
    } 

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
        std::string out;
        out.reserve(tmpl.size() + 32);
        std::size_t index = 0;
        std::size_t i = 0;
        while (i < tmpl.size()) {
            if (tmpl[i] == '[') {
                std::size_t close = tmpl.find(']', i);
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


    std::string DiagnosticEngine::severity_prefix(DiagnosticSeverity sev) {
        switch (sev) {
        case DiagnosticSeverity::Error:   return "error";
        case DiagnosticSeverity::Warning: return "warning";
        case DiagnosticSeverity::Note:    return "note";
        }
        return "note";
    }

    std::string DiagnosticEngine::format_message(ErrorCode code, std::string_view user_msg) {
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
        d.code = ErrorCode::ExpressionSyntaxError; 
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

} 
