#include "../semantic/type_checker.hpp"
#include "../semantic/diagnosed_registry.hpp"
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

    namespace {
        bool statement_contains_fallthrough(const AST::Statement* stmt) {
            if (stmt == nullptr) { return false; }
            if (dynamic_cast<const AST::FallthroughStatement*>(stmt) != nullptr) {
                return true;
            }
            if (dynamic_cast<const AST::SwitchCaseStatement*>(stmt) != nullptr) {
                return false;
            }
            if (auto* block = dynamic_cast<const AST::Block*>(stmt)) {
                for (const std::unique_ptr<AST::Statement>& child : block->statements) {
                    if (statement_contains_fallthrough(child.get())) { return true; }
                }
                return false;
            }
            if (auto* if_stmt = dynamic_cast<const AST::IfStatement*>(stmt)) {
                return statement_contains_fallthrough(if_stmt->then_block.get()) ||
                    statement_contains_fallthrough(if_stmt->else_block.get());
            }
            if (auto* for_stmt = dynamic_cast<const AST::ForStatement*>(stmt)) {
                return statement_contains_fallthrough(for_stmt->init.get()) ||
                    statement_contains_fallthrough(for_stmt->body.get());
            }
            if (auto* while_stmt = dynamic_cast<const AST::WhileStatement*>(stmt)) {
                return statement_contains_fallthrough(while_stmt->body.get());
            }
            return false;
        }

        unsigned long long integer_bit_mask(int width) {
            return width >= 64 ? ~0ull : ((1ull << width) - 1);
        }

        unsigned long long convert_constant_bits(long long value,
            const AST::Type& from, const AST::Type& to) {
            const int from_bits = from.integer_bit_width();
            const int to_bits = to.integer_bit_width();
            if (from_bits <= 0 || to_bits <= 0) {
                return static_cast<unsigned long long>(value);
            }
            const unsigned long long bits =
                static_cast<unsigned long long>(value) & integer_bit_mask(from_bits);
            unsigned long long full = bits;
            if (from_bits < 64 && !from.is_unsigned_integer() &&
                (bits & (1ull << (from_bits - 1))) != 0) {
                full = bits | ~integer_bit_mask(from_bits);
            }
            return full & integer_bit_mask(to_bits);
        }
    }

    void TypeChecker::check_if_statement(AST::IfStatement* if_stmt) {
        if (if_stmt->condition) {
            AST::Type cond_type = check_expression(if_stmt->condition.get());

            if (!cond_type.is_error() && !is_bool_type(cond_type)) {
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

            if (!cond_type.is_error() && !is_bool_type(cond_type)) {
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

            if (!cond_type.is_error() && !is_bool_type(cond_type)) {
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
            report_error_template(break_stmt->location, ErrorCode::BreakOutsideLoop, {});
        }
    }

    void TypeChecker::check_continue_statement(AST::ContinueStatement* continue_stmt) {
        if (loop_depth_ == 0) {
            report_error_template(continue_stmt->location, ErrorCode::BreakOutsideLoop,
                {});
        }
    }

    void TypeChecker::check_fallthrough_statement(
        AST::FallthroughStatement* fallthrough_stmt) {
        if (switch_depth_ == 0) {
            report_error_template(fallthrough_stmt->location,
                ErrorCode::FallthroughOutsideSwitch, {});
        }
    }

    bool TypeChecker::switch_case_types_comparable(const AST::Type& left,
        const AST::Type& right) {
        if (left.is_error() || right.is_error()) { return true; }
        if (is_numeric_type(left) && is_numeric_type(right)) { return true; }
        if (left.kind == TypeKind::Pointer && right.kind == TypeKind::Pointer) {
            if (!left.pointee_type || !right.pointee_type) { return false; }
            return left.pointee_type->kind == TypeKind::Void ||
                right.pointee_type->kind == TypeKind::Void ||
                *left.pointee_type == *right.pointee_type;
        }
        if (left.kind == TypeKind::File && right.kind == TypeKind::File) {
            return true;
        }
        if (left.kind == TypeKind::Function && right.kind == TypeKind::Function) {
            return function_signatures_match(left, right);
        }
        return false;
    }

    void TypeChecker::check_switch_case_statement(AST::SwitchCaseStatement* switch_stmt) {
        AST::Type switch_type = AST::Type::make_error();
        if (switch_stmt->condition) {
            switch_type = decay_array_type(check_expression(switch_stmt->condition.get()));
        }

        const std::size_t clause_count = switch_stmt->clauses.size();
        std::vector<AST::Type> case_types(clause_count);
        std::vector<bool> case_has_condition(clause_count, false);

        for (std::size_t i = 0; i < clause_count; ++i) {
            AST::SwitchCaseStatement::Clause& clause = switch_stmt->clauses[i];
            if (!clause.is_default && clause.condition) {
                AST::Type case_type =
                    decay_array_type(check_expression(clause.condition.get()));
                case_types[i] = case_type;
                case_has_condition[i] = true;
                if (!switch_type.is_error() && !case_type.is_error() &&
                    !switch_case_types_comparable(switch_type, case_type)) {
                    report_error(clause.location, ErrorCode::BinaryOperatorTypeMismatch,
                        "switch-case condition type mismatch: cannot compare '" +
                        switch_type.to_string() + "' with '" + case_type.to_string() + "'");
                }
            }

            enter_scope();
            ++switch_depth_;
            for (std::unique_ptr<AST::Statement>& stmt : clause.statements) {
                check_statement(stmt.get());
            }
            --switch_depth_;
            exit_scope();
        }

        bool has_case_clause = false;
        for (std::size_t i = 0; i < clause_count; ++i) {
            if (case_has_condition[i]) { has_case_clause = true; break; }
        }

        bool eligible = has_case_clause && !switch_type.is_error() &&
            is_integer_type(switch_type);
        AST::Type common_type = switch_type;
        bool common_type_known = false;

        if (eligible) {
            for (std::size_t i = 0; i < clause_count; ++i) {
                if (!case_has_condition[i]) { continue; }
                if (!is_integer_type(case_types[i])) { eligible = false; break; }

                AST::Type pairwise =
                    (switch_type.promotion_rank() >= case_types[i].promotion_rank())
                        ? switch_type : case_types[i];
                if (pairwise.integer_bit_width() == 8) {
                    pairwise = AST::Type::make_int();
                }

                if (!common_type_known) {
                    common_type = pairwise;
                    common_type_known = true;
                } else if (common_type.kind != pairwise.kind) {
                    eligible = false;
                    break;
                }
            }
        }

        std::vector<unsigned long long> case_bits(clause_count, 0);
        if (eligible) {
            for (std::size_t i = 0; i < clause_count; ++i) {
                if (!case_has_condition[i]) { continue; }
                std::optional<long long> evaluated = evaluate_signed_const_integer_expression(
                    switch_stmt->clauses[i].condition.get());
                if (!evaluated.has_value()) { eligible = false; break; }
                case_bits[i] = convert_constant_bits(*evaluated, case_types[i], common_type);
            }
        }

        if (eligible) {
            for (std::size_t i = 0; i < clause_count && eligible; ++i) {
                if (!case_has_condition[i]) { continue; }
                for (std::size_t j = i + 1; j < clause_count; ++j) {
                    if (!case_has_condition[j]) { continue; }
                    if (case_bits[i] == case_bits[j]) { eligible = false; break; }
                }
            }
        }

        if (eligible) {
            for (AST::SwitchCaseStatement::Clause& clause : switch_stmt->clauses) {
                for (std::unique_ptr<AST::Statement>& stmt : clause.statements) {
                    if (statement_contains_fallthrough(stmt.get())) {
                        eligible = false;
                        break;
                    }
                }
                if (!eligible) { break; }
            }
        }

        switch_stmt->jump_table_eligible = eligible;
        if (!eligible) { return; }

        switch_stmt->jump_table_source_type = switch_type;
        switch_stmt->jump_table_type = common_type;

        for (std::size_t i = 0; i < clause_count; ++i) {
            if (!case_has_condition[i]) { continue; }
            switch_stmt->clauses[i].constant_value_known = true;
            switch_stmt->clauses[i].constant_value_bits =
                static_cast<long long>(case_bits[i]);
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

            if (actual.is_error()) {
                return;
            }

            if (expected.kind == TypeKind::Struct && actual.kind == TypeKind::Struct &&
                actual.struct_name == expected.struct_name) {
                if (is_move_source(return_stmt->value.get())) {
                    if (!type_is_movable(expected)) {
                        report_error_template(return_stmt->location,
                            ErrorCode::NoMoveViolation, { expected.struct_name });
                    }
                } else if (!type_is_copyable(expected)) {
                    report_error_template(return_stmt->location,
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
