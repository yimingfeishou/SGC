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

    void TypeChecker::check_extern_declaration(AST::ExternDeclaration* node) {
        if (!is_complete_type(node->return_type) && node->return_type.kind != TypeKind::Void) {
            report_error(node->location, ErrorCode::ExpressionSyntaxError,
                "invalid return type in extern declaration");
            return;
        }
        for (const auto& p : node->parameters) {
            if (p.kind == TypeKind::Void) {
                report_error(node->location, ErrorCode::VoidParameter,
                    "parameter cannot have void type in extern declaration");
            }
            if (!is_complete_type(p)) {
                report_error(node->location, ErrorCode::ExpressionSyntaxError,
                    "incomplete parameter type in extern declaration");
            }
        }
        Symbol sym = Symbol::make_function(
            node->name, node->return_type,
            node->parameters, node->param_names,
            node->location, nullptr);
        sym.extern_node = node;
        Symbol* existing = sym_table_.lookup_current(node->name);
        if (existing != nullptr) {
            if (existing->kind != SymbolKind::Function) {
                report_error(node->location, ErrorCode::RedefinedIdentifier,
                    "identifier '" + node->name + "' already declared");
                return;
            }
            if (!sym_table_.declare_overload(sym)) {
                report_error(node->location, ErrorCode::RedefinedFunction,
                    "function '" + node->name +
                    "' already declared with the same parameter list");
                return;
            }
            if (std::vector<Symbol>* set = sym_table_.lookup_overloads(node->name)) {
                for (const Symbol& other : *set) {
                    if (&other == &(*set).back()) continue;
                    if (overloads_ambiguous_by_defaults(other, (*set).back())) {
                        report_error(node->location, ErrorCode::OverloadAmbiguous, { node->name });
                        break;
                    }
                }
            }
            mangle_overload_set(node->name);
            return;
        }
        if (!sym_table_.declare(sym)) {
            report_error(node->location, ErrorCode::RedefinedFunction,
                "function '" + node->name + "' already declared");
            return;
        }
        sym_table_.declare_overload(sym);
    }

    bool TypeChecker::validate_export_function(AST::FunctionDefinition* node) {
        if (node->name == "main") {
            report_error(node->location, ErrorCode::ExportMainNotAllowed,
                "'main' cannot be declared export");
            return false;
        }
        if (!is_legal_export_identifier(node->name)) {
            diag_.report_error_template(node->location,
                ErrorCode::ExportFunctionNameInvalid, { node->name });
            return false;
        }

        bool overloaded = false;
        for (auto& top : program_->top_levels) {
            if (top.get() == node) continue;
            if (auto* other = dynamic_cast<AST::FunctionDefinition*>(top.get())) {
                if (other->name == node->name) {
                    overloaded = true;
                    break;
                }
            }
            else if (auto* ext = dynamic_cast<AST::ExternDeclaration*>(top.get())) {
                if (ext->name == node->name) {
                    overloaded = true;
                    break;
                }
            }
        }
        if (!overloaded) {
            if (const std::vector<Symbol>* set = sym_table_.lookup_overloads(
                node->name)) {
                if (set->size() > 1) overloaded = true;
            }
        }
        if (overloaded) {
            diag_.report_error_template(node->location,
                ErrorCode::ExportFunctionCannotBeOverloaded, { node->name });
            return false;
        }

        std::string conflict_kind;
        if (const Symbol* existing = sym_table_.lookup(node->name)) {
            if (existing->kind != SymbolKind::Function) {
                conflict_kind = export_conflict_kind_name(existing->kind);
            }
        }
        if (conflict_kind.empty()) {
            for (auto& top : program_->top_levels) {
                if (top.get() == node) continue;
                if (auto* var = dynamic_cast<AST::VariableDeclaration*>(top.get())) {
                    if (var->name == node->name) {
                        conflict_kind = "variable";
                        break;
                    }
                }
                else if (auto* def = dynamic_cast<AST::StructDefinition*>(top.get())) {
                    if (def->name == node->name) {
                        conflict_kind = "type";
                        break;
                    }
                }
            }
        }
        if (!conflict_kind.empty()) {
            diag_.report_error_template(node->location,
                ErrorCode::ExportFunctionDeclarationConflict,
                { node->name, conflict_kind });
            return false;
        }
        return true;
    }

    void TypeChecker::check_function_definition(AST::FunctionDefinition* node) {
        if (node->is_export) {
            validate_export_function(node);
        }
        if (!is_complete_type(node->return_type) && node->return_type.kind != TypeKind::Void) {
            report_error(node->location, ErrorCode::ExpressionSyntaxError,
                "invalid return type in function definition");
            return;
        }
        for (size_t i = 0; i < node->parameters.size(); ++i) {
            const auto& p = node->parameters[i];
            if (p.kind == TypeKind::Void) {
                report_error(node->location, ErrorCode::VoidParameter,
                    "parameter cannot have void type");
            }
            if (!is_complete_type(p) && p.kind != TypeKind::Void) {
                report_error(node->location, ErrorCode::ExpressionSyntaxError,
                    "incomplete parameter type for parameter " + std::to_string(i));
            }
            if (i < node->param_names.size() && !node->param_names[i].empty()) {
                for (size_t j = 0; j < i; ++j) {
                    if (node->param_names[j] == node->param_names[i]) {
                        report_error(node->location, ErrorCode::RedefinedIdentifier,
                            "parameter '" + node->param_names[i] + "' already declared");
                        break;
                    }
                }
            }
        }
        if (auto* existing = sym_table_.lookup(node->name)) {
            if (existing->kind == SymbolKind::Function) {
                if (existing->function_node == nullptr) {
                    bool same_signature = existing->param_types.size() == node->parameters.size() &&
                        std::equal(existing->param_types.begin(), existing->param_types.end(),
                            node->parameters.begin());
                    if (same_signature) {
                        if (existing->type != node->return_type) {
                            report_error(node->location, ErrorCode::FunctionReturnTypeMismatch,
                                "function '" + node->name +
                                "' does not match its extern declaration return type");
                        }
                        existing->function_node = node;
                        if (existing->param_names.empty() && !node->param_names.empty()) {
                            existing->param_names = node->param_names;
                        }
                    }
                    else {
                        Symbol overload_sym = Symbol::make_function(node->name, node->return_type,
                            node->parameters, node->param_names, node->location, node);
                        if (!sym_table_.declare_overload(overload_sym)) {
                            report_error(node->location, ErrorCode::RedefinedFunction,
                                "function '" + node->name +
                                "' already defined with the same parameter list");
                            return;
                        }
                        if (std::vector<Symbol>* set = sym_table_.lookup_overloads(node->name)) {
                            for (const Symbol& other : *set) {
                                if (&other == &(*set).back()) continue;
                                if (overloads_ambiguous_by_defaults(other, (*set).back())) {
                                    report_error(node->location, ErrorCode::OverloadAmbiguous,
                                        { node->name });
                                    break;
                                }
                            }
                        }
                        mangle_overload_set(node->name);
                    }
                }
                else {
                    Symbol overload_sym = Symbol::make_function(
                        node->name, node->return_type, node->parameters, node->param_names,
                        node->location, node);
                    if (!sym_table_.declare_overload(overload_sym)) {
                        report_error(node->location, ErrorCode::RedefinedFunction,
                            "function '" + node->name +
                            "' already defined with the same parameter list");
                        return;
                    }
                    if (std::vector<Symbol>* set = sym_table_.lookup_overloads(node->name)) {
                        for (const Symbol& other : *set) {
                            if (&other == &(*set).back()) continue;
                            if (overloads_ambiguous_by_defaults(other, (*set).back())) {
                                report_error(node->location, ErrorCode::OverloadAmbiguous,
                                    { node->name });
                                break;
                            }
                        }
                    }
                    mangle_overload_set(node->name);
                }
            }
            else {
                report_error(node->location, ErrorCode::RedefinedIdentifier,
                    "identifier '" + node->name + "' already declared as non-function");
                return;
            }
        }
        else {
            Symbol sym = Symbol::make_function(
                node->name, node->return_type,
                node->parameters, node->param_names,
                node->location, node
            );
            if (!sym_table_.declare(sym)) {
                report_error(node->location, ErrorCode::RedefinedFunction,
                    "function '" + node->name + "' already declared");
                return;
            }
            sym_table_.declare_overload(sym);
        }
       enter_scope();
       for (size_t i = 0; i < node->parameters.size(); ++i) {
            std::string pname = (i < node->param_names.size() && !node->param_names[i].empty())
                ? node->param_names[i]
                : "_param" + std::to_string(i);
            Symbol param_sym = Symbol::make_parameter(
                pname, node->parameters[i],
                node->location, i
            );
            if (!sym_table_.declare(param_sym)) {
                report_error(node->location, ErrorCode::RedefinedIdentifier,
                    "parameter '" + pname + "' already declared");
            }
        }
       current_function_ = node;
        for (size_t i = 0; i < node->param_defaults.size() && i < node->parameters.size(); ++i) {
            if (node->param_defaults[i] == nullptr) continue;
            AST::Type default_type = check_expression(node->param_defaults[i].get());
            if (!can_implicit_convert(default_type, node->parameters[i])) {
                report_error(node->param_defaults[i]->location,
                    ErrorCode::FunctionArgTypeMismatch,
                    "default argument " + std::to_string(i + 1) + " of function '" +
                    node->name + "' has type '" + default_type.to_string() +
                    "' which does not match parameter type '" +
                    node->parameters[i].to_string() + "'");
            }
        }
       if (node->body) {
            check_statement(node->body.get());
        }
        if (node->return_type.kind != TypeKind::Void) {
            bool has_return = false;
            std::function<void(AST::Statement*)> check_return_presence = [&](AST::Statement* stmt) {
                if (auto* ret = dynamic_cast<AST::ReturnStatement*>(stmt)) {
                    has_return = true;
                }
                else if (auto* block = dynamic_cast<AST::Block*>(stmt)) {
                    for (auto& s : block->statements) {
                        check_return_presence(s.get());
                    }
                }
                else if (auto* ifs = dynamic_cast<AST::IfStatement*>(stmt)) {
                    check_return_presence(ifs->then_block.get());
                    if (ifs->else_block) {
                        check_return_presence(ifs->else_block.get());
                    }
                }
                else if (auto* for_ = dynamic_cast<AST::ForStatement*>(stmt)) {
                    check_return_presence(for_->body.get());
                }
                else if (auto* while_ = dynamic_cast<AST::WhileStatement*>(stmt)) {
                    check_return_presence(while_->body.get());
                }
                };
            check_return_presence(node->body.get());
            if (!has_return) {
                report_error(node->location, ErrorCode::MissingReturnStatement,
                    "non-void function '" + node->name + "' must return a value");
            }
        }
        exit_scope();
        current_function_ = nullptr;
    }

    void TypeChecker::check_struct_definition(AST::StructDefinition* node) {
        for (size_t i = 0; i < node->members.size(); ++i) {
            for (size_t j = i + 1; j < node->members.size(); ++j) {
                if (node->members[i].name == node->members[j].name) {
                    report_error(node->members[j].location, ErrorCode::RedefinedIdentifier,
                        "struct member '" + node->members[j].name + "' already defined");
                }
            }
        }
        for (auto& member : node->members) {
            if (member.array_size.has_value() && !member.type.array_size.has_value() &&
                member.type.kind == TypeKind::Array) {
                member.type.array_size = member.array_size;
            }
            if (member.type.kind == TypeKind::Array && !member.array_size.has_value() &&
                member.array_size_expr != nullptr) {
                auto value = evaluate_const_integer_expression(member.array_size_expr.get());
                if (value.has_value() && *value > 0) {
                    member.array_size = static_cast<std::size_t>(*value);
                    member.type.array_size = member.array_size;
                }
                else {
                    report_error(member.location, ErrorCode::ArraySizeNotConstant,
                        "array size in a declaration must be a constant integer expression");
                }
            }
            AST::Type& mem_type = member.type;
            std::vector<std::string> containment_visited;
            containment_visited.push_back(node->name);
            if (type_contains_struct_by_value(mem_type, node->name,
                containment_visited)) {
                report_error(member.location, ErrorCode::StructSelfRefNonPtr,
                    "struct cannot directly contain itself; use pointer");
            }
            if (mem_type.kind == TypeKind::Pointer && mem_type.pointee_type) {
                if (mem_type.pointee_type->kind == TypeKind::Struct &&
                    mem_type.pointee_type->struct_name == node->name) {
                }
            }
            if (!is_complete_type(mem_type) && mem_type.kind != TypeKind::Void) {
                if (!(mem_type.kind == TypeKind::Pointer && mem_type.pointee_type &&
                    mem_type.pointee_type->kind == TypeKind::Struct &&
                    struct_defs_.find(mem_type.pointee_type->struct_name) == struct_defs_.end())) {
                    report_error(member.location, ErrorCode::ExpressionSyntaxError,
                        "incomplete type for struct member '" + member.name + "'");
                }
            }
            if (mem_type.kind == TypeKind::Function) {
                if (!is_complete_type(*mem_type.return_type) && mem_type.return_type->kind != TypeKind::Void) {
                    report_error(member.location, ErrorCode::ExpressionSyntaxError,
                        "invalid return type in function pointer member '" + member.name + "'");
                }
                for (const auto& p : mem_type.parameter_types) {
                    if (p.kind == TypeKind::Void) {
                        report_error(member.location, ErrorCode::VoidParameter,
                            "parameter cannot have void type in function pointer member");
                    }
                }
            }
            if (member.array_size.has_value() && member.array_size.value() == 0) {
                report_error(member.location, ErrorCode::ExpressionSyntaxError,
                    "array size must be positive");
            }
            if (member.initializer) {
                if (auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(member.initializer.get())) {
                    AST::Type init_type = check_expression(expr_init->expr.get());
                    if (!can_implicit_convert(init_type, mem_type)) {
                        report_error(member.location, ErrorCode::StructMemberTypeMismatch,
                            "initializer type '" + init_type.to_string() +
                            "' cannot be converted to member type '" + mem_type.to_string() + "'");
                    }
                }
                else if (auto* arr_init = dynamic_cast<AST::ArrayInitializer*>(member.initializer.get())) {
                    if (mem_type.kind != TypeKind::Array) {
                        report_error(member.location, ErrorCode::StructMemberTypeMismatch,
                            "array initializer for non-array member");
                    }
                    else {
                        size_t elem_count = arr_init->elements.size();
                        if (!mem_type.array_size.has_value()) {
                            if (elem_count == 0) {
                                report_error(member.location, ErrorCode::EmptyArrayInitializer,
                                    "cannot infer array size from empty initializer");
                            }
                            else {
                                mem_type.array_size = elem_count;
                                member.array_size = elem_count;
                            }
                        }
                        if (mem_type.array_size.has_value() && mem_type.array_size.value() != elem_count) {
                            report_error(member.location, ErrorCode::ArrayLengthMismatch,
                                "array initializer size " + std::to_string(elem_count) +
                                " does not match declared size " + std::to_string(mem_type.array_size.value()));
                        }
                        auto elem_type = mem_type.element_type;
                        for (auto& elem : arr_init->elements) {
                            if (auto* e = dynamic_cast<AST::ExpressionInitializer*>(elem.get())) {
                                AST::Type etype = check_expression(e->expr.get());
                                if (!can_implicit_convert(etype, *elem_type)) {
                                    report_error(member.location, ErrorCode::StructMemberTypeMismatch,
                                        "array element type mismatch");
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    void TypeChecker::check_variable_declaration(AST::VariableDeclaration* decl) {
        if (decl->type.is_const) {
            check_const_declaration(decl);
        }
        if (decl->type.kind == TypeKind::Array) {
            if (decl->array_size.has_value() && !decl->type.array_size.has_value()) {
                decl->type.array_size = decl->array_size;
            }
            if (!decl->array_size.has_value() && decl->type.array_size.has_value()) {
                decl->array_size = decl->type.array_size;
            }
            if (!decl->array_size.has_value() && decl->array_size_expr != nullptr) {
                auto value = evaluate_const_integer_expression(decl->array_size_expr.get());
                if (value.has_value() && *value > 0) {
                    decl->type.array_size = static_cast<std::size_t>(*value);
                    decl->array_size = static_cast<std::size_t>(*value);
                }
                else {
                    report_error(decl->array_size_expr->location,
                        ErrorCode::ArraySizeNotConstant,
                        "array size in a declaration must be a constant integer expression");
                }
            }
        }
        if (decl->type.kind == TypeKind::Array && !decl->array_size.has_value()) {
            if (auto* arr_init = dynamic_cast<AST::ArrayInitializer*>(decl->initializer.get())) {
                if (!arr_init->elements.empty()) {
                    decl->type.array_size = arr_init->elements.size();
                    decl->array_size = arr_init->elements.size();
                }
                else {
                    report_error(decl->location, ErrorCode::EmptyArrayInitializer,
                        "cannot infer array size from an empty initializer");
                    return;
                }
            }
        }
        if (!is_complete_type(decl->type) && decl->type.kind != TypeKind::Void) {
            report_error(decl->location, ErrorCode::ExpressionSyntaxError,
                "incomplete type in variable declaration");
            return;
        }
        if (decl->type.kind == TypeKind::Void) {
            report_error(decl->location, ErrorCode::ExpressionSyntaxError,
                "variable cannot have void type");
            return;
        }
        if (auto* existing = sym_table_.lookup_current(decl->name)) {
            report_error(decl->location, ErrorCode::RedefinedIdentifier,
                "variable '" + decl->name + "' already declared in this scope");
            return;
        }
        if (const std::vector<Symbol>* set =
                sym_table_.current_scope().overloads(decl->name)) {
            if (!set->empty()) {
                report_error(decl->location, ErrorCode::RedefinedIdentifier,
                    "variable '" + decl->name +
                    "' conflicts with a function of the same name in this scope");
                return;
            }
        }
        if (decl->type.kind == TypeKind::Array) {
            if (!decl->array_size.has_value()) {
                if (decl->initializer) {
                    if (auto* arr_init = dynamic_cast<AST::ArrayInitializer*>(decl->initializer.get())) {
                        size_t size = arr_init->elements.size();
                        if (size == 0) {
                            report_error(decl->location, ErrorCode::EmptyArrayInitializer,
                                "cannot infer array size from empty initializer");
                            return;
                        }
                        decl->type.array_size = size;
                        decl->array_size = size;
                    }
                    else if (auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(decl->initializer.get())) {
                        report_error(decl->location, ErrorCode::ExpressionSyntaxError,
                            "array initializer must be a braced list");
                        return;
                    }
                    else {
                        report_error(decl->location, ErrorCode::ExpressionSyntaxError,
                            "array size must be specified or inferred from initializer");
                        return;
                    }
                }
                else {
                    report_error(decl->location, ErrorCode::ArraySizeNotConstant,
                        "array size must be specified");
                    return;
                }
            }
            else if (decl->array_size.value() == 0) {
                report_error(decl->location, ErrorCode::ExpressionSyntaxError,
                    "array size must be positive");
                return;
            }
        }
        if (decl->initializer) {
            if (decl->type.kind == TypeKind::Struct) {
                if (auto* arr_init =
                    dynamic_cast<AST::ArrayInitializer*>(decl->initializer.get())) {
                    if (arr_init->from_paren_call && arr_init->elements.size() == 1) {
                        if (auto* element = dynamic_cast<AST::ExpressionInitializer*>(
                            arr_init->elements[0].get())) {
                            AST::Type element_type = check_expression(element->expr.get());
                            if (element_type.kind == TypeKind::Pointer ||
                                element_type.kind == TypeKind::Function) {
                                auto conversion = std::make_unique<AST::PostfixExpression>(
                                    decl->location, std::move(element->expr),
                                    AST::PostfixExpression::Operator::Cast, nullptr,
                                    std::vector<std::unique_ptr<AST::Expression>>{},
                                    decl->type);
                                decl->initializer =
                                    std::make_unique<AST::ExpressionInitializer>(
                                        decl->location, std::move(conversion));
                            }
                        }
                    }
                }
            }
            if (auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(decl->initializer.get())) {
                const AST::Type* saved_expected = expected_type_;
                expected_type_ = &decl->type;
                AST::Type init_type = check_expression(expr_init->expr.get());
                expected_type_ = saved_expected;
                if (decl->type.kind == TypeKind::Struct && init_type.kind == TypeKind::Struct &&
                    init_type.struct_name == decl->type.struct_name) {
                    if (is_move_expression(expr_init->expr.get())) {
                        if (!type_is_movable(decl->type)) {
                            diag_.report_error_template(decl->location,
                                ErrorCode::NoMoveViolation, { decl->type.struct_name });
                        }
                    }
                    else if (!type_is_copyable(decl->type)) {
                        diag_.report_error_template(decl->location,
                            ErrorCode::NoCopyViolation, { decl->type.struct_name });
                    }
                }
                bool is_null = false;
                if (auto* primary = dynamic_cast<AST::PrimaryExpression*>(expr_init->expr.get())) {
                    if (primary->kind == AST::PrimaryExpression::Kind::Null) {
                        is_null = true;
                    }
                }
                bool const_drop_reported = false;
                if (decl->type.kind == TypeKind::Pointer &&
                    init_type.kind == TypeKind::Pointer &&
                    decl->type.pointee_type && init_type.pointee_type &&
                    !const_qualification_ok(*init_type.pointee_type,
                        *decl->type.pointee_type) &&
                    is_address_of_const_identifier(expr_init->expr.get())) {
                    const_drop_reported = true;
                }
                if (decl->type.kind == TypeKind::Function &&
                    init_type.kind == TypeKind::Function &&
                    !function_signatures_match(decl->type, init_type)) {
                    report_error(decl->location, ErrorCode::FuncPtrTypeMismatch,
                        "function pointer type mismatch: expected '" +
                        decl->type.to_string() + "', got '" + init_type.to_string() + "'");
                }
                else if (decl->type.kind == TypeKind::Pointer &&
                    init_type.kind == TypeKind::Pointer &&
                    decl->type.pointee_type && init_type.pointee_type &&
                    decl->type.pointee_type->kind != TypeKind::Void &&
                    init_type.pointee_type->kind != TypeKind::Void &&
                    !(types_equal_modulo_const(*decl->type.pointee_type,
                        *init_type.pointee_type) &&
                        const_qualification_ok(*init_type.pointee_type,
                            *decl->type.pointee_type)) &&
                    !const_drop_reported) {
                    report_error(decl->location, ErrorCode::PointerTypeMismatch,
                        "cannot initialize pointer '" + decl->name +
                        "' of type '" + decl->type.to_string() + "' with '" +
                        init_type.to_string() + "'");
                }
               else if ((!is_null || decl->type.kind != TypeKind::Pointer) &&
                   !const_drop_reported) {
                   if (!can_implicit_convert(init_type, decl->type)) {
                      if (is_null && decl->type.kind == TypeKind::File) {
                      }
                      else if (is_expression_parameter_temp(decl->initializer.get())) {
                          diag_.report_error_template(decl->location,
                              ErrorCode::ExprParameterExpansionTypeError,
                              { decl->name, decl->type.to_string(),
                                init_type.to_string() });
                      }
                      else if (decl->name.find("$expr") != std::string::npos) {
                          diag_.report_error_template(decl->location,
                              ErrorCode::ExprParameterExpansionTypeError,
                              { decl->name, decl->type.to_string(),
                                init_type.to_string() });
                      }
                      else {
                       report_error(decl->location, ErrorCode::AssignmentTypeMismatch,
                           "cannot initialize variable '" + decl->name +
                           "' with type '" + init_type.to_string() +
                           "' (expected '" + decl->type.to_string() + "')");
                      }
                   }
               }
            }
            else if (auto* arr_init = dynamic_cast<AST::ArrayInitializer*>(decl->initializer.get())) {
                if (decl->type.kind == TypeKind::Struct) {
                    check_struct_initializer(arr_init, decl->type, decl->location);
                }
                else if (decl->type.kind != TypeKind::Array) {
                    report_error(decl->location, ErrorCode::AssignmentTypeMismatch,
                        "array initializer for non-array variable");
                }
                else {
                    check_array_initializer(arr_init, decl->type, decl->location);
                }
            }
        }
        Symbol sym = Symbol::make_variable(decl->name, decl->type, decl->location,
            decl->initializer != nullptr);
        if (decl->type.is_const) {
            sym.is_mutable = false;
        }
        if (!sym_table_.declare(sym)) {
            report_error(decl->location, ErrorCode::RedefinedIdentifier,
                "variable '" + decl->name + "' already declared");
        }
    }

    void TypeChecker::check_struct_initializer(AST::ArrayInitializer* init,
        const AST::Type& struct_type, SourceLocation loc) {
        if (init == nullptr) return;
        AST::StructDefinition* def = get_struct_definition(struct_type.struct_name);
        if (def == nullptr) {
            report_error(loc, ErrorCode::ExpressionSyntaxError,
                "unknown struct type '" + struct_type.struct_name + "'");
            return;
        }
        if (init->elements.size() > def->members.size()) {
            report_error(loc, ErrorCode::StructInitLengthMismatch,
                "struct initializer has too many elements; expected at most " +
                std::to_string(def->members.size()) + ", got " +
                std::to_string(init->elements.size()));
            return;
        }
        auto check_member_expression = [&](AST::Expression* e, const AST::Type& target) {
            const AST::Type* saved = expected_type_;
            expected_type_ = &target;
            AST::Type result = check_expression(e);
            expected_type_ = saved;
            return result;
        };
        for (std::size_t i = 0; i < init->elements.size(); ++i) {
            const auto& member = def->members[i];
            AST::Initializer* element = init->elements[i].get();
            if (auto* nested = dynamic_cast<AST::ArrayInitializer*>(element)) {
                if (member.type.kind == TypeKind::Struct) {
                    check_struct_initializer(nested, member.type, loc);
                }
                else if (member.type.kind == TypeKind::Array) {
                    check_array_initializer(nested, member.type, loc);
                }
                else {
                    report_error(element->location, ErrorCode::ExpressionSyntaxError,
                        "struct member requires an expression initializer");
                }
                continue;
            }
            auto* e = dynamic_cast<AST::ExpressionInitializer*>(element);
            if (e == nullptr) continue;
            if (member.type.kind == TypeKind::Array) {
                report_error(e->location, ErrorCode::StructMemberTypeMismatch,
                    "array member '" + member.name + "' requires a braced initializer");
                check_member_expression(e->expr.get(), member.type);
                continue;
            }
            AST::Type init_type = check_member_expression(e->expr.get(), member.type);
            if (!can_implicit_convert(init_type, member.type)) {
                report_error(e->location, ErrorCode::StructMemberTypeMismatch,
                    "initializer for member '" + member.name + "' does not match its type");
            }
        }
    }

    void TypeChecker::check_array_initializer(AST::ArrayInitializer* init,
        const AST::Type& array_type, SourceLocation loc) {
        if (init == nullptr || array_type.kind != TypeKind::Array) return;
        AST::Type element = array_type.element_type ? *array_type.element_type
            : AST::Type::make_void();
        const std::size_t provided = init->elements.size();
        if (array_type.array_size.has_value()) {
            if (provided != array_type.array_size.value()) {
                report_error(loc, ErrorCode::ArrayLengthMismatch,
                    "initializer length " + std::to_string(provided) +
                    " does not match array size " +
                    std::to_string(array_type.array_size.value()));
            }
        }
        else if (provided == 0) {
            report_error(loc, ErrorCode::EmptyArrayInitializer,
                "cannot infer array size from empty initializer");
        }
        for (auto& item : init->elements) {
            if (auto* nested = dynamic_cast<AST::ArrayInitializer*>(item.get())) {
                if (element.kind == TypeKind::Struct) {
                    check_struct_initializer(nested, element, item->location);
                }
                else if (element.kind == TypeKind::Array) {
                    check_array_initializer(nested, element, item->location);
                }
                else {
                    report_error(item->location, ErrorCode::AssignmentTypeMismatch,
                        "nested braced initializer for a non-aggregate element");
                }
                continue;
            }
            auto* e = dynamic_cast<AST::ExpressionInitializer*>(item.get());
            if (e == nullptr) continue;
            const AST::Type* saved_expected = expected_type_;
            expected_type_ = &element;
            AST::Type value_type = check_expression(e->expr.get());
            expected_type_ = saved_expected;
            if (element.kind == TypeKind::Array) {
                report_error(e->location, ErrorCode::AssignmentTypeMismatch,
                    "array element requires a braced initializer");
                continue;
            }
            if (!can_implicit_convert(value_type, element)) {
                report_error(e->location, ErrorCode::AssignmentTypeMismatch,
                    "array element type mismatch");
            }
        }
    }

}
