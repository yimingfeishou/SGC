#include "codegen.hpp"
#include "codegen_detail.hpp"
#include "../semantic/constant_folding.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <system_error>

using namespace gallt::AST;

namespace gallt {
    using namespace codegen_detail;

    bool CodeGenerator::gen_operator_call(AST::Expression* expr, ExprValue& out) {
        auto found = resolved_operators_.find(expr);
        if (found == resolved_operators_.end() || found->second == nullptr) {
            return false;
        }
        AST::FunctionDefinition* callee = found->second;
        std::vector<AST::Expression*> final_arguments;
        bool postfix_dummy = false;
        if (auto* comp = dynamic_cast<AST::ComparisonExpression*>(expr)) {
            final_arguments = { comp->left.get(), comp->right.get() };
        }
        else if (auto* add = dynamic_cast<AST::AdditiveExpression*>(expr)) {
            final_arguments = { add->left.get(), add->right.get() };
        }
        else if (auto* mul = dynamic_cast<AST::MultiplicativeExpression*>(expr)) {
            final_arguments = { mul->left.get(), mul->right.get() };
        }
        else if (auto* bit = dynamic_cast<AST::BitwiseExpression*>(expr)) {
            final_arguments = { bit->left.get(), bit->right.get() };
        }
        else if (auto* shift = dynamic_cast<AST::ShiftExpression*>(expr)) {
            final_arguments = { shift->left.get(), shift->right.get() };
        }
        else if (auto* cond = dynamic_cast<AST::ConditionalExpression*>(expr)) {
            final_arguments = { cond->condition.get(), cond->then_expr.get(),
                cond->else_expr.get() };
        }
        else if (auto* pow = dynamic_cast<AST::PowerExpression*>(expr)) {
            final_arguments = { pow->left.get(), pow->right.get() };
        }
        else if (auto* land = dynamic_cast<AST::LogicalAndExpression*>(expr)) {
            final_arguments = { land->left.get(), land->right.get() };
        }
        else if (auto* lor = dynamic_cast<AST::LogicalOrExpression*>(expr)) {
            final_arguments = { lor->left.get(), lor->right.get() };
        }
        else if (auto* unary = dynamic_cast<AST::UnaryExpression*>(expr)) {
            final_arguments = { unary->operand.get() };
        }
        else if (auto* post = dynamic_cast<AST::PostfixExpression*>(expr)) {
            final_arguments = { post->base.get() };
            postfix_dummy = post->op == AST::PostfixExpression::Operator::Increment ||
                post->op == AST::PostfixExpression::Operator::Decrement;
            if (post->op == AST::PostfixExpression::Operator::Subscript &&
                post->subscript_expr != nullptr) {
                final_arguments.push_back(post->subscript_expr.get());
            }
        }
        else if (auto* assign = dynamic_cast<AST::AssignmentExpression*>(expr)) {
            if (assign->op == AST::AssignmentExpression::Operator::Assign) {
                return false;
            }
            final_arguments = { assign->left.get(), assign->right.get() };
        }
        else {
            return false;
        }
        out = emit_operator_invocation(callee, final_arguments, postfix_dummy,
            expr->location);
        return true;
    }

    CodeGenerator::ExprValue CodeGenerator::emit_operator_invocation(
        AST::FunctionDefinition* callee,
        std::vector<AST::Expression*>& final_arguments, bool postfix_dummy,
        SourceLocation loc) {
        if (postfix_dummy && callee->parameters.size() != 2) {
            postfix_dummy = false;
        }
        std::vector<std::unique_ptr<AST::Expression>> address_wrappers;
        std::vector<AST::UnaryExpression*> borrowed_wrappers;
        for (std::size_t i = 0; i < final_arguments.size(); ++i) {
            AST::Expression* operand = final_arguments[i];
            if (operand == nullptr || i >= callee->parameters.size()) {
                continue;
            }
            if (callee->parameters[i].kind != TypeKind::Pointer) {
                continue;
            }
            AST::Type operand_type = resolved_type(operand);
            if (operand_type.kind == TypeKind::Pointer ||
                operand_type.kind == TypeKind::Array ||
                operand_type.kind == TypeKind::Void) {
                continue;
            }
            if (!operand->is_lvalue()) {
                continue;
            }
            auto wrapper = std::make_unique<AST::UnaryExpression>(operand->location,
                AST::UnaryExpression::Operator::AddressOf,
                std::unique_ptr<AST::Expression>(operand));
            AST::UnaryExpression* wrapper_ptr = wrapper.get();
            expression_types_[wrapper_ptr] = AST::Type::make_pointer(
                std::make_shared<AST::Type>(operand_type));
            final_arguments[i] = wrapper_ptr;
            borrowed_wrappers.push_back(wrapper_ptr);
            address_wrappers.push_back(std::move(wrapper));
        }
        if (postfix_dummy) {
            lexeme_pool_.push_back("0");
            Token dummy(TokenType::IntegerLiteral, loc,
                std::string_view(lexeme_pool_.back()));
            auto dummy_node = std::make_unique<AST::PrimaryExpression>(loc, dummy);
            final_arguments.push_back(dummy_node.get());
            address_wrappers.push_back(std::move(dummy_node));
        }
        lexeme_pool_.push_back(callee->name);
        auto callee_name = std::make_unique<AST::PrimaryExpression>(loc,
            std::string_view(lexeme_pool_.back()));
        auto* callee_ptr = callee_name.get();
        auto call = std::make_unique<AST::PostfixExpression>(loc,
            std::unique_ptr<AST::Expression>(callee_name.release()),
            AST::PostfixExpression::Operator::FunctionCall);
        call->borrowed_arguments = final_arguments;
        AST::PostfixExpression* call_ptr = call.get();
        operator_extra_nodes_.push_back(std::move(call));
        for (auto& wrapper : address_wrappers) {
            operator_extra_nodes_.push_back(std::move(wrapper));
        }
        resolved_functions_[callee_ptr] = callee;
        expression_types_[call_ptr] = callee->return_type;
        ExprValue result = gen_postfix(call_ptr);
        for (AST::UnaryExpression* wrapper : borrowed_wrappers) {
            if (wrapper->operand != nullptr) {
                wrapper->operand.release();
            }
        }
        return result;
    }

