
#ifndef GALLT_COMMON_DIAGNOSTICS_HPP
#define GALLT_COMMON_DIAGNOSTICS_HPP
#include "source_location.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <ostream>
#include <optional>
#include <cstdint>
#include <map>
#include <tuple>

namespace gallt {

    enum class DiagnosticSeverity {
        Error,
        Warning,
        Note,
    };

    enum class ErrorCode : std::uint16_t {
        UndefinedIdentifier = 1,
        RedefinedIdentifier = 2,
        AssignmentTypeMismatch = 3,
        BinaryOperatorTypeMismatch = 4,
        UnaryOperatorTypeMismatch = 5,
        FunctionReturnTypeMismatch = 6,
        MissingReturnStatement = 7,
        VoidFunctionReturnsValue = 8,
        UndefinedFunction = 9,
        RedefinedFunction = 10,
        FunctionArgCountMismatch = 11,
        FunctionArgTypeMismatch = 12,
        SubscriptNotInteger = 13,
        EmptyArrayInitializer = 14,
        ArrayLengthMismatch = 15,
        IdentifierNotInScope = 16,
        MissingBraces = 17,
        BreakOutsideLoop = 18,
        ExpressionSyntaxError = 19,
        MainSignatureError = 20,
        MainReturnTypeError = 21,
        InvalidCharLiteral = 22,
        UnclosedStringLiteral = 23,
        InvalidNumericSuffix = 24,
        ElseWithoutIf = 25,
        ReturnOutsideFunction = 26,
        StatementInGlobalScope = 27,
        ConditionNotBoolean = 28,
        ArrayOperatorNotSupported = 29,
        ArraySizeNotConstant = 30,
        VoidParameter = 31,
        InvalidTypeCast = 32,
        InvalidInputFile = 33,
        PointerTypeMismatch = 34,
        DerefNonPointer = 35,
        AddressOfNonLValue = 36,
        PointerArithmeticInvalid = 37,
        FreeNonPointer = 38,
        HeapFirstArgNotType = 39,
        HeapSecondArgNotInteger = 40,
        FuncPtrTypeMismatch = 41,
        StructMemberNotFound = 42,
        StructInitLengthMismatch = 43,
        StructMemberTypeMismatch = 44,
        StructMethodNotAllowed = 45,
        ArrowOnNonStructPtr = 46,
        DotOnNonStruct = 47,
        StructSelfRefNonPtr = 48,
        LibraryNotFound = 49,
        FileArgCountMismatch = 50,
        FileArgTypeMismatch = 51,
        FileHandleRequired = 52,
        FileBufferNotPointer = 53,
        FileSizeNotInt = 54,
        FilePathOrModeNotString = 55,
        GenericStatementNotAllowed = 56,
        GenericUndefined = 57,
        GenericPrimaryRedefined = 58,
        GenericParameterRedefined = 59,
        GenericArgCountMismatch = 60,
        GenericConstraintViolated = 61,
        GenericNoMatch = 62,
        GenericAmbiguousSpecialization = 63,
        GenericConstantPatternNotConstant = 64,
        GenericNonTypeArgNotConstant = 65,
        GenericNonTypeArgTypeMismatch = 66,
        GenericNonTypeArgOutOfRange = 67,
        GenericFreeIdentifierConflict = 68,
        GenericShortNameAmbiguous = 69,
        GenericShortNameConflict = 70,
        GenericSpecializationMissingMember = 71,
        GenericSpecializationWithoutPrimary = 72,
        GenericConstraintInvalid = 73,
        GenericConstantParameterRuntimeUse = 74,
        GenericNormalizationConflict = 75,
        GenericMemberNotInInstantiation = 76,
        GenericSpecializationBeforePrimary = 77,
        GenericPatternDeductionMismatch = 78,
        GenericMemberNameConflictsParameter = 79,
        OverloadAmbiguous = 80,
        SpecialMemberRedefined = 81,
        SpecialMemberParamMismatch = 82,
        DestructorWithParameters = 83,
        ConstructorWithReturnType = 84,
        SpecialMemberReturnValue = 85,
        NoCopyViolation = 86,
        NoMoveViolation = 87,
        SpecialMemberDirectCall = 88,
        SpecialMemberAmbiguous = 89,
        SpecialMemberOnNonStruct = 90,
        SpecialMemberNameConflict = 91,
        KeywordAsIdentifier = 92,
        NamespaceUndefined = 93,
        NamespaceRedefined = 94,
        NamespaceMemberNotFound = 95,
        AccessNamespaceTargetInvalid = 96,
        AdditionNamespaceTargetMissing = 97,
        ScopeOperatorOperandInvalid = 98,
        EmitOutsideGenericBlock = 99,
        EmitStringNotConstant = 100,
        EmitBlockNotExpandable = 101,
        CompileTimePropertyNotApplicable = 102,
        CompileTimePropertyArgCountMismatch = 103,
        CompileTimePropertyArgNotString = 104,
        CompileTimeConditionNotBoolean = 105,
        AccessNamespaceNameConflict = 106,
        AdditionNamespaceMemberConflict = 107,
        ConstModification = 108,
        ConstCannotStoreVariable = 109,
        ConditionUsedAsValue = 110,
        OperatorCannotBeOverloaded = 111,
        OperatorOverloadOperandCountMismatch = 112,
        OperatorOverloadRequiresCustomType = 113,
        ModifyingOperatorFirstParameterNotPointer = 114,
        SubscriptOperatorMustReturnPointer = 115,
        ArrowOperatorMustReturnPointer = 116,
        OperatorOverloadDefaultArgumentNotAllowed = 117,
        OperatorOverloadInsideStruct = 118,
        ConversionOperatorTargetInvalid = 119,
        OperatorOverloadAmbiguous = 120,
        OperatorOverloadParameterMismatch = 121,
        OperatorOverloadRedefined = 122,
        ExportNotAllowedInContext = 123,
        ExportRequiresFunctionDefinition = 124,
        ExportMainNotAllowed = 125,
        ExportFunctionNameInvalid = 126,
        ExportFunctionCannotBeOverloaded = 127,
        ExportFunctionDeclarationConflict = 128,
        VariadicParameterNotLast = 129,
        VariadicElementTypeIsVoid = 130,
        VariadicParameterDefaultArgument = 131,
        ParameterPackUsedAsValue = 132,
        VariadicArgumentTypeMismatch = 133,
        PackExpansionTargetNotPack = 134,
        PackExpansionNotAllowedHere = 135,
        PackSubscriptNotInteger = 136,
        ParameterPackPropertyNotApplicable = 137,
        SpecialMemberVariadicNotAllowed = 138,
        OperatorOverloadVariadicNotAllowed = 139,
        ParameterPackInConstantExpression = 140,
        VariadicFunctionPointerSignatureMismatch = 141,
        VariadicDefaultArgumentAmbiguous = 142,
        CompileTimeConditionOutsideGenericBlock = 143,
        ConstexprArgumentNotConstant = 144,
        ConstexprDisallowedOperation = 145,
        ConstexprNonCompilableType = 146,
        ConstexprStructMemberNotCompilable = 147,
        ConstexprSideEffectStatement = 148,
        InstantiationDepthTooDeep = 149,
        FallthroughOutsideSwitch = 150,
        ConstexprRecursionTooDeep = 151,
        ConstantInitializerSideEffect = 152,
    };

