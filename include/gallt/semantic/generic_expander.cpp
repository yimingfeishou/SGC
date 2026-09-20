#include "generic_expander.hpp"
#include "constant_folding.hpp"
#include "../lexer/lexer.hpp"
#include "../parser/parser.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

using namespace gallt::AST;

namespace gallt {

    namespace {
        bool is_builtin_type_name(const std::string& name) {
            return name == "int" || name == "lint" || name == "uint" ||
                name == "luint" || name == "float" || name == "double" ||
                name == "char" || name == "uchar" || name == "bool" ||
                name == "string" || name == "file" || name == "void";
        }

        std::string arguments_text(const GenericRef& ref) {
            std::string out;
            for (std::size_t i = 0; i < ref.arguments.size(); ++i) {
                if (i != 0) out += ", ";
                out += ref.arguments[i].normalize();
            }
            return out;
        }

        std::string arguments_kind_signature(const GenericRef& ref) {
            std::string out;
            for (std::size_t i = 0; i < ref.arguments.size(); ++i) {
                const GenericArgument& arg = ref.arguments[i];
                out += "|";
                if (arg.is_expr) {
                    out += "E:" + arg.text;
                }
                else if (arg.is_type) {
                    out += "T:" + arg.type.to_string();
                }
                else if (arg.is_string_constant) {
                    out += "CS";
                }
                else if (arg.float_constant) {
                    out += "CF";
                }
                else {
                    out += "CI";
                }
            }
            return out;
        }
    } 

    GenericExpander::GenericExpander(DiagnosticEngine& diag) : diag_(diag) {}

    std::string GenericExpander::signature_text(const std::vector<AST::Type>& types) {
        std::string out;
        for (std::size_t i = 0; i < types.size(); ++i) {
            if (i != 0) out += ", ";
            out += types[i].to_string();
        }
        return out;
    }

    bool GenericExpander::statement_is_allowed_in_expression_body(const AST::Statement* stmt,
        SourceLocation& bad_loc, ErrorCode& code, std::string& detail) {
        if (stmt == nullptr) {
            return true;
        }
        if (dynamic_cast<const StructDefinition*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedDeclaration;
            detail = "struct";
            return false;
        }
        if (dynamic_cast<const GenericDefinition*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedDeclaration;
            detail = "generic";
            return false;
        }
        if (dynamic_cast<const NamespaceDefinition*>(stmt) != nullptr ||
            dynamic_cast<const AdditionNamespaceStatement*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedDeclaration;
            detail = "namespace";
            return false;
        }
        if (dynamic_cast<const EmitStatement*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedConstruct;
            detail = "emit";
            return false;
        }
        if (dynamic_cast<const GuideStatement*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedConstruct;
            detail = "guide";
            return false;
        }
        if (dynamic_cast<const ClibStatement*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedConstruct;
            detail = "clib";
            return false;
        }
        if (dynamic_cast<const ExternDeclaration*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedConstruct;
            detail = "extern";
            return false;
        }
        if (dynamic_cast<const CondDefinition*>(stmt) != nullptr ||
            dynamic_cast<const UncondDefinition*>(stmt) != nullptr ||
            dynamic_cast<const ConditionalBlock*>(stmt) != nullptr ||
            dynamic_cast<const TopLevelBlock*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedConstruct;
            detail = "condition";
            return false;
        }
        if (auto* block = dynamic_cast<const Block*>(stmt)) {
            for (const auto& inner : block->statements) {
                if (!statement_is_allowed_in_expression_body(inner.get(), bad_loc, code,
                    detail)) {
                    return false;
                }
            }
            return true;
        }
        if (auto* if_stmt = dynamic_cast<const IfStatement*>(stmt)) {
            if (!statement_is_allowed_in_expression_body(if_stmt->then_block.get(), bad_loc,
                code, detail)) {
                return false;
            }
            return statement_is_allowed_in_expression_body(if_stmt->else_block.get(),
                bad_loc, code, detail);
        }
        if (auto* for_stmt = dynamic_cast<const ForStatement*>(stmt)) {
            return statement_is_allowed_in_expression_body(for_stmt->body.get(), bad_loc,
                code, detail);
        }
        if (auto* while_stmt = dynamic_cast<const WhileStatement*>(stmt)) {
            return statement_is_allowed_in_expression_body(while_stmt->body.get(), bad_loc,
                code, detail);
        }
        return true;
    }

    void GenericExpander::collect_declared_names(const AST::Statement* stmt,
        std::unordered_set<std::string>& out) const {
        if (stmt == nullptr) {
            return;
        }
        if (auto* decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
            out.insert(decl->name);
            return;
        }
        if (auto* block = dynamic_cast<const Block*>(stmt)) {
            for (const auto& inner : block->statements) {
                collect_declared_names(inner.get(), out);
            }
            return;
        }
        if (auto* if_stmt = dynamic_cast<const IfStatement*>(stmt)) {
            collect_declared_names(if_stmt->then_block.get(), out);
            collect_declared_names(if_stmt->else_block.get(), out);
            return;
        }
        if (auto* for_stmt = dynamic_cast<const ForStatement*>(stmt)) {
            collect_declared_names(for_stmt->init.get(), out);
            collect_declared_names(for_stmt->body.get(), out);
            return;
        }
        if (auto* while_stmt = dynamic_cast<const WhileStatement*>(stmt)) {
            collect_declared_names(while_stmt->body.get(), out);
            return;
        }
    }

    void GenericExpander::collect_free_identifiers(const Expression* expr,
        const std::unordered_set<std::string>& bound,
        std::unordered_set<std::string>& out) const {
        if (expr == nullptr) {
            return;
        }
        if (auto* prim = dynamic_cast<const PrimaryExpression*>(expr)) {
            if (prim->kind == PrimaryExpression::Kind::Identifier) {
                if (bound.count(prim->identifier) == 0) {
                    out.insert(prim->identifier);
                }
                return;
            }
            collect_free_identifiers(prim->paren_expr.get(), bound, out);
            collect_free_identifiers(prim->heap_size.get(), bound, out);
            collect_free_identifiers(prim->placement_target.get(), bound, out);
            for (const auto& arg : prim->construct_args) {
                collect_free_identifiers(arg.get(), bound, out);
            }
            return;
        }
        if (auto* post = dynamic_cast<const PostfixExpression*>(expr)) {
            collect_free_identifiers(post->base.get(), bound, out);
            collect_free_identifiers(post->subscript_expr.get(), bound, out);
            for (const auto& arg : post->arguments) {
                collect_free_identifiers(arg.get(), bound, out);
            }
            return;
        }
        if (auto* e = dynamic_cast<const AssignmentExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const LogicalOrExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const LogicalAndExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const ComparisonExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const AdditiveExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const MultiplicativeExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const PowerExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const UnaryExpression*>(expr)) {
            collect_free_identifiers(e->operand.get(), bound, out);
            return;
        }
    }

    void GenericExpander::collect_free_identifiers_in_statement(const Statement* stmt,
        std::unordered_set<std::string>& out) const {
        if (stmt == nullptr) {
            return;
        }
        std::unordered_set<std::string> bound;
        if (auto* decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
            bound.insert(decl->name);
            if (decl->initializer != nullptr && decl->initializer->is_expression()) {
                collect_free_identifiers(
                    static_cast<const ExpressionInitializer*>(decl->initializer.get())
                        ->expr.get(),
                    bound, out);
            }
            collect_free_identifiers(decl->array_size_expr.get(), bound, out);
            return;
        }
        if (auto* ret = dynamic_cast<const ReturnStatement*>(stmt)) {
            collect_free_identifiers(ret->value.get(), bound, out);
            return;
        }
        if (auto* expr_stmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
            collect_free_identifiers(expr_stmt->expr.get(), bound, out);
            return;
        }
        if (auto* block = dynamic_cast<const Block*>(stmt)) {
            for (const auto& inner : block->statements) {
                collect_free_identifiers_in_statement(inner.get(), out);
            }
            return;
        }
        if (auto* if_stmt = dynamic_cast<const IfStatement*>(stmt)) {
            collect_free_identifiers(if_stmt->condition.get(), bound, out);
            collect_free_identifiers_in_statement(if_stmt->then_block.get(), out);
            collect_free_identifiers_in_statement(if_stmt->else_block.get(), out);
            return;
        }
        if (auto* for_stmt = dynamic_cast<const ForStatement*>(stmt)) {
            collect_free_identifiers(for_stmt->condition.get(), bound, out);
            collect_free_identifiers(for_stmt->step.get(), bound, out);
            collect_free_identifiers_in_statement(for_stmt->init.get(), out);
            collect_free_identifiers_in_statement(for_stmt->body.get(), out);
            return;
        }
        if (auto* while_stmt = dynamic_cast<const WhileStatement*>(stmt)) {
            collect_free_identifiers(while_stmt->condition.get(), bound, out);
            collect_free_identifiers_in_statement(while_stmt->body.get(), out);
            return;
        }
    }

    bool GenericExpander::validate_expression_body(const GenericParameter& param,
        const Substitution::ExpressionBinding& binding, SourceLocation loc) {
        if (binding.body == nullptr || binding.body->statements.empty()) {
            report(loc, ErrorCode::ExprParameterBlockMissingReturn,
                std::vector<std::string>{ param.name });
            return false;
        }
        SourceLocation bad_loc;
        ErrorCode bad_code = ErrorCode::ExprParameterDisallowedSyntax;
        std::string detail;
        for (const auto& stmt : binding.body->statements) {
            if (!statement_is_allowed_in_expression_body(stmt.get(), bad_loc, bad_code,
                detail)) {
                report(bad_loc, bad_code, std::vector<std::string>{ detail });
                return false;
            }
        }
        const Statement* last = binding.body->statements.back().get();
        while (auto* block = dynamic_cast<const Block*>(last)) {
            if (block->statements.empty()) {
                break;
            }
            last = block->statements.back().get();
        }
        const bool void_return = binding.return_type.kind == TypeKind::Void;
        if (!void_return && dynamic_cast<const ReturnStatement*>(last) == nullptr) {
            report(loc, ErrorCode::ExprParameterBlockMissingReturn,
                std::vector<std::string>{ param.name });
            return false;
        }
        if (void_return && dynamic_cast<const ReturnStatement*>(last) == nullptr &&
            dynamic_cast<const ExpressionStatement*>(last) == nullptr) {
            report(loc, ErrorCode::ExprParameterBlockMissingReturn,
                std::vector<std::string>{ param.name });
            return false;
        }
        std::unordered_set<std::string> declared;
        for (const auto& stmt : binding.body->statements) {
            collect_declared_names(stmt.get(), declared);
        }
        for (const std::string& local : declared) {
            for (const std::string& pname : binding.parameter_names) {
                if (!pname.empty() && pname == local) {
                    report(loc, ErrorCode::ExprParameterBlockLocalConflictsParameter,
                        std::vector<std::string>{ local, pname });
                    return false;
                }
            }
        }
        for (const auto& stmt : binding.body->statements) {
            if (auto* decl = dynamic_cast<const VariableDeclaration*>(stmt.get())) {
                if (decl->type.kind == TypeKind::Void) {
                    report(decl->location, ErrorCode::ExprParameterDisallowedSyntax,
                        std::vector<std::string>{});
                    return false;
                }
            }
        }
        return true;
    }

    void GenericExpander::report(SourceLocation loc, ErrorCode code,
        const std::vector<std::string>& values) {
        diag_.report_error_template(loc, code, values);
        had_error_ = true;
    }

    void GenericExpander::report(SourceLocation loc, ErrorCode code, const std::string& message) {
        diag_.report_error(loc, code, message);
        had_error_ = true;
    }


    void GenericExpander::push_scope() {
        declared_scopes_.emplace_back();
        short_scopes_.emplace_back();
        instance_scopes_.emplace_back();
    }

    void GenericExpander::pop_scope() {
        declared_scopes_.pop_back();
        short_scopes_.pop_back();
        instance_scopes_.pop_back();
    }

    void GenericExpander::declare_name(const std::string& name) {
        if (!declared_scopes_.empty()) {
            declared_scopes_.back().insert(name);
        }
    }

    bool GenericExpander::name_declared_in_current_scope(const std::string& name) const {
        if (declared_scopes_.empty()) return false;
        return declared_scopes_.back().count(name) != 0;
    }

    void GenericExpander::register_short_name(const std::string& name,
        const ShortBinding& binding, const std::string& owning_generic, SourceLocation loc) {
        if (short_scopes_.empty()) return;
        auto& shorts = short_scopes_.back();
        auto it = shorts.find(name);
        if (it != shorts.end()) {
            if (it->second.instance == binding.instance) {
                return;  
            }
            if (it->second.target != binding.target) {
                report(loc, ErrorCode::GenericShortNameAmbiguous, std::vector<std::string>{ name });
            }
            return;
        }
        if (name == owning_generic) {
            shorts[name] = binding;
            return;
        }
        if (name_declared_in_current_scope(name)) {
            report(loc, ErrorCode::GenericShortNameConflict, std::vector<std::string>{ name });
            return;
        }
        shorts[name] = binding;
        declare_name(name);
    }

    void GenericExpander::add_top_level(std::unique_ptr<TopLevel> node) {
        if (output_ != nullptr) {
            output_->push_back(std::move(node));
        }
        else {
            program_->top_levels.push_back(std::move(node));
        }
    }


