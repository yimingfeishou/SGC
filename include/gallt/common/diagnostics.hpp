
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
        SubscriptOutOfBounds = 14,
        EmptyArrayInitializer = 15,
        ArrayLengthMismatch = 16,
        IdentifierNotInScope = 17,
        MissingBraces = 18,
        BreakOutsideLoop = 19,
        ExpressionSyntaxError = 20,
        MainSignatureError = 21,
        MainReturnTypeError = 22,
        InvalidCharLiteral = 23,
        UnclosedStringLiteral = 24,
        InvalidNumericSuffix = 25,
        ElseWithoutIf = 26,
        ReturnOutsideFunction = 27,
        StatementInGlobalScope = 28,
        ConditionNotBoolean = 29,
        ArrayOperatorNotSupported = 30,
        ArraySizeNotConstant = 31,
        VoidParameter = 32,
        InvalidTypeCast = 33,
        InvalidInputFile = 34,
        PointerTypeMismatch = 35,
        DerefNonPointer = 36,
        AddressOfNonLValue = 37,
        PointerArithmeticInvalid = 38,
        NullPointerDereference = 39,
        FreeNonPointer = 40,
        HeapFirstArgNotType = 41,
        HeapSecondArgNotInteger = 42,
        FuncPtrTypeMismatch = 43,
        StructMemberNotFound = 44,
        StructInitLengthMismatch = 45,
        StructMemberTypeMismatch = 46,
        StructMethodNotAllowed = 47,
        ArrowOnNonStructPtr = 48,
        DotOnNonStruct = 49,
        StructSelfRefNonPtr = 50,
        LibraryNotFound = 51,
        FileArgCountMismatch = 52,
        FileArgTypeMismatch = 53,
        FileOpenModeInvalid = 54,
        FileHandleRequired = 55,
        FileSeekOriginInvalid = 56,
        FileBufferNotPointer = 57,
        FileSizeNotInt = 58,
        FilePathOrModeNotString = 59,
        GenericStatementNotAllowed = 60,
        GenericUndefined = 61,
        GenericPrimaryRedefined = 62,
        GenericParameterRedefined = 63,
        GenericArgCountMismatch = 64,
        GenericConstraintViolated = 65,
        GenericNoMatch = 66,
        GenericAmbiguousSpecialization = 67,
        GenericConstantPatternNotConstant = 68,
        GenericNonTypeArgNotConstant = 69,
        GenericNonTypeArgTypeMismatch = 70,
        GenericNonTypeArgOutOfRange = 71,
        GenericFreeIdentifierConflict = 72,
        GenericShortNameAmbiguous = 73,
        GenericShortNameConflict = 74,
        GenericSpecializationMissingMember = 75,
        GenericSpecializationWithoutPrimary = 76,
        GenericConstraintInvalid = 77,
        GenericConstantParameterRuntimeUse = 78,
        GenericNormalizationConflict = 79,
        GenericMemberNotInInstantiation = 80,
        GenericSpecializationBeforePrimary = 81,
        GenericPatternDeductionMismatch = 82,
        GenericMemberNameConflictsParameter = 83,
        OverloadAmbiguous = 84,
        SpecialMemberRedefined = 85,
        SpecialMemberParamMismatch = 86,
        DestructorWithParameters = 87,
        ConstructorWithReturnType = 88,
        SpecialMemberReturnValue = 89,
        NoCopyViolation = 90,
        NoMoveViolation = 91,
        DestructNonConstructed = 92,
        DestructTwice = 93,
        PlacementTargetInvalid = 94,
        SpecialMemberDirectCall = 95,
        SpecialMemberAmbiguous = 96,
        SpecialMemberOnNonStruct = 97,
        SpecialMemberNameConflict = 98,
        KeywordAsIdentifier = 99,
        NamespaceUndefined = 100,
        NamespaceRedefined = 101,
        NamespaceMemberNotFound = 102,
        AccessNamespaceTargetInvalid = 103,
        AdditionNamespaceTargetMissing = 104,
        ScopeOperatorOperandInvalid = 105,
        EmitOutsideGenericBlock = 106,
        EmitStringNotConstant = 107,
        EmitBlockNotExpandable = 108,
        CompileTimePropertyNotApplicable = 109,
        CompileTimePropertyArgCountMismatch = 110,
        CompileTimePropertyArgNotString = 111,
        CompileTimeConditionNotBoolean = 112,
        AccessNamespaceNameConflict = 113,
        AdditionNamespaceMemberConflict = 114,
        ConstModification = 115,
        ConstCannotStoreVariable = 116,
        ConditionUsedAsValue = 117,
        OperatorCannotBeOverloaded = 118,
        OperatorOverloadOperandCountMismatch = 119,
        OperatorOverloadRequiresCustomType = 120,
        ModifyingOperatorFirstParameterNotPointer = 121,
        SubscriptOperatorMustReturnPointer = 122,
        ArrowOperatorMustReturnPointer = 123,
        OperatorOverloadDefaultArgumentNotAllowed = 124,
        OperatorOverloadInsideStruct = 125,
        ConversionOperatorTargetInvalid = 126,
        OperatorOverloadAmbiguous = 127,
        OperatorOverloadParameterMismatch = 128,
        OperatorOverloadRedefined = 129,
        ExprParameterUndefined = 130,
        ExprParameterSignatureMismatch = 131,
        ExprParameterReturnTypeMismatch = 132,
        ExprParameterCannotBeUsedAsValue = 133,
        ExprParameterCallArgCountMismatch = 134,
        ExprParameterCallArgTypeMismatch = 135,
        ExprParameterFreeIdentifierUndefined = 136,
        ExprParameterDisallowedSyntax = 137,
        ExprParameterNormalizationConflict = 138,
        ExprParameterInSpecializationPattern = 139,
        ExprParameterRecursionLimitExceeded = 140,
        ExprParameterExpansionTypeError = 141,
        ExprParameterNameConflict = 142,
        ExprParameterBlockMissingReturn = 143,
        ExprParameterBlockReturnTypeMismatch = 144,
        ExprParameterBlockLocalConflictsParameter = 145,
        ExprParameterShorthandRequiresParameterNames = 146,
        ExprParameterBlockDisallowedDeclaration = 147,
        ExprParameterBlockDisallowedConstruct = 148,
        ExportNotAllowedInContext = 149,
        ExportRequiresFunctionDefinition = 150,
        ExportMainNotAllowed = 151,
        ExportFunctionNameInvalid = 152,
        ExportFunctionCannotBeOverloaded = 153,
        ExportFunctionDeclarationConflict = 154,
        VariadicParameterNotLast = 155,
        VariadicElementTypeIsVoid = 156,
        VariadicParameterDefaultArgument = 157,
        ParameterPackUsedAsValue = 158,
        VariadicArgumentTypeMismatch = 159,
        PackExpansionTargetNotPack = 160,
        PackExpansionNotAllowedHere = 161,
        PackSubscriptNotInteger = 162,
        ParameterPackPropertyNotApplicable = 163,
        SpecialMemberVariadicNotAllowed = 164,
        OperatorOverloadVariadicNotAllowed = 165,
        ExpressionParameterVariadicNotAllowed = 166,
        ParameterPackInConstantExpression = 167,
        VariadicFunctionPointerSignatureMismatch = 168,
        VariadicDefaultArgumentAmbiguous = 169,
        CompileTimeConditionOutsideGenericBlock = 170,
    };

    enum class RuntimeErrorCode : std::uint16_t {
        HeapAllocFailure = 1,
        NullFuncPtrCall = 2,
    };

    struct Diagnostic {
        DiagnosticSeverity severity;
        SourceLocation location;
        ErrorCode code;
        std::string message;

        static std::string error_code_string(ErrorCode code);
        static std::string runtime_error_code_string(RuntimeErrorCode code);

        static std::string error_template(ErrorCode code);
        static std::string substitute_placeholders(std::string_view tmpl,
            const std::vector<std::string>& values);
    };

    class DiagnosticEngine {
    public:
        DiagnosticEngine() = default;

        void report_error(SourceLocation loc, ErrorCode code, std::string_view message);
        void report_error_template(SourceLocation loc, ErrorCode code,
            const std::vector<std::string>& values);
        void report_warning(SourceLocation loc, ErrorCode code, std::string_view message);
        void report_note(SourceLocation loc, std::string_view message);

        static void report_runtime_error(RuntimeErrorCode code, std::string_view message);

        bool has_errors() const noexcept { return error_count_ > 0; }

        std::size_t error_count() const noexcept { return error_count_; }

        std::size_t warning_count() const noexcept { return warning_count_; }

        void clear();

        void print_all(std::ostream& os) const;

        void print_all(std::ostream& os, DiagnosticSeverity min_severity) const;

    private:
        std::vector<Diagnostic> diagnostics_;
        std::size_t error_count_ = 0;
        std::size_t warning_count_ = 0;

        void report(DiagnosticSeverity sev, SourceLocation loc, ErrorCode code, std::string_view msg);
        void report(DiagnosticSeverity sev, SourceLocation loc, std::string_view msg);

        static std::string format_message(ErrorCode code, std::string_view user_msg);
        static std::string severity_prefix(DiagnosticSeverity sev);
    };

}

#endif