    struct Diagnostic {
        DiagnosticSeverity severity;
        SourceLocation location;
        ErrorCode code;
        std::string message;

        static std::string error_code_string(ErrorCode code);

        static std::string error_template(ErrorCode code);
        static std::string substitute_placeholders(std::string_view tmpl,
            const std::vector<std::string>& values);
    };

    class DiagnosticEngine {
    public:
        static constexpr std::size_t kDefaultErrorDisplayLimit = 100;

        DiagnosticEngine() = default;

        std::map<std::tuple<std::string, std::size_t, std::size_t>, int>
            diagnosed_stages;

        void report_error(SourceLocation loc, ErrorCode code, std::string_view message);
        void report_error_template(SourceLocation loc, ErrorCode code,
            const std::vector<std::string>& values);
        void report_warning(SourceLocation loc, ErrorCode code, std::string_view message);
        void report_note(SourceLocation loc, std::string_view message);

        bool has_errors() const noexcept { return error_count_ > 0; }

        std::size_t error_count() const noexcept { return error_count_; }

        std::size_t warning_count() const noexcept { return warning_count_; }

        void clear();

        void print_all(std::ostream& os) const;

        void print_all(std::ostream& os, DiagnosticSeverity min_severity) const;

        void print_all(std::ostream& os, DiagnosticSeverity min_severity,
            std::size_t error_display_limit, bool prompt_when_truncated) const;

    private:
        std::vector<Diagnostic> diagnostics_;
        std::size_t error_count_ = 0;
        std::size_t warning_count_ = 0;

        void report(DiagnosticSeverity sev, SourceLocation loc, ErrorCode code, std::string_view msg);
        void report(DiagnosticSeverity sev, SourceLocation loc, std::string_view msg);

        static std::string format_message(ErrorCode code, std::string_view user_msg);
        static std::string severity_prefix(DiagnosticSeverity sev);
        static std::string fallback_message(DiagnosticSeverity sev, ErrorCode code);
        static void print_diagnostic(std::ostream& os, const Diagnostic& diagnostic);
    };

}

#endif