    void GenericExpander::collect_generics() {
        std::map<std::string, std::vector<GenericDefinition*>> by_name;
        for (auto& top : program_->top_levels) {
            if (auto* g = dynamic_cast<GenericDefinition*>(top.get())) {
                by_name[g->name].push_back(g);
            }
        }
        for (auto& pair : by_name) {
            const std::string& name = pair.first;
            std::vector<GenericDefinition*>& defs = pair.second;
            GenericEntry entry;
            std::vector<GenericDefinition*> before_primary;
            bool has_primary_shaped = false;
            for (GenericDefinition* def : defs) {
                if (def->primary_shaped) has_primary_shaped = true;
            }
            for (GenericDefinition* def : defs) {
                std::set<std::string> seen_params;
                for (const GenericParameter& p : def->parameters) {
                    if (!seen_params.insert(p.name).second) {
                        report(def->location, ErrorCode::GenericParameterRedefined, std::vector<std::string>{ p.name });
                    }
                }
                for (const GenericParameter& p : def->parameters) {
                    if (p.is_type && p.constraint.kind == GenericConstraint::Kind::Types) {
                        for (const Type& t : p.constraint.types) {
                            if (t.kind != TypeKind::Struct) continue;
                            if (t.generic_ref) continue;
                            if (!is_builtin_type_name(t.struct_name) &&
                                struct_defs_.find(t.struct_name) == struct_defs_.end()) {
                                report(def->location, ErrorCode::GenericConstraintInvalid, std::vector<std::string>{ p.constraint.to_string() });
                            }
                        }
                    }
                }
                if (!def->primary_shaped) {
                    std::map<std::string, std::string> structure;
                    std::function<std::string(const Type&)> structure_of =
                        [&](const Type& t) -> std::string {
                        switch (t.kind) {
                        case TypeKind::Struct:
                            if (!is_builtin_type_name(t.struct_name) &&
                                struct_defs_.find(t.struct_name) == struct_defs_.end()) {
                                return "T";
                            }
                            return "S:" + t.struct_name;
                        case TypeKind::Pointer:
                            return "P(" + (t.pointee_type ? structure_of(*t.pointee_type)
                                                          : std::string("?")) + ")";
                        case TypeKind::Array:
                            return "A(" + (t.element_type ? structure_of(*t.element_type)
                                                          : std::string("?")) + ")";
                        default:
                            return t.to_string();
                        }
                    };
                    std::function<void(const Type&)> scan = [&](const Type& t) {
                        switch (t.kind) {
                        case TypeKind::Struct:
                            if (!is_builtin_type_name(t.struct_name) &&
                                struct_defs_.find(t.struct_name) == struct_defs_.end()) {
                                auto found = structure.find(t.struct_name);
                                if (found == structure.end()) {
                                    structure[t.struct_name] = "T";
                                }
                            }
                            return;
                        case TypeKind::Pointer:
                            if (t.pointee_type) scan(*t.pointee_type);
                            return;
                        case TypeKind::Array:
                            if (t.element_type) scan(*t.element_type);
                            return;
                        default:
                            return;
                        }
                    };
                    for (const GenericPatternArg& pattern : def->patterns) {
                        if (!pattern.is_constant) scan(pattern.type);
                    }
                    std::map<std::string, std::string> full_structure;
                    std::function<void(const Type&)> verify = [&](const Type& t) {
                        if (t.kind == TypeKind::Struct &&
                            !is_builtin_type_name(t.struct_name) &&
                            struct_defs_.find(t.struct_name) == struct_defs_.end()) {
                            std::string shape = structure_of(t);
                            auto found = full_structure.find(t.struct_name);
                            if (found == full_structure.end()) {
                                full_structure[t.struct_name] = shape;
                            }
                            else if (found->second != shape) {
                                report(def->location,
                                    ErrorCode::GenericFreeIdentifierConflict, std::vector<std::string>{ t.struct_name });
                            }
                            return;
                        }
                        if (t.kind == TypeKind::Pointer && t.pointee_type) {
                            if (t.pointee_type->kind == TypeKind::Struct &&
                                !is_builtin_type_name(t.pointee_type->struct_name) &&
                                struct_defs_.find(t.pointee_type->struct_name) == struct_defs_.end()) {
                                const std::string& n = t.pointee_type->struct_name;
                                auto found = full_structure.find(n);
                                if (found == full_structure.end()) full_structure[n] = "P(T)";
                                else if (found->second != "P(T)") {
                                    report(def->location,
                                        ErrorCode::GenericFreeIdentifierConflict, std::vector<std::string>{ n });
                                }
                                return;
                            }
                            verify(*t.pointee_type);
                            return;
                        }
                        if (t.kind == TypeKind::Array && t.element_type) {
                            if (t.element_type->kind == TypeKind::Struct &&
                                !is_builtin_type_name(t.element_type->struct_name) &&
                                struct_defs_.find(t.element_type->struct_name) == struct_defs_.end()) {
                                const std::string& n = t.element_type->struct_name;
                                auto found = full_structure.find(n);
                                if (found == full_structure.end()) full_structure[n] = "A(T)";
                                else if (found->second != "A(T)") {
                                    report(def->location,
                                        ErrorCode::GenericFreeIdentifierConflict, std::vector<std::string>{ n });
                                }
                                return;
                            }
                            verify(*t.element_type);
                            return;
                        }
                    };
                    for (const GenericPatternArg& pattern : def->patterns) {
                        if (!pattern.is_constant) verify(pattern.type);
                    }
                }
                std::set<std::string> struct_member_names;
                std::set<std::string> function_member_names;
                std::set<std::string> function_signatures;
                for (auto& member : def->members) {
                    std::string member_name;
                    if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) {
                        member_name = sd->name;
                        if (!struct_member_names.insert(member_name).second ||
                            function_member_names.count(member_name) != 0) {
                            report(def->location, ErrorCode::RedefinedFunction,
                                std::vector<std::string>{ member_name });
                        }
                    }
                    else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) {
                        member_name = fd->name;
                        std::string signature = member_name + "(";
                        for (const Type& p : fd->parameters) {
                            signature += p.to_string();
                            signature += ",";
                        }
                        signature += ")";
                        function_member_names.insert(member_name);
                        if (struct_member_names.count(member_name) != 0 ||
                            !function_signatures.insert(signature).second) {
                            report(def->location, ErrorCode::RedefinedFunction,
                                std::vector<std::string>{ member_name });
                        }
                    }
                    if (member_name.empty()) continue;
                    for (const GenericParameter& p : def->parameters) {
                        if (p.name == member_name) {
                            if (p.is_expr) {
                                report(def->location,
                                    ErrorCode::ExprParameterNameConflict,
                                    std::vector<std::string>{ p.name });
                            }
                            else {
                                report(def->location, ErrorCode::GenericMemberNameConflictsParameter, std::vector<std::string>{ member_name, p.name });
                            }
                        }
                    }
                }

                if (def->primary_shaped && entry.primary == nullptr) {
                    entry.primary = def;
                }
                else if (def->primary_shaped) {
                    bool same_shape = false;
                    if (entry.primary != nullptr) {
                        std::vector<GenericPatternArg> primary_patterns =
                            entry.primary->patterns.empty()
                            ? parameters_as_free_patterns(entry.primary)
                            : entry.primary->patterns;
                        std::vector<GenericPatternArg> def_patterns = def->patterns;
                        if (def_patterns.empty() && !def->parameters.empty()) {
                            def_patterns = parameters_as_free_patterns(def);
                        }
                        if (!primary_patterns.empty() &&
                            primary_patterns.size() == def_patterns.size()) {
                            std::function<std::string(const GenericPatternArg&)>
                                raw_signature = [&](const GenericPatternArg& pattern) -> std::string {
                                const Type& t = pattern.type;
                                if (pattern.is_constant) {
                                    return "C=" + pattern.text;
                                }
                                if (t.kind == TypeKind::Struct) {
                                    return t.struct_name;
                                }
                                if (t.kind == TypeKind::Pointer) {
                                    return (t.pointee_type ? raw_signature(GenericPatternArg{ false, false, *t.pointee_type })
                                                           : std::string("?")) + "*";
                                }
                                if (t.kind == TypeKind::Array) {
                                    return (t.element_type ? raw_signature(GenericPatternArg{ false, false, *t.element_type })
                                                           : std::string("?")) + "[]";
                                }
                                return t.to_string();
                            };
                            same_shape = true;
                            for (std::size_t i = 0; i < def_patterns.size(); ++i) {
                                if (raw_signature(primary_patterns[i]) !=
                                    raw_signature(def_patterns[i])) {
                                    same_shape = false;
                                    break;
                                }
                            }
                        }
                    }
                    if (same_shape) {
                        report(def->location, ErrorCode::GenericPrimaryRedefined,
                            std::vector<std::string>{ name });
                        continue;
                    }
                    def->is_specialization = true;
                    if (def->patterns.empty()) {
                        def->patterns = parameters_as_free_patterns(def);
                    }
                    if (duplicate_pattern_signature(entry, def)) {
                        report(def->location, ErrorCode::GenericPrimaryRedefined, std::vector<std::string>{ name });
                    }
                    else {
                        entry.specializations.push_back(def);
                    }
                }
                else if (entry.primary == nullptr) {
                    before_primary.push_back(def);
                }
                else {
                    if (duplicate_pattern_signature(entry, def)) {
                        report(def->location, ErrorCode::GenericPrimaryRedefined, std::vector<std::string>{ name });
                    }
                    else {
                        entry.specializations.push_back(def);
                    }
                }
            }
            if (entry.primary == nullptr) {
                if (!before_primary.empty()) {
                    if (has_primary_shaped) {
                        for (GenericDefinition* def : before_primary) {
                            report(def->location, ErrorCode::GenericSpecializationBeforePrimary, std::vector<std::string>{ def->name, name });
                        }
                    }
                    else {
                        report(before_primary.front()->location,
                            ErrorCode::GenericSpecializationWithoutPrimary, std::vector<std::string>{ name });
                    }
                }
            }
            else {
                for (GenericDefinition* def : before_primary) {
                    report(def->location, ErrorCode::GenericSpecializationBeforePrimary, std::vector<std::string>{ def->name, name });
                }
            }
            generics_[name] = std::move(entry);
        }
    }

    void GenericExpander::collect_declarations() {
        std::function<void(const std::vector<std::unique_ptr<TopLevel>>&)> scan_top;
        for (auto& top : program_->top_levels) {
            if (auto* sd = dynamic_cast<StructDefinition*>(top.get())) {
                struct_defs_[sd->name] = sd;
                declare_name(sd->name);
            }
            else if (auto* fd = dynamic_cast<FunctionDefinition*>(top.get())) {
                func_defs_[fd->name] = fd;
                declare_name(fd->name);
            }
            else if (auto* ed = dynamic_cast<ExternDeclaration*>(top.get())) {
                extern_defs_[ed->name] = ed;
                declare_name(ed->name);
            }
            else if (auto* vd = dynamic_cast<VariableDeclaration*>(top.get())) {
                declare_name(vd->name);
            }
            else if (auto* g = dynamic_cast<GenericDefinition*>(top.get())) {
                declare_name(g->name);
            }
        }
        (void)scan_top;
    }

    std::unique_ptr<Expression> GenericExpander::expand_expression_call(
        const FunctionDefinition* owner, const Substitution& sub,
        const std::string& name,
        std::vector<std::unique_ptr<Expression>>& arguments,
        SourceLocation loc, bool& ok) {
        (void)owner;
        ok = false;
        auto found = sub.expressions.find(name);
        if (found == sub.expressions.end()) {
            return nullptr;
        }
        const Substitution::ExpressionBinding& binding = found->second;
        if (binding.body == nullptr) {
            return nullptr;
        }
        if (arguments.size() != binding.parameter_types.size()) {
            report(loc, ErrorCode::ExprParameterCallArgCountMismatch,
                std::vector<std::string>{ name,
                    std::to_string(binding.parameter_types.size()),
                    std::to_string(arguments.size()) });
            return nullptr;
        }
        if (expression_expansion_depth_ >= kMaxExpressionExpansionDepth) {
            report(loc, ErrorCode::ExprParameterRecursionLimitExceeded,
                std::vector<std::string>{ name });
            return nullptr;
        }
        if (pending_hoisted_ == nullptr) {
            report(loc, ErrorCode::ExprParameterDisallowedSyntax,
                std::vector<std::string>{ name });
            return nullptr;
        }
        Substitution body_sub = sub;
        body_sub.renames.clear();
        body_sub.expr_arguments.clear();
        body_sub.expr_argument_types.clear();
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            expr_argument_storage_.push_back(std::move(arguments[i]));
        }
        std::size_t first_new = expr_argument_storage_.size() - arguments.size();
        for (std::size_t i = 0; i < binding.parameter_types.size(); ++i) {
            if (i >= binding.parameter_names.size() ||
                binding.parameter_names[i].empty()) {
                continue;
            }
            body_sub.expr_arguments[binding.parameter_names[i]] =
                expr_argument_storage_[first_new + i].get();
            body_sub.expr_argument_types[binding.parameter_names[i]] =
                binding.parameter_types[i];
            expression_parameter_index_[binding.parameter_names[i]] = i;
        }
        std::unordered_set<std::string> locals;
        for (const auto& stmt : binding.body->statements) {
            collect_declared_names(stmt.get(), locals);
        }
        const std::string suffix = "$expr" + std::to_string(++expr_temp_counter_);
        for (const std::string& local : locals) {
            body_sub.renames[local] = local + suffix;
        }
        const bool void_result = binding.return_type.kind == TypeKind::Void;
        std::string temp_name;
        if (!void_result) {
            temp_name = "__glt_expr" + std::to_string(expr_temp_counter_);
        }
        std::vector<std::unique_ptr<Statement>> emitted;
        const bool outer_in_expression_body = in_expression_body_;
        in_expression_body_ = true;
        ++expression_expansion_depth_;
        bool body_ok = emit_body_into_temp(binding.body->statements, body_sub,
            temp_name, binding.return_type, emitted);
        --expression_expansion_depth_;
        in_expression_body_ = outer_in_expression_body;
        if (!body_ok) {
            report(binding.body->location, ErrorCode::ExprParameterDisallowedSyntax,
                std::vector<std::string>{ name });
            return nullptr;
        }
        ExpressionCallSite call_site;
        call_site.name = name;
        call_site.declared_return_type = binding.return_type;
        call_site.declared_parameter_types = binding.parameter_types;
        call_site.location = loc;
        call_site.depth = expression_expansion_depth_;
        if (void_result) {
            for (auto& stmt : emitted) {
                pending_hoisted_->push_back(std::move(stmt));
            }
            ok = true;
            lexeme_pool_.push_back("0");
            Token placeholder(TokenType::IntegerLiteral, loc,
                std::string_view(lexeme_pool_.back()));
            auto node = std::make_unique<PrimaryExpression>(loc, placeholder);
            expression_call_sites_[node.get()] = call_site;
            return node;
        }
        auto declaration = std::make_unique<VariableDeclaration>(loc,
            binding.return_type, temp_name, std::nullopt, std::nullopt, nullptr);
        pending_hoisted_->push_back(std::move(declaration));
        for (auto& stmt : emitted) {
            pending_hoisted_->push_back(std::move(stmt));
        }
        ok = true;
        auto node = std::make_unique<PrimaryExpression>(loc, temp_name);
        expression_call_sites_[node.get()] = call_site;
        return node;
    }

    bool GenericExpander::substitute_type(const Type& in, const Substitution& sub, Type& out) {
        switch (in.kind) {
        case TypeKind::Array: {
            Type elem = Type::make_void();
            bool ok = in.element_type ? substitute_type(*in.element_type, sub, elem) : false;
            out = Type::make_array(std::make_shared<Type>(std::move(elem)), in.array_size);
            return ok;
        }
        case TypeKind::Pointer: {
            Type pointee = Type::make_void();
            bool ok = in.pointee_type ? substitute_type(*in.pointee_type, sub, pointee) : false;
            out = Type::make_pointer(std::make_shared<Type>(std::move(pointee)));
            return ok;
        }
        case TypeKind::Function: {
            Type ret = Type::make_void();
            bool ok = in.return_type ? substitute_type(*in.return_type, sub, ret) : true;
            std::vector<Type> params;
            for (const Type& p : in.parameter_types) {
                Type substituted = Type::make_void();
                if (!substitute_type(p, sub, substituted)) ok = false;
                params.push_back(std::move(substituted));
            }
            out = Type::make_function(std::make_shared<Type>(std::move(ret)), params);
            return ok;
        }
        case TypeKind::Struct: {
            if (in.generic_ref) {
                GenericRef ref = *in.generic_ref;
                for (GenericArgument& arg : ref.arguments) {
                    if (arg.is_type) {
                        Type substituted = Type::make_void();
                        if (!substitute_type(arg.type, sub, substituted)) return false;
                        arg.type = std::move(substituted);
                        arg.text = arg.type.to_string();
                    }
                }
                auto mangled = ensure_instantiation(ref, false);
                if (!mangled.has_value()) return false;
                out = Type::make_struct(*mangled);
                return true;
            }
            auto found = sub.types.find(in.struct_name);
            if (found != sub.types.end()) {
                out = found->second;
                return true;
            }
            out = in;
            return true;
        }
        default:
            out = in;
            return true;
        }
    }

    void GenericExpander::resolve_type(Type& type) {
        switch (type.kind) {
        case TypeKind::Array:
            if (type.element_type) resolve_type(*type.element_type);
            break;
        case TypeKind::Pointer:
            if (type.pointee_type) resolve_type(*type.pointee_type);
            break;
        case TypeKind::Function:
            if (type.return_type) resolve_type(*type.return_type);
            for (Type& p : type.parameter_types) resolve_type(p);
            break;
        case TypeKind::Struct: {
            if (type.generic_ref) {
                GenericRef ref = *type.generic_ref;
                if (ref.member.empty()) {
                    auto instance_key = ensure_instantiation(ref, true);
                    if (!instance_key.has_value()) {
                        type.generic_ref.reset();
                        break;
                    }
                    auto members = instance_struct_members_.find(*instance_key);
                    if (members != instance_struct_members_.end() && members->second.size() == 1) {
                        ref.member = members->second.front();
                    }
                    else {
                        report(type.generic_ref->location, ErrorCode::GenericMemberNotInInstantiation, std::vector<std::string>{ ref.generic_name, ref.generic_name });
                        type.generic_ref.reset();
                        break;
                    }
                }
                auto mangled = ensure_instantiation(ref, false);
                if (mangled.has_value()) {
                    type.struct_name = *mangled;
                }
                type.generic_ref.reset();
                break;
            }
            for (auto it = short_scopes_.rbegin(); it != short_scopes_.rend(); ++it) {
                auto found = it->find(type.struct_name);
                if (found != it->end()) {
                    if (found->second.is_type) {
                        type.struct_name = found->second.target;
                    }
                    break;
                }
            }
            break;
        }
        default:
            break;
        }
    }

    bool GenericExpander::resolve_constant_argument(const GenericArgument& arg,
        const Substitution* sub, ConstantValue& out) {
        if (arg.is_type) return false;
        if (arg.constant_actual_type.kind == TypeKind::String) {
            if (arg.is_string_constant) {
                out.is_string = true;
                out.is_float = false;
                out.string_value = arg.string_constant;
                return true;
            }
            if (arg.expression != nullptr) {
                if (auto* prim = dynamic_cast<const PrimaryExpression*>(arg.expression.get())) {
                    if (prim->kind == PrimaryExpression::Kind::Literal &&
                        prim->literal_token.type == TokenType::StringLiteral) {
                        out.is_string = true;
                        out.is_float = false;
                        if (!decode_string_literal(prim->literal_token.lexeme,
                            out.string_value)) {
                            return false;
                        }
                        return true;
                    }
                }
            }
            return false;
        }
        if (arg.expression != nullptr) {
            Substitution empty;
            const Substitution& env = sub != nullptr ? *sub : empty;
            if (evaluate_with_substitution(arg.expression.get(), env, out)) {
                return true;
            }
            return false;
        }
        if (arg.float_constant) {
            out.is_float = true;
            out.float_value = arg.float_value;
            return true;
        }
        if (!arg.text.empty() && sub != nullptr) {
            auto found = sub->constants.find(arg.text);
            if (found != sub->constants.end()) {
                out = found->second;
                return true;
            }
        }
        out.is_float = false;
        out.int_value = arg.int_value;
        return true;
    }

    bool GenericExpander::evaluate_with_substitution(const AST::Expression* expr,
        const Substitution& sub, ConstantValue& out) {
        ConstantEvaluationContext context;
        context.lookup_constant = [&](const std::string& name, long long& int_value,
            double& float_value, bool& is_float) {
            auto found = sub.constants.find(name);
            if (found == sub.constants.end()) return false;
            int_value = found->second.int_value;
            float_value = found->second.float_value;
            is_float = found->second.is_float;
            return true;
        };
        context.type_layout = [&](const std::string& name, std::size_t& size,
            std::size_t& align) { return resolve_type_layout(name, size, align, sub); };
        long long int_value = 0;
        double float_value = 0.0;
        bool is_float = false;
        if (!evaluate_constant_expression(expr, int_value, float_value, is_float, context)) {
            return false;
        }
        out.int_value = int_value;
        out.float_value = float_value;
        out.is_float = is_float;
        return true;
    }

    bool GenericExpander::resolve_type_layout(const std::string& type_name, std::size_t& size,
        std::size_t& align, const Substitution& sub) const {
        if (type_name == "int" || type_name == "uint" || type_name == "float") {
            size = 4; align = 4; return true;
        }
        if (type_name == "lint" || type_name == "luint" || type_name == "double") {
            size = 8; align = 8; return true;
        }
        if (type_name == "char" || type_name == "uchar" || type_name == "bool") {
            size = 1; align = 1; return true;
        }
        if (type_name == "string") { size = 32; align = 8; return true; }
        if (type_name == "file") { size = 8; align = 8; return true; }
        auto bound = sub.types.find(type_name);
        if (bound != sub.types.end()) {
            return resolve_type_layout(bound->second.struct_name, size, align, sub) ||
                resolve_type_layout(bound->second.to_string(), size, align, sub);
        }
        auto it = struct_defs_.find(type_name);
        if (it == struct_defs_.end() || it->second == nullptr) return false;
        auto round_up = [](std::size_t value, std::size_t alignment) {
            if (alignment <= 1) return value;
            return (value + alignment - 1) / alignment * alignment;
        };
        std::function<bool(const AST::Type&, std::size_t&, std::size_t&)> type_layout =
            [&](const AST::Type& type, std::size_t& out_size, std::size_t& out_align) -> bool {
            switch (type.kind) {
            case TypeKind::Int: case TypeKind::Float: out_size = 4; out_align = 4; return true;
            case TypeKind::Double: out_size = 8; out_align = 8; return true;
            case TypeKind::Char: case TypeKind::Bool: out_size = 1; out_align = 1; return true;
            case TypeKind::String: out_size = 32; out_align = 8; return true;
            case TypeKind::File: out_size = 8; out_align = 8; return true;
            case TypeKind::Pointer:
            case TypeKind::Function: out_size = 8; out_align = 8; return true;
            case TypeKind::Array: {
                std::size_t element_size = 0;
                std::size_t element_align = 1;
                if (!type.element_type ||
                    !type_layout(*type.element_type, element_size, element_align)) {
                    return false;
                }
                out_size = element_size * type.array_size.value_or(0);
                out_align = element_align;
                return true;
            }
            case TypeKind::Struct: {
                auto def = struct_defs_.find(type.struct_name);
                if (def == struct_defs_.end()) return false;
                std::size_t offset = 0;
                std::size_t max_align = 1;
                for (const auto& member : def->second->members) {
                    std::size_t member_size = 0;
                    std::size_t member_align = 1;
                    if (!type_layout(member.type, member_size, member_align)) return false;
                    max_align = std::max(max_align, member_align);
                    offset = round_up(offset, member_align);
                    offset += member_size;
                }
                out_size = round_up(offset, max_align);
                out_align = max_align;
                return true;
            }
            default:
                return false;
            }
        };
        return type_layout(AST::Type::make_struct(type_name), size, align);
    }

    std::unique_ptr<Expression> GenericExpander::make_typed_constant_literal(SourceLocation loc,
        const ConstantValue& value, const AST::Type& type) {
        if (value.is_float || type.kind == TypeKind::Float || type.kind == TypeKind::Double) {
            return make_constant_literal(loc, value);
        }
        return make_constant_literal(loc, value);
    }

    namespace {
        bool is_free_identifier(const std::string& name,
            const std::unordered_map<std::string, StructDefinition*>& structs) {
            if (is_builtin_type_name(name)) return false;
            return structs.find(name) == structs.end();
        }
    }

    std::vector<GenericPatternArg> GenericExpander::parameters_as_free_patterns(
        GenericDefinition* def) const {
        std::vector<GenericPatternArg> patterns;
        for (const GenericParameter& param : def->parameters) {
            GenericPatternArg pattern;
            if (param.is_type) {
                pattern.is_constant = false;
                pattern.type = Type::make_struct(param.name);
                pattern.text = param.name;
            }
            else {
                pattern.is_constant = true;
                pattern.free_constant = true;
                pattern.text = param.name;
            }
            patterns.push_back(std::move(pattern));
        }
        return patterns;
    }

    std::string GenericExpander::pattern_signature(const GenericPatternArg& pattern) const {
        std::function<std::string(const Type&)> type_sig = [&](const Type& t) -> std::string {
            switch (t.kind) {
            case TypeKind::Struct:
                return is_free_identifier(t.struct_name, struct_defs_)
                    ? std::string("F") : ("S:" + t.struct_name);
            case TypeKind::Pointer:
                return "P(" + (t.pointee_type ? type_sig(*t.pointee_type) : std::string("?")) + ")";
            case TypeKind::Array:
                return "A(" + (t.element_type ? type_sig(*t.element_type) : std::string("?")) +
                    (t.array_size.has_value() ? "," + std::to_string(*t.array_size) : "") + ")";
            default:
                return t.to_string();
            }
        };
        if (pattern.is_constant) {
            if (pattern.free_constant) return "FC";
            if (pattern.string_constant) return "CS:" + pattern.string_value;
            if (pattern.float_constant) {
                char buffer[64];
                std::snprintf(buffer, sizeof(buffer), "%g", pattern.float_value);
                return std::string("C:") + buffer;
            }
            return "C:" + std::to_string(pattern.int_value);
        }
        return type_sig(pattern.type);
    }

    bool GenericExpander::duplicate_pattern_signature(const GenericEntry& entry,
        GenericDefinition* def) const {
        auto same_signature = [&](GenericDefinition* other) {
            if (other == def) return false;
            if (other->patterns.size() != def->patterns.size()) return false;
            for (std::size_t i = 0; i < def->patterns.size(); ++i) {
                if (pattern_signature(other->patterns[i]) != pattern_signature(def->patterns[i])) {
                    return false;
                }
            }
            return true;
        };
        if (entry.primary != nullptr && same_signature(entry.primary)) return true;
        for (GenericDefinition* spec : entry.specializations) {
            if (same_signature(spec)) return true;
        }
        return false;
    }

    GenericExpander::PatternOrder GenericExpander::compare_pattern_type(const Type& a,
        const Type& b) const {
        bool a_free = (a.kind == TypeKind::Struct &&
            is_free_identifier(a.struct_name, struct_defs_));
        bool b_free = (b.kind == TypeKind::Struct &&
            is_free_identifier(b.struct_name, struct_defs_));
        if (a_free && b_free) return PatternOrder::Equal;
        if (a_free) return PatternOrder::Worse;        
        if (b_free) return PatternOrder::Better;       
        bool a_composite = (a.kind == TypeKind::Pointer || a.kind == TypeKind::Array);
        bool b_composite = (b.kind == TypeKind::Pointer || b.kind == TypeKind::Array);
        if (!a_composite && !b_composite) {
            return PatternOrder::Equal;
        }
        if (a_composite && !b_composite) return PatternOrder::Worse;
        if (!a_composite && b_composite) return PatternOrder::Better;
        if (a.kind != b.kind) return PatternOrder::Incomparable;
        if (a.kind == TypeKind::Pointer) {
            if (!a.pointee_type || !b.pointee_type) return PatternOrder::Incomparable;
            return compare_pattern_type(*a.pointee_type, *b.pointee_type);
        }
        if (!a.element_type || !b.element_type) return PatternOrder::Incomparable;
        if (a.array_size.has_value() != b.array_size.has_value()) {
            return PatternOrder::Incomparable;
        }
        if (a.array_size.has_value() && *a.array_size != *b.array_size) {
            return PatternOrder::Incomparable;
        }
        return compare_pattern_type(*a.element_type, *b.element_type);
    }

    GenericExpander::PatternOrder GenericExpander::compare_pattern(
        const GenericPatternArg& a, const GenericPatternArg& b) const {
        if (a.is_constant && b.is_constant) {
            if (a.free_constant && b.free_constant) return PatternOrder::Equal;
            if (a.free_constant) return PatternOrder::Worse;
            if (b.free_constant) return PatternOrder::Better;
            if (a.string_constant || b.string_constant) {
                if (a.string_constant != b.string_constant) {
                    return PatternOrder::Incomparable;
                }
                return (a.string_value == b.string_value)
                    ? PatternOrder::Equal : PatternOrder::Incomparable;
            }
            if (a.float_constant != b.float_constant) return PatternOrder::Incomparable;
            bool same = a.float_constant ? (a.float_value == b.float_value)
                                         : (a.int_value == b.int_value);
            return same ? PatternOrder::Equal : PatternOrder::Equal;
        }
        if (a.is_constant || b.is_constant) return PatternOrder::Incomparable;
        return compare_pattern_type(a.type, b.type);
    }

    bool GenericExpander::at_least_as_specialized(GenericDefinition* a,
        GenericDefinition* b) const {
        if (a->patterns.size() != b->patterns.size()) return false;
        for (std::size_t i = 0; i < a->patterns.size(); ++i) {
            if (compare_pattern(a->patterns[i], b->patterns[i]) == PatternOrder::Worse) {
                return false;
            }
        }
        return true;
    }

    bool GenericExpander::more_specialized(GenericDefinition* a,
        GenericDefinition* b) const {
        return at_least_as_specialized(a, b) && !at_least_as_specialized(b, a);
    }

    GenericExpander::MatchResult GenericExpander::match_specialization(
        GenericDefinition* def, const GenericRef& ref) {
        MatchResult result;
        if (def->patterns.size() != ref.arguments.size()) return result;

        std::function<bool(const Type&, const Type&)> match;
        match = [&](const Type& pattern, const Type& actual) -> bool {
            switch (pattern.kind) {
            case TypeKind::Struct:
                if (is_free_identifier(pattern.struct_name, struct_defs_)) {
                    auto found = result.substitution.types.find(pattern.struct_name);
                    if (found == result.substitution.types.end()) {
                        result.substitution.types[pattern.struct_name] = actual;
                    }
                    else if (!(found->second == actual)) {
                        report(def->location, ErrorCode::GenericPatternDeductionMismatch, std::vector<std::string>{ pattern.struct_name });
                        return false;
                    }
                    return true;
                }
                return pattern.struct_name == actual.struct_name && actual.kind == TypeKind::Struct;
            case TypeKind::Pointer:
                if (actual.kind != TypeKind::Pointer || !actual.pointee_type || !pattern.pointee_type) {
                    return false;
                }
                return match(*pattern.pointee_type, *actual.pointee_type);
            case TypeKind::Array:
                if (actual.kind != TypeKind::Array || !actual.element_type || !pattern.element_type) {
                    return false;
                }
                if (pattern.array_size.has_value() && actual.array_size.has_value() &&
                    pattern.array_size.value() != actual.array_size.value()) {
                    return false;
                }
                return match(*pattern.element_type, *actual.element_type);
            default:
                return pattern == actual;
            }
        };

        for (std::size_t i = 0; i < def->patterns.size(); ++i) {
            const GenericPatternArg& pattern = def->patterns[i];
            const GenericArgument& arg = ref.arguments[i];
            if (pattern.is_constant) {
                if (arg.is_type) return MatchResult{};
                if (pattern.free_constant) continue;
                ConstantValue value;
                if (!resolve_constant_argument(arg, nullptr, value)) return MatchResult{};
                if (pattern.string_constant) {
                    if (!value.is_string) return MatchResult{};
                    if (pattern.string_value != value.string_value) return MatchResult{};
                    continue;
                }
                if (value.is_string) return MatchResult{};
                if (pattern.float_constant != value.is_float) return MatchResult{};
                if (pattern.float_constant) {
                    if (pattern.float_value != value.float_value) return MatchResult{};
                }
                else if (pattern.int_value != value.int_value) {
                    return MatchResult{};
                }
            }
            else {
                if (!arg.is_type) return MatchResult{};
                if (!match(pattern.type, arg.type)) return MatchResult{};
            }
        }
        result.matched = true;
        return result;
    }

    bool GenericExpander::constraint_satisfied(const GenericConstraint& constraint,
        const Type& arg) {
        switch (constraint.kind) {
        case GenericConstraint::Kind::Any:
            return true;
        case GenericConstraint::Kind::Struct:
            return arg.kind == TypeKind::Struct;
        case GenericConstraint::Kind::Pointer:
            return arg.kind == TypeKind::Pointer;
        case GenericConstraint::Kind::Array:
            return arg.kind == TypeKind::Array;
        case GenericConstraint::Kind::File:
            return arg.kind == TypeKind::File;
        case GenericConstraint::Kind::Types:
            for (const Type& allowed : constraint.types) {
                if (allowed.kind == TypeKind::Struct && arg.kind == TypeKind::Struct) {
                    Type resolved = allowed;
                    if (resolved.generic_ref) {
                        resolve_type(resolved);
                    }
                    if (resolved.struct_name == arg.struct_name) return true;
                    continue;
                }
                if (allowed == arg) return true;
            }
            return false;
        }
        return false;
    }


    std::optional<std::string> GenericExpander::ensure_instantiation(const GenericRef& ref,
        bool whole_block) {
        auto entry_it = generics_.find(ref.generic_name);
        if (entry_it == generics_.end()) {
            report(ref.location, ErrorCode::GenericUndefined, std::vector<std::string>{ ref.generic_name });
            return std::nullopt;
        }
        GenericEntry& entry = entry_it->second;
        if (entry.primary == nullptr) {
            report(ref.location, ErrorCode::GenericNoMatch, std::vector<std::string>{ ref.generic_name, arguments_text(ref) });
            return std::nullopt;
        }
        std::size_t expected = entry.primary->parameters.size();
        if (ref.arguments.size() != expected) {
            report(ref.location, ErrorCode::GenericArgCountMismatch, std::vector<std::string>{ ref.generic_name, arguments_text(ref),
                  std::to_string(expected), std::to_string(ref.arguments.size()) });
            return std::nullopt;
        }

        Substitution primary_sub;
        for (std::size_t i = 0; i < expected; ++i) {
            const GenericParameter& param = entry.primary->parameters[i];
            const GenericArgument& arg = ref.arguments[i];
            if (param.is_expr) {
                if (!arg.is_expr) {
                    report(ref.location, ErrorCode::ExprParameterSignatureMismatch,
                        std::vector<std::string>{ param.name, "expr", arg.normalize() });
                    return std::nullopt;
                }
                if (!arg.expr_name.empty() && arg.expr_name != param.name) {
                    report(ref.location, ErrorCode::ExprParameterUndefined,
                        std::vector<std::string>{ arg.expr_name });
                    return std::nullopt;
                }
                std::vector<AST::Type> declared_parameter_types;
                for (const AST::Type& declared : param.expr_param_types) {
                    AST::Type resolved = declared;
                    AST::Type substituted = AST::Type::make_void();
                    if (substitute_type(declared, primary_sub, substituted)) {
                        resolved = substituted;
                    }
                    declared_parameter_types.push_back(resolved);
                }
                AST::Type declared_return_type = param.expr_return_type;
                {
                    AST::Type substituted = AST::Type::make_void();
                    if (substitute_type(param.expr_return_type, primary_sub, substituted)) {
                        declared_return_type = substituted;
                    }
                }
                Substitution::ExpressionBinding binding;
                binding.name = param.name;
                binding.body = arg.expr_body;
                binding.normalized = arg.text;
                if (arg.expr_shorthand) {
                    bool all_named = !declared_parameter_types.empty() &&
                        param.expr_param_names.size() == declared_parameter_types.size();
                    for (const std::string& pname : param.expr_param_names) {
                        if (pname.empty()) all_named = false;
                    }
                    if (declared_parameter_types.empty()) {
                        all_named = true;
                    }
                    if (!all_named) {
                        report(ref.location, ErrorCode::ExprParameterShorthandRequiresParameterNames,
                            std::vector<std::string>{ param.name });
                        return std::nullopt;
                    }
                    binding.parameter_types = declared_parameter_types;
                    binding.parameter_names = param.expr_param_names;
                    binding.return_type = declared_return_type;
                }
                else {
                    if (arg.expr_param_types.size() != declared_parameter_types.size()) {
                        report(ref.location, ErrorCode::ExprParameterSignatureMismatch,
                            std::vector<std::string>{ param.name,
                                signature_text(declared_parameter_types),
                                signature_text(arg.expr_param_types) });
                        return std::nullopt;
                    }
                    for (std::size_t k = 0; k < arg.expr_param_types.size(); ++k) {
                        if (!(arg.expr_param_types[k] == declared_parameter_types[k])) {
                            report(ref.location, ErrorCode::ExprParameterSignatureMismatch,
                                std::vector<std::string>{ param.name,
                                    signature_text(declared_parameter_types),
                                    signature_text(arg.expr_param_types) });
                            return std::nullopt;
                        }
                    }
                    binding.parameter_types = declared_parameter_types;
                    binding.parameter_names = arg.expr_param_names;
                    if (binding.parameter_names.size() != binding.parameter_types.size()) {
                        binding.parameter_names = param.expr_param_names;
                    }
                    binding.return_type = declared_return_type;
                    if (!(arg.expr_return_type == declared_return_type)) {
                        report(ref.location, ErrorCode::ExprParameterReturnTypeMismatch,
                            std::vector<std::string>{ param.name,
                                declared_return_type.to_string(),
                                arg.expr_return_type.to_string() });
                        return std::nullopt;
                    }
                }
                if (!validate_expression_body(param, binding, ref.location)) {
                    return std::nullopt;
                }
                if (primary_sub.types.count(param.name) != 0 ||
                    primary_sub.constants.count(param.name) != 0 ||
                    primary_sub.expressions.count(param.name) != 0) {
                    report(ref.location, ErrorCode::ExprParameterNameConflict,
                        std::vector<std::string>{ param.name });
                    return std::nullopt;
                }
                {
                    bool conflicts = false;
                    for (std::size_t k = 0; k < entry.primary->parameters.size(); ++k) {
                        const GenericParameter& other = entry.primary->parameters[k];
                        if (k != i && other.name == param.name) {
                            conflicts = true;
                            break;
                        }
                    }
                    if (!conflicts) {
                        for (auto& member : entry.primary->members) {
                            std::string member_name;
                            if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) {
                                member_name = sd->name;
                            }
                            else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) {
                                member_name = fd->name;
                            }
                            if (!member_name.empty() && member_name == param.name) {
                                conflicts = true;
                                break;
                            }
                        }
                    }
                    if (conflicts) {
                        report(ref.location, ErrorCode::ExprParameterNameConflict,
                            std::vector<std::string>{ param.name });
                        return std::nullopt;
                    }
                }
                primary_sub.expressions[param.name] = std::move(binding);
                continue;
            }
            if (param.is_type) {
                if (!arg.is_type) {
                    report(ref.location, ErrorCode::GenericNonTypeArgTypeMismatch, std::vector<std::string>{ param.name, arg.normalize() });
                    return std::nullopt;
                }
                if (!constraint_satisfied(param.constraint, arg.type)) {
                    Type resolved_arg = arg.type;
                    if (resolved_arg.generic_ref) resolve_type(resolved_arg);
                    if (constraint_satisfied(param.constraint, resolved_arg)) {
                        primary_sub.types[param.name] = resolved_arg;
                        continue;
                    }
                    report(ref.location, ErrorCode::GenericConstraintViolated, std::vector<std::string>{ ref.generic_name, arguments_text(ref),
                          std::to_string(i + 1), param.constraint.to_string(),
                          arg.type.to_string() });
                    return std::nullopt;
                }
                primary_sub.types[param.name] = arg.type;
            }
            else {
                if (arg.is_type) {
                    const std::string type_text = arg.type.to_string();
                    bool parameter_name = false;
                    if (entry.primary != nullptr) {
                        for (const GenericParameter& candidate : entry.primary->parameters) {
                            if (candidate.name == type_text) {
                                parameter_name = true;
                                break;
                            }
                        }
                    }
                    const bool known_type = is_builtin_type_name(type_text) ||
                        struct_defs_.find(type_text) != struct_defs_.end() ||
                        parameter_name;
                    if (!known_type) {
                        report(ref.location, ErrorCode::GenericNonTypeArgNotConstant,
                            std::vector<std::string>{ arg.text.empty()
                                ? arg.normalize() : arg.text });
                    }
                    else {
                        report(ref.location, ErrorCode::GenericNonTypeArgTypeMismatch,
                            std::vector<std::string>{ param.constant_type_is_parameter ? param.constant_type_parameter
                                                       : param.constant_type.to_string(),
                              arg.type.to_string() });
                    }
                    return std::nullopt;
                }
                if (arg.constant_actual_type.kind != TypeKind::Void) {
                    Type target = param.constant_type;
                    if (param.constant_type_is_parameter) {
                        auto found = primary_sub.types.find(param.constant_type_parameter);
                        if (found != primary_sub.types.end()) target = found->second;
                    }
                    bool string_argument =
                        (arg.constant_actual_type.kind == TypeKind::String);
                    if (target.kind != TypeKind::Void &&
                        !(string_argument && target.kind == TypeKind::String)) {
                        report(ref.location, ErrorCode::GenericNonTypeArgTypeMismatch, std::vector<std::string>{ target.to_string(), arg.constant_actual_type.to_string() });
                        return std::nullopt;
                    }
                }
              ConstantValue value;
              if (!resolve_constant_argument(arg, nullptr, value)) {
                  report(ref.location, ErrorCode::GenericNonTypeArgNotConstant,
                      std::vector<std::string>{ arg.text.empty() ? arg.normalize()
                                                                 : arg.text });
                  return std::nullopt;
              }
                Type target = param.constant_type;
                if (param.constant_type_is_parameter) {
                    auto found = primary_sub.types.find(param.constant_type_parameter);
                    if (found != primary_sub.types.end()) target = found->second;
                }
                if (target.kind != TypeKind::Void) {
                    bool integral = (target.kind == TypeKind::Int || target.kind == TypeKind::Char ||
                        target.kind == TypeKind::Bool);
                    if (integral && value.is_float) {
                        value.is_float = false;
                        value.int_value = static_cast<long long>(value.float_value);
                    }
                    else if (integral) {
                        long long limit = (target.kind == TypeKind::Char) ? 255
                            : (target.kind == TypeKind::Bool ? 1
                                : (long long)0x7fffffffLL);
                        if (value.int_value > limit || value.int_value < -limit - 1) {
                            report(ref.location, ErrorCode::GenericNonTypeArgOutOfRange, std::vector<std::string>{ target.to_string() });
                            return std::nullopt;
                        }
                    }
                    else if (target.kind == TypeKind::Float || target.kind == TypeKind::Double) {
                        if (!value.is_float) {
                            value.float_value = static_cast<double>(value.int_value);
                            value.is_float = true;
                        }
                    }
                }
                primary_sub.constants[param.name] = value;
                AST::Type declared = param.constant_type;
                if (param.constant_type_is_parameter) {
                    auto found = primary_sub.types.find(param.constant_type_parameter);
                    if (found != primary_sub.types.end()) declared = found->second;
                }
                primary_sub.constant_types[param.name] = declared;
            }
        }

        std::vector<GenericDefinition*> candidates;
        std::vector<Substitution> candidate_subs;
        for (GenericDefinition* spec : entry.specializations) {
            MatchResult mr = match_specialization(spec, ref);
            if (!mr.matched) continue;
            candidates.push_back(spec);
            candidate_subs.push_back(mr.substitution);
        }
        GenericDefinition* block = entry.primary;
        Substitution sub = primary_sub;
        if (!candidates.empty()) {
            int best = -1;
            bool ambiguous = false;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                bool is_best = true;
                for (std::size_t j = 0; j < candidates.size(); ++j) {
                    if (i == j) continue;
                    if (!more_specialized(candidates[i], candidates[j])) {
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
               report(ref.location, ErrorCode::GenericAmbiguousSpecialization, std::vector<std::string>{ ref.generic_name, arguments_text(ref) });
               return std::nullopt;
           }
            if (best >= 0) {
                block = candidates[static_cast<std::size_t>(best)];
                sub = candidate_subs[static_cast<std::size_t>(best)];
                for (const auto& binding_entry : primary_sub.expressions) {
                    if (sub.expressions.count(binding_entry.first) == 0) {
                        sub.expressions[binding_entry.first] = binding_entry.second;
                    }
                }
            }
        }

        for (const auto& binding_entry : sub.expressions) {
            for (auto& member : block->members) {
                std::string member_name;
                if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) {
                    member_name = sd->name;
                }
                else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) {
                    member_name = fd->name;
                }
                if (!member_name.empty() && member_name == binding_entry.first) {
                    report(ref.location, ErrorCode::ExprParameterNameConflict,
                        std::vector<std::string>{ binding_entry.first });
                    return std::nullopt;
                }
            }
        }

        GenericRef key_ref = ref;
        key_ref.member.clear();
        std::string instance_key = key_ref.mangle();
        auto kind_it = instantiated_kind_signatures_.find(instance_key);
        std::string kind_signature = arguments_kind_signature(ref);
        if (kind_it == instantiated_kind_signatures_.end()) {
            instantiated_kind_signatures_[instance_key] = kind_signature;
        }
        else if (kind_it->second != kind_signature) {
            bool has_expression_argument = false;
            for (const GenericArgument& arg : ref.arguments) {
                if (arg.is_expr) {
                    has_expression_argument = true;
                    break;
                }
            }
            if (has_expression_argument) {
                report(ref.location, ErrorCode::ExprParameterNormalizationConflict,
                    std::vector<std::string>{});
            }
            else {
                report(ref.location, ErrorCode::GenericNormalizationConflict,
                    std::vector<std::string>{});
            }
            return std::nullopt;
        }

        sub.instance = key_ref;
        std::vector<TopLevel*> block_member_ptrs;
        std::vector<std::unique_ptr<TopLevel>> parsed_emit_members;
        for (auto& member : block->members) {
            block_member_ptrs.push_back(member.get());
        }
        if (!block->compile_time_items.empty()) {
            std::string emit_text;
            SourceLocation emit_loc = ref.location;
            if (!process_compile_time_items(block->compile_time_items, sub, emit_text,
                emit_loc, block_member_ptrs, parsed_emit_members)) {
                return std::nullopt;
            }
            if (!flush_emit_string(emit_text, emit_loc, block_member_ptrs,
                parsed_emit_members)) {
                return std::nullopt;
            }
        }
        if (!instance_scopes_.empty()) {
            instance_scopes_.back().insert(instance_key);
        }
        if (block != entry.primary) {
            std::unordered_set<std::string> block_members;
            for (auto& member : block->members) {
                if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) {
                    block_members.insert(sd->name);
                }
                else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) {
                    block_members.insert(fd->name);
                }
            }
            for (auto& member : entry.primary->members) {
                std::string member_name;
                if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) member_name = sd->name;
                else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) member_name = fd->name;
                if (member_name.empty() || block_members.count(member_name) != 0) continue;
                missing_members_[instance_key].insert(member_name);
            }
        }
        {
            std::vector<std::string>& members = instance_struct_members_[instance_key];
            members.clear();
            for (TopLevel* member : block_member_ptrs) {
                if (auto* sd = dynamic_cast<StructDefinition*>(member)) {
                    members.push_back(sd->name);
                }
            }
        }
        std::unordered_map<std::string, std::string> member_types;
        std::unordered_map<std::string, std::string> member_functions;
        for (TopLevel* member : block_member_ptrs) {
           if (auto* sd = dynamic_cast<StructDefinition*>(member)) {
               std::string mangled = instance_key + "$" + sd->name;
               member_types[sd->name] = mangled;
               sub.types[sd->name] = Type::make_struct(mangled);
                sub.members[sd->name] = mangled;
           }
           else if (auto* fd = dynamic_cast<FunctionDefinition*>(member)) {
               member_functions[fd->name] = instance_key + "$" + fd->name;
                sub.members[fd->name] = instance_key + "$" + fd->name;
           }
        }

        auto materialize = [&](const std::string& member_name, bool register_short) -> std::string {
            std::string first_target;
            for (TopLevel* member : block_member_ptrs) {
                if (auto* sd = dynamic_cast<StructDefinition*>(member)) {
                    if (!member_name.empty() && sd->name != member_name) continue;
                    std::string mangled = instance_key + "$" + sd->name;
                    if (instantiated_members_.insert(mangled).second) {
                        auto clone = std::make_unique<StructDefinition>(sd->location, mangled,
                            std::vector<StructDefinition::Member>{});
                        clone->no_copy = sd->no_copy;
                        clone->no_move = sd->no_move;
                        for (const auto& m : sd->members) {
                            clone->members.push_back(clone_member(m, sub));
                        }
                        for (const auto& special : sd->special_members) {
                            clone->special_members.push_back(clone_special_member(special.get(),
                                sub));
                        }
                        StructDefinition* raw = clone.get();
                        struct_defs_[mangled] = raw;
                        instantiated_structs_[mangled] = raw;
                        add_top_level(std::move(clone));
                    }
                    if (register_short) {
                        ShortBinding binding;
                        binding.target = mangled;
                        binding.is_type = true;
                        binding.instance = instance_key;
                        register_short_name(sd->name, binding, ref.generic_name, sd->location);
                    }
                    if (!member_name.empty() && first_target.empty()) {
                        first_target = mangled;
                    }
                }
                else if (auto* fd = dynamic_cast<FunctionDefinition*>(member)) {
                    if (!member_name.empty() && fd->name != member_name) continue;
                    std::string mangled = instance_key + "$" + fd->name;
                    std::string member_key = mangled + "(";
                    for (const Type& p : fd->parameters) {
                        member_key += p.to_string();
                        member_key += ",";
                    }
                    member_key += ")";
                    if (instantiated_members_.insert(member_key).second) {
                        auto clone = clone_function(fd, sub, mangled);
                        func_defs_[mangled] = clone.get();
                        add_top_level(std::move(clone));
                    }
                    if (register_short) {
                        ShortBinding binding;
                        binding.target = mangled;
                        binding.is_type = false;
                        binding.instance = instance_key;
                        if (!fd->is_operator) {
                            register_short_name(fd->name, binding, ref.generic_name,
                                fd->location);
                        }
                    }
                    if (!member_name.empty() && first_target.empty()) {
                        first_target = mangled;
                    }
                }
            }
            return first_target;
        };

        if (whole_block) {
            if (instantiated_blocks_.insert(instance_key).second) {
                materialize(std::string(), true);
            }
            else {
                materialize(std::string(), false);
            }
            return instance_key;
        }

        if (ref.member.empty()) {
            return instance_key;
        }
        std::string target = materialize(ref.member, ref.member_scope_access);
        if (target.empty()) {
            bool in_primary = false;
            if (entry.primary != block) {
                for (auto& member : entry.primary->members) {
                    std::string member_name;
                    if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) member_name = sd->name;
                    else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) member_name = fd->name;
                    if (member_name == ref.member) { in_primary = true; break; }
                }
            }
            if (in_primary) {
                report(ref.location, ErrorCode::GenericSpecializationMissingMember, std::vector<std::string>{ instance_key, ref.member });
            }
            else {
                report(ref.location, ErrorCode::GenericMemberNotInInstantiation, std::vector<std::string>{ ref.member, instance_key });
            }
            return std::nullopt;
        }
        return target;
    }


    AST::StructDefinition::Member GenericExpander::clone_member(
        const StructDefinition::Member& member, const Substitution& sub) {
        return clone_member_impl(member, sub);
    }

    AST::StructDefinition::Member GenericExpander::clone_member_impl(
        const StructDefinition::Member& member, const Substitution& sub) {
        Type type = member.type;
        Type substituted = Type::make_void();
        if (!substitute_type(type, sub, substituted)) {
            substituted = Type::make_void();
        }
        std::optional<Type> func_ptr = std::nullopt;
        if (member.function_pointer_type.has_value()) {
            Type fp = Type::make_void();
            substitute_type(*member.function_pointer_type, sub, fp);
            func_ptr = fp;
        }
        std::unique_ptr<Initializer> init = member.initializer
            ? clone_initializer(member.initializer.get(), sub) : nullptr;
        StructDefinition::Member clone(member.location, std::move(substituted), member.name,
            member.array_size, std::move(func_ptr), std::move(init));
        if (member.array_size_expr) {
            ConstantValue value;
            if (evaluate_with_substitution(member.array_size_expr.get(), sub, value)) {
                long long length = value.is_float
                    ? static_cast<long long>(value.float_value) : value.int_value;
                if (length > 0) {
                    clone.array_size = static_cast<std::size_t>(length);
                    clone.type.array_size = static_cast<std::size_t>(length);
                }
            }
        }
        return clone;
    }

    std::unique_ptr<Initializer> GenericExpander::clone_initializer(
        const Initializer* init, const Substitution& sub) {
        if (auto* expr_init = dynamic_cast<const ExpressionInitializer*>(init)) {
            return std::make_unique<ExpressionInitializer>(init->location,
                clone_expression(expr_init->expr.get(), sub));
        }
        if (auto* arr_init = dynamic_cast<const ArrayInitializer*>(init)) {
            std::vector<std::unique_ptr<Initializer>> elements;
            for (const auto& element : arr_init->elements) {
                elements.push_back(clone_initializer(element.get(), sub));
            }
            auto clone = std::make_unique<ArrayInitializer>(init->location,
                std::move(elements));
            clone->from_paren_call = arr_init->from_paren_call;
            return clone;
        }
        return nullptr;
    }

    std::unique_ptr<Expression> GenericExpander::make_constant_literal(SourceLocation loc,
        const ConstantValue& value) {
        std::string text;
        TokenType type = TokenType::IntegerLiteral;
        if (value.is_string) {
            text = "\"";
            for (char c : value.string_value) {
                switch (c) {
                case '\\': text += "\\\\"; break;
                case '"':  text += "\\\""; break;
                case '\n': text += "\\n"; break;
                case '\t': text += "\\t"; break;
                case '\r': text += "\\r"; break;
                default:   text.push_back(c); break;
                }
            }
            text += "\"";
            type = TokenType::StringLiteral;
        }
        else if (value.is_float) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%g", value.float_value);
            text = buffer;
            type = TokenType::FloatLiteral;
        }
        else {
            text = std::to_string(value.int_value);
        }
        lexeme_pool_.push_back(text);
        Token literal(type, loc, lexeme_pool_.back());
        return std::make_unique<PrimaryExpression>(loc, literal);
    }

    std::unique_ptr<Expression> GenericExpander::clone_expression(
        const Expression* expr, const Substitution& sub) {
        if (expr == nullptr) return nullptr;
        if (auto* e = dynamic_cast<const PostfixExpression*>(expr)) {
            if (auto rewritten = rewrite_property_expression(e, sub)) {
                return rewritten;
            }
        }
        if (auto* e = dynamic_cast<const CompileTimePropertyExpression*>(expr)) {
            std::vector<const Expression*> args;
            for (const auto& arg : e->arguments) args.push_back(arg.get());
            bool value = false;
            if (eval_bool_property(e->receiver.get(), e->property, args, sub, value)) {
                return make_constant_literal(e->location,
                    ConstantValue{ value ? 1LL : 0LL, 0.0, false, false, std::string() });
            }
            auto* receiver = dynamic_cast<const PrimaryExpression*>(e->receiver.get());
            report(e->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ e->property, receiver != nullptr ? receiver->identifier
                                                   : std::string("<expr>") });
            return make_constant_literal(e->location,
                ConstantValue{ 0LL, 0.0, false, false, std::string() });
        }
        if (auto* e = dynamic_cast<const AssignmentExpression*>(expr)) {
            if (auto* target = dynamic_cast<const PrimaryExpression*>(e->left.get())) {
                if (target->kind == PrimaryExpression::Kind::Identifier &&
                    sub.constants.count(target->identifier) != 0) {
                    report(target->location, ErrorCode::GenericConstantParameterRuntimeUse, std::vector<std::string>{ target->identifier });
                }
            }
            return std::make_unique<AssignmentExpression>(e->location,
                clone_expression(e->left.get(), sub), e->op,
                clone_expression(e->right.get(), sub));
        }
        if (auto* e = dynamic_cast<const LogicalOrExpression*>(expr)) {
            return std::make_unique<LogicalOrExpression>(e->location,
                clone_expression(e->left.get(), sub), clone_expression(e->right.get(), sub));
        }
        if (auto* e = dynamic_cast<const LogicalAndExpression*>(expr)) {
            return std::make_unique<LogicalAndExpression>(e->location,
                clone_expression(e->left.get(), sub), clone_expression(e->right.get(), sub));
        }
        if (auto* e = dynamic_cast<const ComparisonExpression*>(expr)) {
            return std::make_unique<ComparisonExpression>(e->location,
                clone_expression(e->left.get(), sub), e->op,
                clone_expression(e->right.get(), sub));
        }
        if (auto* e = dynamic_cast<const AdditiveExpression*>(expr)) {
            return std::make_unique<AdditiveExpression>(e->location,
                clone_expression(e->left.get(), sub), e->op,
                clone_expression(e->right.get(), sub));
        }
        if (auto* e = dynamic_cast<const MultiplicativeExpression*>(expr)) {
            return std::make_unique<MultiplicativeExpression>(e->location,
                clone_expression(e->left.get(), sub), e->op,
                clone_expression(e->right.get(), sub));
        }
        if (auto* e = dynamic_cast<const PowerExpression*>(expr)) {
            return std::make_unique<PowerExpression>(e->location,
                clone_expression(e->left.get(), sub), clone_expression(e->right.get(), sub));
        }
        if (auto* e = dynamic_cast<const UnaryExpression*>(expr)) {
            if (e->op == UnaryExpression::Operator::AddressOf ||
                e->op == UnaryExpression::Operator::Increment ||
                e->op == UnaryExpression::Operator::Decrement) {
                if (auto* operand = dynamic_cast<const PrimaryExpression*>(e->operand.get())) {
                    if (operand->kind == PrimaryExpression::Kind::Identifier &&
                        sub.constants.count(operand->identifier) != 0) {
                        report(operand->location,
                            ErrorCode::GenericConstantParameterRuntimeUse, std::vector<std::string>{ operand->identifier });
                    }
                }
            }
            return std::make_unique<UnaryExpression>(e->location, e->op,
                clone_expression(e->operand.get(), sub));
        }
        if (auto* e = dynamic_cast<const PostfixExpression*>(expr)) {
            if (e->op == PostfixExpression::Operator::FunctionCall) {
                if (auto* callee = dynamic_cast<const PrimaryExpression*>(e->base.get())) {
                    if (callee->kind == PrimaryExpression::Kind::Identifier &&
                        sub.expressions.count(callee->identifier) != 0) {
                        std::vector<std::unique_ptr<Expression>> arg_clones;
                        for (const auto& arg : e->arguments) {
                            arg_clones.push_back(clone_expression(arg.get(), sub));
                        }
                        bool ok = true;
                        auto expanded = expand_expression_call(current_function_, sub,
                            callee->identifier, arg_clones, e->location, ok);
                        if (ok) {
                            return expanded;
                        }
                        return make_constant_literal(e->location,
                            ConstantValue{ 0LL, 0.0, false, false, std::string() });
                    }
                }
            }
            if (e->op == PostfixExpression::Operator::Cast) {
                if (auto* prim = dynamic_cast<const PrimaryExpression*>(e->base.get())) {
                    if (prim->kind == PrimaryExpression::Kind::Identifier) {
                        auto constant = sub.constants.find(prim->identifier);
                        if (constant != sub.constants.end()) {
                            auto clone = std::make_unique<PostfixExpression>(e->location,
                                make_constant_literal(e->location, constant->second), e->op);
                            Type cast = Type::make_void();
                            substitute_type(e->cast_type, sub, cast);
                            clone->cast_type = std::move(cast);
                            return clone;
                        }
                    }
                }
            }
            auto clone = std::make_unique<PostfixExpression>(e->location,
                clone_expression(e->base.get(), sub), e->op);
            clone->subscript_expr = clone_expression(e->subscript_expr.get(), sub);
            for (const auto& arg : e->arguments) {
                clone->arguments.push_back(clone_expression(arg.get(), sub));
            }
            Type cast = Type::make_void();
            substitute_type(e->cast_type, sub, cast);
            clone->cast_type = std::move(cast);
            clone->member_name = e->member_name;
            return clone;
        }
        if (auto* e = dynamic_cast<const PrimaryExpression*>(expr)) {
            switch (e->kind) {
            case PrimaryExpression::Kind::Literal:
                return std::make_unique<PrimaryExpression>(e->location, e->literal_token);
            case PrimaryExpression::Kind::Identifier: {
                if (sub.expressions.count(e->identifier) != 0 &&
                    sub.expr_arguments.count(e->identifier) == 0) {
                    report(e->location, ErrorCode::ExprParameterCannotBeUsedAsValue,
                        std::vector<std::string>{ e->identifier });
                    return make_constant_literal(e->location,
                        ConstantValue{ 0LL, 0.0, false, false, std::string() });
                }
                auto argument = sub.expr_arguments.find(e->identifier);
                if (argument != sub.expr_arguments.end() && argument->second != nullptr) {
                    Substitution neutral;
                    auto cloned_argument = clone_expression(argument->second, neutral);
                    auto declared = sub.expr_argument_types.find(e->identifier);
                    if (cloned_argument != nullptr && declared != sub.expr_argument_types.end()) {
                        Type declared_type = declared->second;
                        bool wrap = false;
                        switch (declared_type.kind) {
                        case TypeKind::Int:
                        case TypeKind::Lint:
                        case TypeKind::Uint:
                        case TypeKind::Luint:
                        case TypeKind::Float:
                        case TypeKind::Double:
                        case TypeKind::Char:
                        case TypeKind::Uchar:
                        case TypeKind::Bool:
                            wrap = true;
                            break;
                        default:
                            break;
                        }
                        if (wrap) {
                            auto cast = std::make_unique<PostfixExpression>(e->location,
                                std::move(cloned_argument),
                                PostfixExpression::Operator::Cast);
                            cast->cast_type = declared_type;
                            auto index_it =
                                expression_parameter_index_.find(e->identifier);
                            if (index_it != expression_parameter_index_.end()) {
                                expression_argument_casts_[cast.get()] =
                                    std::make_tuple(e->identifier, index_it->second,
                                        declared_type);
                            }
                            return cast;
                        }
                    }
                    return cloned_argument;
                }
                auto renamed = sub.renames.find(e->identifier);
                if (renamed != sub.renames.end()) {
                    return std::make_unique<PrimaryExpression>(e->location,
                        renamed->second);
                }
                auto found = sub.types.find(e->identifier);
                if (found != sub.types.end()) {
                    return std::make_unique<PrimaryExpression>(e->location,
                        found->second.to_string());
                }
                auto constant = sub.constants.find(e->identifier);
                if (constant != sub.constants.end()) {
                    AST::Type declared = AST::Type::make_int();
                    auto declared_type = sub.constant_types.find(e->identifier);
                    if (declared_type != sub.constant_types.end()) {
                        declared = declared_type->second;
                    }
                    return make_typed_constant_literal(e->location, constant->second, declared);
                }
                auto member = sub.members.find(e->identifier);
                if (member != sub.members.end()) {
                    GenericRef dependency = sub.instance;
                    dependency.member = e->identifier;
                    dependency.location = e->location;
                    ensure_instantiation(dependency, false);
                    return std::make_unique<PrimaryExpression>(e->location, member->second);
                }
                for (auto it = short_scopes_.rbegin(); it != short_scopes_.rend(); ++it) {
                    auto binding = it->find(e->identifier);
                    if (binding != it->end()) {
                        if (binding->second.is_type) {
                            return std::make_unique<PrimaryExpression>(e->location,
                                binding->second.target);
                        }
                        break;
                    }
                }
                {
                    auto plain = std::make_unique<PrimaryExpression>(e->location,
                        e->identifier);
                    if (in_expression_body_) {
                        expression_free_identifiers_[plain.get()] = e->identifier;
                    }
                    return plain;
                }
            }
            case PrimaryExpression::Kind::Parens:
                return std::make_unique<PrimaryExpression>(e->location,
                    clone_expression(e->paren_expr.get(), sub));
           case PrimaryExpression::Kind::Null:
                return PrimaryExpression::make_null(e->location);
            case PrimaryExpression::Kind::Heap: {
                Type heap_type = Type::make_void();
                substitute_type(e->heap_type, sub, heap_type);
                return std::make_unique<PrimaryExpression>(e->location,
                    std::move(heap_type), clone_expression(e->heap_size.get(), sub));
            }
            case PrimaryExpression::Kind::QualifiedName: {
                GenericRef ref = *e->generic_ref;
                for (GenericArgument& arg : ref.arguments) {
                    if (arg.is_type) {
                        Type substituted = Type::make_void();
                        substitute_type(arg.type, sub, substituted);
                        arg.type = std::move(substituted);
                        arg.text = arg.type.to_string();
                    }
                    else {
                        auto found = sub.constants.find(arg.text);
                        if (found != sub.constants.end()) {
                            if (found->second.is_float) {
                                arg.float_constant = true;
                                arg.float_value = found->second.float_value;
                            }
                            else {
                                arg.int_value = found->second.int_value;
                            }
                            arg.text = arg.normalize();
                        }
                    }
                }
                auto mangled = ensure_instantiation(ref, false);
                if (mangled.has_value()) {
                    return std::make_unique<PrimaryExpression>(e->location, *mangled);
                }
                return std::make_unique<PrimaryExpression>(e->location, ref.to_string());
            }
            case PrimaryExpression::Kind::Construct:
            case PrimaryExpression::Kind::PlacementConstruct: {
                Type construct_type = Type::make_void();
                substitute_type(e->construct_type, sub, construct_type);
                std::vector<std::unique_ptr<Expression>> args;
                for (const auto& arg : e->construct_args) {
                    args.push_back(clone_expression(arg.get(), sub));
                }
                return std::make_unique<PrimaryExpression>(e->location, std::move(construct_type),
                    std::move(args), clone_expression(e->placement_target.get(), sub));
            }
            case PrimaryExpression::Kind::CopyMove:
                return std::make_unique<PrimaryExpression>(e->location, e->copy_move_kind,
                    clone_expression(e->paren_expr.get(), sub));
            case PrimaryExpression::Kind::NamespaceQualified:
                return std::make_unique<PrimaryExpression>(e->location, e->qualified_path);
            }
        }
        return nullptr;
    }

    std::unique_ptr<Statement> GenericExpander::clone_statement(
        const Statement* stmt, const Substitution& sub) {
        bool outermost = pending_hoisted_ == nullptr;
        std::vector<std::unique_ptr<Statement>> hoisted;
        if (outermost) {
            pending_hoisted_ = &hoisted;
        }
        auto cloned = clone_statement_impl(stmt, sub);
        if (outermost) {
            pending_hoisted_ = nullptr;
            if (!hoisted.empty()) {
                hoisted.push_back(std::move(cloned));
                return std::make_unique<Block>(stmt->location, std::move(hoisted));
            }
        }
        return cloned;
    }

    std::unique_ptr<Statement> GenericExpander::clone_substatement(
        const Statement* stmt, const Substitution& sub) {
        if (stmt == nullptr) {
            return nullptr;
        }
        if (pending_hoisted_ != nullptr) {
            return clone_statement_impl(stmt, sub);
        }
        std::vector<std::unique_ptr<Statement>> hoisted;
        pending_hoisted_ = &hoisted;
        auto cloned = clone_statement_impl(stmt, sub);
        pending_hoisted_ = nullptr;
        if (hoisted.empty()) {
            return cloned;
        }
        hoisted.push_back(std::move(cloned));
        return std::make_unique<Block>(stmt->location, std::move(hoisted));
    }

    bool GenericExpander::body_always_returns(const Statement* stmt) const {
        if (stmt == nullptr) {
            return false;
        }
        if (dynamic_cast<const ReturnStatement*>(stmt) != nullptr) {
            return true;
        }
        if (auto* block = dynamic_cast<const Block*>(stmt)) {
            for (auto it = block->statements.rbegin(); it != block->statements.rend(); ++it) {
                if (dynamic_cast<const EmptyStatement*>(it->get()) != nullptr) {
                    continue;
                }
                return body_always_returns(it->get());
            }
            return false;
        }
        if (auto* if_stmt = dynamic_cast<const IfStatement*>(stmt)) {
            return if_stmt->else_block != nullptr &&
                body_always_returns(if_stmt->then_block.get()) &&
                body_always_returns(if_stmt->else_block.get());
        }
        return false;
    }

    bool GenericExpander::emit_body_into_temp(
        const std::vector<std::unique_ptr<Statement>>& stmts,
        const Substitution& sub, const std::string& temp_name,
        const Type& result_type,
        std::vector<std::unique_ptr<Statement>>& out) {
        if (stmts.empty()) {
            return false;
        }
        std::size_t last_index = stmts.size();
        while (last_index > 0 &&
            dynamic_cast<const EmptyStatement*>(stmts[last_index - 1].get()) != nullptr) {
            --last_index;
        }
        if (last_index == 0) {
            return false;
        }
        for (std::size_t i = 0; i + 1 < last_index; ++i) {
            const Statement* stmt = stmts[i].get();
            if (dynamic_cast<const EmptyStatement*>(stmt) != nullptr) {
                continue;
            }
            if (body_always_returns(stmt)) {
                if (auto* if_stmt = dynamic_cast<const IfStatement*>(stmt)) {
                    std::vector<std::unique_ptr<Statement>> then_body;
                    std::vector<std::unique_ptr<Statement>> else_body;
                    const Statement* then_stmt = if_stmt->then_block.get();
                    if (auto* block = dynamic_cast<const Block*>(then_stmt)) {
                        for (const auto& s : block->statements) {
                            then_body.push_back(clone_statement(s.get(), sub));
                        }
                    }
                    else {
                        then_body.push_back(clone_statement(then_stmt, sub));
                    }
                    const Statement* else_stmt = if_stmt->else_block.get();
                    else_body.push_back(std::make_unique<Block>(if_stmt->location,
                        std::vector<std::unique_ptr<Statement>>{}));
                    std::vector<std::unique_ptr<Statement>> rest;
                    for (std::size_t k = i + 1; k < last_index; ++k) {
                        rest.push_back(clone_statement(stmts[k].get(), sub));
                    }
                    std::vector<std::unique_ptr<Statement>> then_result;
                    std::vector<std::unique_ptr<Statement>> rest_result;
                    if (!emit_body_into_temp(then_body, sub, temp_name, result_type,
                        then_result)) {
                        return false;
                    }
                    if (!emit_body_into_temp(rest, sub, temp_name, result_type,
                        rest_result)) {
                        return false;
                    }
                    if (else_stmt != nullptr) {
                        std::vector<std::unique_ptr<Statement>> explicit_else;
                        if (auto* eb = dynamic_cast<const Block*>(else_stmt)) {
                            for (const auto& s : eb->statements) {
                                explicit_else.push_back(clone_statement(s.get(), sub));
                            }
                        }
                        else {
                            explicit_else.push_back(clone_statement(else_stmt, sub));
                        }
                        std::vector<std::unique_ptr<Statement>> else_result;
                        if (!emit_body_into_temp(explicit_else, sub, temp_name, result_type,
                            else_result)) {
                            return false;
                        }
                        for (auto& s : else_result) rest_result.push_back(std::move(s));
                    }
                    out.push_back(std::make_unique<IfStatement>(if_stmt->location,
                        clone_expression(if_stmt->condition.get(), sub),
                        std::make_unique<Block>(if_stmt->location, std::move(then_result)),
                        std::make_unique<Block>(if_stmt->location, std::move(rest_result))));
                    return true;
                }
                return false;
            }
            out.push_back(clone_statement(stmt, sub));
        }
        const Statement* tail = stmts[last_index - 1].get();
        auto make_assign = [&](const Expression* value) -> std::unique_ptr<Statement> {
            auto target = std::make_unique<PrimaryExpression>(tail->location, temp_name);
            auto assignment = std::make_unique<AssignmentExpression>(tail->location,
                std::move(target), AssignmentExpression::Operator::Assign,
                clone_expression(value, sub));
            return std::make_unique<ExpressionStatement>(tail->location,
                std::move(assignment));
        };
        if (auto* ret = dynamic_cast<const ReturnStatement*>(tail)) {
            if (ret->value == nullptr) {
                return result_type.kind == TypeKind::Void;
            }
            if (result_type.kind != TypeKind::Void) {
                out.push_back(make_assign(ret->value.get()));
            }
            return true;
        }
        if (auto* expr_stmt = dynamic_cast<const ExpressionStatement*>(tail)) {
            if (result_type.kind != TypeKind::Void) {
                out.push_back(make_assign(expr_stmt->expr.get()));
            }
            else {
                out.push_back(clone_statement(tail, sub));
            }
            return true;
        }
        if (auto* block = dynamic_cast<const Block*>(tail)) {
            std::vector<std::unique_ptr<Statement>> tail_stmts;
            for (const auto& s : block->statements) {
                tail_stmts.push_back(clone_statement(s.get(), sub));
            }
            return emit_body_into_temp(tail_stmts, sub, temp_name, result_type, out);
        }
        return false;
    }

    std::unique_ptr<Statement> GenericExpander::clone_statement_impl(
        const Statement* stmt, const Substitution& sub) {
        if (stmt == nullptr) return nullptr;
        if (auto* s = dynamic_cast<const Block*>(stmt)) {
            std::vector<std::unique_ptr<Statement>> statements;
            for (const auto& child : s->statements) {
                statements.push_back(clone_substatement(child.get(), sub));
            }
            return std::make_unique<Block>(s->location, std::move(statements));
        }
        if (auto* s = dynamic_cast<const VariableDeclaration*>(stmt)) {
            Type type = Type::make_void();
            substitute_type(s->type, sub, type);
            std::string declared_name = s->name;
            auto renamed = sub.renames.find(s->name);
            if (renamed != sub.renames.end()) {
                declared_name = renamed->second;
            }
            auto clone = std::make_unique<VariableDeclaration>(s->location, std::move(type),
                declared_name, s->array_size, std::nullopt,
                s->initializer ? clone_initializer(s->initializer.get(), sub) : nullptr);
            if (s->function_pointer_type.has_value()) {
                Type fp = Type::make_void();
                substitute_type(*s->function_pointer_type, sub, fp);
                clone->function_pointer_type = fp;
            }
            if (s->array_size_expr) {
                ConstantValue value;
                if (evaluate_with_substitution(s->array_size_expr.get(), sub, value)) {
                    long long length = value.is_float
                        ? static_cast<long long>(value.float_value) : value.int_value;
                    if (length > 0) {
                        clone->array_size = static_cast<std::size_t>(length);
                        clone->type.array_size = static_cast<std::size_t>(length);
                    }
                }
            }
            return clone;
        }
        if (auto* s = dynamic_cast<const IfStatement*>(stmt)) {
            bool compile_time = false;
            bool taken = false;
            if (eval_compile_time_condition(s->condition.get(), sub, taken)) {
                compile_time = true;
            }
            if (compile_time) {
                const Statement* branch = taken ? s->then_block.get() : s->else_block.get();
                if (branch == nullptr) {
                    return std::make_unique<Block>(s->location,
                        std::vector<std::unique_ptr<Statement>>{});
                }
                if (dynamic_cast<const Block*>(branch) != nullptr) {
                    return clone_substatement(branch, sub);
                }
                std::vector<std::unique_ptr<Statement>> wrapped;
                wrapped.push_back(clone_substatement(branch, sub));
                return std::make_unique<Block>(s->location, std::move(wrapped));
            }
            return std::make_unique<IfStatement>(s->location,
                clone_expression(s->condition.get(), sub),
                clone_substatement(s->then_block.get(), sub),
                clone_substatement(s->else_block.get(), sub));
        }
        if (auto* s = dynamic_cast<const ForStatement*>(stmt)) {
            return std::make_unique<ForStatement>(s->location,
                clone_substatement(s->init.get(), sub),
                clone_expression(s->condition.get(), sub),
                clone_expression(s->step.get(), sub),
                clone_substatement(s->body.get(), sub));
        }
        if (auto* s = dynamic_cast<const WhileStatement*>(stmt)) {
            return std::make_unique<WhileStatement>(s->location,
                clone_expression(s->condition.get(), sub),
                clone_substatement(s->body.get(), sub));
        }
        if (auto* s = dynamic_cast<const BreakStatement*>(stmt)) {
            return std::make_unique<BreakStatement>(s->location);
        }
        if (auto* s = dynamic_cast<const ReturnStatement*>(stmt)) {
            return std::make_unique<ReturnStatement>(s->location,
                clone_expression(s->value.get(), sub));
        }
        if (auto* s = dynamic_cast<const ExpressionStatement*>(stmt)) {
            return std::make_unique<ExpressionStatement>(s->location,
                clone_expression(s->expr.get(), sub));
        }
        if (auto* s = dynamic_cast<const DestructStatement*>(stmt)) {
            return std::make_unique<DestructStatement>(s->location,
                clone_expression(s->target.get(), sub));
        }
        if (auto* s = dynamic_cast<const EmptyStatement*>(stmt)) {
            return std::make_unique<EmptyStatement>(s->location);
        }
        if (auto* s = dynamic_cast<const InstantiationStatement*>(stmt)) {
            return std::make_unique<InstantiationStatement>(s->location, s->reference);
        }
        return nullptr;
    }

    std::unique_ptr<FunctionDefinition> GenericExpander::clone_function(
        const FunctionDefinition* func, const Substitution& sub, const std::string& name) {
        Substitution body_sub = sub;
        if (!sub.expressions.empty()) {
            std::unordered_set<std::string> free_names;
            for (const auto& entry : sub.expressions) {
                std::unordered_set<std::string> bound_names(
                    entry.second.parameter_names.begin(),
                    entry.second.parameter_names.end());
                if (entry.second.body != nullptr) {
                    for (const auto& stmt : entry.second.body->statements) {
                        collect_free_identifiers_in_statement(stmt.get(), free_names);
                    }
                }
                for (const std::string& bound_name : bound_names) {
                    free_names.erase(bound_name);
                }
            }
            if (!free_names.empty()) {
                std::unordered_set<std::string> locals;
                collect_declared_names(func->body.get(), locals);
                for (const std::string& local : locals) {
                    if (free_names.count(local) != 0) {
                        body_sub.renames[local] = local + "$" + name;
                    }
                }
            }
        }
        Type ret = Type::make_void();
        substitute_type(func->return_type, sub, ret);
        std::vector<Type> params;
        for (const Type& p : func->parameters) {
            Type substituted = Type::make_void();
            substitute_type(p, sub, substituted);
            params.push_back(std::move(substituted));
        }
        auto clone = std::make_unique<FunctionDefinition>(func->location, std::move(ret), name,
            params, func->param_names, clone_statement(func->body.get(), body_sub));
        clone->is_operator = func->is_operator;
        clone->overloaded_operator = func->overloaded_operator;
        clone->is_conversion_operator = func->is_conversion_operator;
        if (func->is_conversion_operator) {
            Type target = Type::make_void();
            substitute_type(func->conversion_target_type, sub, target);
            clone->conversion_target_type = std::move(target);
        }
        else {
            clone->conversion_target_type = func->conversion_target_type;
        }
        clone->param_defaults.reserve(func->param_defaults.size());
        for (const auto& default_value : func->param_defaults) {
            if (default_value == nullptr) {
                clone->param_defaults.push_back(nullptr);
            }
            else {
                clone->param_defaults.push_back(
                    clone_expression(default_value.get(), body_sub));
            }
        }
        return clone;
    }

    std::unique_ptr<SpecialMemberFunction> GenericExpander::clone_special_member(
        const SpecialMemberFunction* member, const Substitution& sub) {
        auto clone = std::make_unique<SpecialMemberFunction>(member->location, member->kind);
        for (const Type& param : member->parameters) {
            Type substituted = Type::make_void();
            substitute_type(param, sub, substituted);
            clone->parameters.push_back(std::move(substituted));
        }
        clone->parameter_names = member->parameter_names;
        clone->parameter_defaults.reserve(member->parameter_defaults.size());
        for (const auto& default_value : member->parameter_defaults) {
            if (default_value == nullptr) {
                clone->parameter_defaults.push_back(nullptr);
            }
            else {
                clone->parameter_defaults.push_back(clone_expression(default_value.get(), sub));
            }
        }
        Type source = Type::make_void();
        substitute_type(member->parameter_type, sub, source);
        clone->parameter_type = std::move(source);
        clone->parameter_name = member->parameter_name;
        clone->body = clone_statement(member->body.get(), sub);
        return clone;
    }

    bool GenericExpander::decode_string_literal(std::string_view lexeme,
        std::string& out) const {
        std::string_view content = lexeme;
        if (content.size() >= 2 && content.front() == '"' && content.back() == '"') {
            content = content.substr(1, content.size() - 2);
        }
        out.clear();
        for (std::size_t i = 0; i < content.size(); ++i) {
            char c = content[i];
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (++i >= content.size()) break;
            char e = content[i];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'v': out.push_back('\v'); break;
            case '0': out.push_back('\0'); break;
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            case '\'': out.push_back('\''); break;
            case 'x': {
                int value = 0;
                int digits = 0;
                while (i + 1 < content.size() && digits < 2) {
                    char h = content[i + 1];
                    int d = (h >= '0' && h <= '9') ? (h - '0')
                        : (h >= 'a' && h <= 'f') ? (h - 'a' + 10)
                        : (h >= 'A' && h <= 'F') ? (h - 'A' + 10) : -1;
                    if (d < 0) break;
                    ++i;
                    value = value * 16 + d;
                    ++digits;
                }
                out.push_back(static_cast<char>(value));
                break;
            }
            default:
                if (e >= '0' && e <= '7') {
                    int value = e - '0';
                    int digits = 1;
                    while (i + 1 < content.size() && digits < 3 &&
                        content[i + 1] >= '0' && content[i + 1] <= '7') {
                        ++i;
                        value = value * 8 + (content[i] - '0');
                        ++digits;
                    }
                    out.push_back(static_cast<char>(value));
                }
                else {
                    out.push_back(e);
                }
                break;
            }
        }
        return true;
    }

    std::string GenericExpander::expression_text(const Expression* expr) const {
        if (expr == nullptr) return "<empty>";
        if (auto* e = dynamic_cast<const PrimaryExpression*>(expr)) {
            switch (e->kind) {
            case PrimaryExpression::Kind::Literal:
                return std::string(e->literal_token.lexeme);
            case PrimaryExpression::Kind::Identifier:
                return e->identifier;
            default:
                return "<expression>";
            }
        }
        if (auto* e = dynamic_cast<const PostfixExpression*>(expr)) {
            if (e->op == PostfixExpression::Operator::Dot) {
                return expression_text(e->base.get()) + "." + e->member_name;
            }
            if (e->op == PostfixExpression::Operator::FunctionCall) {
                return expression_text(e->base.get()) + "(...)";
            }
            return "<expression>";
        }
        if (auto* e = dynamic_cast<const CompileTimePropertyExpression*>(expr)) {
            return expression_text(e->receiver.get()) + "." + e->property + "<...>";
        }
        if (auto* e = dynamic_cast<const AdditiveExpression*>(expr)) {
            return expression_text(e->left.get()) + " + " + expression_text(e->right.get());
        }
        if (auto* e = dynamic_cast<const ComparisonExpression*>(expr)) {
            const char* op = "==";
            switch (e->op) {
            case ComparisonExpression::Operator::Greater: op = ">"; break;
            case ComparisonExpression::Operator::Less: op = "<"; break;
            case ComparisonExpression::Operator::Equal: op = "=="; break;
            case ComparisonExpression::Operator::NotEqual: op = "!="; break;
            case ComparisonExpression::Operator::GreaterEqual: op = ">="; break;
            case ComparisonExpression::Operator::LessEqual: op = "<="; break;
            }
            return expression_text(e->left.get()) + " " + op + " " +
                expression_text(e->right.get());
        }
        if (auto* e = dynamic_cast<const LogicalAndExpression*>(expr)) {
            return expression_text(e->left.get()) + " && " +
                expression_text(e->right.get());
        }
        if (auto* e = dynamic_cast<const LogicalOrExpression*>(expr)) {
            return expression_text(e->left.get()) + " || " +
                expression_text(e->right.get());
        }
        return "<expression>";
    }

    std::unique_ptr<Expression> GenericExpander::rewrite_property_expression(
        const PostfixExpression* expr, const Substitution& sub) {
        if (expr == nullptr || expr->op != PostfixExpression::Operator::Dot) return nullptr;
        auto* prim = dynamic_cast<const PrimaryExpression*>(expr->base.get());
        if (prim == nullptr || prim->kind != PrimaryExpression::Kind::Identifier) {
            return nullptr;
        }
        const std::string& parameter = prim->identifier;
        bool is_type_param = (sub.types.find(parameter) != sub.types.end());
        bool is_constant_param =
            (sub.constant_types.find(parameter) != sub.constant_types.end());
        if (!is_type_param && !is_constant_param) return nullptr;

        if (expr->member_name == "size" || expr->member_name == "align") {
            long long value = 0;
            if (!eval_size_align_property(parameter, expr->member_name, sub, value)) {
                report(expr->location, ErrorCode::CompileTimePropertyNotApplicable,
                    std::vector<std::string>{ expr->member_name, parameter });
                value = 0;
            }
            return make_constant_literal(expr->location,
                ConstantValue{ value, 0.0, false, false, std::string() });
        }
        if (expr->member_name == "typename") {
            if (!is_type_param) {
                report(expr->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ expr->member_name, parameter });
                return make_constant_literal(expr->location,
                    ConstantValue{ 0LL, 0.0, false, false, std::string() });
            }
            ConstantValue value;
            value.is_string = true;
            value.string_value = sub.types.at(parameter).to_string();
            return make_constant_literal(expr->location, value);
        }
        if (expr->member_name.rfind("is_", 0) == 0) {
            bool value = false;
            if (eval_bool_property(expr->base.get(), expr->member_name, {}, sub, value)) {
                return make_constant_literal(expr->location,
                    ConstantValue{ value ? 1LL : 0LL, 0.0, false, false, std::string() });
            }
            report(expr->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ expr->member_name, parameter });
            return make_constant_literal(expr->location,
                ConstantValue{ 0LL, 0.0, false, false, std::string() });
        }
        return nullptr;
    }

    bool GenericExpander::param_type_of(const std::string& name,
        const Substitution& sub, AST::Type& out) const {
        auto type_param = sub.types.find(name);
        if (type_param != sub.types.end()) {
            out = type_param->second;
            return true;
        }
        auto constant_param = sub.constant_types.find(name);
        if (constant_param != sub.constant_types.end()) {
            out = constant_param->second;
            return true;
        }
        return false;
    }

    bool GenericExpander::base_type_of(const std::string& name, const Substitution& sub,
        AST::Type& out) const {
        if (param_type_of(name, sub, out)) return true;
        if (name == "int") { out = AST::Type::make_int(); return true; }
        if (name == "lint") { out = AST::Type::make_lint(); return true; }
        if (name == "uint") { out = AST::Type::make_uint(); return true; }
        if (name == "luint") { out = AST::Type::make_luint(); return true; }
        if (name == "float") { out = AST::Type::make_float(); return true; }
        if (name == "double") { out = AST::Type::make_double(); return true; }
        if (name == "char") { out = AST::Type::make_char(); return true; }
        if (name == "uchar") { out = AST::Type::make_uchar(); return true; }
        if (name == "bool") { out = AST::Type::make_bool(); return true; }
        if (name == "string") { out = AST::Type::make_string(); return true; }
        if (name == "file") { out = AST::Type::make_file(); return true; }
        if (name == "void") { out = AST::Type::make_void(); return true; }
        if (struct_defs_.count(name) != 0 || instantiated_structs_.count(name) != 0) {
            out = AST::Type::make_struct(name);
            return true;
        }
        return false;
    }

    bool GenericExpander::property_argument_type(const std::string& text,
        const Substitution& sub, AST::Type& out) const {
        std::string base = text;
        int pointers = 0;
        bool array = false;
        for (;;) {
            if (base.size() >= 2 && base.compare(base.size() - 2, 2, "**") == 0) {
                pointers += 2;
                base.resize(base.size() - 2);
                continue;
            }
            if (!base.empty() && base.back() == '*') {
                ++pointers;
                base.pop_back();
                continue;
            }
            if (base.size() >= 2 && base.compare(base.size() - 2, 2, "[]") == 0) {
                array = true;
                base.resize(base.size() - 2);
                continue;
            }
            break;
        }
        AST::Type type;
        if (!base_type_of(base, sub, type)) return false;
        if (array) {
            type = AST::Type::make_array(std::make_shared<AST::Type>(type), std::nullopt);
        }
        for (int i = 0; i < pointers; ++i) {
            type = AST::Type::make_pointer(std::make_shared<AST::Type>(type));
        }
        out = type;
        return true;
    }

    bool GenericExpander::property_convertible(const AST::Type& from,
        const AST::Type& to) const {
        if (from == to) return true;
        auto arithmetic_rank = [](const AST::Type& t) -> int {
            switch (t.kind) {
            case TypeKind::Bool: return 0;
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
        if (arithmetic_rank(from) >= 0 && arithmetic_rank(to) >= 0) return true;
        if (to.kind == TypeKind::Bool && from.is_integer()) return true;
        if (from.kind == TypeKind::Pointer && to.kind == TypeKind::Pointer) {
            if (from.pointee_type && from.pointee_type->kind == TypeKind::Void) {
                return true;
            }
            if (to.pointee_type && to.pointee_type->kind == TypeKind::Void) {
                return true;
            }
            return from == to;
        }
        if (from.kind == TypeKind::Array && to.kind == TypeKind::Pointer) {
            if (!from.element_type || !to.pointee_type) return false;
            if (to.pointee_type->kind == TypeKind::Void) return true;
            return *from.element_type == *to.pointee_type;
        }
        return false;
    }

    bool GenericExpander::eval_size_align_property(const std::string& parameter,
        const std::string& property, const Substitution& sub, long long& out) {
        bool want_align = (property == "align");
        AST::Type type;
        if (!param_type_of(parameter, sub, type)) return false;
        std::size_t size = 0;
        std::size_t align = 0;
        if (!layout_of_composite_type(type, size, align, sub) &&
            !resolve_type_layout(type.to_string(), size, align, sub) &&
            !resolve_type_layout(type.struct_name, size, align, sub)) {
            return false;
        }
        out = static_cast<long long>(want_align ? align : size);
        return true;
    }

    bool GenericExpander::layout_of_composite_type(const AST::Type& type,
        std::size_t& size, std::size_t& align, const Substitution& sub) const {
        switch (type.kind) {
        case TypeKind::Int:
        case TypeKind::Uint:
        case TypeKind::Float:
            size = 4; align = 4; return true;
        case TypeKind::Lint:
        case TypeKind::Luint:
        case TypeKind::Double:
            size = 8; align = 8; return true;
        case TypeKind::Char:
        case TypeKind::Uchar:
        case TypeKind::Bool:
            size = 1; align = 1; return true;
        case TypeKind::String:
            size = 32; align = 8; return true;
        case TypeKind::File:
            size = 8; align = 8; return true;
        case TypeKind::Pointer:
        case TypeKind::Function:
            size = 8; align = 8; return true;
        case TypeKind::Void:
            size = 0; align = 1; return true;
        case TypeKind::Array:
            if (!type.element_type) return false;
            if (!layout_of_composite_type(*type.element_type, size, align, sub)) {
                return false;
            }
            size *= type.array_size.value_or(0);
            return true;
        case TypeKind::Struct:
            return resolve_type_layout(type.struct_name, size, align, sub);
        }
        return false;
    }

    bool GenericExpander::eval_bool_property(const Expression* receiver,
        const std::string& property, const std::vector<const Expression*>& args,
        const Substitution& sub, bool& out) {
        auto* prim = dynamic_cast<const PrimaryExpression*>(receiver);
        if (prim == nullptr || prim->kind != PrimaryExpression::Kind::Identifier) {
            return false;
        }
        const std::string& parameter = prim->identifier;
        AST::Type type;
        bool is_type_param = (sub.types.find(parameter) != sub.types.end());
        bool is_constant_param = (sub.constant_types.find(parameter) != sub.constant_types.end());
        if (!is_type_param && !is_constant_param) {
            return false;   
        }
        if (!param_type_of(parameter, sub, type)) return false;

        if (property == "is_same" || property == "is_convertible" ||
            property == "has_member") {
            if (!is_type_param) {
                report(receiver->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ property, parameter });
                return false;
            }
            if (args.empty()) {
                report(receiver->location, ErrorCode::CompileTimePropertyArgCountMismatch, std::vector<std::string>{ property, "1", "0" });
                return false;
            }
            if (property == "has_member") {
                const StructDefinition* def = nullptr;
                if (type.kind == TypeKind::Struct) {
                    auto found = struct_defs_.find(type.struct_name);
                    if (found != struct_defs_.end()) def = found->second;
                }
                for (std::size_t i = 0; i < args.size(); ++i) {
                    auto* arg_prim = dynamic_cast<const PrimaryExpression*>(args[i]);
                    if (arg_prim == nullptr ||
                        arg_prim->kind != PrimaryExpression::Kind::Literal ||
                        arg_prim->literal_token.type != TokenType::StringLiteral) {
                        report(receiver->location,
                            ErrorCode::CompileTimePropertyArgNotString, std::vector<std::string>{ property, std::to_string(i + 1) });
                        return false;
                    }
                    std::string member_name;
                    decode_string_literal(arg_prim->literal_token.lexeme, member_name);
                    if (def == nullptr) { out = false; return true; }
                    bool has = false;
                    for (const auto& member : def->members) {
                        if (member.name == member_name) { has = true; break; }
                    }
                    if (!has) { out = false; return true; }
                }
                out = true;
                return true;
            }
            for (const Expression* arg : args) {
                std::string arg_text = expression_text(arg);
                AST::Type arg_type;
                if (!property_argument_type(arg_text, sub, arg_type)) {
                    return false;
                }
                if (property == "is_same") {
                    if (!(type == arg_type)) { out = false; return true; }
                }
                else {
                    if (!property_convertible(type, arg_type)) { out = false; return true; }
                }
            }
            out = true;
            return true;
        }

        auto matches = [&](TypeKind kind) { out = (type.kind == kind); return true; };
        const bool type_property = property == "is_integer" || property == "is_float" ||
            property == "is_double" || property == "is_char" || property == "is_bool" ||
            property == "is_struct" || property == "is_string" ||
            property == "is_pointer" || property == "is_array";
        if (type_property && !is_type_param) {
            report(receiver->location, ErrorCode::CompileTimePropertyNotApplicable,
                std::vector<std::string>{ property, parameter });
            return false;
        }
        if (property == "is_integer") {
            out = type.kind == TypeKind::Int || type.kind == TypeKind::Lint ||
                type.kind == TypeKind::Uint || type.kind == TypeKind::Luint ||
                type.kind == TypeKind::Char || type.kind == TypeKind::Uchar ||
                type.kind == TypeKind::Bool;
            return true;
        }
        if (property == "is_float") return matches(TypeKind::Float);
        if (property == "is_double") return matches(TypeKind::Double);
        if (property == "is_char") return matches(TypeKind::Char);
        if (property == "is_bool") return matches(TypeKind::Bool);
        if (property == "is_struct") return matches(TypeKind::Struct);
        if (property == "is_string") return matches(TypeKind::String);
        if (property == "is_pointer") return matches(TypeKind::Pointer);
        if (property == "is_array") return matches(TypeKind::Array);
        return false;
    }

    bool GenericExpander::eval_compile_time_condition(const Expression* expr,
        const Substitution& sub, bool& out) {
        if (expr == nullptr) return false;
        if (auto* e = dynamic_cast<const UnaryExpression*>(expr)) {
            if (e->op == UnaryExpression::Operator::LogicalNot) {
                bool inner = false;
                if (!eval_compile_time_condition(e->operand.get(), sub, inner)) return false;
                out = !inner;
                return true;
            }
            return false;
        }
        if (auto* e = dynamic_cast<const LogicalAndExpression*>(expr)) {
            bool left = false;
            bool right = false;
            if (!eval_compile_time_condition(e->left.get(), sub, left)) return false;
            if (!eval_compile_time_condition(e->right.get(), sub, right)) return false;
            out = left && right;
            return true;
        }
        if (auto* e = dynamic_cast<const LogicalOrExpression*>(expr)) {
            bool left = false;
            bool right = false;
            if (!eval_compile_time_condition(e->left.get(), sub, left)) return false;
            if (!eval_compile_time_condition(e->right.get(), sub, right)) return false;
            out = left || right;
            return true;
        }

        auto property_side = [&](const Expression* side, bool& value,
            bool& is_property) -> bool {
            is_property = false;
            if (auto* e = dynamic_cast<const PostfixExpression*>(side)) {
                if (e->op == PostfixExpression::Operator::Dot) {
                    is_property = true;
                    return eval_bool_property(e->base.get(), e->member_name, {}, sub, value);
                }
            }
            if (auto* e = dynamic_cast<const CompileTimePropertyExpression*>(side)) {
                is_property = true;
                std::vector<const Expression*> args;
                for (const auto& arg : e->arguments) args.push_back(arg.get());
                return eval_bool_property(e->receiver.get(), e->property, args, sub, value);
            }
            return false;
        };
        auto literal_side = [&](const Expression* side, bool& value) -> bool {
            auto* prim = dynamic_cast<const PrimaryExpression*>(side);
            if (prim == nullptr || prim->kind != PrimaryExpression::Kind::Literal) return false;
            if (prim->literal_token.type != TokenType::BoolLiteral) return false;
            value = (prim->literal_token.lexeme == "true");
            return true;
        };

        if (auto* e = dynamic_cast<const ComparisonExpression*>(expr)) {
            auto string_operand = [&](const Expression* side, std::string& value) -> bool {
                auto* prim = dynamic_cast<const PrimaryExpression*>(side);
                if (prim == nullptr) return false;
                if (prim->kind == PrimaryExpression::Kind::Literal &&
                    prim->literal_token.type == TokenType::StringLiteral) {
                    return decode_string_literal(prim->literal_token.lexeme, value);
                }
                if (prim->kind == PrimaryExpression::Kind::Identifier) {
                    auto found = sub.constants.find(prim->identifier);
                    if (found != sub.constants.end() && found->second.is_string) {
                        value = found->second.string_value;
                        return true;
                    }
                }
                return false;
            };
            std::string left_text;
            std::string right_text;
            auto typename_operand = [&](const Expression* side, std::string& value) -> bool {
                const Expression* receiver = nullptr;
                std::string property;
                if (auto* post = dynamic_cast<const PostfixExpression*>(side)) {
                    if (post->op == PostfixExpression::Operator::Dot) {
                        receiver = post->base.get();
                        property = post->member_name;
                    }
                }
                else if (auto* prop = dynamic_cast<const CompileTimePropertyExpression*>(side)) {
                    receiver = prop->receiver.get();
                    property = prop->property;
                }
                if (receiver == nullptr || property != "typename") return false;
                auto* prim = dynamic_cast<const PrimaryExpression*>(receiver);
                if (prim == nullptr || prim->kind != PrimaryExpression::Kind::Identifier) {
                    return false;
                }
                if (sub.types.find(prim->identifier) == sub.types.end()) return false;
                value = sub.types.at(prim->identifier).to_string();
                return true;
            };
            if (typename_operand(e->left.get(), left_text)) {
                if (!string_operand(e->right.get(), right_text)) return false;
                switch (e->op) {
                case ComparisonExpression::Operator::Equal:
                    out = (left_text == right_text);
                    return true;
                case ComparisonExpression::Operator::NotEqual:
                    out = (left_text != right_text);
                    return true;
                default:
                    return false;
                }
            }
            if (typename_operand(e->right.get(), right_text)) {
                if (!string_operand(e->left.get(), left_text)) return false;
                switch (e->op) {
                case ComparisonExpression::Operator::Equal:
                    out = (left_text == right_text);
                    return true;
                case ComparisonExpression::Operator::NotEqual:
                    out = (left_text != right_text);
                    return true;
                default:
                    return false;
                }
            }
            if (string_operand(e->left.get(), left_text) &&
                string_operand(e->right.get(), right_text)) {
                switch (e->op) {
                case ComparisonExpression::Operator::Equal:
                    out = (left_text == right_text);
                    return true;
                case ComparisonExpression::Operator::NotEqual:
                    out = (left_text != right_text);
                    return true;
                default:
                    return false;
                }
            }
            bool left_prop = false;
            bool left_value = false;
            bool left_is_property = false;
            bool right_value = false;
            bool right_is_property = false;
            bool left_ok = property_side(e->left.get(), left_value, left_is_property);
            bool right_ok = property_side(e->right.get(), right_value, right_is_property);
            if (!left_ok) {
                left_ok = literal_side(e->left.get(), left_value);
                left_is_property = false;
            }
            if (!right_ok) {
                right_ok = literal_side(e->right.get(), right_value);
                right_is_property = false;
            }
            if (!left_ok || !right_ok) return false;
            (void)left_prop;
            (void)left_is_property;
            (void)right_is_property;
            switch (e->op) {
            case ComparisonExpression::Operator::Equal:
                out = (left_value == right_value);
                return true;
            case ComparisonExpression::Operator::NotEqual:
                out = (left_value != right_value);
                return true;
            default:
                return false;
            }
        }
        bool value = false;
        bool is_property = false;
        if (property_side(expr, value, is_property) && is_property) {
            out = value;
            return true;
        }
        if (literal_side(expr, value)) {
            out = value;
            return true;
        }
        return false;
    }

    bool GenericExpander::eval_emit_piece(const Expression* expr, const Substitution& sub,
        std::string& out) {
        if (expr == nullptr) return false;
        if (auto* e = dynamic_cast<const AdditiveExpression*>(expr)) {
            if (e->op != AdditiveExpression::Operator::Plus) return false;
            std::string left;
            std::string right;
            bool left_ok = eval_emit_piece(e->left.get(), sub, left);
            bool right_ok = eval_emit_piece(e->right.get(), sub, right);
            if (left_ok && right_ok) {
                out += left;
                out += right;
                return true;
            }
            ConstantValue value;
            if (evaluate_with_substitution(expr, sub, value)) {
                if (value.is_float) {
                    char buffer[64];
                    std::snprintf(buffer, sizeof(buffer), "%g", value.float_value);
                    out += buffer;
                }
                else {
                    out += std::to_string(value.int_value);
                }
                return true;
            }
            return false;
        }
        if (auto* e = dynamic_cast<const PrimaryExpression*>(expr)) {
            if (e->kind == PrimaryExpression::Kind::Literal) {
                switch (e->literal_token.type) {
                case TokenType::StringLiteral: {
                    std::string decoded;
                    decode_string_literal(e->literal_token.lexeme, decoded);
                    out += decoded;
                    return true;
                }
                case TokenType::IntegerLiteral:
                case TokenType::FloatLiteral:
                case TokenType::CharLiteral:
                case TokenType::BoolLiteral:
                    out += std::string(e->literal_token.lexeme);
                    return true;
                default:
                    return false;
                }
            }
            if (e->kind == PrimaryExpression::Kind::Identifier) {
                auto constant = sub.constants.find(e->identifier);
                if (constant != sub.constants.end()) {
                    if (constant->second.is_string) {
                        out += constant->second.string_value;
                    }
                    else if (constant->second.is_float) {
                        char buffer[64];
                        std::snprintf(buffer, sizeof(buffer), "%g",
                            constant->second.float_value);
                        out += buffer;
                    }
                    else {
                        out += std::to_string(constant->second.int_value);
                    }
                    return true;
                }
                return false;
            }
            return false;
        }
        if (auto* e = dynamic_cast<const PostfixExpression*>(expr)) {
            if (e->op != PostfixExpression::Operator::Dot) return false;
            auto* prim = dynamic_cast<const PrimaryExpression*>(e->base.get());
            if (prim == nullptr || prim->kind != PrimaryExpression::Kind::Identifier) {
                return false;
            }
            const std::string& parameter = prim->identifier;
            bool is_type_param = (sub.types.find(parameter) != sub.types.end());
            bool is_constant_param =
                (sub.constant_types.find(parameter) != sub.constant_types.end());
            if (!is_type_param && !is_constant_param) return false;
            if (e->member_name == "typename") {
                if (!is_type_param) {
                    report(e->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ e->member_name, parameter });
                    return false;
                }
                out += sub.types.at(parameter).to_string();
                return true;
            }
            if (e->member_name == "size" || e->member_name == "align") {
                long long value = 0;
                if (!eval_size_align_property(parameter, e->member_name, sub, value)) {
                    report(e->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ e->member_name, parameter });
                    return false;
                }
                out += std::to_string(value);
                return true;
            }
            return false;
        }
        return false;
    }

    bool GenericExpander::flush_emit_string(const std::string& text, SourceLocation loc,
        std::vector<AST::TopLevel*>& member_ptrs,
        std::vector<std::unique_ptr<AST::TopLevel>>& owner) {
        if (text.empty()) return true;
        emit_source_pool_.push_back(text);
        std::string& buffer = emit_source_pool_.back();
        std::string name = "<emit>";
        std::string_view source_view(buffer);
        std::string_view name_view(name);
        DiagnosticEngine emit_diag;
        Lexer lexer(source_view, name_view, emit_diag);
        Parser parser(lexer, emit_diag);
        auto program = parser.parse();
        if (program == nullptr || emit_diag.has_errors()) {
            report(loc, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
            return false;
        }
        for (auto& top : program->top_levels) {
            if (dynamic_cast<GenericDefinition*>(top.get()) != nullptr) {
                report(loc, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
                return false;
            }
            member_ptrs.push_back(top.get());
            owner.push_back(std::move(top));
        }
        return true;
    }

    bool GenericExpander::process_compile_time_items(
        const std::vector<std::unique_ptr<AST::Statement>>& items,
        const Substitution& sub, std::string& text, SourceLocation& text_loc,
        std::vector<AST::TopLevel*>& member_ptrs,
        std::vector<std::unique_ptr<AST::TopLevel>>& owner) {
        for (const auto& item : items) {
            if (auto* emit = dynamic_cast<const EmitStatement*>(item.get())) {
                if (emit->is_block) {
                    if (!flush_emit_string(text, text_loc, member_ptrs, owner)) return false;
                    text.clear();
                    for (const auto& node : emit->block_items) {
                        if (auto* fd = dynamic_cast<FunctionDefinition*>(node.get())) {
                            member_ptrs.push_back(fd);
                        }
                        else if (auto* sd = dynamic_cast<StructDefinition*>(node.get())) {
                            member_ptrs.push_back(sd);
                        }
                        else {
                            report(node->location, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
                            return false;
                        }
                    }
                }
                else {
                    if (text.empty()) text_loc = emit->location;
                    for (const auto& piece : emit->pieces) {
                        std::string piece_text;
                        if (!eval_emit_piece(piece.get(), sub, piece_text)) {
                            report(piece->location, ErrorCode::EmitStringNotConstant, std::vector<std::string>{ expression_text(piece.get()) });
                            return false;
                        }
                        text += piece_text;
                    }
                }
                continue;
            }
            if (auto* ifs = dynamic_cast<const IfStatement*>(item.get())) {
                bool condition = false;
                if (!eval_compile_time_condition(ifs->condition.get(), sub, condition)) {
                    report(ifs->location, ErrorCode::CompileTimeConditionNotBoolean, std::vector<std::string>{ expression_text(ifs->condition.get()) });
                    return false;
                }
                const Statement* branch = condition ? ifs->then_block.get()
                                                    : ifs->else_block.get();
                if (branch == nullptr) continue;
                if (auto* block = dynamic_cast<const Block*>(branch)) {
                    if (!process_compile_time_items(block->statements, sub, text, text_loc,
                        member_ptrs, owner)) {
                        return false;
                    }
                }
                else {
                    report(branch->location, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
                    return false;
                }
                continue;
            }
            report(item->location, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
            return false;
        }
        return true;
    }

    void GenericExpander::expand_expression(std::unique_ptr<Expression>& expr_holder) {
        Expression* expr = expr_holder.get();
        if (expr == nullptr) return;
        if (auto* e = dynamic_cast<AssignmentExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<LogicalOrExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<LogicalAndExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<ComparisonExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<AdditiveExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<MultiplicativeExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<PowerExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<UnaryExpression*>(expr)) {
            expand_expression(e->operand);
        }
        else if (auto* e = dynamic_cast<PostfixExpression*>(expr)) {
            expand_expression(e->base);
            expand_expression(e->subscript_expr);
            for (auto& arg : e->arguments) expand_expression(arg);
            resolve_type(e->cast_type);
        }
        else if (auto* e = dynamic_cast<PrimaryExpression*>(expr)) {
            switch (e->kind) {
            case PrimaryExpression::Kind::Parens:
                expand_expression(e->paren_expr);
                break;
            case PrimaryExpression::Kind::Heap:
                resolve_type(e->heap_type);
                expand_expression(e->heap_size);
                break;
            case PrimaryExpression::Kind::QualifiedName: {
                auto mangled = ensure_instantiation(*e->generic_ref, false);
                std::string name = mangled.has_value() ? *mangled : e->generic_ref->to_string();
                expr_holder = std::make_unique<PrimaryExpression>(e->location, name);
                break;
            }
            case PrimaryExpression::Kind::Construct:
            case PrimaryExpression::Kind::PlacementConstruct: {
                resolve_type(e->construct_type);
                for (auto& arg : e->construct_args) expand_expression(arg);
                expand_expression(e->placement_target);
                break;
            }
            case PrimaryExpression::Kind::CopyMove:
                expand_expression(e->paren_expr);
                break;
            case PrimaryExpression::Kind::Identifier: {
                std::string name = e->identifier;
                bool resolved_short_name = false;
                for (std::size_t depth = short_scopes_.size(); depth > 0; --depth) {
                    const auto& shorts = short_scopes_[depth - 1];
                    auto found = shorts.find(name);
                    if (found != shorts.end()) {
                        expr_holder = std::make_unique<PrimaryExpression>(e->location,
                            found->second.target);
                        resolved_short_name = true;
                        break;
                    }
                    const auto& declared = declared_scopes_[depth - 1];
                    if (declared.count(name) != 0) {
                        break;   
                    }
                }
                if (!resolved_short_name) {
                    for (std::size_t depth = instance_scopes_.size(); depth > 0; --depth) {
                        for (const std::string& instance : instance_scopes_[depth - 1]) {
                            auto missing = missing_members_.find(instance);
                            if (missing != missing_members_.end() &&
                                missing->second.count(name) != 0) {
                                report(e->location, ErrorCode::GenericSpecializationMissingMember, std::vector<std::string>{ instance, name });
                                return;
                            }
                        }
                    }
                }
                break;
            }
            default:
                break;
            }
        }
    }

    void GenericExpander::expand_initializer(Initializer* init) {
        if (init == nullptr) return;
        if (auto* e = dynamic_cast<ExpressionInitializer*>(init)) {
            expand_expression(e->expr);
        }
        else if (auto* a = dynamic_cast<ArrayInitializer*>(init)) {
            for (auto& element : a->elements) expand_initializer(element.get());
        }
    }

    void GenericExpander::expand_statement(Statement* stmt) {
        if (stmt == nullptr) return;
        if (auto* block = dynamic_cast<Block*>(stmt)) {
            push_scope();
            for (auto& child : block->statements) {
                if (auto* vd = dynamic_cast<VariableDeclaration*>(child.get())) declare_name(vd->name);
                else if (auto* sd = dynamic_cast<StructDefinition*>(child.get())) declare_name(sd->name);
                else if (auto* def = dynamic_cast<FunctionDefinition*>(child.get())) declare_name(def->name);
            }
            for (auto& child : block->statements) expand_statement(child.get());
            std::vector<std::unique_ptr<Statement>> kept;
            kept.reserve(block->statements.size());
            for (auto& child : block->statements) {
                if (dynamic_cast<InstantiationStatement*>(child.get()) != nullptr) continue;
                kept.push_back(std::move(child));
            }
            block->statements = std::move(kept);
            pop_scope();
            return;
        }
        if (auto* vd = dynamic_cast<VariableDeclaration*>(stmt)) {
            resolve_type(vd->type);
            expand_initializer(vd->initializer.get());
            return;
        }
        if (auto* sd = dynamic_cast<StructDefinition*>(stmt)) {
            struct_defs_[sd->name] = sd;
            for (auto& member : sd->members) {
                resolve_type(member.type);
                if (member.array_size_expr) {
                    expand_expression(member.array_size_expr);
                }
                if (member.function_pointer_type.has_value()) {
                    resolve_type(*member.function_pointer_type);
                }
                expand_initializer(member.initializer.get());
            }
            for (auto& smf : sd->special_members) {
                for (Type& p : smf->parameters) resolve_type(p);
                resolve_type(smf->parameter_type);
                expand_statement(smf->body.get());
            }
            return;
        }
        if (auto* is = dynamic_cast<IfStatement*>(stmt)) {
            expand_expression(is->condition);
            expand_statement(is->then_block.get());
            expand_statement(is->else_block.get());
            return;
        }
        if (auto* fs = dynamic_cast<ForStatement*>(stmt)) {
            push_scope();
            if (fs->init) {
                if (auto* vd = dynamic_cast<VariableDeclaration*>(fs->init.get())) declare_name(vd->name);
                expand_statement(fs->init.get());
            }
            expand_expression(fs->condition);
            expand_expression(fs->step);
            expand_statement(fs->body.get());
            pop_scope();
            return;
        }
        if (auto* ws = dynamic_cast<WhileStatement*>(stmt)) {
            expand_expression(ws->condition);
            expand_statement(ws->body.get());
            return;
        }
        if (auto* rs = dynamic_cast<ReturnStatement*>(stmt)) {
            expand_expression(rs->value);
            return;
        }
        if (auto* es = dynamic_cast<ExpressionStatement*>(stmt)) {
            expand_expression(es->expr);
            return;
        }
        if (auto* ds = dynamic_cast<DestructStatement*>(stmt)) {
            expand_expression(ds->target);
            return;
        }
        if (auto* inst = dynamic_cast<InstantiationStatement*>(stmt)) {
            ensure_instantiation(inst->reference, inst->reference.member.empty());
            return;
        }
    }

    void GenericExpander::expand_top_level(TopLevel* node) {
        if (dynamic_cast<GenericDefinition*>(node) != nullptr) {
            return;   
        }
        if (auto* inst = dynamic_cast<InstantiationStatement*>(node)) {
            ensure_instantiation(inst->reference, inst->reference.member.empty());
            return;
        }
        if (auto* sd = dynamic_cast<StructDefinition*>(node)) {
            struct_defs_[sd->name] = sd;
            for (auto& member : sd->members) {
                resolve_type(member.type);
                if (member.array_size_expr) expand_expression(member.array_size_expr);
                if (member.function_pointer_type.has_value()) {
                    resolve_type(*member.function_pointer_type);
                }
                expand_initializer(member.initializer.get());
            }
            for (auto& smf : sd->special_members) {
                for (Type& p : smf->parameters) resolve_type(p);
                resolve_type(smf->parameter_type);
                for (auto& d : smf->parameter_defaults) {
                    expand_expression(d);
                }
                push_scope();
                if (!smf->parameter_name.empty()) declare_name(smf->parameter_name);
                for (const std::string& pname : smf->parameter_names) declare_name(pname);
                expand_statement(smf->body.get());
                pop_scope();
            }
            return;
        }
        if (auto* fd = dynamic_cast<FunctionDefinition*>(node)) {
            func_defs_[fd->name] = fd;
            resolve_type(fd->return_type);
            for (Type& p : fd->parameters) resolve_type(p);
            for (auto& d : fd->param_defaults) {
                expand_expression(d);
            }
            push_scope();
            for (const std::string& pname : fd->param_names) {
                if (!pname.empty()) declare_name(pname);
            }
            expand_statement(fd->body.get());
            pop_scope();
            return;
        }
        if (auto* vd = dynamic_cast<VariableDeclaration*>(node)) {
            resolve_type(vd->type);
            if (vd->array_size_expr) expand_expression(vd->array_size_expr);
            expand_initializer(vd->initializer.get());
            return;
        }
        if (auto* ed = dynamic_cast<ExternDeclaration*>(node)) {
            resolve_type(ed->return_type);
            for (Type& p : ed->parameters) resolve_type(p);
            return;
        }
    }

    bool GenericExpander::expand(AST::Program* program) {
        if (program == nullptr) return false;
        program_ = program;
        had_error_ = false;
        push_scope();               
        collect_declarations();
        collect_generics();
        std::vector<std::unique_ptr<TopLevel>> output;
        output.reserve(program_->top_levels.size());
        output_ = &output;
        for (auto& top : program_->top_levels) {
            if (dynamic_cast<GenericDefinition*>(top.get()) != nullptr) {
                continue;
            }
            if (auto* inst = dynamic_cast<InstantiationStatement*>(top.get())) {
                ensure_instantiation(inst->reference, inst->reference.member.empty());
                continue;
            }
            expand_top_level(top.get());
            output.push_back(std::move(top));
        }
        output_ = nullptr;
        pop_scope();
        program_->top_levels = std::move(output);
        return !had_error_;
    }

} 
