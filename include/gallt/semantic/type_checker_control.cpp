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

    void TypeChecker::check_if_statement(AST::IfStatement* if_stmt) {
        if (if_stmt->condition) {
            AST::Type cond_type = check_expression(if_stmt->condition.get());

            if (!is_bool_type(cond_type)) {
                report_error(if_stmt->condition->location, ErrorCode::ConditionNotBoolean,
                    "if condition must be boolean type, got '" + cond_type.to_string() + "'");
            }
        }

        if (if_stmt->then_block) {
            check_statement(if_stmt->then_block.get());
        }

        if (if_stmt->else_block) {
            check_statement(if_stmt->else_block.get());
        }
    }

    void TypeChecker::check_for_statement(AST::ForStatement* for_stmt) {
        enter_scope();

        if (for_stmt->init) {
            check_statement(for_stmt->init.get());
        }

        if (for_stmt->condition) {
            AST::Type cond_type = check_expression(for_stmt->condition.get());

            if (!is_bool_type(cond_type)) {
                report_error(for_stmt->condition->location, ErrorCode::ConditionNotBoolean,
                    "for condition must be boolean type, got '" + cond_type.to_string() + "'");
            }
        }

        if (for_stmt->step) {
            check_expression(for_stmt->step.get());
        }

        loop_depth_++;

        if (for_stmt->body) {
            check_statement(for_stmt->body.get());
        }

        loop_depth_--;
        exit_scope();
    }

    void TypeChecker::check_while_statement(AST::WhileStatement* while_stmt) {
        if (while_stmt->condition) {
            AST::Type cond_type = check_expression(while_stmt->condition.get());

            if (!is_bool_type(cond_type)) {
                report_error(while_stmt->condition->location, ErrorCode::ConditionNotBoolean,
                    "while condition must be boolean type, got '" + cond_type.to_string() + "'");
            }
        }

        loop_depth_++;

        if (while_stmt->body) {
            check_statement(while_stmt->body.get());
        }

        loop_depth_--;
    }

    void TypeChecker::check_break_statement(AST::BreakStatement* break_stmt) {
        if (loop_depth_ == 0) {
            report_error(break_stmt->location, ErrorCode::BreakOutsideLoop,
                "break statement must be inside a loop");
        }
    }

    void TypeChecker::check_return_statement(AST::ReturnStatement* return_stmt) {
        if (current_function_ == nullptr) {
            report_error(return_stmt->location, ErrorCode::ReturnOutsideFunction,
                "return statement outside function");
            return;
        }

        AST::Type expected = current_function_->return_type;

        if (return_stmt->value) {
            const AST::Type* saved_expected = expected_type_;
            expected_type_ = &expected;
            AST::Type actual = check_expression(return_stmt->value.get());
            expected_type_ = saved_expected;

            if (expected.kind == TypeKind::Struct && actual.kind == TypeKind::Struct &&
                actual.struct_name == expected.struct_name) {
                if (is_move_expression(return_stmt->value.get())) {
                    if (!type_is_movable(expected)) {
                        diag_.report_error_template(return_stmt->location,
                            ErrorCode::NoMoveViolation, { expected.struct_name });
                    }
                } else if (!type_is_copyable(expected)) {
                    diag_.report_error_template(return_stmt->location,
                        ErrorCode::NoCopyViolation, { expected.struct_name });
                }
            }

            if (expected.kind == TypeKind::Void) {
                report_error(return_stmt->location, ErrorCode::VoidFunctionReturnsValue,
                    "void function cannot return a value");
            } else {
                if (!can_implicit_convert(actual, expected)) {
                    report_error(return_stmt->location, ErrorCode::FunctionReturnTypeMismatch,
                        "return type '" + actual.to_string() +
                        "' does not match function return type '" + expected.to_string() + "'");
                }
            }
        } else {
            if (expected.kind != TypeKind::Void) {
                report_error(return_stmt->location, ErrorCode::FunctionReturnTypeMismatch,
                    "non-void function must return a value");
            }
        }
    }

    void TypeChecker::check_expression_statement(AST::ExpressionStatement* expr_stmt) {
        if (expr_stmt->expr) {
            check_expression(expr_stmt->expr.get(), true);
        }
    }

}