    CodeGenerator::ExprValue CodeGenerator::gen_expr(AST::Expression* expr) {
        if (expr == nullptr) {
            return ExprValue{};
        }
        {
            ExprValue operator_value;
            if (gen_operator_call(expr, operator_value)) {
                return operator_value;
            }
        }
        if (auto* assign = dynamic_cast<AST::AssignmentExpression*>(expr)) {
            ExprValue left = gen_expr(assign->left.get());
            if (left.address.empty()) return ExprValue{};

            if (assign->op == AST::AssignmentExpression::Operator::Assign) {
                bool is_move = false;
                AST::Expression* source_expr = assign->right.get();
                if (auto* cm = dynamic_cast<AST::PrimaryExpression*>(source_expr)) {
                    if (cm->kind == AST::PrimaryExpression::Kind::CopyMove) {
                        source_expr = cm->paren_expr.get();
                    }
                }
                if (auto* cm = dynamic_cast<AST::PrimaryExpression*>(assign->right.get())) {
                    if (cm->kind == AST::PrimaryExpression::Kind::CopyMove &&
                        cm->copy_move_kind == AST::PrimaryExpression::CopyMoveKind::Move) {
                        is_move = true;
                    }
                }
                if (left.type.kind == AST::TypeKind::Struct && source_expr != nullptr) {
                    std::string source_address = operand_address(source_expr);
                    if (!source_address.empty()) {
                        if (is_move) {
                            emit_memberwise_move(left.type, left.address, source_address, true);
                        }
                        else {
                            emit_memberwise_copy(left.type, left.address, source_address, true);
                        }
                        ExprValue result;
                        result.type = left.type;
                        result.address = left.address;
                        result.is_lvalue = true;
                        result.value = new_temp("assign_result");
                        emit_line(result.value + " = load " + llvm_type(left.type) +
                            ", ptr " + left.address);
                        return result;
                    }
                }
            }
            ExprValue right = gen_expr(assign->right.get());
            if (assign->op == AST::AssignmentExpression::Operator::Assign) {
                emit_aggregate_assign(left.address, left.type, right, true);
            } else {
                std::string old_value = new_temp("old");
                std::string type_text = llvm_type(left.type);
                emit_line(old_value + " = load " + type_text + ", ptr " + left.address);
                std::string right_value = right.value;
                AST::Type rhs_type = right.type;
                if (left.type.kind == TypeKind::Pointer && rhs_type.is_integer()) {
                    std::string idx64 = convert_value(right_value, rhs_type, Type::make_int());
                    std::string idx = new_temp("pari");
                    emit_line(idx + " = sext i32 " + idx64 + " to i64");
                    std::string pointee_type = left.type.pointee_type ? llvm_type(*left.type.pointee_type) : "i8";
                    std::string result = new_temp("parr");
                    if (assign->op == AST::AssignmentExpression::Operator::MinusAssign) {
                        std::string neg = new_temp("neg");
                        emit_line(neg + " = sub i64 0, " + idx);
                        emit_line(result + " = getelementptr " + pointee_type +
                            ", ptr " + old_value + ", i64 " + neg);
                    } else {
                        emit_line(result + " = getelementptr " + pointee_type +
                            ", ptr " + old_value + ", i64 " + idx);
                    }
                    emit_line("store ptr " + result + ", ptr " + left.address);
                } else {
                    std::string result = new_temp("cmpd");
                    std::string type_text2 = llvm_type(left.type);
                    const bool integer_compound_new =
                        assign->op == AST::AssignmentExpression::Operator::AndAssign ||
                        assign->op == AST::AssignmentExpression::Operator::OrAssign ||
                        assign->op == AST::AssignmentExpression::Operator::XorAssign ||
                        assign->op == AST::AssignmentExpression::Operator::ShiftLeftAssign ||
                        assign->op == AST::AssignmentExpression::Operator::ShiftRightAssign;
                    AST::Type op_type = left.type;
                    if (left.type.is_integer()) {
                        if (!integer_compound_new || left.type.integer_bit_width() <= 8) {
                            op_type = Type::make_int();
                        }
                    }
                    std::string lv = convert_value(old_value, left.type, op_type);
                    std::string rv = convert_value(right_value, rhs_type, op_type);
                    std::string opcode;
                    std::string ir_type = llvm_type(op_type);
                    bool applied = true;
                    if (op_type.kind == TypeKind::Float || op_type.kind == TypeKind::Double) {
                        if (assign->op == AST::AssignmentExpression::Operator::PlusAssign) {
                            opcode = "fadd";
                        }
                        else if (assign->op == AST::AssignmentExpression::Operator::MinusAssign) {
                            opcode = "fsub";
                        }
                        else {
                            applied = false;
                        }
                    }
                    else {
                        switch (assign->op) {
                        case AST::AssignmentExpression::Operator::PlusAssign:
                            opcode = "add"; break;
                        case AST::AssignmentExpression::Operator::MinusAssign:
                            opcode = "sub"; break;
                        case AST::AssignmentExpression::Operator::AndAssign:
                            opcode = "and"; break;
                        case AST::AssignmentExpression::Operator::OrAssign:
                            opcode = "or"; break;
                        case AST::AssignmentExpression::Operator::XorAssign:
                            opcode = "xor"; break;
                        case AST::AssignmentExpression::Operator::ShiftLeftAssign:
                            opcode = "shl"; break;
                        case AST::AssignmentExpression::Operator::ShiftRightAssign:
                            opcode = left.type.is_unsigned_integer() ? "lshr" : "ashr";
                            break;
                        default:
                            applied = false;
                            break;
                        }
                    }
                    if (applied) {
                        emit_line(result + " = " + opcode + " " + ir_type + " " +
                            lv + ", " + rv);
                    }
                    std::string converted = convert_value(result, op_type, left.type);
                    emit_line("store " + type_text2 + " " + converted + ", ptr " + left.address);
                }
            }
            ExprValue result;
            result.type = resolved_type(expr);
            result.address = left.address;
            result.is_lvalue = true;
            std::string type_text = llvm_type(result.type);
            result.value = new_temp("assignresult");
            emit_line(result.value + " = load " + type_text + ", ptr " + result.address);
            return result;
        }
        if (auto* paren = dynamic_cast<AST::PrimaryExpression*>(expr)) {
            return gen_primary(paren);
        }
        if (auto* unary = dynamic_cast<AST::UnaryExpression*>(expr)) {
            return gen_unary(unary);
        }
        if (auto* post = dynamic_cast<AST::PostfixExpression*>(expr)) {
            return gen_postfix(post);
        }
        if (auto* logical = dynamic_cast<AST::LogicalOrExpression*>(expr)) {
            ExprValue left = gen_expr(logical->left.get());
            std::string left_i1 = truth_condition(left.value, left.type);
            std::string eval_label = new_label("oreval");
            std::string true_label = new_label("ortrue");
            std::string end_label = new_label("orend");
            emit_line("br i1 " + left_i1 + ", label %" + true_label +
                ", label %" + eval_label);

            start_block(eval_label);
            ExprValue right = gen_expr(logical->right.get());
            std::string rhs_i1 = truth_condition(right.value, right.type);
            std::string rhs_i8 = new_temp("or_rhs");
            emit_line(rhs_i8 + " = zext i1 " + rhs_i1 + " to i8");
            emit_line("br label %" + end_label);

            start_block(true_label);
            emit_line("br label %" + end_label);

            start_block(end_label);
            ExprValue result;
            result.type = Type::make_bool();
            result.value = new_temp("or_result");
            emit_line(result.value + " = phi i8 [ 1, %" + true_label +
                " ], [ " + rhs_i8 + ", %" + eval_label + " ]");
            return result;
        }
        if (auto* logical_and = dynamic_cast<AST::LogicalAndExpression*>(expr)) {
            ExprValue left = gen_expr(logical_and->left.get());
            std::string left_i1 = truth_condition(left.value, left.type);
            std::string eval_label = new_label("andeval");
            std::string false_label = new_label("andfalse");
            std::string end_label = new_label("andend");
            emit_line("br i1 " + left_i1 + ", label %" + eval_label +
                ", label %" + false_label);

            start_block(eval_label);
            ExprValue right = gen_expr(logical_and->right.get());
            std::string rhs_i1 = truth_condition(right.value, right.type);
            std::string rhs_i8 = new_temp("and_rhs");
            emit_line(rhs_i8 + " = zext i1 " + rhs_i1 + " to i8");
            emit_line("br label %" + end_label);

            start_block(false_label);
            emit_line("br label %" + end_label);

            start_block(end_label);
            ExprValue result;
            result.type = Type::make_bool();
            result.value = new_temp("and_result");
            emit_line(result.value + " = phi i8 [ 0, %" + false_label +
                " ], [ " + rhs_i8 + ", %" + eval_label + " ]");
            return result;
        }
        if (auto* comp = dynamic_cast<AST::ComparisonExpression*>(expr)) {
            ExprValue l = gen_expr(comp->left.get());
            ExprValue r = gen_expr(comp->right.get());
            std::string cmp = "icmp ";
            if (l.type.kind == TypeKind::Float || r.type.kind == TypeKind::Float ||
                l.type.kind == TypeKind::Double || r.type.kind == TypeKind::Double) {
                AST::Type common = (l.type.kind == TypeKind::Double || r.type.kind == TypeKind::Double)
                    ? Type::make_double() : Type::make_float();
                std::string lv = convert_value(l.value, l.type, common);
                std::string rv = convert_value(r.value, r.type, common);
                cmp = "fcmp ";
                cmp += (common.kind == TypeKind::Double ? "double " : "float ");
                std::string op;
                switch (comp->op) {
                case AST::ComparisonExpression::Operator::Equal: op = "oeq"; break;
                case AST::ComparisonExpression::Operator::NotEqual: op = "one"; break;
                case AST::ComparisonExpression::Operator::Greater: op = "ogt"; break;
                case AST::ComparisonExpression::Operator::Less: op = "olt"; break;
                case AST::ComparisonExpression::Operator::GreaterEqual: op = "oge"; break;
                case AST::ComparisonExpression::Operator::LessEqual: op = "ole"; break;
                }
                std::string t = new_temp("fcmp");
                emit_line(t + " = fcmp " + op + " " + (common.kind == TypeKind::Double ? "double" : "float") +
                    " " + lv + ", " + rv);
                ExprValue result;
                result.type = Type::make_bool();
                result.value = new_temp("bool");
                emit_line(result.value + " = zext i1 " + t + " to i8");
                return result;
            }
            bool left_is_address = l.type.kind == TypeKind::Pointer ||
                l.type.kind == TypeKind::File || l.type.kind == TypeKind::Function;
            bool right_is_address = r.type.kind == TypeKind::Pointer ||
                r.type.kind == TypeKind::File || r.type.kind == TypeKind::Function;
            if (left_is_address || right_is_address) {
                std::string lv;
                std::string rv;
                if (left_is_address) {
                    lv = new_temp("ptrtoint_l");
                    emit_line(lv + " = ptrtoint ptr " + l.value + " to i64");
                } else {
                    lv = to_i64_value(l.value, l.type);
                }
                if (right_is_address) {
                    rv = new_temp("ptrtoint_r");
                    emit_line(rv + " = ptrtoint ptr " + r.value + " to i64");
                } else {
                    rv = to_i64_value(r.value, r.type);
                }
                std::string op;
                switch (comp->op) {
                case AST::ComparisonExpression::Operator::Equal: op = "eq"; break;
                case AST::ComparisonExpression::Operator::NotEqual: op = "ne"; break;
                case AST::ComparisonExpression::Operator::Greater: op = "sgt"; break;
                case AST::ComparisonExpression::Operator::Less: op = "slt"; break;
                case AST::ComparisonExpression::Operator::GreaterEqual: op = "sge"; break;
                case AST::ComparisonExpression::Operator::LessEqual: op = "sle"; break;
                }
                std::string t = new_temp("icmp");
                emit_line(t + " = icmp " + op + " i64 " + lv + ", " + rv);
                ExprValue result;
                result.type = Type::make_bool();
                result.value = new_temp("bool");
                emit_line(result.value + " = zext i1 " + t + " to i8");
                return result;
            }
            AST::Type common = Type::make_int();
            if (l.type.integer_bit_width() > 0 && r.type.integer_bit_width() > 0) {
                common = (l.type.promotion_rank() >= r.type.promotion_rank())
                    ? l.type : r.type;
                if (common.integer_bit_width() == 8) {
                    common = Type::make_int();
                }
            }
            if (common.kind == TypeKind::Float || common.kind == TypeKind::Double) {
                return ExprValue{};
            }
            std::string lv = convert_value(l.value, l.type, common);
            std::string rv = convert_value(r.value, r.type, common);
            const char* prefix = common.is_unsigned_integer() ? "u" : "s";
            std::string op;
            switch (comp->op) {
            case AST::ComparisonExpression::Operator::Equal: op = "eq"; break;
            case AST::ComparisonExpression::Operator::NotEqual: op = "ne"; break;
            case AST::ComparisonExpression::Operator::Greater: op = std::string(prefix) + "gt"; break;
            case AST::ComparisonExpression::Operator::Less: op = std::string(prefix) + "lt"; break;
            case AST::ComparisonExpression::Operator::GreaterEqual: op = std::string(prefix) + "ge"; break;
            case AST::ComparisonExpression::Operator::LessEqual: op = std::string(prefix) + "le"; break;
            }
            std::string t = new_temp("icmp");
            emit_line(t + " = icmp " + op + " " + llvm_type(common) + " " + lv + ", " + rv);
            ExprValue result;
            result.type = Type::make_bool();
            result.value = new_temp("bool");
            emit_line(result.value + " = zext i1 " + t + " to i8");
            return result;
        }
        if (auto* bit = dynamic_cast<AST::BitwiseExpression*>(expr)) {
            ExprValue l = gen_expr(bit->left.get());
            ExprValue r = gen_expr(bit->right.get());
            ExprValue v;
            v.type = resolved_type(expr);
            if (!l.type.is_integer() || !r.type.is_integer()) {
                return v;
            }
            AST::Type work = v.type.is_integer() ? v.type : Type::make_int();
            if (work.integer_bit_width() == 8) {
                work = Type::make_int();
            }
            std::string ir_type = llvm_type(work);
            std::string lv = convert_value(l.value, l.type, work);
            std::string rv = convert_value(r.value, r.type, work);
            std::string opcode = "and";
            if (bit->op == AST::BitwiseExpression::Operator::Xor) opcode = "xor";
            else if (bit->op == AST::BitwiseExpression::Operator::Or) opcode = "or";
            std::string tmp = new_temp("bitop");
            emit_line(tmp + " = " + opcode + " " + ir_type + " " + lv + ", " + rv);
            v.value = convert_value(tmp, work, v.type);
            return v;
        }
        if (auto* shift = dynamic_cast<AST::ShiftExpression*>(expr)) {
            ExprValue l = gen_expr(shift->left.get());
            ExprValue r = gen_expr(shift->right.get());
            ExprValue v;
            v.type = resolved_type(expr);
            if (!l.type.is_integer() || !r.type.is_integer()) {
                return v;
            }
            AST::Type work = v.type.is_integer() ? v.type : Type::make_int();
            if (work.integer_bit_width() == 8) {
                work = Type::make_int();
            }
            const bool unsigned_shift = v.type.is_unsigned_integer();
            std::string opcode;
            if (shift->op == AST::ShiftExpression::Operator::Left) {
                opcode = "shl";
            } else {
                opcode = unsigned_shift ? "lshr" : "ashr";
            }
            std::string ir_type = llvm_type(work);
            std::string lv = convert_value(l.value, l.type, work);
            std::string rv = convert_value(r.value, r.type, work);
            std::string tmp = new_temp("shiftop");
            emit_line(tmp + " = " + opcode + " " + ir_type + " " + lv + ", " + rv);
            v.value = convert_value(tmp, work, v.type);
            return v;
        }
        if (auto* cond = dynamic_cast<AST::ConditionalExpression*>(expr)) {
            ExprValue v;
            v.type = resolved_type(expr);
            ExprValue condition = gen_expr(cond->condition.get());
            std::string predicate = truth_condition(condition.value, condition.type);
            std::string slot = emit_alloca(llvm_type(v.type), "cond_slot");
            if (v.type.kind == TypeKind::String) {
                emit_line("call void @llvm.memset.p0.i64(ptr " + slot +
                    ", i8 0, i64 32, i1 false)");
            }
            else if (v.type.kind == TypeKind::Struct || v.type.kind == TypeKind::Array) {
                emit_line("call void @llvm.memset.p0.i64(ptr " + slot +
                    ", i8 0, i64 " + std::to_string(type_size(v.type)) +
                    ", i1 false)");
            }
            std::string then_label = new_label("condthen");
            std::string else_label = new_label("condelse");
            std::string end_label = new_label("condend");
            emit_line("br i1 " + predicate + ", label %" + then_label +
                ", label %" + else_label);
            start_block(then_label);
            ExprValue taken = gen_expr(cond->then_expr.get());
            emit_aggregate_assign(slot, v.type, taken);
            emit_line("br label %" + end_label);
            start_block(else_label);
            ExprValue alternative = gen_expr(cond->else_expr.get());
            emit_aggregate_assign(slot, v.type, alternative);
            emit_line("br label %" + end_label);
            start_block(end_label);
            v.address = slot;
            if (v.type.kind == TypeKind::String) {
                v.owned_string = slot;
            }
            else if (type_contains_string(v.type)) {
                register_string_cleanup(slot, v.type);
            }
            if (v.type.kind == TypeKind::Struct || v.type.kind == TypeKind::Array) {
                return v;
            }
            v.value = new_temp("condval");
            emit_line(v.value + " = load " + llvm_type(v.type) + ", ptr " + slot);
            return v;
        }
        if (auto* add = dynamic_cast<AST::AdditiveExpression*>(expr)) {
            ExprValue l = gen_expr(add->left.get());
            ExprValue r = gen_expr(add->right.get());
            AST::Type result_type = resolved_type(expr);
            if (result_type.kind == TypeKind::String) {
                return gen_binary_string_plus(add->left.get(), add->right.get());
            }
            if (l.type.kind == TypeKind::Pointer || r.type.kind == TypeKind::Pointer) {
                bool left_ptr = l.type.kind == TypeKind::Pointer;
                AST::Type ptr_type = left_ptr ? l.type : r.type;
                std::string pointee_ir = ptr_type.pointee_type ? llvm_type(*ptr_type.pointee_type) : "i8";
                std::string base_ptr = left_ptr ? l.value : r.value;
                std::string out = new_temp("ptrmath");
                if (add->op == AST::AdditiveExpression::Operator::Minus &&
                    l.type.kind == TypeKind::Pointer && r.type.kind == TypeKind::Pointer) {
                    std::string ia = new_temp("pia");
                    std::string ib = new_temp("pib");
                    emit_line(ia + " = ptrtoint ptr " + l.value + " to i64");
                    emit_line(ib + " = ptrtoint ptr " + r.value + " to i64");
                    std::string diff = new_temp("pd");
                    emit_line(diff + " = sub i64 " + ia + ", " + ib);
                    std::size_t elem_size = ptr_type.pointee_type ? type_size(*ptr_type.pointee_type) : 1u;
                    out = new_temp("ptrsub");
                    emit_line(out + " = sdiv i64 " + diff + ", " + std::to_string(elem_size));
                    std::string truncated = new_temp("ptrsubint");
                    emit_line(truncated + " = trunc i64 " + out + " to i32");
                    out = truncated;
                    ExprValue v;
                    v.type = result_type;
                    v.value = out;
                    return v;
                }
                AST::Type int_type = left_ptr ? r.type : l.type;
                std::string int_val = left_ptr ? r.value : l.value;
                std::string idx = to_i64_value(int_val, int_type);
                if (add->op == AST::AdditiveExpression::Operator::Plus) {
                    emit_line(out + " = getelementptr " + pointee_ir +
                        ", ptr " + base_ptr + ", i64 " + idx);
                } else {
                    std::string neg = new_temp("negi");
                    emit_line(neg + " = sub i64 0, " + idx);
                    emit_line(out + " = getelementptr " + pointee_ir +
                        ", ptr " + base_ptr + ", i64 " + neg);
                }
                ExprValue v;
                v.type = result_type;
                v.value = out;
                return v;
            }
            AST::Type common = result_type;
            std::string opcode;
            std::string lv = convert_value(l.value, l.type, common);
            std::string rv = convert_value(r.value, r.type, common);
            std::string ir_type = llvm_type(common);
            if (common.kind == TypeKind::Float || common.kind == TypeKind::Double) {
                opcode = add->op == AST::AdditiveExpression::Operator::Plus ? "fadd" : "fsub";
            } else {
                if (common.integer_bit_width() == 8) {
                    common = Type::make_int();
                }
                AST::Type wide = common;
                lv = convert_value(l.value, l.type, wide);
                rv = convert_value(r.value, r.type, wide);
                ir_type = llvm_type(wide);
                opcode = add->op == AST::AdditiveExpression::Operator::Plus ? "add" : "sub";
            }
            std::string tmp = new_temp("arith");
            emit_line(tmp + " = " + opcode + " " + ir_type + " " + lv + ", " + rv);
            ExprValue v;
            v.type = result_type;
            v.value = convert_value(tmp, common, result_type);
            return v;
        }
        if (auto* mul = dynamic_cast<AST::MultiplicativeExpression*>(expr)) {
            ExprValue l = gen_expr(mul->left.get());
            ExprValue r = gen_expr(mul->right.get());
            AST::Type result_type = resolved_type(expr);
            AST::Type common = result_type;
            std::string lv = convert_value(l.value, l.type, common);
            std::string rv = convert_value(r.value, r.type, common);
            std::string ir_type = llvm_type(common);
            std::string opcode;
            if (common.kind == TypeKind::Float || common.kind == TypeKind::Double) {
                opcode = mul->op == AST::MultiplicativeExpression::Operator::Multiply ? "fmul" : "fdiv";
            } else {
                if (common.integer_bit_width() == 8) {
                    common = Type::make_int();
                }
                AST::Type wide = common;
                lv = convert_value(l.value, l.type, wide);
                rv = convert_value(r.value, r.type, wide);
                ir_type = llvm_type(wide);
                const bool unsigned_op = result_type.is_unsigned_integer();
                switch (mul->op) {
                case AST::MultiplicativeExpression::Operator::Multiply: opcode = "mul"; break;
                case AST::MultiplicativeExpression::Operator::Divide:
                    opcode = unsigned_op ? "udiv" : "sdiv"; break;
                case AST::MultiplicativeExpression::Operator::Remainder:
                    opcode = unsigned_op ? "urem" : "srem"; break;
                }
            }
            std::string tmp = new_temp("mul");
            emit_line(tmp + " = " + opcode + " " + ir_type + " " + lv + ", " + rv);
            ExprValue v;
            v.type = result_type;
            v.value = convert_value(tmp, common, result_type);
            return v;
        }
        if (auto* pow = dynamic_cast<AST::PowerExpression*>(expr)) {
            ExprValue l = gen_expr(pow->left.get());
            ExprValue r = gen_expr(pow->right.get());
            AST::Type result_type = resolved_type(expr);
            if (result_type.is_integer() && l.type.is_integer() &&
                r.type.is_integer()) {
                AST::Type work_type = result_type;
                if (work_type.integer_bit_width() == 8) {
                    work_type = Type::make_int();
                }
                std::string work_ir = llvm_type(work_type);
                std::string base_addr = emit_alloca(work_ir, "powbase");
                std::string acc_addr = emit_alloca(work_ir, "powacc");
                std::string idx_addr = emit_alloca("i64", "powidx");
                emit_line("store " + work_ir + " " +
                    convert_value(l.value, l.type, work_type) + ", ptr " + base_addr);
                emit_line("store " + work_ir + " " +
                    convert_value("1", Type::make_int(), work_type) +
                    ", ptr " + acc_addr);
                emit_line("store i64 0, ptr " + idx_addr);
                std::string exponent = to_i64_value(r.value, r.type);
                std::string cond_label = new_label("powcond");
                std::string body_label = new_label("powbody");
                std::string end_label = new_label("powend");
                emit_line("br label %" + cond_label);
                start_block(cond_label);
                std::string index = new_temp("powi");
                emit_line(index + " = load i64, ptr " + idx_addr);
                std::string in_range = new_temp("powin");
                emit_line(in_range + " = icmp slt i64 " + index + ", " + exponent);
                emit_line("br i1 " + in_range + ", label %" + body_label +
                    ", label %" + end_label);
                start_block(body_label);
                std::string acc = new_temp("powaccv");
                emit_line(acc + " = load " + work_ir + ", ptr " + acc_addr);
                std::string base = new_temp("powbasev");
                emit_line(base + " = load " + work_ir + ", ptr " + base_addr);
                std::string product = new_temp("powmul");
                emit_line(product + " = mul " + work_ir + " " + acc + ", " + base);
                emit_line("store " + work_ir + " " + product + ", ptr " + acc_addr);
                std::string next_index = new_temp("pownext");
                emit_line(next_index + " = add i64 " + index + ", 1");
                emit_line("store i64 " + next_index + ", ptr " + idx_addr);
                emit_line("br label %" + cond_label);
                start_block(end_label);
                ExprValue v;
                v.type = result_type;
                std::string accumulated = new_temp("powres");
                emit_line(accumulated + " = load " + work_ir + ", ptr " + acc_addr);
                v.value = convert_value(accumulated, work_type, result_type);
                return v;
            }
            std::string ld = convert_value(l.value, l.type, Type::make_double());
            std::string rd = convert_value(r.value, r.type, Type::make_double());
            std::string tmp = new_temp("pow");
            emit_line(tmp + " = call double @pow(double " + ld + ", double " + rd + ")");
            ExprValue v;
            v.type = result_type;
            v.value = convert_value(tmp, Type::make_double(), result_type);
            return v;
        }
        return ExprValue{};
    }

