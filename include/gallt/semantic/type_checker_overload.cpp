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

    bool TypeChecker::is_custom_type(const AST::Type& type) const {
        if (type.kind == TypeKind::Struct) {
            return true;
        }
        if (type.kind == TypeKind::Pointer && type.pointee_type) {
            return is_custom_type(*type.pointee_type);
        }
        if (type.kind == TypeKind::Array && type.element_type) {
            return is_custom_type(*type.element_type);
        }
        return false;
    }

    bool TypeChecker::validate_operator_definition(AST::FunctionDefinition* node) {
        if (node->is_conversion_operator) {
            if (node->parameters.size() != 1) {
                report_error(node->location, ErrorCode::OperatorOverloadOperandCountMismatch,
                    "operator " + node->conversion_target_type.to_string());
                return false;
            }
            if (!is_custom_type(node->parameters[0])) {
                report_error(node->location, ErrorCode::OperatorOverloadRequiresCustomType,
                    "operator " + node->conversion_target_type.to_string());
                return false;
            }
            return true;
        }
        const std::string& op = node->overloaded_operator;
        if (op == "." || op == "::" || op == "=" || op.empty()) {
            report_error(node->location, ErrorCode::OperatorCannotBeOverloaded, op);
            return false;
        }
        bool binary_operator = (op == "/" || op == "%" || op == "**" || op == "==" ||
            op == "!=" || op == ">" || op == "<" || op == ">=" || op == "<=" ||
            op == "&&" || op == "||" || op == "+=" || op == "-=" ||
            op == "^" || op == "|" || op == "<<" || op == ">>" ||
            op == "&=" || op == "|=" || op == "^=" || op == "<<=" || op == ">>=");
        std::size_t expected = binary_operator ? 2u : 1u;
        if (op == "[]") {
            expected = 2u;
        }
        if (op == "?:") {
            expected = 3u;
        }
        if (op == "->") {
            expected = 1u;
        }
        if (!binary_operator && (op == "+" || op == "-" || op == "*" || op == "&") &&
            node->parameters.size() == 2) {
            expected = 2u;
        }
        if ((op == "++" || op == "--") && node->parameters.size() == 2 &&
            node->parameters[0].kind == TypeKind::Pointer) {
            expected = 2u;
        }
        if (node->parameters.size() != expected) {
            report_error(node->location, ErrorCode::OperatorOverloadOperandCountMismatch,
                op + " (" + std::to_string(expected) + "/" +
                std::to_string(node->parameters.size()) + ")");
            return false;
        }
        bool has_custom = false;
        for (const AST::Type& param : node->parameters) {
            if (is_custom_type(param)) {
                has_custom = true;
            }
        }
        if (!has_custom) {
            report_error(node->location, ErrorCode::OperatorOverloadRequiresCustomType, op);
            return false;
        }
        if (op == "+=" || op == "-=" || op == "&=" || op == "|=" || op == "^=" ||
            op == "<<=" || op == ">>=") {
            if (node->parameters[0].kind != TypeKind::Pointer ||
                node->parameters[0].pointee_type == nullptr ||
                !is_custom_type(*node->parameters[0].pointee_type)) {
                report_error(node->location,
                    ErrorCode::ModifyingOperatorFirstParameterNotPointer, op);
                return false;
            }
        }
        if (op == "++" || op == "--") {
            if (node->parameters[0].kind != TypeKind::Pointer ||
                node->parameters[0].pointee_type == nullptr ||
                !is_custom_type(*node->parameters[0].pointee_type)) {
                report_error(node->location,
                    ErrorCode::ModifyingOperatorFirstParameterNotPointer, op);
                return false;
            }
            node->operator_postfix_dummy = node->parameters.size() == 2;
        }
        if (op == "[]") {
            if (node->return_type.kind != TypeKind::Pointer) {
                report_error(node->location, ErrorCode::SubscriptOperatorMustReturnPointer, op);
                return false;
            }
            if (node->parameters.size() == 2 && !node->parameters[1].is_integer()) {
                report_error(node->location, ErrorCode::OperatorOverloadParameterMismatch,
                    op);
                return false;
            }
        }
        if (op == "->") {
            if (node->return_type.kind != TypeKind::Pointer) {
                report_error(node->location, ErrorCode::ArrowOperatorMustReturnPointer, op);
                return false;
            }
        }
        for (const std::unique_ptr<AST::Expression>& def : node->param_defaults) {
            if (def != nullptr) {
                report_error(node->location,
                    ErrorCode::OperatorOverloadDefaultArgumentNotAllowed, op);
                return false;
            }
        }
        return true;
    }

    void TypeChecker::collect_operator_overloads() {
        for (auto& top : program_->top_levels) {
            auto* func = dynamic_cast<AST::FunctionDefinition*>(top.get());
            if (func == nullptr || !func->is_operator) {
                continue;
            }
            if (!validate_operator_definition(func)) {
                continue;
            }
            std::string key = func->is_conversion_operator
                ? std::string("operator") + func->conversion_target_type.to_string()
                : std::string("operator") + func->overloaded_operator;
            std::vector<AST::FunctionDefinition*>& bucket = operator_overloads_[key];
            for (AST::FunctionDefinition* existing : bucket) {
                if (existing->parameters.size() != func->parameters.size()) {
                    continue;
                }
                bool same = true;
                for (std::size_t i = 0; i < func->parameters.size(); ++i) {
                    if (!(existing->parameters[i] == func->parameters[i])) {
                        same = false;
                        break;
                    }
                }
                if (same) {
                    report_error(func->location, ErrorCode::OperatorOverloadRedefined,
                        func->overloaded_operator);
                    same = false;
                    break;
                }
            }
            bucket.push_back(func);
        }
    }

    AST::FunctionDefinition* TypeChecker::resolve_user_operator(const std::string& op,
        const std::vector<AST::Type>& operand_types, SourceLocation loc) {
        auto it = operator_overloads_.find("operator" + op);
        if (it == operator_overloads_.end()) {
            return nullptr;
        }
        std::unordered_set<std::string> operand_structs;
        for (const AST::Type& type : operand_types) {
            collect_struct_names_from_type(type, operand_structs);
        }

        struct Candidate {
            AST::FunctionDefinition* node = nullptr;
            std::vector<int> ranks;
        };
        std::vector<Candidate> viable;
        for (AST::FunctionDefinition* candidate : it->second) {
            if (candidate->parameters.size() != operand_types.size()) {
                continue;
            }
            if (!operand_structs.empty()) {
                std::unordered_set<std::string> declared_structs;
                for (const AST::Type& parameter : candidate->parameters) {
                    collect_struct_names_from_type(parameter, declared_structs);
                }
                bool associated = false;
                for (const std::string& name : declared_structs) {
                    if (operand_structs.count(name) != 0) {
                        associated = true;
                        break;
                    }
                }
                if (!associated) continue;
            }
            Candidate entry;
            entry.node = candidate;
            bool ok = true;
            for (std::size_t i = 0; i < operand_types.size(); ++i) {
                int rank = conversion_rank(operand_types[i], candidate->parameters[i]);
                if (rank < 0) {
                    ok = false;
                    break;
                }
                entry.ranks.push_back(rank);
            }
            if (ok) viable.push_back(std::move(entry));
        }
        if (viable.empty()) {
            return nullptr;
        }

        auto dominates = [](const std::vector<int>& a, const std::vector<int>& b) {
            bool strictly_better = false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (a[i] > b[i]) return false;
                if (a[i] < b[i]) strictly_better = true;
            }
            return strictly_better;
        };

        std::vector<std::size_t> maximal;
        for (std::size_t i = 0; i < viable.size(); ++i) {
            bool dominated = false;
            for (std::size_t j = 0; j < viable.size(); ++j) {
                if (i == j) continue;
                if (dominates(viable[j].ranks, viable[i].ranks)) {
                    dominated = true;
                    break;
                }
            }
            if (!dominated) maximal.push_back(i);
        }
        if (maximal.empty()) {
            return nullptr;
        }
        if (maximal.size() > 1) {
            report_error(loc, ErrorCode::OperatorOverloadAmbiguous, op);
            return nullptr;
        }
        return viable[maximal.front()].node;
    }

    bool TypeChecker::try_user_defined_operator(AST::Expression* expr, AST::Type& out) {
        if (operator_overloads_.empty()) {
            return false;
        }
        if (auto* cast = dynamic_cast<AST::PostfixExpression*>(expr)) {
            if (cast->op == AST::PostfixExpression::Operator::Cast) {
                AST::Type operand_type = check_expression(cast->base.get());
                if (is_custom_type(operand_type)) {
                    std::string key = std::string("operator") + cast->cast_type.to_string();
                    auto found = operator_overloads_.find(key);
                    if (found != operator_overloads_.end()) {
                        for (AST::FunctionDefinition* candidate : found->second) {
                            if (candidate->parameters.size() != 1) {
                                continue;
                            }
                            if (conversion_rank(operand_type, candidate->parameters[0]) >= 0) {
                                resolved_operators_[expr] = candidate;
                                out = candidate->return_type;
                                return true;
                            }
                        }
                    }
                }
                return false;
            }
        }
        std::string symbol;
        std::vector<AST::Expression*> operands;
        std::vector<bool> operand_needs_address;
        bool auto_wrap_pointer = false;
        bool postfix_increment = false;
        if (auto* comp = dynamic_cast<AST::ComparisonExpression*>(expr)) {
            switch (comp->op) {
            case AST::ComparisonExpression::Operator::Equal: symbol = "=="; break;
            case AST::ComparisonExpression::Operator::NotEqual: symbol = "!="; break;
            case AST::ComparisonExpression::Operator::Greater: symbol = ">"; break;
            case AST::ComparisonExpression::Operator::Less: symbol = "<"; break;
            case AST::ComparisonExpression::Operator::GreaterEqual: symbol = ">="; break;
            case AST::ComparisonExpression::Operator::LessEqual: symbol = "<="; break;
            }
            operands = { comp->left.get(), comp->right.get() };
        }
        else if (auto* add = dynamic_cast<AST::AdditiveExpression*>(expr)) {
            symbol = add->op == AST::AdditiveExpression::Operator::Plus ? "+" : "-";
            operands = { add->left.get(), add->right.get() };
        }
        else if (auto* mul = dynamic_cast<AST::MultiplicativeExpression*>(expr)) {
            switch (mul->op) {
            case AST::MultiplicativeExpression::Operator::Multiply: symbol = "*"; break;
            case AST::MultiplicativeExpression::Operator::Divide: symbol = "/"; break;
            case AST::MultiplicativeExpression::Operator::Remainder: symbol = "%"; break;
            }
            operands = { mul->left.get(), mul->right.get() };
        }
        else if (auto* bit = dynamic_cast<AST::BitwiseExpression*>(expr)) {
            switch (bit->op) {
            case AST::BitwiseExpression::Operator::And: symbol = "&"; break;
            case AST::BitwiseExpression::Operator::Xor: symbol = "^"; break;
            case AST::BitwiseExpression::Operator::Or: symbol = "|"; break;
            }
            operands = { bit->left.get(), bit->right.get() };
        }
        else if (auto* shift = dynamic_cast<AST::ShiftExpression*>(expr)) {
            symbol = shift->op == AST::ShiftExpression::Operator::Left ? "<<" : ">>";
            operands = { shift->left.get(), shift->right.get() };
        }
        else if (auto* cond = dynamic_cast<AST::ConditionalExpression*>(expr)) {
            symbol = "?:";
            operands = { cond->condition.get(), cond->then_expr.get(),
                cond->else_expr.get() };
        }
        else if (auto* pow = dynamic_cast<AST::PowerExpression*>(expr)) {
            symbol = "**";
            operands = { pow->left.get(), pow->right.get() };
        }
        else if (auto* land = dynamic_cast<AST::LogicalAndExpression*>(expr)) {
            symbol = "&&";
            operands = { land->left.get(), land->right.get() };
        }
        else if (auto* lor = dynamic_cast<AST::LogicalOrExpression*>(expr)) {
            symbol = "||";
            operands = { lor->left.get(), lor->right.get() };
        }
        else if (auto* unary = dynamic_cast<AST::UnaryExpression*>(expr)) {
            switch (unary->op) {
            case AST::UnaryExpression::Operator::LogicalNot: symbol = "!"; break;
            case AST::UnaryExpression::Operator::UnaryMinus: symbol = "-"; break;
            case AST::UnaryExpression::Operator::UnaryPlus: symbol = "+"; break;
            case AST::UnaryExpression::Operator::AddressOf: symbol = "&"; break;
            case AST::UnaryExpression::Operator::Dereference: symbol = "*"; break;
            case AST::UnaryExpression::Operator::Increment: symbol = "++"; break;
            case AST::UnaryExpression::Operator::Decrement: symbol = "--"; break;
            case AST::UnaryExpression::Operator::BitwiseNot: symbol = "~"; break;
            }
            operands = { unary->operand.get() };
            operand_needs_address = { true };
        }
        else if (auto* post = dynamic_cast<AST::PostfixExpression*>(expr)) {
            if (post->op == AST::PostfixExpression::Operator::Increment) {
                symbol = "++";
            }
            else if (post->op == AST::PostfixExpression::Operator::Decrement) {
                symbol = "--";
            }
            else if (post->op == AST::PostfixExpression::Operator::Subscript) {
                symbol = "[]";
            }
            else {
                return false;
            }
            postfix_increment = post->op == AST::PostfixExpression::Operator::Increment ||
                post->op == AST::PostfixExpression::Operator::Decrement;
            operands = { post->base.get() };
            operand_needs_address = { true };
            auto_wrap_pointer = postfix_increment ||
                post->op == AST::PostfixExpression::Operator::Subscript;
            if (post->op == AST::PostfixExpression::Operator::Subscript) {
                operands.push_back(post->subscript_expr.get());
                operand_needs_address.push_back(false);
            }
        }
        else if (auto* assign = dynamic_cast<AST::AssignmentExpression*>(expr)) {
            if (assign->op == AST::AssignmentExpression::Operator::PlusAssign) {
                symbol = "+=";
            }
            else if (assign->op == AST::AssignmentExpression::Operator::MinusAssign) {
                symbol = "-=";
            }
            else if (assign->op == AST::AssignmentExpression::Operator::AndAssign) {
                symbol = "&=";
            }
            else if (assign->op == AST::AssignmentExpression::Operator::OrAssign) {
                symbol = "|=";
            }
            else if (assign->op == AST::AssignmentExpression::Operator::XorAssign) {
                symbol = "^=";
            }
            else if (assign->op == AST::AssignmentExpression::Operator::ShiftLeftAssign) {
                symbol = "<<=";
            }
            else if (assign->op == AST::AssignmentExpression::Operator::ShiftRightAssign) {
                symbol = ">>=";
            }
            else {
                return false;
            }
            operands = { assign->left.get(), assign->right.get() };
            operand_needs_address = { true, false };
            auto_wrap_pointer = true;
        }
        if (symbol.empty()) {
            return false;
        }
        std::vector<AST::Type> probe;
        for (AST::Expression* operand : operands) {
            if (operand == nullptr) {
                probe.push_back(AST::Type::make_int());
                continue;
            }
            auto known = expression_types_.find(operand);
            if (known != expression_types_.end()) {
                probe.push_back(known->second);
            }
            else {
                probe.push_back(check_expression(operand));
            }
        }
        bool any_custom = false;
        for (const AST::Type& type : probe) {
            if (is_custom_type(type)) {
                any_custom = true;
            }
        }
        if (!any_custom) {
            return false;
        }
        (void)auto_wrap_pointer;
        std::vector<AST::Type> addressable_probe = probe;
        bool has_addressable = false;
        for (std::size_t i = 0; i < addressable_probe.size(); ++i) {
            if (i >= operand_needs_address.size() || !operand_needs_address[i]) {
                continue;
            }
            if (addressable_probe[i].kind == TypeKind::Pointer ||
                addressable_probe[i].kind == TypeKind::Array ||
                addressable_probe[i].kind == TypeKind::Void) {
                continue;
            }
            AST::Expression* operand = operands[i];
            if (operand == nullptr || !operand->is_lvalue()) {
                continue;
            }
            addressable_probe[i] = AST::Type::make_pointer(
                std::make_shared<AST::Type>(addressable_probe[i]));
            has_addressable = true;
        }
        AST::FunctionDefinition* chosen = nullptr;
        if (postfix_increment) {
            std::vector<AST::Type> binary_probe =
                has_addressable ? addressable_probe : probe;
            binary_probe.push_back(AST::Type::make_int());
            AST::FunctionDefinition* post_fix = resolve_user_operator(symbol, binary_probe,
                expr->location);
            if (post_fix != nullptr && post_fix->parameters.size() == 2) {
                operands.push_back(nullptr);
                operand_needs_address.push_back(false);
                chosen = post_fix;
            }
        }
        if (chosen == nullptr) {
            chosen = resolve_user_operator(symbol, probe, expr->location);
        }
        if (chosen == nullptr && has_addressable) {
            chosen = resolve_user_operator(symbol, addressable_probe, expr->location);
        }
        if (chosen == nullptr) {
            return false;
        }
        resolved_operators_[expr] = chosen;
        out = chosen->return_type;
        return true;
    }

    void TypeChecker::mangle_overload_set(const std::string& name) {
        std::vector<Symbol>* set = sym_table_.lookup_overloads(name);
        if (set == nullptr) return;
        for (Symbol& sym : *set) {
            const std::string base = name + "$" + overload_signature(sym.param_types);
            if (sym.function_node != nullptr) {
                auto it = mangled_functions_.find(sym.function_node);
                if (it == mangled_functions_.end()) {
                    it = mangled_functions_
                        .emplace(sym.function_node, unique_mangled_name(base)).first;
                }
                sym.function_node->name = it->second;
            }
            else if (sym.extern_node != nullptr) {
                auto it = mangled_externs_.find(sym.extern_node);
                if (it == mangled_externs_.end()) {
                    it = mangled_externs_
                        .emplace(sym.extern_node, unique_mangled_name(base)).first;
                }
                sym.extern_node->name = it->second;
            }
        }
    }

    std::string TypeChecker::overload_signature(const std::vector<AST::Type>& params) const {
        std::function<std::string(const AST::Type&)> encode = [&](const AST::Type& t) -> std::string {
            switch (t.kind) {
            case TypeKind::Int: return "i";
            case TypeKind::Lint: return "l";
            case TypeKind::Uint: return "u";
            case TypeKind::Luint: return "q";
            case TypeKind::Float: return "f";
            case TypeKind::Double: return "d";
            case TypeKind::Char: return "c";
            case TypeKind::Uchar: return "h";
            case TypeKind::Bool: return "b";
            case TypeKind::String: return "s";
            case TypeKind::File: return "F";
            case TypeKind::Void: return "v";
            case TypeKind::Pointer:
                return "P" + (t.pointee_type ? encode(*t.pointee_type) : std::string("v"));
            case TypeKind::Array:
                return "A" + (t.array_size.has_value() ? std::to_string(*t.array_size) : "u") +
                    (t.element_type ? encode(*t.element_type) : std::string("v"));
            case TypeKind::Struct: return "S" + sanitize_identifier(t.struct_name);
            case TypeKind::Function: {
                std::string out = "R" + (t.return_type ? encode(*t.return_type) : std::string("v"));
                for (const AST::Type& p : t.parameter_types) out += "_" + encode(p);
                return out;
            }
            }
            return "x";
        };
        if (params.empty()) return "void";
        std::string out;
        for (const AST::Type& p : params) {
            if (!out.empty()) out += "_";
            out += encode(p);
        }
        return out;
    }

    std::string TypeChecker::unique_mangled_name(const std::string& base) {
        std::string candidate = base;
        int suffix = 2;
        while (!used_mangled_names_.insert(candidate).second) {
            candidate = base + "$" + std::to_string(suffix++);
        }
        return candidate;
    }

    void TypeChecker::record_function_resolution(AST::PrimaryExpression* callee,
        const Symbol& symbol) {
        if (callee == nullptr) return;
        if (symbol.function_node != nullptr) {
            resolved_functions_[callee] = symbol.function_node;
        }
        else if (symbol.extern_node != nullptr) {
            resolved_externs_[callee] = symbol.extern_node;
        }
    }

    Symbol* TypeChecker::select_overload_by_target_type(std::vector<Symbol>& set,
        const AST::Type& target, const std::string& name, SourceLocation loc) {
        AST::Type target_function = target;
        if (target.kind == TypeKind::Pointer && target.pointee_type &&
            target.pointee_type->kind == TypeKind::Function) {
            target_function = *target.pointee_type;
        }
        if (target_function.kind != TypeKind::Function) return nullptr;
        Symbol* best = nullptr;
        for (Symbol& sym : set) {
            if (sym.param_types.size() != target_function.parameter_types.size()) continue;
            if (!(sym.type == *target_function.return_type)) continue;
            bool same = true;
            for (std::size_t i = 0; i < sym.param_types.size(); ++i) {
                if (!(sym.param_types[i] == target_function.parameter_types[i])) {
                    same = false;
                    break;
                }
            }
            if (!same) continue;
            if (best != nullptr) {
                report_error(loc, ErrorCode::OverloadAmbiguous, { name });
                return nullptr;
            }
            best = &sym;
        }
        if (best == nullptr) {
            report_error(loc, ErrorCode::FuncPtrTypeMismatch,
                "no overload of '" + name + "' matches the target function pointer type '" +
                target_function.to_string() + "'");
        }
        return best;
    }

    bool TypeChecker::overloads_ambiguous_by_defaults(const Symbol& a, const Symbol& b) const {
        auto required_arity = [](const Symbol& sym) -> std::size_t {
            std::size_t required = sym.param_types.size();
            if (sym.function_node != nullptr) {
                const auto& defaults = sym.function_node->param_defaults;
                for (std::size_t i = defaults.size(); i > 0; --i) {
                    if (defaults[i - 1] != nullptr) required = i - 1;
                    else break;
                }
            }
            return required;
        };
        const std::size_t a_min = required_arity(a);
        const std::size_t b_min = required_arity(b);
        const std::size_t a_max = a.param_types.size();
        const std::size_t b_max = b.param_types.size();
        const std::size_t lo = std::max(a_min, b_min);
        const std::size_t hi = std::min(a_max, b_max);
        for (std::size_t arity = lo; arity <= hi; ++arity) {
            bool identical = true;
            for (std::size_t i = 0; i < arity; ++i) {
                if (!(a.param_types[i] == b.param_types[i])) {
                    identical = false;
                    break;
                }
            }
            if (identical) return true;
        }
        return false;
    }

    Symbol* TypeChecker::resolve_constructor_overload(const std::string& struct_name,
        const std::vector<AST::Type>& arg_types, const std::vector<bool>& arg_is_null,
        SourceLocation loc) {
        std::vector<Symbol>* set = sym_table_.lookup_overloads("__sgc_ctor$" + struct_name);
        if (set == nullptr || set->empty()) {
            if (!arg_types.empty()) {
                report_error(loc, ErrorCode::FunctionArgCountMismatch,
                    "type '" + struct_name + "' has no constructor taking " +
                    std::to_string(arg_types.size()) + " argument(s)");
            }
            return nullptr;
        }
        struct Candidate {
            Symbol* symbol = nullptr;
            std::vector<int> ranks;
        };
        std::vector<Candidate> viable;
        for (Symbol& sym : *set) {
            if (sym.param_types.empty()) continue;
            const std::size_t max_params = sym.param_types.size() - 1;
            std::size_t required = max_params;
            if (sym.function_node != nullptr) {
                const auto& defaults = sym.function_node->param_defaults;
                for (std::size_t i = defaults.size(); i > 1; --i) {
                    if (defaults[i - 1] != nullptr) required = (i - 1) - 1;
                    else break;
                }
            }
            if (arg_types.size() > max_params || arg_types.size() < required) continue;
            Candidate candidate;
            candidate.symbol = &sym;
            bool ok = true;
            for (std::size_t i = 0; i < arg_types.size(); ++i) {
                int rank = conversion_rank(arg_types[i], sym.param_types[i + 1]);
                if (rank < 0 && i < arg_is_null.size() && arg_is_null[i] &&
                    sym.param_types[i + 1].kind == TypeKind::Pointer) {
                    rank = 0;
                }
                if (rank < 0) {
                    ok = false;
                    break;
                }
                candidate.ranks.push_back(rank);
            }
            if (ok) viable.push_back(std::move(candidate));
        }
        if (viable.empty()) {
            report_error(loc, ErrorCode::FunctionArgTypeMismatch,
                "no constructor of '" + struct_name + "' matches the given arguments");
            return nullptr;
        }
        int best = -1;
        bool ambiguous = false;
        for (std::size_t i = 0; i < viable.size(); ++i) {
            bool is_best = true;
            for (std::size_t j = 0; j < viable.size(); ++j) {
                if (i == j) continue;
                const std::vector<int>& a = viable[i].ranks;
                const std::vector<int>& b = viable[j].ranks;
                bool i_better = false;
                bool j_better = false;
                for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) {
                    if (a[k] < b[k]) i_better = true;
                    if (a[k] > b[k]) j_better = true;
                }
                if (!(i_better && !j_better)) {
                    is_best = false;
                    break;
                }
            }
            if (is_best) {
                if (best != -1) ambiguous = true;
                else best = static_cast<int>(i);
            }
        }
        if (ambiguous || best < 0) {
            report_error(loc, ErrorCode::SpecialMemberAmbiguous, { struct_name });
            return nullptr;
        }
        return viable[static_cast<std::size_t>(best)].symbol;
    }

    int TypeChecker::conversion_rank(const AST::Type& from, const AST::Type& to) {
        constexpr int kExact = 0;
        constexpr int kPromotion = 100;
        constexpr int kConversion = 200;
        auto arithmetic_position = [](const AST::Type& t) -> int {
            switch (t.kind) {
            case TypeKind::Char:
            case TypeKind::Bool:
                return 0;
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
        if (from == to) return 0;
        if (from.kind == TypeKind::Pointer && to.kind == TypeKind::Pointer) {
            if (from.pointee_type && to.pointee_type &&
                types_equal_modulo_const(*from.pointee_type, *to.pointee_type) &&
                const_qualification_ok(from, to)) {
                return kExact;
            }
            return can_implicit_convert(from, to) ? kConversion : -1;
        }
        const int from_pos = arithmetic_position(from);
        const int to_pos = arithmetic_position(to);
        if (from_pos >= 0 && to_pos >= 0) {
            const bool small_integer = from.kind == TypeKind::Bool ||
                from.kind == TypeKind::Char || from.kind == TypeKind::Uchar;
            const bool integral_promotion = small_integer &&
                (to.kind == TypeKind::Int || to.kind == TypeKind::Uint);
            const bool floating_promotion = from.kind == TypeKind::Float &&
                to.kind == TypeKind::Double;
            const bool promotion = integral_promotion || floating_promotion;
            int distance = std::abs(to_pos - from_pos);
            if (distance == 0) distance = 1;
            return (promotion ? kPromotion : kConversion) + distance;
        }
        if (can_implicit_convert(from, to)) return kConversion;
        return -1;
    }

    Symbol* TypeChecker::resolve_overload_call(const std::string& name,
        AST::PostfixExpression* call, AST::PrimaryExpression* callee) {
        std::vector<Symbol>* set = sym_table_.lookup_overloads(name);
        if (set == nullptr || set->empty()) return nullptr;

        std::vector<AST::Type> arg_types;
        std::vector<bool> arg_is_null;
        for (auto& arg : call->arguments) {
            arg_types.push_back(check_expression(arg.get()));
            bool is_null = false;
            if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(arg.get())) {
                if (prim->kind == AST::PrimaryExpression::Kind::Null) is_null = true;
            }
            arg_is_null.push_back(is_null);
        }
        const std::size_t provided = call->arguments.size();

        struct Candidate {
            Symbol* sym = nullptr;
            std::vector<int> ranks;
        };
        std::vector<Candidate> viable;
        for (Symbol& sym : *set) {
            const std::size_t max_params = sym.param_types.size();
            std::size_t required = max_params;
            if (sym.function_node != nullptr) {
                const auto& defaults = sym.function_node->param_defaults;
                for (std::size_t i = defaults.size(); i > 0; --i) {
                    if (defaults[i - 1]) {
                        required = i - 1;
                    }
                    else {
                        break;
                    }
                }
            }
            if (provided > max_params || provided < required) continue;
            Candidate candidate;
            candidate.sym = &sym;
            bool ok = true;
            for (std::size_t i = 0; i < provided; ++i) {
                int rank = conversion_rank(arg_types[i], sym.param_types[i]);
                if (rank < 0 && arg_is_null[i] && sym.param_types[i].kind == TypeKind::Pointer) {
                    rank = 0;
                }
                if (rank < 0) {
                    ok = false;
                    break;
                }
                candidate.ranks.push_back(rank);
            }
            if (ok) viable.push_back(std::move(candidate));
        }

        if (viable.empty()) {
            report_error(call->location, ErrorCode::FunctionArgTypeMismatch,
                "no viable overload of '" + name + "' for the given arguments");
            return nullptr;
        }

        int best = -1;
        bool ambiguous = false;
        for (std::size_t i = 0; i < viable.size(); ++i) {
            bool is_best = true;
            for (std::size_t j = 0; j < viable.size(); ++j) {
                if (i == j) continue;
                const std::vector<int>& a = viable[i].ranks;
                const std::vector<int>& b = viable[j].ranks;
                bool i_better = false;
                bool j_better = false;
                for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) {
                    if (a[k] < b[k]) i_better = true;
                    if (a[k] > b[k]) j_better = true;
                }
                if (!(i_better && !j_better)) {
                    is_best = false;
                    break;
                }
            }
            if (is_best) {
                if (best != -1) ambiguous = true;
                else best = static_cast<int>(i);
            }
        }
        if (ambiguous || best < 0) {
            report_error(call->location, ErrorCode::OverloadAmbiguous, { name });
            return nullptr;
        }

        Symbol* chosen = viable[static_cast<std::size_t>(best)].sym;
        if (callee != nullptr) {
            std::string resolved_name;
            if (chosen->function_node != nullptr) {
                resolved_name = chosen->function_node->name;
            }
            else if (chosen->extern_node != nullptr) {
                resolved_name = chosen->extern_node->name;
            }
            if (!resolved_name.empty()) callee->identifier = resolved_name;
            record_function_resolution(callee, *chosen);
        }
        if (chosen->function_node != nullptr && provided < chosen->param_types.size()) {
            const auto& defaults = chosen->function_node->param_defaults;
            for (std::size_t i = provided; i < chosen->param_types.size(); ++i) {
                if (i < defaults.size() && defaults[i]) {
                    call->appended_defaults.push_back(defaults[i].get());
                }
            }
        }
        return chosen;
    }

}
