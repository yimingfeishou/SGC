#include "../semantic/type_checker.hpp"
#include "type_checker_detail.hpp"
#include "../parser/ast.hpp"
#include "../semantic/constant_folding.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <system_error>
#include <unordered_set>
#include <functional>

using namespace gallt::AST;

namespace gallt {
    using namespace type_checker_detail;

    AST::Type TypeChecker::check_expression(AST::Expression* expr, bool allow_void) {
        if (expr == nullptr) {
            return AST::Type::make_void();
        }
        {
            auto site = expression_call_sites_.find(expr);
            if (site != expression_call_sites_.end() &&
                site->second.kind == TypeKind::Void) {
                if (!allow_void) {
                    diag_.report_error_template(expr->location,
                        ErrorCode::ExprParameterExpansionTypeError,
                        { expression_display_name(expr), "void", "value" });
                }
                expression_types_[expr] = AST::Type::make_void();
                return AST::Type::make_void();
            }
        }
        AST::Type operator_result;
        if (try_user_defined_operator(expr, operator_result)) {
            expression_types_[expr] = operator_result;
            return operator_result;
        }
        AST::Type result;
        if (auto* assign = dynamic_cast<AST::AssignmentExpression*>(expr)) {
            result = check_assignment(assign);
        }
        else if (auto* cond = dynamic_cast<AST::ConditionalExpression*>(expr)) {
            result = check_conditional(cond);
        }
        else if (auto* log_or = dynamic_cast<AST::LogicalOrExpression*>(expr)) {
            result = check_logical_or(log_or);
        }
        else if (auto* log_and = dynamic_cast<AST::LogicalAndExpression*>(expr)) {
            result = check_logical_and(log_and);
        }
        else if (auto* bit = dynamic_cast<AST::BitwiseExpression*>(expr)) {
            result = check_bitwise(bit);
        }
        else if (auto* comp = dynamic_cast<AST::ComparisonExpression*>(expr)) {
            result = check_comparison(comp);
        }
        else if (auto* shift = dynamic_cast<AST::ShiftExpression*>(expr)) {
            result = check_shift(shift);
        }
        else if (auto* add = dynamic_cast<AST::AdditiveExpression*>(expr)) {
            result = check_additive(add);
        }
        else if (auto* mul = dynamic_cast<AST::MultiplicativeExpression*>(expr)) {
            result = check_multiplicative(mul);
        }
        else if (auto* pow = dynamic_cast<AST::PowerExpression*>(expr)) {
            result = check_power(pow);
        }
        else if (auto* unary = dynamic_cast<AST::UnaryExpression*>(expr)) {
            result = check_unary(unary);
        }
        else if (auto* post = dynamic_cast<AST::PostfixExpression*>(expr)) {
            result = check_postfix(post);
        }
        else if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr)) {
            result = check_primary(prim);
        }
        else {
            report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                "unknown expression type");
            result = AST::Type::make_void();
        }
        expression_types_[expr] = result;
        (void)allow_void;
        return result;
    }

    AST::Type TypeChecker::check_assignment(AST::AssignmentExpression* expr) {
        if (!expr->left->is_lvalue()) {
            report_error(expr->left->location, ErrorCode::ExpressionSyntaxError,
                "left-hand side of assignment must be an lvalue");
        }
        AST::Type left_type = check_expression(expr->left.get());
        if (left_type.is_const) {
            diag_.report_error_template(expr->left->location, ErrorCode::ConstModification,
                { expression_display_name(expr->left.get()) });
        }
        const AST::Type* saved_expected = expected_type_;
        expected_type_ = &left_type;
        AST::Type right_type = check_expression(expr->right.get());
        expected_type_ = saved_expected;
        if (expr->op == AST::AssignmentExpression::Operator::Assign &&
            left_type.kind == TypeKind::Struct && right_type.kind == TypeKind::Struct &&
            left_type.struct_name == right_type.struct_name) {
            if (is_move_expression(expr->right.get())) {
                if (!type_is_movable(left_type)) {
                    diag_.report_error_template(expr->location, ErrorCode::NoMoveViolation,
                        { left_type.struct_name });
                }
            }
            else if (!type_is_copyable(left_type)) {
                diag_.report_error_template(expr->location, ErrorCode::NoCopyViolation,
                    { left_type.struct_name });
            }
        }
        if (left_type.kind == TypeKind::Array) {
            report_error(expr->location, ErrorCode::ArrayOperatorNotSupported,
                "array type does not support assignment");
            return left_type;
        }
        const bool const_address_assignment =
            left_type.kind == TypeKind::Pointer &&
            right_type.kind == TypeKind::Pointer &&
            left_type.pointee_type && right_type.pointee_type &&
            !const_qualification_ok(*right_type.pointee_type,
                *left_type.pointee_type) &&
            is_address_of_const_identifier(expr->right.get());
        if (!const_address_assignment &&
            left_type.kind == TypeKind::Pointer && right_type.kind == TypeKind::Pointer &&
            left_type.pointee_type && right_type.pointee_type &&
            left_type.pointee_type->kind != TypeKind::Void &&
            right_type.pointee_type->kind != TypeKind::Void &&
            !(types_equal_modulo_const(*left_type.pointee_type,
                *right_type.pointee_type) &&
                const_qualification_ok(*right_type.pointee_type,
                    *left_type.pointee_type))) {
            report_error(expr->location, ErrorCode::PointerTypeMismatch,
                "cannot assign pointer type '" + right_type.to_string() +
                "' to '" + left_type.to_string() + "'");
            return left_type;
        }
        if (left_type.kind == TypeKind::Function && right_type.kind == TypeKind::Function &&
            !function_signatures_match(left_type, right_type)) {
            report_error(expr->location, ErrorCode::FuncPtrTypeMismatch,
                "function pointer type mismatch: expected '" + left_type.to_string() +
                "', got '" + right_type.to_string() + "'");
            return left_type;
        }
       bool ok = false;
       if (expr->op == AST::AssignmentExpression::Operator::Assign) {
           ok = can_implicit_convert(right_type, left_type);
            if (!ok && left_type.kind == TypeKind::File) {
                if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr->right.get())) {
                    if (prim->kind == AST::PrimaryExpression::Kind::Null) ok = true;
                }
            }
       }
        else if (expr->op == AST::AssignmentExpression::Operator::PlusAssign ||
            expr->op == AST::AssignmentExpression::Operator::MinusAssign) {
            if (is_numeric_type(left_type) && is_numeric_type(right_type)) {
                ok = can_implicit_convert(right_type, left_type);
            }
            else {
                if (left_type.kind == TypeKind::Pointer && right_type.is_integer()) {
                    ok = true;
                }
                else {
                    report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                        "operator '" + std::string(expr->op == AST::AssignmentExpression::Operator::PlusAssign ? "+=" : "-=") +
                        "' requires arithmetic types or pointer and integer");
                    ok = false;
                }
            }
        }
        else if (expr->op == AST::AssignmentExpression::Operator::AndAssign ||
            expr->op == AST::AssignmentExpression::Operator::OrAssign ||
            expr->op == AST::AssignmentExpression::Operator::XorAssign ||
            expr->op == AST::AssignmentExpression::Operator::ShiftLeftAssign ||
            expr->op == AST::AssignmentExpression::Operator::ShiftRightAssign) {
            const char* compound_text = "&=";
            switch (expr->op) {
            case AST::AssignmentExpression::Operator::AndAssign: compound_text = "&="; break;
            case AST::AssignmentExpression::Operator::OrAssign: compound_text = "|="; break;
            case AST::AssignmentExpression::Operator::XorAssign: compound_text = "^="; break;
            case AST::AssignmentExpression::Operator::ShiftLeftAssign: compound_text = "<<="; break;
            case AST::AssignmentExpression::Operator::ShiftRightAssign: compound_text = ">>="; break;
            default: break;
            }
            if (left_type.is_integer() && right_type.is_integer()) {
                ok = can_implicit_convert(right_type, left_type);
            }
            else {
                report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                    "operator '" + std::string(compound_text) +
                    "' requires integer types, got '" + left_type.to_string() +
                    "' and '" + right_type.to_string() + "'");
                ok = false;
            }
        }
        if (!ok && !const_address_assignment) {
            auto* target = dynamic_cast<AST::PrimaryExpression*>(expr->left.get());
            if (target != nullptr && target->kind == AST::PrimaryExpression::Kind::Identifier &&
                target->identifier.rfind("__glt_expr", 0) == 0) {
                diag_.report_error_template(expr->location,
                    ErrorCode::ExprParameterBlockReturnTypeMismatch,
                    { left_type.to_string(), right_type.to_string() });
            }
            else {
                report_error(expr->location, ErrorCode::AssignmentTypeMismatch,
                    "cannot assign type '" + right_type.to_string() +
                    "' to type '" + left_type.to_string() + "'");
            }
        }
        return left_type;
    }

    AST::Type TypeChecker::check_logical_or(AST::LogicalOrExpression* expr) {
        AST::Type left = check_expression(expr->left.get());
        AST::Type right = check_expression(expr->right.get());
        if (!is_bool_type(left) && !left.is_integer()) {
            report_error(expr->left->location, ErrorCode::BinaryOperatorTypeMismatch,
                "left operand of '||' must be boolean or integer, got '" + left.to_string() + "'");
        }
        if (!is_bool_type(right) && !right.is_integer()) {
            report_error(expr->right->location, ErrorCode::BinaryOperatorTypeMismatch,
                "right operand of '||' must be boolean or integer, got '" + right.to_string() + "'");
        }
        return AST::Type::make_bool();
    }

    AST::Type TypeChecker::check_conditional(AST::ConditionalExpression* expr) {
        AST::Type condition = decay_array_type(check_expression(expr->condition.get()));
        if (!is_bool_type(condition) && !condition.is_integer()) {
            report_error(expr->condition->location, ErrorCode::ConditionNotBoolean,
                "condition of '?:' must be boolean or integer, got '" +
                condition.to_string() + "'");
        }
        const AST::Type* saved_expected = expected_type_;
        expected_type_ = nullptr;
        AST::Type left = decay_array_type(check_expression(expr->then_expr.get()));
        AST::Type right = decay_array_type(check_expression(expr->else_expr.get()));
        expected_type_ = saved_expected;

        if (left.kind == TypeKind::Void || right.kind == TypeKind::Void) {
            report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                "operator '?:' requires value operands, got '" + left.to_string() +
                "' and '" + right.to_string() + "'");
            return AST::Type::make_void();
        }
        if (left == right) {
            return left;
        }
        if (is_numeric_type(left) && is_numeric_type(right)) {
            return usual_arithmetic_conversion(left, right);
        }
        if (left.kind == TypeKind::Pointer && is_null_literal_expr(expr->else_expr.get())) {
            return left;
        }
        if (right.kind == TypeKind::Pointer && is_null_literal_expr(expr->then_expr.get())) {
            return right;
        }
        if (left.kind == TypeKind::Function && is_null_literal_expr(expr->else_expr.get())) {
            return left;
        }
        if (right.kind == TypeKind::Function && is_null_literal_expr(expr->then_expr.get())) {
            return right;
        }
        if (left.kind == TypeKind::File && is_null_literal_expr(expr->else_expr.get())) {
            return left;
        }
        if (right.kind == TypeKind::File && is_null_literal_expr(expr->then_expr.get())) {
            return right;
        }
        if (left.kind == TypeKind::Pointer && right.kind == TypeKind::Pointer &&
            left.pointee_type != nullptr && right.pointee_type != nullptr &&
            (left.pointee_type->kind == TypeKind::Void ||
                right.pointee_type->kind == TypeKind::Void)) {
            return left.pointee_type->kind == TypeKind::Void ? left : right;
        }
        if (left.kind == TypeKind::Function && right.kind == TypeKind::Function &&
            function_signatures_match(left, right)) {
            return left;
        }
        report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
            "operator '?:' operands must have a common type, got '" +
            left.to_string() + "' and '" + right.to_string() + "'");
        return AST::Type::make_void();
    }

    AST::Type TypeChecker::check_bitwise(AST::BitwiseExpression* expr) {
        AST::Type left = decay_array_type(check_expression(expr->left.get()));
        AST::Type right = decay_array_type(check_expression(expr->right.get()));
        const char* op_text = "&";
        switch (expr->op) {
        case AST::BitwiseExpression::Operator::And: op_text = "&"; break;
        case AST::BitwiseExpression::Operator::Xor: op_text = "^"; break;
        case AST::BitwiseExpression::Operator::Or: op_text = "|"; break;
        }
        if (left.kind == TypeKind::Array || right.kind == TypeKind::Array) {
            report_error(expr->location, ErrorCode::ArrayOperatorNotSupported,
                "array type does not support operator '" + std::string(op_text) + "'");
            return AST::Type::make_void();
        }
        if (left.is_integer() && right.is_integer()) {
            return usual_arithmetic_conversion(left, right);
        }
        report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
            "operator '" + std::string(op_text) + "' requires integer types, got '" +
            left.to_string() + "' and '" + right.to_string() + "'");
        return AST::Type::make_void();
    }

    AST::Type TypeChecker::check_shift(AST::ShiftExpression* expr) {
        AST::Type left = decay_array_type(check_expression(expr->left.get()));
        AST::Type right = decay_array_type(check_expression(expr->right.get()));
        const char* op_text =
            expr->op == AST::ShiftExpression::Operator::Left ? "<<" : ">>";
        if (left.kind == TypeKind::Array || right.kind == TypeKind::Array) {
            report_error(expr->location, ErrorCode::ArrayOperatorNotSupported,
                "array type does not support operator '" + std::string(op_text) + "'");
            return AST::Type::make_void();
        }
        if (left.is_integer() && right.is_integer()) {
            return usual_arithmetic_conversion(left, right);
        }
        report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
            "operator '" + std::string(op_text) + "' requires integer types, got '" +
            left.to_string() + "' and '" + right.to_string() + "'");
        return AST::Type::make_void();
    }

    AST::Type TypeChecker::check_logical_and(AST::LogicalAndExpression* expr) {
        AST::Type left = check_expression(expr->left.get());
        AST::Type right = check_expression(expr->right.get());
        if (!is_bool_type(left) && !left.is_integer()) {
            report_error(expr->left->location, ErrorCode::BinaryOperatorTypeMismatch,
                "left operand of '&&' must be boolean or integer, got '" + left.to_string() + "'");
        }
        if (!is_bool_type(right) && !right.is_integer()) {
            report_error(expr->right->location, ErrorCode::BinaryOperatorTypeMismatch,
                "right operand of '&&' must be boolean or integer, got '" + right.to_string() + "'");
        }
        return AST::Type::make_bool();
    }

    AST::Type TypeChecker::check_comparison(AST::ComparisonExpression* expr) {
        AST::Type left = decay_array_type(check_expression(expr->left.get()));
        AST::Type right = decay_array_type(check_expression(expr->right.get()));
        bool ok = false;
        bool reported = false;
        if (is_numeric_type(left) && is_numeric_type(right)) {
            ok = true;
        }
        else if (left.kind == TypeKind::Pointer && right.kind == TypeKind::Pointer) {
            if (left.pointee_type && right.pointee_type) {
                if (left.pointee_type->kind == TypeKind::Void || right.pointee_type->kind == TypeKind::Void) {
                    ok = true;
                }
                else if (*left.pointee_type == *right.pointee_type) {
                    ok = true;
                }
            }
        }
        else if (left.kind == TypeKind::Pointer && right.is_integer()) {
            if (is_null_literal_expr(expr->right.get())) {
                ok = true;
            }
            else {
                report_error(expr->location, ErrorCode::PointerArithmeticInvalid,
                    "pointer and integer cannot be compared; got '" + left.to_string() +
                    "' and '" + right.to_string() + "'");
                reported = true;
            }
        }
        else if (left.is_integer() && right.kind == TypeKind::Pointer) {
            if (is_null_literal_expr(expr->left.get())) {
                ok = true;
            }
            else {
                report_error(expr->location, ErrorCode::PointerArithmeticInvalid,
                    "pointer and integer cannot be compared; got '" + left.to_string() +
                    "' and '" + right.to_string() + "'");
                reported = true;
            }
        }
        else if (left.kind == TypeKind::Pointer && is_null_literal_expr(expr->right.get())) {
            ok = true;
        }
        else if (right.kind == TypeKind::Pointer && is_null_literal_expr(expr->left.get())) {
            ok = true;
        }
       else if (left.kind == TypeKind::File && right.kind == TypeKind::File) {
           ok = true;
       }
        else if ((left.kind == TypeKind::File || is_file_pointer_type(left)) &&
            is_null_literal_expr(expr->right.get())) {
            ok = true;
        }
       else if ((right.kind == TypeKind::File || is_file_pointer_type(right)) &&
            is_null_literal_expr(expr->left.get())) {
            ok = true;
        }
        else if (left.kind == TypeKind::Function || right.kind == TypeKind::Function) {
            bool is_equal_op = (expr->op == AST::ComparisonExpression::Operator::Equal ||
                expr->op == AST::ComparisonExpression::Operator::NotEqual);
            const AST::Type& fn_side = (left.kind == TypeKind::Function) ? left : right;
            const AST::Type& other_side = (left.kind == TypeKind::Function) ? right : left;
            bool other_is_null = is_null_literal_expr(left.kind == TypeKind::Function
                ? expr->right.get() : expr->left.get());
            if (!is_equal_op) {
                report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                    "relational comparison is not allowed for function pointers");
                reported = true;
            }
            else if (other_is_null) {
                ok = true;
            }
            else if (other_side.kind == TypeKind::Function) {
                bool same_signature = fn_side.parameter_types.size() ==
                    other_side.parameter_types.size();
                if (same_signature) {
                    for (std::size_t i = 0; i < fn_side.parameter_types.size(); ++i) {
                        if (!(fn_side.parameter_types[i] == other_side.parameter_types[i])) {
                            same_signature = false;
                            break;
                        }
                    }
                }
                if (same_signature) {
                    same_signature = fn_side.return_type && other_side.return_type &&
                        (*fn_side.return_type == *other_side.return_type);
                }
                if (same_signature) {
                    ok = true;
                }
                else {
                    report_error(expr->location, ErrorCode::FuncPtrTypeMismatch,
                        "function pointer signature mismatch in comparison");
                    reported = true;
                }
            }
            else {
                report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                    "comparison operands must be compatible function pointers, got '" +
                    left.to_string() + "' and '" + right.to_string() + "'");
                reported = true;
            }
        }
       if (!ok && !reported) {
            report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                "comparison operands must be arithmetic types or compatible pointers, got '" +
                left.to_string() + "' and '" + right.to_string() + "'");
        }
        return AST::Type::make_bool();
    }

    AST::Type TypeChecker::check_additive(AST::AdditiveExpression* expr) {
        AST::Type left = decay_array_type(check_expression(expr->left.get()));
        AST::Type right = decay_array_type(check_expression(expr->right.get()));
       if (expr->op == AST::AdditiveExpression::Operator::Plus) {
            if (left.kind == TypeKind::String || right.kind == TypeKind::String) {
                if (left.kind == TypeKind::String && right.kind == TypeKind::String) {
                    return AST::Type::make_string();
                }
                if (is_numeric_type(left) && is_numeric_type(right)) {
                }
                else {
                    report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                        "operator '+' cannot mix string and numeric operands, got '" +
                        left.to_string() + "' and '" + right.to_string() + "'");
                    return AST::Type::make_void();
                }
            }
            if (left.kind == TypeKind::Pointer && right.is_integer()) {
                return left;
            }
            else if (left.is_integer() && right.kind == TypeKind::Pointer) {
                return right;
            }
            else if (is_numeric_type(left) && is_numeric_type(right)) {
                return usual_arithmetic_conversion(left, right);
            }
            else {
                report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                    "operator '+' requires arithmetic types or pointer and integer, got '" +
                    left.to_string() + "' and '" + right.to_string() + "'");
                return AST::Type::make_void();
            }
        }
        else {
            if (left.kind == TypeKind::Pointer && right.is_integer()) {
                return left;
            }
            else if (left.kind == TypeKind::Pointer && right.kind == TypeKind::Pointer) {
                if (left.pointee_type && right.pointee_type &&
                    (*left.pointee_type == *right.pointee_type ||
                        left.pointee_type->kind == TypeKind::Void ||
                        right.pointee_type->kind == TypeKind::Void)) {
                    return AST::Type::make_int();
                }
                else {
                    report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                        "pointer subtraction requires compatible pointer types");
                    return AST::Type::make_void();
                }
            }
            else if (is_numeric_type(left) && is_numeric_type(right)) {
                return usual_arithmetic_conversion(left, right);
            }
            else {
                report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                    "operator '-' requires arithmetic types or pointer and integer, got '" +
                    left.to_string() + "' and '" + right.to_string() + "'");
                return AST::Type::make_void();
            }
        }
    }

    AST::Type TypeChecker::check_multiplicative(AST::MultiplicativeExpression* expr) {
        AST::Type left = check_expression(expr->left.get());
        AST::Type right = check_expression(expr->right.get());
        if (left.kind == TypeKind::Array || right.kind == TypeKind::Array) {
            report_error(expr->location, ErrorCode::ArrayOperatorNotSupported,
                "array type does not support operator '" +
                std::string(expr->op == AST::MultiplicativeExpression::Operator::Multiply ? "*" :
                    (expr->op == AST::MultiplicativeExpression::Operator::Divide ? "/" : "%")) +
                "'");
            return AST::Type::make_void();
        }
        if (expr->op == AST::MultiplicativeExpression::Operator::Remainder) {
            if (left.is_integer() && right.is_integer()) {
                return usual_arithmetic_conversion(left, right);
            }
            report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                "operator '%' requires integer types, got '" +
                left.to_string() + "' and '" + right.to_string() + "'");
            return AST::Type::make_void();
        }
        if (is_numeric_type(left) && is_numeric_type(right)) {
            return usual_arithmetic_conversion(left, right);
        }
        else {
            report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                "operator '" + std::string(expr->op == AST::MultiplicativeExpression::Operator::Multiply ? "*" : "/") +
                "' requires arithmetic types, got '" +
                left.to_string() + "' and '" + right.to_string() + "'");
            return AST::Type::make_void();
        }
    }

    AST::Type TypeChecker::check_power(AST::PowerExpression* expr) {
        AST::Type left = check_expression(expr->left.get());
        AST::Type right = check_expression(expr->right.get());
        if (left.kind == TypeKind::Array || right.kind == TypeKind::Array) {
            report_error(expr->location, ErrorCode::ArrayOperatorNotSupported,
                "array type does not support operator '**'");
            return AST::Type::make_void();
        }
        if (is_numeric_type(left) && is_numeric_type(right)) {
            return usual_arithmetic_conversion(left, right);
        }
        else {
            report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                "operator '**' requires arithmetic types, got '" +
                left.to_string() + "' and '" + right.to_string() + "'");
            return AST::Type::make_void();
        }
    }

    AST::Type TypeChecker::check_unary(AST::UnaryExpression* expr) {
        if (expr->op == AST::UnaryExpression::Operator::AddressOf) {
            if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr->operand.get())) {
                if (prim->kind == AST::PrimaryExpression::Kind::Identifier) {
                    const Symbol* constant = sym_table_.lookup(prim->identifier);
                    if (constant != nullptr && constant->kind != SymbolKind::Function &&
                        constant->type.is_const) {
                        diag_.report_error_template(expr->operand->location,
                            ErrorCode::ConstModification, { prim->identifier });
                    }
                    std::vector<Symbol>* set = sym_table_.lookup_overloads(prim->identifier);
                    if (set != nullptr && !set->empty()) {
                        Symbol* chosen = nullptr;
                        if (set->size() == 1) {
                            chosen = &(*set)[0];
                        }
                        else if (expected_type_ != nullptr) {
                            chosen = select_overload_by_target_type(*set, *expected_type_,
                                prim->identifier, expr->location);
                        }
                        else {
                            report_error(expr->location, ErrorCode::OverloadAmbiguous,
                                { prim->identifier });
                        }
                        if (chosen == nullptr) return AST::Type::make_void();
                        record_function_resolution(prim, *chosen);
                        return AST::Type::make_function(
                            std::make_shared<AST::Type>(chosen->type), chosen->param_types);
                    }
                }
            }
        }
        AST::Type operand = check_expression(expr->operand.get());
        switch (expr->op) {
        case AST::UnaryExpression::Operator::Increment:
        case AST::UnaryExpression::Operator::Decrement:
            if (!expr->operand->is_lvalue()) {
                report_error(expr->operand->location, ErrorCode::ExpressionSyntaxError,
                    "operand of increment/decrement must be an lvalue");
            }
            if (operand.is_const) {
                diag_.report_error_template(expr->operand->location,
                    ErrorCode::ConstModification,
                    { expression_display_name(expr->operand.get()) });
            }
            if (!is_numeric_type(operand) && operand.kind != TypeKind::Pointer) {
                report_error(expr->location, ErrorCode::UnaryOperatorTypeMismatch,
                    "increment/decrement requires arithmetic or pointer type, got '" +
                    operand.to_string() + "'");
            }
            return operand;
        case AST::UnaryExpression::Operator::LogicalNot:
            if (!is_bool_type(operand) && !operand.is_integer()) {
                report_error(expr->location, ErrorCode::UnaryOperatorTypeMismatch,
                    "operator '!' requires boolean or integer type, got '" +
                    operand.to_string() + "'");
            }
            return AST::Type::make_bool();
        case AST::UnaryExpression::Operator::BitwiseNot:
            if (operand.kind == TypeKind::Array) {
                report_error(expr->location, ErrorCode::ArrayOperatorNotSupported,
                    "array type does not support operator '~'");
                return AST::Type::make_void();
            }
            if (!operand.is_integer()) {
                report_error(expr->location, ErrorCode::UnaryOperatorTypeMismatch,
                    "operator '~' requires integer type, got '" +
                    operand.to_string() + "'");
                return AST::Type::make_void();
            }
            return operand;
        case AST::UnaryExpression::Operator::UnaryPlus:
        case AST::UnaryExpression::Operator::UnaryMinus: {
            const char* op_text =
                (expr->op == AST::UnaryExpression::Operator::UnaryMinus) ? "-" : "+";
            if (!is_numeric_type(operand)) {
                report_error(expr->location, ErrorCode::UnaryOperatorTypeMismatch,
                    std::string("unary operator '") + op_text +
                    "' requires arithmetic type, got '" + operand.to_string() + "'");
                return AST::Type::make_void();
            }
            return operand;
        }
        case AST::UnaryExpression::Operator::AddressOf:
            if (operand.kind == TypeKind::Function) {
                return operand;
            }
            if (!expr->operand->is_lvalue()) {
                report_error(expr->operand->location, ErrorCode::AddressOfNonLValue,
                    "address-of operator requires an lvalue");
            }
            return AST::Type::make_pointer(std::make_shared<AST::Type>(operand));
        case AST::UnaryExpression::Operator::Dereference:
            if (operand.kind != TypeKind::Pointer) {
                report_error(expr->location, ErrorCode::DerefNonPointer,
                    "dereference operator requires pointer type, got '" +
                    operand.to_string() + "'");
                return AST::Type::make_void();
            }
            if (is_null_literal_expr(expr->operand.get())) {
                report_error(expr->location, ErrorCode::NullPointerDereference,
                    "cannot dereference a null pointer constant");
                return AST::Type::make_void();
            }
            if (operand.pointee_type) {
                if (operand.pointee_type->kind == TypeKind::Void) {
                    report_error(expr->location, ErrorCode::DerefNonPointer,
                        "dereference operator cannot be applied to 'void*'");
                    return AST::Type::make_void();
                }
                return *operand.pointee_type;
            }
            else {
                report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                    "pointer has no pointee type");
                return AST::Type::make_void();
            }
        default:
            report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                "unknown unary operator");
            return AST::Type::make_void();
        }
    }

    AST::Type TypeChecker::check_postfix(AST::PostfixExpression* expr) {
        if (expr->op == AST::PostfixExpression::Operator::FunctionCall) {
            if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr->base.get())) {
                if (prim->kind == AST::PrimaryExpression::Kind::Identifier) {
                    const std::string& callee_name = prim->identifier;
                    const bool declared =
                        sym_table_.lookup_overloads(callee_name) != nullptr ||
                        sym_table_.lookup(callee_name) != nullptr;
                    if (!declared) {
                        diag_.report_error_template(expr->location,
                            ErrorCode::UndefinedFunction, { callee_name });
                        for (auto& arg : expr->arguments) {
                            check_expression(arg.get());
                        }
                        return AST::Type::make_void();
                    }
                }
            }
        }
        AST::Type base_type = check_expression(expr->base.get());

        if (expr->op == AST::PostfixExpression::Operator::Subscript ||
            expr->op == AST::PostfixExpression::Operator::Arrow ||
            expr->op == AST::PostfixExpression::Operator::Increment ||
            expr->op == AST::PostfixExpression::Operator::Decrement) {
            AST::Type operator_type;
            if (try_user_defined_operator(expr, operator_type)) {
                return operator_type;
            }
        }

        switch (expr->op) {
        case AST::PostfixExpression::Operator::Subscript: {
            if (base_type.kind != TypeKind::Array && base_type.kind != TypeKind::Pointer) {
                report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                    "subscript operator requires array or pointer type, got '" +
                    base_type.to_string() + "'");
                return AST::Type::make_void();
            }
            if (expr->subscript_expr) {
                AST::Type index_type = check_expression(expr->subscript_expr.get());
                if (!index_type.is_integer()) {
                    report_error(expr->subscript_expr->location, ErrorCode::SubscriptNotInteger,
                        "array index must be integer type, got '" + index_type.to_string() + "'");
                }
                else if (base_type.kind == TypeKind::Array &&
                    base_type.array_size.has_value()) {
                    auto index_value =
                        evaluate_signed_const_integer_expression(
                            expr->subscript_expr.get());
                    if (index_value.has_value()) {
                        const bool negative = *index_value < 0;
                        const bool too_large = !negative &&
                            static_cast<std::size_t>(*index_value) >=
                                *base_type.array_size;
                        if (negative || too_large) {
                            diag_.report_error_template(expr->subscript_expr->location,
                                ErrorCode::SubscriptOutOfBounds,
                                { std::to_string(*index_value),
                                  std::to_string(*base_type.array_size) });
                        }
                    }
                }
            }
            if (base_type.kind == TypeKind::Array) {
                if (base_type.element_type) {
                    return *base_type.element_type;
                }
            }
            else if (base_type.kind == TypeKind::Pointer) {
                if (base_type.pointee_type) {
                    return *base_type.pointee_type;
                }
            }
            return AST::Type::make_void();
        }
        case AST::PostfixExpression::Operator::FunctionCall: {
            std::string func_name;
            AST::PrimaryExpression* direct_primary = nullptr;
            if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr->base.get())) {
                if (prim->kind == AST::PrimaryExpression::Kind::Identifier) {
                    func_name = prim->identifier;
                    direct_primary = prim;
                }
            }

            if (direct_primary != nullptr && !func_name.empty()) {
                std::vector<Symbol>* set = sym_table_.lookup_overloads(func_name);
                if (set != nullptr && set->size() > 1) {
                    Symbol* chosen = resolve_overload_call(func_name, expr, direct_primary);
                    if (chosen == nullptr) return AST::Type::make_void();
                    return chosen->type;
                }
            }
            if (is_file_builtin_name(func_name)) {
                return check_file_builtin_call(expr, func_name);
            }
            if (is_string_builtin_name(func_name)) {
                return check_string_builtin_call(expr, func_name);
            }
            if (func_name == "output") {
                for (std::size_t i = 0; i < expr->arguments.size(); ++i) {
                    AST::Type arg_type = check_expression(expr->arguments[i].get());
                    const bool printable = arg_type.is_arithmetic() ||
                        arg_type.kind == TypeKind::Bool ||
                        arg_type.kind == TypeKind::String ||
                        arg_type.kind == TypeKind::Pointer ||
                        arg_type.kind == TypeKind::Function ||
                        arg_type.kind == TypeKind::Array;
                    if (!printable) {
                        diag_.report_error_template(expr->arguments[i]->location,
                            ErrorCode::FunctionArgTypeMismatch,
                            { std::to_string(i + 1), "printable value",
                              arg_type.to_string() });
                    }
                }
                return AST::Type::make_void();
            }
            else if (func_name == "size" || func_name == "align") {
                if (expr->arguments.size() != 1) {
                    report_error(expr->location, ErrorCode::FunctionArgCountMismatch,
                        func_name + " expects exactly one argument");
                    return AST::Type::make_void();
                }
                auto* arg = expr->arguments[0].get();
                bool is_type_name = false;
                if (auto* prim_arg = dynamic_cast<AST::PrimaryExpression*>(arg)) {
                    if (prim_arg->kind == AST::PrimaryExpression::Kind::Identifier) {
                        std::string name = prim_arg->identifier;
                        if (name == "int" || name == "lint" || name == "uint" ||
                            name == "luint" || name == "float" || name == "double" ||
                            name == "char" || name == "uchar" ||
                            name == "bool" || name == "string" || name == "file" ||
                            name == "void") {
                            is_type_name = true;
                        }
                        else if (struct_defs_.find(name) != struct_defs_.end()) {
                            is_type_name = true;
                        }
                    }
                }
                if (is_type_name) {
                    return AST::Type::make_int();
                }
                else {
                    AST::Type arg_type = check_expression(arg);
                    if (arg_type.kind == TypeKind::Void) {
                        report_error(arg->location, ErrorCode::InvalidTypeCast,
                            "cannot take size/align of void");
                    }
                    return AST::Type::make_int();
                }
            }
            else if (func_name == "free") {
                if (expr->arguments.size() != 1) {
                    report_error(expr->location, ErrorCode::FunctionArgCountMismatch,
                        "free expects exactly one argument");
                    return AST::Type::make_void();
                }
                AST::Type arg_type = check_expression(expr->arguments[0].get());
                if (arg_type.kind != TypeKind::Pointer) {
                    report_error(expr->arguments[0]->location, ErrorCode::FreeNonPointer,
                        "free requires pointer type, got '" + arg_type.to_string() + "'");
                }
                return AST::Type::make_void();
            }
            else if (func_name == "input") {
                if (expr->arguments.size() > 1) {
                    report_error(expr->location, ErrorCode::FunctionArgCountMismatch,
                        "input expects zero or one argument");
                    return AST::Type::make_void();
                }
                if (expr->arguments.size() == 1) {
                    AST::Expression* arg = expr->arguments[0].get();
                    if (!arg->is_lvalue()) {
                        report_error(arg->location, ErrorCode::ExpressionSyntaxError,
                            "input argument must be an lvalue");
                    }
                    AST::Type arg_type = check_expression(arg);
                    if (arg_type.kind == TypeKind::Array) {
                        report_error(arg->location, ErrorCode::ExpressionSyntaxError,
                            "input cannot read directly into an array; use an element or pointer");
                    }
                    else if (arg_type.kind != TypeKind::Int &&
                        arg_type.kind != TypeKind::Uint &&
                        arg_type.kind != TypeKind::Lint &&
                        arg_type.kind != TypeKind::Luint &&
                        arg_type.kind != TypeKind::Char &&
                        arg_type.kind != TypeKind::Uchar &&
                        arg_type.kind != TypeKind::Bool &&
                        arg_type.kind != TypeKind::Float &&
                        arg_type.kind != TypeKind::Double &&
                        arg_type.kind != TypeKind::String) {
                        diag_.report_error_template(arg->location,
                            ErrorCode::FunctionArgTypeMismatch,
                            { std::to_string(1), "input-compatible type",
                              arg_type.to_string() });
                    }
                    return AST::Type::make_void();
                }
                return AST::Type::make_int();
            }

            if (direct_primary != nullptr && struct_defs_.find(func_name) != struct_defs_.end()) {
                std::vector<AST::Type> arg_types;
                std::vector<bool> arg_is_null;
                for (auto& arg : expr->arguments) {
                    arg_types.push_back(check_expression(arg.get()));
                    bool is_null = false;
                    if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(arg.get())) {
                        if (prim->kind == AST::PrimaryExpression::Kind::Null) is_null = true;
                    }
                    arg_is_null.push_back(is_null);
                }
                bool has_declared_constructor = false;
                if (AST::StructDefinition* definition = get_struct_definition(func_name)) {
                    for (const auto& member : definition->special_members) {
                        if (member->kind ==
                            AST::SpecialMemberFunction::Kind::Constructor) {
                            has_declared_constructor = true;
                        }
                    }
                }
                if (!has_declared_constructor && arg_types.size() == 1 &&
                    (arg_types[0].kind == TypeKind::Pointer ||
                        arg_types[0].kind == TypeKind::Function)) {
                    AST::Type converted_type = AST::Type::make_struct(func_name);
                    expr->cast_type = converted_type;
                    expr->base = std::move(expr->arguments[0]);
                    expr->arguments.clear();
                    expr->op = AST::PostfixExpression::Operator::Cast;
                    expression_types_[expr] = converted_type;
                    return converted_type;
                }
                Symbol* ctor = resolve_constructor_overload(func_name, arg_types, arg_is_null,
                    expr->location);
                if (ctor != nullptr && ctor->function_node != nullptr) {
                    resolved_functions_[direct_primary] = ctor->function_node;
                    for (std::size_t i = 0; i < arg_types.size() &&
                        i + 1 < ctor->param_types.size(); ++i) {
                        bool compatible = can_implicit_convert(arg_types[i],
                            ctor->param_types[i + 1]);
                        if (!compatible && arg_is_null[i] &&
                            ctor->param_types[i + 1].kind == TypeKind::Pointer) {
                            compatible = true;
                        }
                        if (!compatible) {
                            report_error(expr->arguments[i]->location,
                                ErrorCode::FunctionArgTypeMismatch,
                                "constructor argument " + std::to_string(i + 1) +
                                " type '" + arg_types[i].to_string() +
                                "' does not match parameter type '" +
                                ctor->param_types[i + 1].to_string() + "'");
                        }
                    }
                }
                return AST::Type::make_struct(func_name);
            }
            AST::Type func_type;
            bool is_direct_function = false;
            if (direct_primary != nullptr) {
                Symbol* symbol = sym_table_.lookup(direct_primary->identifier);
                if (symbol != nullptr && symbol->kind == SymbolKind::Function) {
                    func_type = AST::Type::make_function(
                        std::make_shared<AST::Type>(symbol->type),
                        symbol->param_types);
                    is_direct_function = true;
                }
            }
            if (!is_direct_function) {
                func_type = base_type;
                if (func_type.kind == TypeKind::Pointer && func_type.pointee_type &&
                    func_type.pointee_type->kind == TypeKind::Function) {
                    func_type = *func_type.pointee_type;
                }
            }
            if (func_type.kind != TypeKind::Function) {
                report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                    "function call requires a function or function pointer, got '" +
                    base_type.to_string() + "'");
                return AST::Type::make_void();
            }
           size_t expected = func_type.parameter_types.size();
           size_t provided = expr->arguments.size();
            std::vector<AST::Expression*> defaults;
            if (direct_primary != nullptr) {
                Symbol* callee_sym = sym_table_.lookup(direct_primary->identifier);
                if (callee_sym != nullptr && callee_sym->function_node != nullptr) {
                    for (auto& d : callee_sym->function_node->param_defaults) {
                        defaults.push_back(d.get());
                    }
                }
            }
            size_t required = expected;
            for (size_t i = defaults.size(); i > 0; --i) {
                if (defaults[i - 1] != nullptr) required = i - 1;
                else break;
            }
            if (provided > expected || provided < required) {
                report_error(expr->location, ErrorCode::FunctionArgCountMismatch,
                    "function expects " + std::to_string(expected) +
                    " arguments, but " + std::to_string(provided) + " provided");
                return AST::Type::make_void();
            }
            if (provided < expected) {
                for (size_t i = provided; i < expected; ++i) {
                    if (i < defaults.size() && defaults[i] != nullptr) {
                        expr->appended_defaults.push_back(defaults[i]);
                    }
                }
            }
            for (size_t i = 0; i < expected; ++i) {
                if (i < expr->arguments.size()) {
                    const AST::Type* saved_expected = expected_type_;
                    expected_type_ = &func_type.parameter_types[i];
                    AST::Type arg_type = check_expression(expr->arguments[i].get());
                    expected_type_ = saved_expected;
                    if (func_type.parameter_types[i].kind == TypeKind::Struct &&
                        arg_type.kind == TypeKind::Struct &&
                        arg_type.struct_name == func_type.parameter_types[i].struct_name) {
                        if (is_move_expression(expr->arguments[i].get())) {
                            if (!type_is_movable(func_type.parameter_types[i])) {
                                diag_.report_error_template(expr->arguments[i]->location,
                                    ErrorCode::NoMoveViolation,
                                    { func_type.parameter_types[i].struct_name });
                            }
                        }
                        else if (!type_is_copyable(func_type.parameter_types[i])) {
                            diag_.report_error_template(expr->arguments[i]->location,
                                ErrorCode::NoCopyViolation,
                                { func_type.parameter_types[i].struct_name });
                        }
                    }
                    bool compatible = can_implicit_convert(arg_type, func_type.parameter_types[i]);
                    if (!compatible && arg_type.kind == TypeKind::Pointer &&
                        arg_type.pointee_type && arg_type.pointee_type->kind == TypeKind::Void &&
                        func_type.parameter_types[i].kind == TypeKind::Pointer) {
                        if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr->arguments[i].get())) {
                            if (prim->kind == AST::PrimaryExpression::Kind::Null) {
                                compatible = true;
                            }
                        }
                    }
                    if (!compatible) {
                        report_error(expr->arguments[i]->location, ErrorCode::FunctionArgTypeMismatch,
                            "argument " + std::to_string(i + 1) + " type '" + arg_type.to_string() +
                            "' does not match expected type '" + func_type.parameter_types[i].to_string() + "'");
                    }
                }
            }
            if (func_type.return_type) {
                return *func_type.return_type;
            }
            return AST::Type::make_void();
        }
        case AST::PostfixExpression::Operator::Cast: {
            if (!is_complete_type(expr->cast_type) && expr->cast_type.kind != TypeKind::Void) {
                report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                    "cast target type is incomplete");
                return AST::Type::make_void();
            }
            if (expr->cast_type.kind == TypeKind::Void) {
                report_error(expr->location, ErrorCode::InvalidTypeCast,
                    "cannot cast to void");
                return AST::Type::make_void();
            }
            bool allowed = false;
            if (is_numeric_type(base_type) && is_numeric_type(expr->cast_type)) {
                allowed = true;
            }
            else if (base_type.kind == TypeKind::Pointer && expr->cast_type.kind == TypeKind::Pointer) {
                allowed = true;
            }
            else if (base_type.kind == TypeKind::Pointer && is_integer_type(expr->cast_type)) {
                allowed = true;
            }
            else if (is_integer_type(base_type) && expr->cast_type.kind == TypeKind::Pointer) {
                allowed = true;
            }
            else if (base_type.kind == TypeKind::Pointer ||
                base_type.kind == TypeKind::Function) {
                allowed = true;
            }
            else if (can_implicit_convert(base_type, expr->cast_type)) {
                allowed = true;
            }
            if (!allowed) {
                auto argument = expression_argument_casts_.find(expr);
                if (argument != expression_argument_casts_.end()) {
                    diag_.report_error_template(expr->location,
                        ErrorCode::ExprParameterCallArgTypeMismatch,
                        { std::get<0>(argument->second),
                          std::to_string(std::get<1>(argument->second) + 1),
                          std::get<2>(argument->second).to_string(),
                          base_type.to_string() });
                }
                else {
                    report_error(expr->location, ErrorCode::InvalidTypeCast,
                        "cannot cast type '" + base_type.to_string() +
                        "' to '" + expr->cast_type.to_string() + "'");
                }
                return AST::Type::make_void();
            }
            return expr->cast_type;
        }
        case AST::PostfixExpression::Operator::Increment:
        case AST::PostfixExpression::Operator::Decrement: {
            if (!expr->base->is_lvalue()) {
                report_error(expr->base->location, ErrorCode::ExpressionSyntaxError,
                    "operand of increment/decrement must be an lvalue");
            }
            if (base_type.is_const) {
                diag_.report_error_template(expr->base->location,
                    ErrorCode::ConstModification,
                    { expression_display_name(expr->base.get()) });
            }
            if (!is_numeric_type(base_type) && base_type.kind != TypeKind::Pointer) {
                report_error(expr->location, ErrorCode::UnaryOperatorTypeMismatch,
                    "increment/decrement requires arithmetic or pointer type, got '" +
                    base_type.to_string() + "'");
            }
            return base_type;
        }
        case AST::PostfixExpression::Operator::Dot: {
            if (base_type.kind != TypeKind::Struct) {
                report_error(expr->location, ErrorCode::DotOnNonStruct,
                    "dot operator requires struct type, got '" + base_type.to_string() + "'");
                return AST::Type::make_void();
            }
           const auto* member = get_struct_member(base_type, expr->member_name);
            if (expr->member_name == "destructor") {
                return AST::Type::make_function(
                    std::make_shared<AST::Type>(AST::Type::make_void()),
                    std::vector<AST::Type>{});
            }
           if (!member) {
               report_error(expr->location, ErrorCode::StructMemberNotFound,
                   "struct has no member named '" + expr->member_name + "'");
                return AST::Type::make_void();
            }
            AST::Type member_type = member->type;
            if (base_type.is_const) {
                member_type.is_const = true;
            }
            return member_type;
        }
       case AST::PostfixExpression::Operator::Arrow: {
           if (base_type.kind == TypeKind::Struct &&
               operator_overloads_.count("operator->") != 0) {
               std::vector<AST::Type> addressable;
               if (expr->base->is_lvalue()) {
                   addressable.push_back(AST::Type::make_pointer(
                       std::make_shared<AST::Type>(base_type)));
               }
               std::vector<AST::Type> by_value;
               by_value.push_back(base_type);
               AST::FunctionDefinition* arrow_operator =
                   resolve_user_operator("->", by_value, expr->location);
               if (arrow_operator == nullptr && !addressable.empty()) {
                   arrow_operator = resolve_user_operator("->", addressable,
                       expr->location);
               }
               if (arrow_operator != nullptr) {
                   resolved_operators_[expr->base.get()] = arrow_operator;
                   base_type = arrow_operator->return_type;
               }
           }
           if (base_type.kind != TypeKind::Pointer) {
               report_error(expr->location, ErrorCode::ArrowOnNonStructPtr,
                   "arrow operator requires pointer to struct, got '" + base_type.to_string() + "'");
               return AST::Type::make_void();
           }
           auto pointee = base_type.pointee_type;
           if (!pointee || pointee->kind != TypeKind::Struct) {
               report_error(expr->location, ErrorCode::ArrowOnNonStructPtr,
                   "arrow operator requires pointer to struct, got '" + base_type.to_string() + "'");
               return AST::Type::make_void();
           }
            if (expr->member_name == "destructor") {
                return AST::Type::make_function(
                    std::make_shared<AST::Type>(AST::Type::make_void()),
                    std::vector<AST::Type>{});
            }
           const auto* member = get_struct_member(*pointee, expr->member_name);
           if (!member) {
               report_error(expr->location, ErrorCode::StructMemberNotFound,
                   "struct has no member named '" + expr->member_name + "'");
                return AST::Type::make_void();
            }
            AST::Type member_type = member->type;
            if (pointee->is_const) {
                member_type.is_const = true;
            }
            return member_type;
        }
        default:
            report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                "unknown postfix operator");
            return AST::Type::make_void();
        }
    }

    AST::Type TypeChecker::check_string_builtin_call(AST::PostfixExpression* expr,
        const std::string& func_name) {
        auto info_it = string_builtin_table().find(func_name);
        if (info_it == string_builtin_table().end()) {
            return AST::Type::make_void();
        }
        const StringBuiltinInfo& info = info_it->second;

        AST::Type result_type;
        switch (info.result) {
        case StringResultKind::Int: result_type = AST::Type::make_int(); break;
        case StringResultKind::Bool: result_type = AST::Type::make_bool(); break;
        case StringResultKind::Char: result_type = AST::Type::make_char(); break;
        case StringResultKind::String: result_type = AST::Type::make_string(); break;
        case StringResultKind::Void: result_type = AST::Type::make_void(); break;
        }

        if (expr->arguments.size() != info.params.size()) {
            diag_.report_error_template(expr->location,
                ErrorCode::FunctionArgCountMismatch,
                { std::to_string(info.params.size()),
                  std::to_string(expr->arguments.size()) });
            for (auto& arg : expr->arguments) {
                check_expression(arg.get());
            }
            return result_type;
        }

        for (size_t i = 0; i < expr->arguments.size(); ++i) {
            AST::Expression* arg = expr->arguments[i].get();
            AST::Type arg_type = check_expression(arg);
            const std::string index = std::to_string(i + 1);

            bool is_null_literal = false;
            if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(arg)) {
                if (prim->kind == AST::PrimaryExpression::Kind::Null) {
                    is_null_literal = true;
                }
            }

            switch (info.params[i]) {
            case StringParamKind::StringValue:
                if (arg_type.kind != TypeKind::String) {
                    diag_.report_error_template(arg->location,
                        ErrorCode::FunctionArgTypeMismatch,
                        { index, "string", arg_type.to_string() });
                }
                break;
            case StringParamKind::StringRef:
                if (arg_type.kind != TypeKind::String) {
                    diag_.report_error_template(arg->location,
                        ErrorCode::FunctionArgTypeMismatch,
                        { index, "string", arg_type.to_string() });
                }
                else if (!arg->is_lvalue()) {
                    report_error(arg->location, ErrorCode::ExpressionSyntaxError,
                        "string operation '" + func_name +
                        "' argument " + index +
                        " must be a modifiable string lvalue");
                }
                break;
            case StringParamKind::IntValue:
                if (!arg_type.is_integer()) {
                    diag_.report_error_template(arg->location,
                        ErrorCode::FunctionArgTypeMismatch,
                        { index, "int", arg_type.to_string() });
                }
                break;
            case StringParamKind::CharValue:
                if (!arg_type.is_integer()) {
                    diag_.report_error_template(arg->location,
                        ErrorCode::FunctionArgTypeMismatch,
                        { index, "char", arg_type.to_string() });
                }
                break;
            case StringParamKind::FileHandle:
                if (!is_file_pointer_type(arg_type) && !is_null_literal) {
                    diag_.report_error_template(arg->location,
                        ErrorCode::FunctionArgTypeMismatch,
                        { index, "file*", arg_type.to_string() });
                }
                break;
            }
        }
        return result_type;
    }

    AST::Type TypeChecker::check_file_builtin_call(AST::PostfixExpression* expr,
        const std::string& func_name) {
        auto info_it = file_builtin_table().find(func_name);
        if (info_it == file_builtin_table().end()) {
            return AST::Type::make_void();
        }
        const FileBuiltinInfo& info = info_it->second;

        AST::Type result_type;
        switch (info.result) {
        case FileResultKind::FileHandlePtr:
            result_type = AST::Type::make_pointer(
                std::make_shared<AST::Type>(AST::Type::make_file()));
            break;
        case FileResultKind::Bool: result_type = AST::Type::make_bool(); break;
        case FileResultKind::Int: result_type = AST::Type::make_int(); break;
        case FileResultKind::String: result_type = AST::Type::make_string(); break;
        }

        if (expr->arguments.size() != info.params.size()) {
            report_error(expr->location, ErrorCode::FileArgCountMismatch,
                "file operation '" + func_name + "' expects " +
                std::to_string(info.params.size()) + " argument(s), but " +
                std::to_string(expr->arguments.size()) + " provided");
            for (auto& arg : expr->arguments) {
                check_expression(arg.get());
            }
            return result_type;
        }

        for (size_t i = 0; i < expr->arguments.size(); ++i) {
            AST::Expression* arg = expr->arguments[i].get();
            AST::Type arg_type = check_expression(arg);
            const std::string index = std::to_string(i + 1);

            bool is_null_literal = false;
            if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(arg)) {
                if (prim->kind == AST::PrimaryExpression::Kind::Null) {
                    is_null_literal = true;
                }
            }

            switch (info.params[i]) {
            case FileParamKind::FileHandle:
                if (!is_file_pointer_type(arg_type) && !is_null_literal) {
                    report_error(arg->location, ErrorCode::FileHandleRequired,
                        "file operation '" + func_name + "' argument " + index +
                        " requires 'file*', got '" + arg_type.to_string() + "'");
                }
                break;
            case FileParamKind::Buffer:
                if (arg_type.kind != TypeKind::Pointer && arg_type.kind != TypeKind::Array) {
                    report_error(arg->location, ErrorCode::FileBufferNotPointer,
                        "file operation '" + func_name + "' argument " + index +
                        " buffer must be a pointer, got '" + arg_type.to_string() + "'");
                }
                break;
            case FileParamKind::IntValue:
                if (!arg_type.is_integer()) {
                    report_error(arg->location, ErrorCode::FileArgTypeMismatch,
                        "file operation '" + func_name + "' argument " + index +
                        " type mismatch: expected 'int', got '" + arg_type.to_string() + "'");
                }
                break;
            case FileParamKind::SizeValue:
                if (!arg_type.is_integer()) {
                    report_error(arg->location, ErrorCode::FileSizeNotInt,
                        "file operation '" + func_name + "' argument " + index +
                        " size must be 'int', got '" + arg_type.to_string() + "'");
                }
                break;
            case FileParamKind::StringValue:
                if (arg_type.kind != TypeKind::String) {
                    report_error(arg->location, ErrorCode::FilePathOrModeNotString,
                        "file operation '" + func_name + "' argument " + index +
                        " must be 'string', got '" + arg_type.to_string() + "'");
                }
                break;
            case FileParamKind::TextValue:
                if (arg_type.kind != TypeKind::String) {
                    report_error(arg->location, ErrorCode::FileArgTypeMismatch,
                        "file operation '" + func_name + "' argument " + index +
                        " type mismatch: expected 'string', got '" + arg_type.to_string() + "'");
                }
                break;
            case FileParamKind::SeekOrigin:
                if (!arg_type.is_integer()) {
                    report_error(arg->location, ErrorCode::FileArgTypeMismatch,
                        "file operation '" + func_name + "' argument " + index +
                        " type mismatch: expected 'int', got '" + arg_type.to_string() + "'");
                }
                break;
            }

            if (func_name == "fileopen" && i == 1 && arg_type.kind == TypeKind::String) {
                if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(arg)) {
                    if (prim->kind == AST::PrimaryExpression::Kind::Literal &&
                        prim->literal_token.type == TokenType::StringLiteral) {
                        std::string mode;
                        if (string_literal_content(prim->literal_token.lexeme, mode) &&
                            !is_valid_file_open_mode(mode)) {
                            report_error(arg->location, ErrorCode::FileOpenModeInvalid,
                                "invalid file open mode: '" + mode + "'");
                        }
                    }
                }
            }

            if (func_name == "fileseek" && i == 2) {
                if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(arg)) {
                    if (prim->kind == AST::PrimaryExpression::Kind::Literal &&
                        prim->literal_token.type == TokenType::IntegerLiteral) {
                        long long origin = 0;
                        std::string_view lexeme = prim->literal_token.lexeme;
                        auto parsed = std::from_chars(lexeme.data(),
                            lexeme.data() + lexeme.size(), origin);
                        if (parsed.ec == std::errc() &&
                            origin != 0 && origin != 1 && origin != 2) {
                            report_error(arg->location, ErrorCode::FileSeekOriginInvalid,
                                "fileseek() origin must be 0, 1 or 2, got " +
                                std::to_string(origin));
                        }
                    }
                }
            }
        }
        return result_type;
    }

    AST::Type TypeChecker::check_primary(AST::PrimaryExpression* expr) {
        switch (expr->kind) {
        case AST::PrimaryExpression::Kind::Literal: {
            switch (expr->literal_token.type) {
            case TokenType::IntegerLiteral:
                return AST::Type::integer_literal_type(expr->literal_token.lexeme);
            case TokenType::FloatLiteral:
                return AST::Type::float_literal_type(expr->literal_token.lexeme);
            case TokenType::CharLiteral:
                return AST::Type::make_char();
            case TokenType::StringLiteral:
                return AST::Type::make_string();
            case TokenType::BoolLiteral:
                return AST::Type::make_bool();
            default:
                report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                    "unknown literal type");
                return AST::Type::make_void();
            }
        }
        case AST::PrimaryExpression::Kind::Identifier: {
            if (std::vector<Symbol>* set = sym_table_.lookup_overloads(expr->identifier)) {
                if (set->size() > 1 && expected_type_ != nullptr) {
                    Symbol* chosen = select_overload_by_target_type(*set, *expected_type_,
                        expr->identifier, expr->location);
                    if (chosen != nullptr) {
                        record_function_resolution(expr, *chosen);
                        return AST::Type::make_function(
                            std::make_shared<AST::Type>(chosen->type), chosen->param_types);
                    }
                }
            }
            auto* sym = sym_table_.lookup(expr->identifier);
            if (sym) {
                if (sym->kind == SymbolKind::Function) {
                    record_function_resolution(expr, *sym);
                    return AST::Type::make_function(
                        std::make_shared<AST::Type>(sym->type),
                        sym->param_types);
                }
                return sym->type;
            }
            auto from_expression = expression_free_identifiers_.find(expr);
            if (from_expression != expression_free_identifiers_.end()) {
                diag_.report_error_template(expr->location,
                    ErrorCode::ExprParameterFreeIdentifierUndefined,
                    { from_expression->second });
            }
            else if (sym_table_.was_declared(expr->identifier)) {
                diag_.report_error_template(expr->location,
                    ErrorCode::IdentifierNotInScope, { expr->identifier });
            }
            else {
                report_error(expr->location, ErrorCode::UndefinedIdentifier,
                    "undefined identifier '" + expr->identifier + "'");
            }
            return AST::Type::make_void();
        }
        case AST::PrimaryExpression::Kind::Parens: {
            if (expr->paren_expr) {
                return check_expression(expr->paren_expr.get());
            }
            return AST::Type::make_void();
        }
        case AST::PrimaryExpression::Kind::Null: {
            return AST::Type::make_pointer(std::make_shared<AST::Type>(AST::Type::make_void()));
        }
       case AST::PrimaryExpression::Kind::Heap: {
            if (!is_complete_type(expr->heap_type) && expr->heap_type.kind != TypeKind::Void) {
                report_error(expr->location, ErrorCode::HeapFirstArgNotType,
                    "heap() first argument must be a complete type");
                return AST::Type::make_void();
            }
            if (expr->heap_size) {
                AST::Type size_type = check_expression(expr->heap_size.get());
                if (!size_type.is_integer()) {
                    report_error(expr->heap_size->location, ErrorCode::HeapSecondArgNotInteger,
                        "heap() second argument must be integer type, got '" + size_type.to_string() + "'");
                }
                return AST::Type::make_pointer(std::make_shared<AST::Type>(expr->heap_type));
            }
            else {
                return AST::Type::make_pointer(std::make_shared<AST::Type>(expr->heap_type));
            }
        }
        case AST::PrimaryExpression::Kind::Construct:
        case AST::PrimaryExpression::Kind::PlacementConstruct: {
           if (expr->construct_type.kind != TypeKind::Struct) {
                diag_.report_error_template(expr->location, ErrorCode::SpecialMemberOnNonStruct,
                    { expr->construct_type.to_string() });
                return AST::Type::make_void();
            }
            if (!is_complete_type(expr->construct_type)) {
                report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                    "construct target type is incomplete");
                return AST::Type::make_void();
            }
            for (auto& arg : expr->construct_args) {
                check_expression(arg.get());
            }
            {
                std::vector<AST::Type> arg_types;
                std::vector<bool> arg_is_null;
                for (auto& arg : expr->construct_args) {
                    const AST::Type* cached = nullptr;
                    auto found = expression_types_.find(arg.get());
                    if (found != expression_types_.end()) cached = &found->second;
                    arg_types.push_back(cached != nullptr ? *cached : AST::Type::make_void());
                    bool is_null = false;
                    if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(arg.get())) {
                        if (prim->kind == AST::PrimaryExpression::Kind::Null) is_null = true;
                    }
                    arg_is_null.push_back(is_null);
                }
                Symbol* ctor = resolve_constructor_overload(expr->construct_type.struct_name,
                    arg_types, arg_is_null, expr->location);
                if (ctor != nullptr && ctor->function_node != nullptr) {
                    resolved_functions_[expr] = ctor->function_node;
                    for (std::size_t i = 0; i < arg_types.size() &&
                        i + 1 < ctor->param_types.size(); ++i) {
                        bool compatible = can_implicit_convert(arg_types[i],
                            ctor->param_types[i + 1]);
                        if (!compatible && arg_is_null[i] &&
                            ctor->param_types[i + 1].kind == TypeKind::Pointer) {
                            compatible = true;
                        }
                        if (!compatible) {
                            report_error(expr->construct_args[i]->location,
                                ErrorCode::FunctionArgTypeMismatch,
                                "constructor argument " + std::to_string(i + 1) +
                                " type '" + arg_types[i].to_string() +
                                "' does not match parameter type '" +
                                ctor->param_types[i + 1].to_string() + "'");
                        }
                    }
                }
            }
            if (expr->kind == AST::PrimaryExpression::Kind::PlacementConstruct) {
                AST::Type target_type = check_expression(expr->placement_target.get());
                bool invalid = false;
                if (target_type.kind != TypeKind::Pointer) {
                    invalid = true;
                }
                else if (auto* target_prim =
                    dynamic_cast<AST::PrimaryExpression*>(expr->placement_target.get())) {
                    if (target_prim->kind == AST::PrimaryExpression::Kind::Null) invalid = true;
                }
                if (!invalid && target_type.pointee_type &&
                    target_type.pointee_type->kind != TypeKind::Void) {
                    std::size_t target_size = 0;
                    std::size_t target_align = 1;
                    std::size_t needed_size = 0;
                    std::size_t needed_align = 1;
                    if (type_layout(*target_type.pointee_type, target_size, target_align) &&
                        type_layout(expr->construct_type, needed_size, needed_align)) {
                        if (target_size < needed_size || target_align < needed_align) {
                            invalid = true;
                        }
                    }
                }
                if (invalid) {
                    diag_.report_error_template(expr->location,
                        ErrorCode::PlacementTargetInvalid, std::vector<std::string>{});
                }
            }
            return AST::Type::make_pointer(
                std::make_shared<AST::Type>(expr->construct_type));
        }
        case AST::PrimaryExpression::Kind::CopyMove: {
            AST::Type operand = check_expression(expr->paren_expr.get());
            return operand;
        }
        case AST::PrimaryExpression::Kind::QualifiedName: {
           report_error(expr->location, ErrorCode::GenericUndefined,
                expr->generic_ref ? expr->generic_ref->generic_name : std::string());
            return AST::Type::make_void();
        }
        default:
            report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                "unknown primary expression kind");
            return AST::Type::make_void();
        }
    }

    bool TypeChecker::can_implicit_convert(const AST::Type& from, const AST::Type& to) {
        if (from == to) return true;
        auto documented_lattice_rank = [](const AST::Type& type) -> int {
            switch (type.kind) {
            case TypeKind::Char: return 0;
            case TypeKind::Uchar: return 1;
            case TypeKind::Int: return 2;
            case TypeKind::Uint: return 3;
            case TypeKind::Lint: return 4;
            case TypeKind::Luint: return 5;
            case TypeKind::Float: return 6;
            case TypeKind::Double: return 7;
            default: return -1;
            }
        };
        if (documented_lattice_rank(from) >= 0 &&
            documented_lattice_rank(to) >= 0) {
            return true;
        }
        if (to.kind == TypeKind::Bool && from.is_arithmetic()) return true;
        if (from.kind == TypeKind::Bool && to.is_arithmetic()) return true;
        if (from.kind == TypeKind::Pointer && to.kind == TypeKind::Pointer) {
            if (from.pointee_type && from.pointee_type->kind == TypeKind::Void) return true;
            if (to.pointee_type && to.pointee_type->kind == TypeKind::Void) return true;
            if (!const_qualification_ok(from, to)) return false;
            return types_equal_modulo_const(from, to);
        }
        if (from.kind == TypeKind::Pointer && to.kind == TypeKind::Function) {
            if (from.pointee_type && from.pointee_type->kind == TypeKind::Void) return true;
        }
        if (from.kind == TypeKind::Function && to.kind == TypeKind::Pointer) {
            if (to.pointee_type && to.pointee_type->kind == TypeKind::Void) return true;
        }
        if (from.kind == TypeKind::Array && to.kind == TypeKind::Pointer) {
            if (from.element_type && to.pointee_type) {
                if (!const_qualification_ok(*from.element_type, *to.pointee_type)) {
                    return false;
                }
                return types_equal_modulo_const(*from.element_type, *to.pointee_type);
            }
        }
        return false;
    }

    AST::Type TypeChecker::usual_arithmetic_conversion(const AST::Type& left, const AST::Type& right) {
        auto without_const = [](AST::Type t) {
            t.is_const = false;
            return t;
        };
        if (left == right) return without_const(left);
        const int rank_left = left.promotion_rank();
        const int rank_right = right.promotion_rank();
        return without_const((rank_left >= rank_right) ? left : right);
    }

    bool TypeChecker::is_numeric_type(const AST::Type& type) const {
        return type.is_arithmetic();
    }

    bool TypeChecker::is_integer_type(const AST::Type& type) const {
        return type.is_integer();
    }

    bool TypeChecker::is_bool_type(const AST::Type& type) const {
        return type.kind == TypeKind::Bool;
    }

    bool TypeChecker::is_pointer_type(const AST::Type& type) const {
        return type.kind == TypeKind::Pointer;
    }

    bool TypeChecker::is_struct_type(const AST::Type& type) const {
        return type.kind == TypeKind::Struct;
    }

    bool TypeChecker::is_function_type(const AST::Type& type) const {
        return type.kind == TypeKind::Function;
    }

    bool TypeChecker::is_array_type(const AST::Type& type) const {
        return type.kind == TypeKind::Array;
    }

    AST::Type TypeChecker::decay_array_type(const AST::Type& type) {
        if (type.kind == TypeKind::Array && type.element_type) {
            return AST::Type::make_pointer(
                std::make_shared<AST::Type>(*type.element_type));
        }
        return type;
    }

    bool TypeChecker::function_signatures_match(const AST::Type& expected,
        const AST::Type& actual) {
        if (expected.kind != TypeKind::Function || actual.kind != TypeKind::Function) {
            return false;
        }
        if (!expected.return_type || !actual.return_type) {
            return false;
        }
        if (!(*expected.return_type == *actual.return_type)) {
            return false;
        }
        if (expected.parameter_types.size() != actual.parameter_types.size()) {
            return false;
        }
        for (std::size_t i = 0; i < expected.parameter_types.size(); ++i) {
            if (!(expected.parameter_types[i] == actual.parameter_types[i])) {
                return false;
            }
        }
        return true;
    }

    bool TypeChecker::is_void_type(const AST::Type& type) const {
        return type.kind == TypeKind::Void;
    }

    bool TypeChecker::is_file_type(const AST::Type& type) const {
        return type.kind == TypeKind::File;
    }

    bool TypeChecker::is_file_pointer_type(const AST::Type& type) const {
        return type.kind == TypeKind::Pointer && type.pointee_type &&
            type.pointee_type->kind == TypeKind::File;
    }

}