    std::string CodeGenerator::to_i64_value(const std::string& value, const AST::Type& type) {
        const int bits = type.integer_bit_width();
        if (bits > 0) {
            if (bits == 64) return value;
            std::string out = new_temp("int64");
            const char* from_ir = (bits == 32) ? "i32" : "i8";
            const char* opcode = type.is_unsigned_integer() ? "zext" : "sext";
            emit_line(out + " = " + opcode + " " + from_ir + " " + value + " to i64");
            return out;
        }
        if (type.kind == TypeKind::Float) {
            std::string out = new_temp("fptosi");
            emit_line(out + " = fptosi float " + value + " to i64");
            return out;
        }
        if (type.kind == TypeKind::Double) {
            std::string out = new_temp("fptosi");
            emit_line(out + " = fptosi double " + value + " to i64");
            return out;
        }
        return value;
    }

    std::string CodeGenerator::convert_value(const std::string& value,
        const AST::Type& from, const AST::Type& to) {
        AST::Type from_type = from;
        AST::Type to_type = to;
        from_type.is_const = false;
        to_type.is_const = false;
        if (from_type == to_type) return value;
        auto int_ir = [](int bits) -> const char* {
            switch (bits) {
            case 64: return "i64";
            case 32: return "i32";
            default: return "i8";
            }
        };
        if (from.kind == TypeKind::Array &&
            (to.kind == TypeKind::Pointer || to.kind == TypeKind::Function)) {
            return value;
        }
        if (from.kind == TypeKind::Pointer || from.kind == TypeKind::Function) {
            if (to.kind == TypeKind::Pointer || to.kind == TypeKind::Function ||
                to.kind == TypeKind::File) {
                return value;
            }
            if (to.kind == TypeKind::String || to.kind == TypeKind::Struct ||
                to.kind == TypeKind::Array) {
                return "zeroinitializer";
            }
            const int to_bits = to.integer_bit_width();
            if (to_bits > 0) {
                std::string converted = new_temp("ptrtoint");
                emit_line(converted + " = ptrtoint ptr " + value + " to " +
                    int_ir(to_bits));
                return converted;
            }
            if (to.kind == TypeKind::Float || to.kind == TypeKind::Double) {
                std::string converted = new_temp("ptrtoint");
                emit_line(converted + " = ptrtoint ptr " + value + " to i64");
                std::string result = new_temp("cast");
                emit_line(result + " = sitofp i64 " + converted + " to " +
                    (to.kind == TypeKind::Float ? "float" : "double"));
                return result;
            }
            return value;
        }
        if (to.kind == TypeKind::Pointer || to.kind == TypeKind::Function ||
            to.kind == TypeKind::File || from.kind == TypeKind::File) {
            if (from.kind == TypeKind::File) return value;
            if (to.kind == TypeKind::Pointer || to.kind == TypeKind::Function) {
                std::string out = new_temp("inttoptr");
                const int from_bits = from.integer_bit_width();
                if (from_bits > 0) {
                    emit_line(out + " = inttoptr " + int_ir(from_bits) + " " + value +
                        " to ptr");
                } else {
                    emit_line(out + " = inttoptr i64 0 to ptr");
                }
                return out;
            }
        }
        std::string tmp = new_temp("cast");
        if (to.kind == TypeKind::Float) {
            if (from.kind == TypeKind::Double) {
                emit_line(tmp + " = fptrunc double " + value + " to float");
                return tmp;
            } else if (from.integer_bit_width() > 0) {
                const bool unsigned_from = from.is_unsigned_integer();
                emit_line(tmp + std::string(" = ") + (unsigned_from ? "uitofp " : "sitofp ") +
                    int_ir(from.integer_bit_width()) + " " + value + " to float");
                return tmp;
            }
            return value;
        }
        if (to.kind == TypeKind::Double) {
            if (from.kind == TypeKind::Float) {
                emit_line(tmp + " = fpext float " + value + " to double");
                return tmp;
            } else if (from.integer_bit_width() > 0) {
                const bool unsigned_from = from.is_unsigned_integer();
                emit_line(tmp + std::string(" = ") + (unsigned_from ? "uitofp " : "sitofp ") +
                    int_ir(from.integer_bit_width()) + " " + value + " to double");
                return tmp;
            }
            return value;
        }
        if (from.kind == TypeKind::Float || from.kind == TypeKind::Double) {
            const int to_bits = to.integer_bit_width();
            if (to_bits <= 0) return value;
            const bool unsigned_to = to.is_unsigned_integer();
            emit_line(tmp + std::string(" = ") + (unsigned_to ? "fptoui " : "fptosi ") +
                (from.kind == TypeKind::Float ? "float" : "double") + " " + value +
                " to " + int_ir(to_bits));
            return tmp;
        }
        const int from_bits = from.integer_bit_width();
        const int to_bits = to.integer_bit_width();
        if (from_bits <= 0 || to_bits <= 0) return value;
        if (from_bits == to_bits) return value;
        const char* from_ir = int_ir(from_bits);
        const char* to_ir = int_ir(to_bits);
        if (from_bits < to_bits) {
            const char* opcode = from.is_unsigned_integer() ? "zext" : "sext";
            emit_line(tmp + " = " + opcode + " " + from_ir + " " + value + " to " + to_ir);
        }
        else {
            emit_line(tmp + " = trunc " + from_ir + " " + value + " to " + to_ir);
        }
        return tmp;
    }

    std::string CodeGenerator::truth_condition(const std::string& value, const AST::Type& type) {
        if (type.kind == TypeKind::Bool) {
            std::string tmp = new_temp("cond");
            emit_line(tmp + " = trunc i8 " + value + " to i1");
            return tmp;
        }
        if (type.integer_bit_width() > 0) {
            std::string tmp = new_temp("cond");
            const int bits = type.integer_bit_width();
            emit_line(tmp + " = icmp ne " + std::string(bits == 64 ? "i64" :
                (bits == 32 ? "i32" : "i8")) + " " + value + ", 0");
            return tmp;
        }
        if (type.kind == TypeKind::Float) {
            std::string tmp = new_temp("cond");
            emit_line(tmp + " = fcmp une float " + value + ", 0.0");
            return tmp;
        }
        if (type.kind == TypeKind::Double) {
            std::string tmp = new_temp("cond");
            emit_line(tmp + " = fcmp une double " + value + ", 0.0");
            return tmp;
        }
        if (type.kind == TypeKind::Pointer || type.kind == TypeKind::Function ||
            type.kind == TypeKind::File) {
            std::string tmp = new_temp("cond");
            emit_line(tmp + " = icmp ne ptr " + value + ", null");
            return tmp;
        }
        return "true";
    }

    CodeGenerator::ExprValue CodeGenerator::gen_primary(AST::PrimaryExpression* expr) {
        ExprValue out;
        out.type = resolved_type(expr);
        switch (expr->kind) {
        case AST::PrimaryExpression::Kind::Literal: {
            switch (expr->literal_token.type) {
            case TokenType::IntegerLiteral: {
                std::string_view lexeme =
                    AST::Type::strip_integer_suffix(expr->literal_token.lexeme);
                int base = 10;
                if (lexeme.size() > 2 && lexeme[0] == '0' &&
                    (lexeme[1] == 'x' || lexeme[1] == 'X')) {
                    base = 16;
                    lexeme = lexeme.substr(2);
                } else if (lexeme.size() > 1 && lexeme[0] == '0') {
                    base = 8;
                    lexeme = lexeme.substr(1);
                }
                long long value = 0;
                auto res = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(),
                    value, base);
                if (res.ec == std::errc()) {
                    out.value = std::to_string(value);
                }
                else {
                    unsigned long long uvalue = 0;
                    auto ures = std::from_chars(lexeme.data(),
                        lexeme.data() + lexeme.size(), uvalue, base);
                    out.value = (ures.ec == std::errc())
                        ? std::to_string(uvalue)
                        : "0";
                }
                break;
            }
            case TokenType::FloatLiteral: {
                std::string text = std::string(expr->literal_token.lexeme);
                bool single_precision = false;
                if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) {
                    text.pop_back();
                    single_precision = true;
                }
                double d = std::strtod(text.c_str(), nullptr);
                if (single_precision || resolved_type(expr).kind == TypeKind::Float) {
                    d = static_cast<double>(static_cast<float>(d));
                }
                std::ostringstream fmt;
                fmt.precision(17);
                fmt << d;
                out.value = llvm_float_constant_text(fmt.str());
                break;
            }
            case TokenType::CharLiteral: {
                std::string bytes = decode_escaped_bytes(expr->literal_token.lexeme, true);
                out.value = std::to_string(static_cast<int>(static_cast<unsigned char>(
                    bytes.empty() ? '\0' : bytes[0])));
                break;
            }
            case TokenType::BoolLiteral:
                out.value = expr->literal_token.lexeme == "true" ? "1" : "0";
                break;
            case TokenType::StringLiteral: {
                std::string type_text = llvm_type(Type::make_string());
                std::string temp = emit_alloca(type_text, "stringtemp");
                std::string bytes = decode_escaped_bytes(expr->literal_token.lexeme, false);
                auto found = string_literal_ids_.find(bytes);
                std::string global;
                if (found != string_literal_ids_.end()) {
                    global = found->second;
                } else {
                    global = "str" + std::to_string(string_literals_.size());
                    string_literal_ids_[bytes] = global;
                    string_literals_.push_back({ bytes, global });
                }
                std::string ptr = new_temp("strdata");
                std::size_t array_len = bytes.empty() ? 1u : bytes.size();
                emit_line(ptr + " = getelementptr [" + std::to_string(array_len) +
                    " x i8], ptr @" + global + ", i64 0, i64 0");
                emit_line("call void @gallt_string_init(ptr " + temp +
                    ", ptr " + ptr + ", i64 " + std::to_string(bytes.size()) + ")");
                out.value = temp;
                out.owned_string = temp;
                break;
            }
            default:
                break;
            }
            return out;
        }
        case AST::PrimaryExpression::Kind::Identifier: {
            LocalInfo* local = lookup_local(expr->identifier);
            if (local != nullptr) {
                if (local->is_constant) {
                    out.type = local->type;
                    if (!local->address.empty()) {
                        out.address = local->address;
                        if (local->type.kind == TypeKind::Array &&
                            local->type.element_type) {
                            out.value = new_temp("constdecay");
                            emit_line(out.value + " = getelementptr " +
                                llvm_type(local->type) + ", ptr " + local->address +
                                ", i64 0, i64 0");
                            return out;
                        }
                        out.is_lvalue = true;
                        out.value = new_temp("constload");
                        emit_line(out.value + " = load " + llvm_type(local->type) +
                            ", ptr " + local->address);
                        return out;
                    }
                    if (local->constant_expr != nullptr) {
                        return gen_expr(const_cast<AST::Expression*>(local->constant_expr));
                    }
                    out.value = local->constant_text;
                    return out;
                }
                out.is_lvalue = true;
                out.address = local->address;
                if (local->type.kind == TypeKind::Array) {
                    out.value = new_temp("arraydecay");
                    std::string elem_type = llvm_type(*local->type.element_type);
                    std::size_t n = local->type.array_size.value_or(0);
                    std::string array_type = "[" + std::to_string(n) + " x " + elem_type + "]";
                    emit_line(out.value + " = getelementptr " + array_type +
                        ", ptr " + local->address + ", i64 0, i64 0");
                } else {
                    std::string type_text = llvm_type(local->type);
                    out.value = new_temp("load");
                    emit_line(out.value + " = load " + type_text + ", ptr " + local->address);
                }
                return out;
            }
            {
                std::string reference = function_reference_for(expr);
                if (reference.empty()) {
                    reference = function_reference(expr->identifier);
                }
                if (!reference.empty()) {
                    out.value = reference;
                    return out;
                }
            }
            return out;
        }
        case AST::PrimaryExpression::Kind::Parens:
            return gen_expr(expr->paren_expr.get());
        case AST::PrimaryExpression::Kind::Null:
            out.value = "null";
            return out;
        case AST::PrimaryExpression::Kind::Heap: {
            AST::Type alloc_type = expr->heap_type;
            std::string count = "1";
            if (expr->heap_size) {
                ExprValue n = gen_expr(expr->heap_size.get());
                count = to_i64_value(n.value, n.type);
            }
            std::size_t elem_size = type_size(alloc_type);
            std::string total = new_temp("heapsize");
            emit_line(total + " = mul i64 " + count + ", " + std::to_string(elem_size));
            out.value = new_temp("heap");
            emit_line(out.value + " = call ptr @gallt_alloc_bytes(i64 " + total + ")");
            return out;
        }
        case AST::PrimaryExpression::Kind::Construct:
        case AST::PrimaryExpression::Kind::PlacementConstruct: {
            AST::Type constructed = expr->construct_type;
            std::string storage;
            if (expr->kind == AST::PrimaryExpression::Kind::PlacementConstruct) {
                ExprValue target = gen_expr(expr->placement_target.get());
                storage = target.value;
            }
            else {
                std::size_t elem_size = type_size(constructed);
                storage = new_temp("construct");
                emit_line(storage + " = call ptr @gallt_alloc_bytes(i64 " +
                    std::to_string(elem_size) + ")");
            }
            out.type = AST::Type::make_pointer(std::make_shared<AST::Type>(constructed));
            out.value = storage;
            if (!expr->lowered_ctor.empty()) {
                std::string ctor_name = expr->lowered_ctor;
                if (auto resolved = resolved_functions_.find(expr);
                    resolved != resolved_functions_.end() && resolved->second != nullptr) {
                    ctor_name = resolved->second->name;
                }
                std::string callee = function_reference(ctor_name);
                if (!callee.empty()) {
                    std::vector<AST::Expression*> ctor_args;
                    for (auto& arg : expr->construct_args) ctor_args.push_back(arg.get());
                    collect_constructor_defaults(ctor_name, ctor_args);
                    std::string call_text = "call void " + callee + "(ptr " + storage;
                    for (AST::Expression* arg : ctor_args) {
                        ExprValue value = gen_expr(arg);
                        std::string type_text = llvm_type(value.type);
                        if (value.type.kind == TypeKind::String) {
                            std::string addr = !value.address.empty() ? value.address : value.value;
                            std::string agg = new_temp("ctorstr");
                            emit_line(agg + " = load %struct.gallt.string, ptr " + addr);
                            value.value = agg;
                            type_text = "%struct.gallt.string";
                        }
                        call_text += ", " + type_text + " " + value.value;
                        destroy_owned_string(value);
                    }
                    call_text += ")";
                    emit_line(call_text);
                }
                return out;
            }
            AST::ArrayInitializer empty_init(expr->location,
                std::vector<std::unique_ptr<AST::Initializer>>{});
            emit_struct_brace_initialization(storage, constructed, &empty_init);
            return out;
        }
        case AST::PrimaryExpression::Kind::CopyMove: {
            AST::Type operand_type;
            std::string source_address = operand_address(expr->paren_expr.get(), &operand_type);
            if (source_address.empty() ||
                (operand_type.kind != AST::TypeKind::Struct &&
                    operand_type.kind != AST::TypeKind::String)) {
                return gen_expr(expr->paren_expr.get());
            }
            std::string temp = emit_alloca(llvm_type(operand_type), "copy_move_temp");
            emit_line("store " + llvm_type(operand_type) + " zeroinitializer, ptr " + temp);
            switch (expr->copy_move_kind) {
            case AST::PrimaryExpression::CopyMoveKind::Copy:
                emit_memberwise_copy(operand_type, temp, source_address, false);
                break;
            case AST::PrimaryExpression::CopyMoveKind::Move:
                emit_memberwise_move(operand_type, temp, source_address, false);
                break;
            case AST::PrimaryExpression::CopyMoveKind::DeepCopy:
                emit_deep_copy(operand_type, temp, source_address);
                break;
            case AST::PrimaryExpression::CopyMoveKind::ShallowCopy:
                emit_shallow_copy(operand_type, temp, source_address);
                break;
            }
            bool needs_cleanup = operand_type.kind == AST::TypeKind::String ||
                type_contains_string(operand_type);
            if (!needs_cleanup && operand_type.kind == AST::TypeKind::Struct) {
                auto def_it = struct_by_name_.find(operand_type.struct_name);
                needs_cleanup = def_it != struct_by_name_.end() && def_it->second != nullptr &&
                    def_it->second->needs_destruction;
            }
            if (needs_cleanup) {
                CleanupRecord record;
                record.type = operand_type;
                record.address = temp;
                statement_temporaries_.push_back(record);
            }
            out.type = operand_type;
            out.address = temp;
            out.is_lvalue = true;
            out.value = new_temp("copy_move_value");
            emit_line(out.value + " = load " + llvm_type(operand_type) + ", ptr " + temp);
            return out;
        }
        case AST::PrimaryExpression::Kind::QualifiedName:
            return out;
        case AST::PrimaryExpression::Kind::NamespaceQualified:
            return out;
        }
        return out;
    }

    std::string CodeGenerator::gen_address(AST::Expression* expr) {
        if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr)) {
            if (prim->kind == AST::PrimaryExpression::Kind::Identifier) {
                LocalInfo* local = lookup_local(prim->identifier);
                if (local) return local->address;
            }
            if (prim->kind == AST::PrimaryExpression::Kind::Parens && prim->paren_expr) {
                return gen_address(prim->paren_expr.get());
            }
            return std::string();
        }
        if (auto* unary = dynamic_cast<AST::UnaryExpression*>(expr)) {
            if (unary->op == AST::UnaryExpression::Operator::Dereference) {
                ExprValue p = gen_expr(unary->operand.get());
                return p.value;
            }
            return std::string();
        }
        if (auto* post = dynamic_cast<AST::PostfixExpression*>(expr)) {
            if (post->op == AST::PostfixExpression::Operator::Subscript) {
                ExprValue base = gen_expr(post->base.get());
                ExprValue idx = gen_expr(post->subscript_expr.get());
                std::string i64 = to_i64_value(idx.value, idx.type);
                std::string elem_type;
                std::string base_pointer = base.value;
                if (base.type.kind == TypeKind::Array && base.type.element_type) {
                    elem_type = llvm_type(*base.type.element_type);
                    std::string address = gen_address(post->base.get());
                    if (!address.empty()) {
                        base_pointer = address;
                    }
                } else if (base.type.kind == TypeKind::Pointer && base.type.pointee_type) {
                    elem_type = llvm_type(*base.type.pointee_type);
                } else {
                    return std::string();
                }
                std::string ptr = new_temp("indexptr");
                emit_line(ptr + " = getelementptr " + elem_type +
                    ", ptr " + base_pointer + ", i64 " + i64);
                return ptr;
            }
            if (post->op == AST::PostfixExpression::Operator::Dot ||
                post->op == AST::PostfixExpression::Operator::Arrow) {
                ExprValue base;
                bool operator_arrow = false;
                if (post->op == AST::PostfixExpression::Operator::Arrow) {
                    auto arrow_it = resolved_operators_.find(post->base.get());
                    if (arrow_it != resolved_operators_.end() &&
                        arrow_it->second != nullptr) {
                        std::vector<AST::Expression*> arrow_arguments = {
                            post->base.get()
                        };
                        ExprValue arrow_result = emit_operator_invocation(
                            arrow_it->second, arrow_arguments, false, post->location);
                        if (arrow_result.type.kind == TypeKind::Pointer) {
                            base = arrow_result;
                            operator_arrow = true;
                        }
                    }
                }
                if (!operator_arrow) {
                    base = gen_expr(post->base.get());
                }
                AST::Type struct_type;
                std::string base_addr;
                if (post->op == AST::PostfixExpression::Operator::Arrow) {
                    if (base.type.kind == TypeKind::Pointer && base.type.pointee_type) {
                        struct_type = *base.type.pointee_type;
                    }
                    base_addr = base.value;
                } else {
                    struct_type = base.type;
                    if (base.is_lvalue && !base.address.empty()) {
                        base_addr = base.address;
                    } else {
                        return std::string();
                    }
                }
                if (struct_type.kind != TypeKind::Struct) return std::string();
                auto struct_it = struct_by_name_.find(struct_type.struct_name);
                if (struct_it == struct_by_name_.end()) return std::string();
                size_t index = 0;
                bool found = false;
                for (size_t i = 0; i < struct_it->second->members.size(); ++i) {
                    if (struct_it->second->members[i].name == post->member_name) {
                        index = i;
                        found = true;
                        break;
                    }
                }
                if (!found) return std::string();
                std::string ptr = new_temp("memberptr");
                emit_line(ptr + " = getelementptr " + llvm_type(struct_type) +
                    ", ptr " + base_addr + ", i32 0, i32 " + std::to_string(index));
                return ptr;
            }
            if (post->op == AST::PostfixExpression::Operator::Cast ||
                post->op == AST::PostfixExpression::Operator::Increment ||
                post->op == AST::PostfixExpression::Operator::Decrement) {
                return std::string();
            }
        }
        return std::string();
    }

    std::string CodeGenerator::gen_pointer_value(AST::Expression* expr) {
        return gen_expr(expr).value;
    }

    CodeGenerator::ExprValue CodeGenerator::gen_unary(AST::UnaryExpression* expr) {
        ExprValue out;
        out.type = resolved_type(expr);
        if (expr->op == AST::UnaryExpression::Operator::AddressOf) {
            if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr->operand.get())) {
                if (prim->kind == AST::PrimaryExpression::Kind::Identifier) {
                    std::string reference = function_reference_for(prim);
                    if (reference.empty()) {
                        reference = function_reference(prim->identifier);
                    }
                    if (!reference.empty()) {
                        out.value = reference;
                        return out;
                    }
                }
            }
            std::string address = gen_address(expr->operand.get());
            if (!address.empty()) {
                out.value = address;
            }
            return out;
        }
        if (expr->op == AST::UnaryExpression::Operator::Dereference) {
            ExprValue p = gen_expr(expr->operand.get());
            if (p.type.kind != TypeKind::Pointer) return out;
            out.is_lvalue = true;
            out.address = p.value;
            out.type = resolved_type(expr);
            std::string type_text = llvm_type(out.type);
            out.value = new_temp("deref");
            emit_line(out.value + " = load " + type_text + ", ptr " + p.value);
            return out;
        }
        if (expr->op == AST::UnaryExpression::Operator::LogicalNot) {
            ExprValue v = gen_expr(expr->operand.get());
            std::string cond = truth_condition(v.value, v.type);
            std::string notval = new_temp("notcond");
            emit_line(notval + " = xor i1 " + cond + ", true");
            out.value = new_temp("notbool");
            emit_line(out.value + " = zext i1 " + notval + " to i8");
            return out;
        }
        if (expr->op == AST::UnaryExpression::Operator::BitwiseNot) {
            ExprValue v = gen_expr(expr->operand.get());
            if (!v.type.is_integer()) return out;
            AST::Type work = v.type.integer_bit_width() == 8 ? Type::make_int() : v.type;
            std::string operand_value = convert_value(v.value, v.type, work);
            std::string inverted = new_temp("bitnot");
            emit_line(inverted + " = xor " + llvm_type(work) + " " + operand_value + ", -1");
            out.type = resolved_type(expr);
            out.value = convert_value(inverted, work, out.type);
            return out;
        }
        if (expr->op == AST::UnaryExpression::Operator::UnaryPlus ||
            expr->op == AST::UnaryExpression::Operator::UnaryMinus) {
            ExprValue v = gen_expr(expr->operand.get());
            out.type = resolved_type(expr);
            if (expr->op == AST::UnaryExpression::Operator::UnaryPlus) {
                out.value = convert_value(v.value, v.type, out.type);
                return out;
            }
            if (out.type.kind == TypeKind::Float || out.type.kind == TypeKind::Double) {
                std::string type_text = llvm_type(out.type);
                std::string operand_value = convert_value(v.value, v.type, out.type);
                out.value = new_temp("fneg");
                emit_line(out.value + " = fneg " + type_text + " " + operand_value);
                return out;
            }
            AST::Type neg_type = v.type;
            if (neg_type.integer_bit_width() <= 0) {
                neg_type = Type::make_int();
            }
            else if (neg_type.integer_bit_width() == 8) {
                neg_type = Type::make_int();
            }
            std::string operand_value = convert_value(v.value, v.type, neg_type);
            std::string negated = new_temp("neg");
            emit_line(negated + " = sub " + llvm_type(neg_type) + " 0, " + operand_value);
            out.value = convert_value(negated, neg_type, out.type);
            return out;
        }
        if (expr->op == AST::UnaryExpression::Operator::Increment ||
            expr->op == AST::UnaryExpression::Operator::Decrement) {
            std::string address = gen_address(expr->operand.get());
            ExprValue old = gen_expr(expr->operand.get());
            if (address.empty()) return out;
            std::string one;
            std::string type_text = llvm_type(old.type);
            if (old.type.kind == TypeKind::Pointer) {
                std::string pointee = old.type.pointee_type ? llvm_type(*old.type.pointee_type) : "i8";
                std::string delta = expr->op == AST::UnaryExpression::Operator::Increment ? "1" : "-1";
                std::string ptr = new_temp("incptr");
                emit_line(ptr + " = getelementptr " + pointee + ", ptr " + old.value +
                    ", i64 " + delta);
                emit_line("store ptr " + ptr + ", ptr " + address);
                out.value = ptr;
                return out;
            }
            std::string opcode;
            if (old.type.kind == TypeKind::Float || old.type.kind == TypeKind::Double) {
                opcode = expr->op == AST::UnaryExpression::Operator::Increment ? "fadd" : "fsub";
                one = (old.type.kind == TypeKind::Double) ? "1.0" : "1.0";
            } else {
                opcode = expr->op == AST::UnaryExpression::Operator::Increment ? "add" : "sub";
                one = "1";
            }
            std::string updated = new_temp("updated");
            emit_line(updated + " = " + opcode + " " + type_text + " " + old.value + ", " + one);
            emit_line("store " + type_text + " " + updated + ", ptr " + address);
            out.value = updated;
            return out;
        }
        return out;
    }

    CodeGenerator::ExprValue CodeGenerator::gen_postfix(AST::PostfixExpression* expr) {
        ExprValue out;
        out.type = resolved_type(expr);
        switch (expr->op) {
        case AST::PostfixExpression::Operator::Subscript: {
            std::string address = gen_address(expr);
            if (address.empty()) return out;
            out.is_lvalue = true;
            out.address = address;
            if (out.type.kind == TypeKind::Array && out.type.element_type) {
                out.value = new_temp("arraydecay");
                emit_line(out.value + " = getelementptr " + llvm_type(out.type) +
                    ", ptr " + address + ", i64 0, i64 0");
                return out;
            }
            std::string type_text = llvm_type(out.type);
            out.value = new_temp("subscript_load");
            emit_line(out.value + " = load " + type_text + ", ptr " + address);
            return out;
        }
        case AST::PostfixExpression::Operator::Dot:
        case AST::PostfixExpression::Operator::Arrow: {
            std::string address = gen_address(expr);
            if (address.empty()) return out;
            out.is_lvalue = true;
            out.address = address;
            if (out.type.kind == TypeKind::Array && out.type.element_type) {
                out.value = new_temp("arraydecay");
                emit_line(out.value + " = getelementptr " + llvm_type(out.type) +
                    ", ptr " + address + ", i64 0, i64 0");
                return out;
            }
            std::string type_text = llvm_type(out.type);
            out.value = new_temp("member_load");
            emit_line(out.value + " = load " + type_text + ", ptr " + address);
            return out;
        }
        case AST::PostfixExpression::Operator::Cast: {
            ExprValue operand = gen_expr(expr->base.get());
            out.value = convert_value(operand.value, operand.type, expr->cast_type);
            return out;
        }
        case AST::PostfixExpression::Operator::Increment:
        case AST::PostfixExpression::Operator::Decrement: {
            std::string address = gen_address(expr->base.get());
            if (address.empty()) return out;
            std::string type_text = llvm_type(out.type);
            std::string old_val = new_temp("postold");
            emit_line(old_val + " = load " + type_text + ", ptr " + address);
            std::string updated = new_temp("postnew");
            std::string one = (out.type.kind == TypeKind::Float ||
                out.type.kind == TypeKind::Double) ? "1.0" : "1";
            std::string opcode;
            if (out.type.kind == TypeKind::Float || out.type.kind == TypeKind::Double) {
                opcode = expr->op == AST::PostfixExpression::Operator::Increment ? "fadd" : "fsub";
            } else if (out.type.kind == TypeKind::Pointer) {
                std::string pointee = out.type.pointee_type ? llvm_type(*out.type.pointee_type) : "i8";
                std::string step = expr->op == AST::PostfixExpression::Operator::Increment ? "1" : "-1";
                updated = new_temp("postptr");
                emit_line(updated + " = getelementptr " + pointee + ", ptr " + old_val +
                    ", i64 " + step);
                emit_line("store ptr " + updated + ", ptr " + address);
                out.value = old_val;
                out.is_lvalue = true;
                out.address = address;
                return out;
            } else {
                opcode = expr->op == AST::PostfixExpression::Operator::Increment ? "add" : "sub";
            }
            emit_line(updated + " = " + opcode + " " + type_text + " " + old_val + ", " + one);
            emit_line("store " + type_text + " " + updated + ", ptr " + address);
            out.value = old_val;
            out.is_lvalue = true;
            out.address = address;
            return out;
        }
        case AST::PostfixExpression::Operator::FunctionCall: {
            AST::PrimaryExpression* direct = nullptr;
            if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr->base.get())) {
                if (prim->kind == AST::PrimaryExpression::Kind::Identifier) {
                    direct = prim;
                }
            }
            std::string direct_name = direct ? direct->identifier : std::string();
            if (direct_name == "output") {
                emit_output_call(expr);
                return out;
            }
            if (direct_name == "input") {
                emit_input_call(expr, out);
                return out;
            }
            if (direct_name == "free") {
                emit_free_call(expr);
                return out;
            }
            if (is_file_builtin_name(direct_name)) {
                return emit_file_builtin_call(expr, direct_name);
            }
            if (is_string_builtin_name(direct_name)) {
                return emit_string_builtin_call(expr, direct_name);
            }
            if (direct_name == "size" || direct_name == "align") {
                return emit_size_align_call(expr, direct_name == "size");
            }
            if (direct != nullptr) {
                auto struct_it = struct_by_name_.find(direct->identifier);
                if (struct_it != struct_by_name_.end() && struct_it->second != nullptr &&
                    struct_it->second->constructor_names.empty() &&
                    expr->arguments.empty()) {
                    AST::StructDefinition* def = struct_it->second;
                    AST::Type temp_type = AST::Type::make_struct(direct->identifier);
                    std::string storage = emit_alloca(llvm_type(temp_type), "value_temp");
                    std::string callee = function_reference("__sgc_ctor$" + def->name);
                    if (!callee.empty()) {
                        emit_line("call void " + callee + "(ptr " + storage + ")");
                    }
                    if (def->needs_destruction) {
                        CleanupRecord record;
                        record.type = temp_type;
                        record.address = storage;
                        statement_temporaries_.push_back(record);
                    }
                    out.type = temp_type;
                    out.value = storage;
                    out.address = storage;
                    out.is_lvalue = true;
                    return out;
                }
                if (struct_it != struct_by_name_.end() && struct_it->second != nullptr &&
                    !struct_it->second->constructor_names.empty()) {
                    AST::StructDefinition* def = struct_it->second;
                    AST::Type temp_type = AST::Type::make_struct(direct->identifier);
                    std::string storage = emit_alloca(llvm_type(temp_type), "value_temp");
                    std::string resolved_ctor_name;
                    if (auto resolved = resolved_functions_.find(direct);
                        resolved != resolved_functions_.end() && resolved->second != nullptr) {
                        resolved_ctor_name = resolved->second->name;
                    }
                    std::size_t index = 0;
                    for (std::size_t i = 0; i < def->constructor_names.size(); ++i) {
                        const std::vector<AST::Type>& params = def->constructor_param_types[i];
                        std::size_t required = params.size();
                        auto fit = function_by_name_.find(def->constructor_names[i]);
                        if (fit != function_by_name_.end() && fit->second != nullptr) {
                            const std::vector<std::unique_ptr<AST::Expression>>& defaults =
                                fit->second->param_defaults;
                            for (std::size_t k = defaults.size(); k > 0; --k) {
                                if (defaults[k - 1] != nullptr) required = k - 1;
                                else break;
                            }
                        }
                        if (expr->arguments.size() <= params.size() &&
                            expr->arguments.size() >= required) {
                            index = i;
                            break;
                        }
                    }
                    std::string ctor_name = resolved_ctor_name.empty()
                        ? def->constructor_names[index] : resolved_ctor_name;
                    std::string callee = function_reference(ctor_name);
                    if (!callee.empty()) {
                        std::vector<AST::Expression*> ctor_args;
                        for (auto& arg : expr->arguments) ctor_args.push_back(arg.get());
                        collect_constructor_defaults(ctor_name, ctor_args);
                        std::string call_text = "call void " + callee + "(ptr " + storage;
                        const std::vector<AST::Type>& params =
                            def->constructor_param_types[index];
                        for (std::size_t i = 0; i < ctor_args.size(); ++i) {
                            ExprValue value = gen_expr(ctor_args[i]);
                            AST::Type want = i < params.size() ? params[i] : value.type;
                            if (aggregate_parameter_uses_pointer(want) &&
                                (value.type.kind == TypeKind::Struct ||
                                    value.type.kind == TypeKind::String)) {
                                call_text += ", ptr " +
                                    aggregate_argument_pointer(want, value);
                            }
                            else {
                                call_text += ", " + llvm_type(want) + " " +
                                    convert_value(value.value, value.type, want);
                            }
                            destroy_owned_string(value);
                        }
                        call_text += ")";
                        emit_line(call_text);
                    }
                    if (def->needs_destruction) {
                        CleanupRecord record;
                        record.type = temp_type;
                        record.address = storage;
                        statement_temporaries_.push_back(record);
                    }
                    out.type = temp_type;
                    out.value = storage;
                    out.address = storage;
                    out.is_lvalue = true;
                    return out;
                }
            }
            if (direct == nullptr) {
                if (auto* member_access = dynamic_cast<AST::PostfixExpression*>(expr->base.get())) {
                    if ((member_access->op == AST::PostfixExpression::Operator::Dot ||
                        member_access->op == AST::PostfixExpression::Operator::Arrow) &&
                        member_access->member_name == "destructor") {
                        AST::Type owner = resolved_type(member_access->base.get());
                        if (member_access->op == AST::PostfixExpression::Operator::Arrow &&
                            owner.kind == TypeKind::Pointer && owner.pointee_type) {
                            owner = *owner.pointee_type;
                        }
                        if (owner.kind == TypeKind::Struct) {
                            ExprValue base_value = gen_expr(member_access->base.get());
                            std::string address = base_value.value;
                            if (member_access->op == AST::PostfixExpression::Operator::Dot) {
                                std::string object_address =
                                    gen_address(member_access->base.get());
                                if (!object_address.empty()) address = object_address;
                            }
                            auto it = struct_by_name_.find(owner.struct_name);
                            if (it != struct_by_name_.end() && it->second != nullptr) {
                                AST::StructDefinition* def = it->second;
                                std::string symbol = def->destructor_name;
                                if (symbol.empty()) {
                                    symbol = "__sgc_dtor$" + def->name;
                                }
                                std::string callee = function_reference(symbol);
                                if (!callee.empty() && !address.empty()) {
                                    emit_line("call void " + callee + "(ptr " + address + ")");
                                }
                            }
                        }
                        return out;
                    }
                }
            }

            AST::Type return_type = out.type;
            AST::Type func_type;
            std::vector<AST::Type> params;
            std::string callee;
            bool pointer_call = false;
            if (!direct_name.empty()) {
                if (direct_name == "main") {
                    callee = "@main";
                } else {
                    if (AST::PrimaryExpression* callee_node =
                        dynamic_cast<AST::PrimaryExpression*>(expr->base.get())) {
                        auto resolved = resolved_functions_.find(callee_node);
                        if (resolved != resolved_functions_.end() && resolved->second != nullptr) {
                            const AST::FunctionDefinition* f = resolved->second;
                            callee = source_function_symbol(f->name);
                            params = f->parameters;
                            func_type = AST::Type::make_function(
                                std::make_shared<AST::Type>(f->return_type), params);
                        }
                        else {
                            auto resolved_ext = resolved_externs_.find(callee_node);
                            if (resolved_ext != resolved_externs_.end() &&
                                resolved_ext->second != nullptr) {
                                const AST::ExternDeclaration* e = resolved_ext->second;
                                callee = "@" + extern_ir_symbol(e);
                                params = e->parameters;
                                func_type = AST::Type::make_function(
                                    std::make_shared<AST::Type>(e->return_type), params);
                            }
                        }
                    }
                    if (!callee.empty()) {
                    }
                    else {
                    auto fit = function_by_name_.find(direct_name);
                    if (fit != function_by_name_.end()) {
                        AST::FunctionDefinition* f = fit->second;
                        callee = source_function_symbol(f->name);
                        params = f->parameters;
                        func_type = AST::Type::make_function(
                            std::make_shared<AST::Type>(f->return_type), params);
                    } else {
                        auto eit = extern_by_name_.find(direct_name);
                        if (eit != extern_by_name_.end()) {
                            AST::ExternDeclaration* e = eit->second;
                            callee = "@" + extern_ir_symbol(e);
                            params = e->parameters;
                            func_type = AST::Type::make_function(
                                std::make_shared<AST::Type>(e->return_type), params);
                        }
                    }
                    }
                    if (callee.empty()) {
                        direct_name.clear();
                        ExprValue base = gen_expr(expr->base.get());
                        if (base.type.kind == TypeKind::Function) {
                            func_type = base.type;
                            params = func_type.parameter_types;
                        } else if (base.type.kind == TypeKind::Pointer &&
                            base.type.pointee_type &&
                            base.type.pointee_type->kind == TypeKind::Function) {
                            func_type = *base.type.pointee_type;
                            params = func_type.parameter_types;
                        }
                        pointer_call = true;
                        callee = base.value;
                    }
                }
            } else {
                ExprValue base = gen_expr(expr->base.get());
                if (base.type.kind == TypeKind::Function) {
                    func_type = base.type;
                    params = func_type.parameter_types;
                } else if (base.type.kind == TypeKind::Pointer && base.type.pointee_type &&
                    base.type.pointee_type->kind == TypeKind::Function) {
                    func_type = *base.type.pointee_type;
                    params = func_type.parameter_types;
                }
                pointer_call = true;
                callee = base.value;
            }
            if (callee.empty() || func_type.return_type == nullptr) {
                return out;
            }
            return_type = *func_type.return_type;

           std::vector<std::string> ir_args;
            std::vector<std::string> owned_args;
            std::vector<AST::Expression*> all_args;
            if (!expr->arguments.empty() || !expr->appended_defaults.empty()) {
                all_args.reserve(expr->arguments.size() + expr->appended_defaults.size());
                for (auto& a : expr->arguments) all_args.push_back(a.get());
                for (AST::Expression* d : expr->appended_defaults) all_args.push_back(d);
            }
            else {
                all_args = expr->borrowed_arguments;
            }
            for (size_t i = 0; i < all_args.size(); ++i) {
                ExprValue arg = gen_expr(all_args[i]);
                if (!arg.owned_string.empty()) {
                    owned_args.push_back(arg.owned_string);
                }
                AST::Type want = (i < params.size()) ? params[i] : arg.type;
                bool extern_call = !direct_name.empty() && function_is_extern(direct_name);
                const bool aggregate_argument =
                    aggregate_parameter_uses_pointer(want) &&
                    (arg.type.kind == TypeKind::Struct ||
                        arg.type.kind == TypeKind::String);
                if (want.kind == TypeKind::String && extern_call) {
                    std::string addr = !arg.address.empty() ? arg.address : arg.value;
                    ir_args.push_back(string_cstr_pointer(addr));
                } else if (aggregate_argument) {
                    ir_args.push_back(aggregate_argument_pointer(want, arg));
                } else {
                    ir_args.push_back(convert_value(arg.value, arg.type, want));
                }
            }

            std::string ret_ir = llvm_type(return_type);
            if (return_type.kind == TypeKind::Function) ret_ir = "ptr";
            if (pointer_call && !callee.empty()) {
                emit_line("call void @gallt_check_fptr(ptr " + callee + ")");
            }
            bool extern_call = !direct_name.empty() && function_is_extern(direct_name);
            bool sret_call = returns_via_sret(return_type) && !extern_call;
            std::string sret_storage;
            if (sret_call) {
                if (!pending_sret_destination_.empty()) {
                    sret_storage = pending_sret_destination_;
                    pending_sret_destination_.clear();
                }
                else {
                    sret_storage = emit_alloca(llvm_type(return_type), "sret_temp");
                    bool needs_cleanup = type_contains_string(return_type);
                    if (!needs_cleanup) {
                        auto def_it = struct_by_name_.find(return_type.struct_name);
                        needs_cleanup = def_it != struct_by_name_.end() &&
                            def_it->second != nullptr && def_it->second->needs_destruction;
                    }
                    if (needs_cleanup) {
                        CleanupRecord record;
                        record.type = return_type;
                        record.address = sret_storage;
                        statement_temporaries_.push_back(record);
                    }
                }
                ret_ir = "void";
            }
            std::string call_text = "call " + ret_ir + " " + callee + "(";
            if (sret_call) {
                call_text += "ptr " + sret_storage;
            }
            for (size_t i = 0; i < ir_args.size(); ++i) {
                if (i != 0 || sret_call) call_text += ", ";
                AST::Type want = (i < params.size()) ? params[i] : out.type;
                if (want.kind == TypeKind::Function) want = Type::make_pointer(
                    std::make_shared<Type>(Type::make_void()));
                std::string want_type = llvm_type(want);
                if (direct_name.empty()) {
                    want_type = i < params.size() ? parameter_ir_type(params[i]) : "ptr";
                } else if (!direct_name.empty() && function_is_extern(direct_name) &&
                    want.kind == TypeKind::String) {
                    want_type = "ptr";
                } else if (aggregate_parameter_uses_pointer(want)) {
                    want_type = "ptr";
                }
                call_text += want_type + " " + ir_args[i];
            }
            call_text += ")";
            if (sret_call) {
                emit_line(call_text);
                out.type = return_type;
                out.address = sret_storage;
                out.is_lvalue = true;
                out.value = new_temp("sret_value");
                emit_line(out.value + " = load " + llvm_type(return_type) +
                    ", ptr " + sret_storage);
            }
            else if (return_type.kind == TypeKind::Void) {
                emit_line(call_text);
            } else {
                out.value = new_temp("callresult");
                emit_line(out.value + " = " + call_text);
                if (return_type.kind == TypeKind::String) {
                    std::string storage = emit_alloca("%struct.gallt.string", "string_return");
                    emit_line("store %struct.gallt.string " + out.value +
                        ", ptr " + storage);
                    out.value = storage;
                    out.address = storage;
                    out.owned_string = storage;
                }
            }
            for (const std::string& owned : owned_args) {
                emit_line("call void @gallt_string_destroy(ptr " + owned + ")");
            }
            return out;
        }
        }
        return out;
    }

    std::string CodeGenerator::string_cstr_pointer(const std::string& value) {
        std::string tmp = new_temp("cstr");
        emit_line(tmp + " = call ptr @gallt_string_cstr(ptr " + value + ")");
        return tmp;
    }

    CodeGenerator::ExprValue CodeGenerator::gen_binary_string_plus(
        AST::Expression* left, AST::Expression* right) {
        ExprValue l = gen_expr(left);
        ExprValue r = gen_expr(right);
        ExprValue out;
        out.type = Type::make_string();
        bool ls_owned = false;
        bool rs_owned = false;
        std::string ls = string_value_or_converted(l, &ls_owned);
        std::string rs = string_value_or_converted(r, &rs_owned);
        out.value = emit_alloca(llvm_type(Type::make_string()), "concat_result");
        emit_line("call void @llvm.memset.p0.i64(ptr " + out.value +
            ", i8 0, i64 32, i1 false)");
        emit_line("call void @gallt_string_concat(ptr " + out.value +
            ", ptr " + ls + ", ptr " + rs + ")");
        if (ls_owned) {
            emit_line("call void @gallt_string_destroy(ptr " + ls + ")");
        }
        if (rs_owned) {
            emit_line("call void @gallt_string_destroy(ptr " + rs + ")");
        }
        destroy_owned_string(l);
        destroy_owned_string(r);
        out.owned_string = out.value;
        return out;
    }

    std::string CodeGenerator::string_value_or_converted(const ExprValue& v, bool* owned_temp) {
        if (owned_temp != nullptr) *owned_temp = false;
        if (v.type.kind == TypeKind::String) {
            return !v.address.empty() ? v.address : v.value;
        }
        std::string result = emit_alloca(llvm_type(Type::make_string()), "scalarstring");
        if (owned_temp != nullptr) *owned_temp = true;
        switch (v.type.kind) {
        case TypeKind::Int:
            emit_line("call void @gallt_string_from_i32(ptr " + result +
                ", i32 " + v.value + ")");
            break;
        case TypeKind::Uint:
            emit_line("call void @gallt_string_from_u32(ptr " + result +
                ", i32 " + v.value + ")");
            break;
        case TypeKind::Lint:
            emit_line("call void @gallt_string_from_i64(ptr " + result +
                ", i64 " + v.value + ")");
            break;
        case TypeKind::Luint:
            emit_line("call void @gallt_string_from_u64(ptr " + result +
                ", i64 " + v.value + ")");
            break;
        case TypeKind::Float:
            emit_line("call void @gallt_string_from_f32(ptr " + result +
                ", float " + v.value + ")");
            break;
        case TypeKind::Double:
            emit_line("call void @gallt_string_from_f64(ptr " + result +
                ", double " + v.value + ")");
            break;
        case TypeKind::Char:
            emit_line("call void @gallt_string_from_char(ptr " + result +
                ", i8 " + v.value + ")");
            break;
        case TypeKind::Uchar:
            emit_line("call void @gallt_string_from_char(ptr " + result +
                ", i8 " + v.value + ")");
            break;
        case TypeKind::Bool:
            emit_line("call void @gallt_string_from_bool(ptr " + result +
                ", i8 " + v.value + ")");
            break;
        default:
            break;
        }
        return result;
    }

}
