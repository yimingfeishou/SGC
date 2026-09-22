#include "lifecycle.hpp"
#include "lifecycle_detail.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <functional>

using namespace gallt::AST;

namespace gallt {
    using namespace lifecycle_detail;

    LifecycleLowering::LifecycleLowering(DiagnosticEngine& diag) : diag_(diag) {}

    void LifecycleLowering::report_template(SourceLocation loc, ErrorCode code,
        const std::vector<std::string>& values) {
        diag_.report_error_template(loc, code, values);
        had_error_ = true;
    }

    void LifecycleLowering::report(SourceLocation loc, ErrorCode code,
        const std::string& message) {
        diag_.report_error(loc, code, message);
        had_error_ = true;
    }

    void LifecycleLowering::collect_structs() {
        std::function<void(Statement*)> walk = [&](Statement* stmt) {
            if (stmt == nullptr) return;
            if (auto* def = dynamic_cast<StructDefinition*>(stmt)) {
                register_struct_definition(def);
            }
            if (auto* block = dynamic_cast<Block*>(stmt)) {
                for (auto& inner : block->statements) walk(inner.get());
            }
            else if (auto* ifs = dynamic_cast<IfStatement*>(stmt)) {
                walk(ifs->then_block.get());
                if (ifs->else_block) walk(ifs->else_block.get());
            }
            else if (auto* for_ = dynamic_cast<ForStatement*>(stmt)) {
                if (for_->init) walk(for_->init.get());
                if (for_->body) walk(for_->body.get());
            }
            else if (auto* while_ = dynamic_cast<WhileStatement*>(stmt)) {
                if (while_->body) walk(while_->body.get());
            }
        };
        for (auto& top : program_->top_levels) {
            if (auto* def = dynamic_cast<StructDefinition*>(top.get())) {
                register_struct_definition(def);
            }
            else if (auto* func = dynamic_cast<FunctionDefinition*>(top.get())) {
                if (func->body != nullptr) walk(func->body.get());
            }
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& pair : structs_) {
                if (has_destructor(pair.first)) continue;
                for (const auto& member : pair.second.def->members) {
                    if (type_needs_destruction(member.type)) {
                        mark_needs_destruction(pair.first);
                        changed = true;
                        break;
                    }
                }
            }
        }
    }

    void LifecycleLowering::register_struct_definition(StructDefinition* def) {
        if (def == nullptr) return;
        if (structs_.count(def->name) != 0) return;
        struct_defs_[def->name] = def;
            StructInfo& info = structs_[def->name];
            info.def = def;
            info.no_copy = def->no_copy;
            info.no_move = def->no_move;
            if (!def->special_members.empty()) {
                for (auto& member : def->special_members) {
                    if (member->kind == SpecialMemberFunction::Kind::Destructor) {
                        def->needs_destruction = true;
                    }
                }
            }
                for (auto& member : def->special_members) {
                    switch (member->kind) {
                    case SpecialMemberFunction::Kind::Constructor:
                        info.constructors.push_back(member.get());
                        break;
                    case SpecialMemberFunction::Kind::Destructor:
                        if (info.destructor != nullptr) {
                            report_template(member->location, ErrorCode::SpecialMemberRedefined,
                                { "destructor" });
                        }
                        else info.destructor = member.get();
                        break;
                    case SpecialMemberFunction::Kind::CopyConstructor:
                        if (info.copy_constructor != nullptr) {
                            report_template(member->location, ErrorCode::SpecialMemberRedefined,
                                { "copy_constructor" });
                        }
                        else info.copy_constructor = member.get();
                        break;
                    case SpecialMemberFunction::Kind::MoveConstructor:
                        if (info.move_constructor != nullptr) {
                            report_template(member->location, ErrorCode::SpecialMemberRedefined,
                                { "move_constructor" });
                        }
                        else info.move_constructor = member.get();
                        break;
                    case SpecialMemberFunction::Kind::CopyAssignment:
                        if (info.copy_assignment != nullptr) {
                            report_template(member->location, ErrorCode::SpecialMemberRedefined,
                                { "copy_assignment" });
                        }
                        else info.copy_assignment = member.get();
                        break;
                    case SpecialMemberFunction::Kind::MoveAssignment:
                        if (info.move_assignment != nullptr) {
                            report_template(member->location, ErrorCode::SpecialMemberRedefined,
                                { "move_assignment" });
                        }
                        else info.move_assignment = member.get();
                        break;
                    }
                }
                std::string ctor_overload = "__sgc_ctor$" + def->name;
                for (std::size_t i = 0; i < info.constructors.size(); ++i) {
                    info.ctor_names.push_back(ctor_overload);
                }
                info.dtor_name = "__sgc_dtor$" + def->name;
                info.copy_ctor_name = "__sgc_copyctor$" + def->name;
                info.move_ctor_name = "__sgc_movector$" + def->name;
                info.copy_assign_name = "__sgc_copyassign$" + def->name;
                info.move_assign_name = "__sgc_moveassign$" + def->name;
    }

    void LifecycleLowering::mark_needs_destruction(const std::string& name) {
        auto it = structs_.find(name);
        if (it == structs_.end()) return;
        it->second.def->needs_destruction = true;
    }

    bool LifecycleLowering::has_destructor(const std::string& struct_name) const {
        auto it = structs_.find(struct_name);
        if (it == structs_.end()) return false;
        return it->second.def->needs_destruction;
    }

    bool LifecycleLowering::type_needs_destruction(const AST::Type& type) const {
        switch (type.kind) {
        case TypeKind::String:
            return false;
        case TypeKind::Struct:
            return has_destructor(type.struct_name);
        case TypeKind::Array:
            return type.element_type && type_needs_destruction(*type.element_type);
        case TypeKind::Pointer:
            return false;
        default:
            return false;
        }
    }

    bool LifecycleLowering::is_copyable(const AST::Type& type) const {
        switch (type.kind) {
        case TypeKind::Array:
            return type.element_type ? is_copyable(*type.element_type) : true;
        case TypeKind::Struct: {
            auto it = structs_.find(type.struct_name);
            if (it == structs_.end()) return true;
            if (it->second.no_copy) return false;
            for (const auto& member : it->second.def->members) {
                if (!is_copyable(member.type)) return false;
            }
            return true;
        }
        default:
            return true;
        }
    }

    bool LifecycleLowering::is_movable(const AST::Type& type) const {
        switch (type.kind) {
        case TypeKind::Array:
            return type.element_type ? is_movable(*type.element_type) : true;
        case TypeKind::Struct: {
            auto it = structs_.find(type.struct_name);
            if (it == structs_.end()) return true;
            if (it->second.no_move) return false;
            for (const auto& member : it->second.def->members) {
                if (!is_movable(member.type)) return false;
            }
            return true;
        }
        default:
            return true;
        }
    }

    void LifecycleLowering::validate_special_members() {
        for (auto& pair : structs_) {
            StructInfo& info = pair.second;
            const std::string& name = pair.first;
            auto check_pointer_param = [&](SpecialMemberFunction* member, const char* what) {
                if (member == nullptr) return;
                const AST::Type& param = member->parameter_type;
                bool ok = param.kind == TypeKind::Pointer && param.pointee_type &&
                    param.pointee_type->kind == TypeKind::Struct &&
                    param.pointee_type->struct_name == name;
                if (!ok) {
                    report_template(member->location, ErrorCode::SpecialMemberParamMismatch,
                        { what, name + "*", param.to_string() });
                }
            };
            check_pointer_param(info.copy_constructor, "copy_constructor");
            check_pointer_param(info.move_constructor, "move_constructor");
            check_pointer_param(info.copy_assignment, "copy_assignment");
            check_pointer_param(info.move_assignment, "move_assignment");

            for (std::size_t i = 0; i < info.constructors.size(); ++i) {
                for (std::size_t j = i + 1; j < info.constructors.size(); ++j) {
                    const auto& a = info.constructors[i]->parameters;
                    const auto& b = info.constructors[j]->parameters;
                    if (a.size() != b.size()) continue;
                    bool same = true;
                    for (std::size_t k = 0; k < a.size(); ++k) {
                        if (!(a[k] == b[k])) { same = false; break; }
                    }
                    if (same) {
                        report_template(info.constructors[j]->location,
                            ErrorCode::SpecialMemberRedefined, { "constructor" });
                    }
                }
            }

            if (info.no_copy) {
                if (info.copy_constructor != nullptr || info.copy_assignment != nullptr) {
                    report(info.def->location, ErrorCode::NoCopyViolation, { name });
                }
            }
            if (info.no_move) {
                if (info.move_constructor != nullptr || info.move_assignment != nullptr) {
                    report(info.def->location, ErrorCode::NoMoveViolation, { name });
                }
            }

            std::function<void(const Statement*)> scan_return = [&](const Statement* stmt) {
                if (stmt == nullptr) return;
                if (auto* ret = dynamic_cast<const ReturnStatement*>(stmt)) {
                    if (ret->value != nullptr) {
                    report_template(ret->location, ErrorCode::SpecialMemberReturnValue,
                        { "special_member" });
                    }
                    return;
                }
                if (auto* block = dynamic_cast<const Block*>(stmt)) {
                    for (const auto& child : block->statements) scan_return(child.get());
                }
                else if (auto* ifs = dynamic_cast<const IfStatement*>(stmt)) {
                    scan_return(ifs->then_block.get());
                    scan_return(ifs->else_block.get());
                }
                else if (auto* fors = dynamic_cast<const ForStatement*>(stmt)) {
                    scan_return(fors->body.get());
                }
                else if (auto* whiles = dynamic_cast<const WhileStatement*>(stmt)) {
                    scan_return(whiles->body.get());
                }
            };
            for (auto* ctor : info.constructors) scan_return(ctor->body.get());
            if (info.destructor) scan_return(info.destructor->body.get());
            if (info.copy_constructor) scan_return(info.copy_constructor->body.get());
            if (info.move_constructor) scan_return(info.move_constructor->body.get());
            if (info.copy_assignment) scan_return(info.copy_assignment->body.get());
            if (info.move_assignment) scan_return(info.move_assignment->body.get());

            check_copy_constructor_source(info);
        }
    }

    void LifecycleLowering::check_copy_constructor_source(const StructInfo& info) {
        const SpecialMemberFunction* copy_constructor = info.copy_constructor;
        if (copy_constructor == nullptr || copy_constructor->body == nullptr) return;
        const std::string source = copy_constructor->parameter_name;
        if (source.empty()) return;

        std::unordered_set<std::string> shadowed;

        std::function<bool(const Expression*)> targets_source =
            [&](const Expression* expr) -> bool {
            if (expr == nullptr) return false;
            if (auto* prim = dynamic_cast<const PrimaryExpression*>(expr)) {
                if (prim->kind == PrimaryExpression::Kind::Parens) {
                    return targets_source(prim->paren_expr.get());
                }
                return prim->kind == PrimaryExpression::Kind::Identifier &&
                    prim->identifier == source && shadowed.count(source) == 0;
            }
            if (auto* post = dynamic_cast<const PostfixExpression*>(expr)) {
                switch (post->op) {
                case PostfixExpression::Operator::Dot:
                case PostfixExpression::Operator::Arrow:
                case PostfixExpression::Operator::Subscript:
                case PostfixExpression::Operator::Increment:
                case PostfixExpression::Operator::Decrement:
                    return targets_source(post->base.get());
                default:
                    return false;
                }
            }
            if (auto* un = dynamic_cast<const UnaryExpression*>(expr)) {
                if (un->op == UnaryExpression::Operator::Dereference) {
                    return targets_source(un->operand.get());
                }
                return false;
            }
            return false;
        };

        auto report_source_write = [&](SourceLocation loc) {
            report(loc, ErrorCode::ConstModification,
                "copy constructor must not modify its source object '" + source + "'");
        };

        std::function<void(const Expression*)> scan_expression =
            [&](const Expression* expr) {
            if (expr == nullptr) return;
            if (auto* assign = dynamic_cast<const AssignmentExpression*>(expr)) {
                if (targets_source(assign->left.get())) report_source_write(assign->location);
            }
            if (auto* prim = dynamic_cast<const PrimaryExpression*>(expr)) {
                scan_expression(prim->paren_expr.get());
                scan_expression(prim->heap_size.get());
                scan_expression(prim->placement_target.get());
                for (const auto& arg : prim->construct_args) scan_expression(arg.get());
                return;
            }
            if (auto* post = dynamic_cast<const PostfixExpression*>(expr)) {
                if ((post->op == PostfixExpression::Operator::Increment ||
                    post->op == PostfixExpression::Operator::Decrement) &&
                    targets_source(post->base.get())) {
                    report_source_write(post->location);
                }
                scan_expression(post->base.get());
                scan_expression(post->subscript_expr.get());
                for (const auto& arg : post->arguments) scan_expression(arg.get());
                return;
            }
            if (auto* un = dynamic_cast<const UnaryExpression*>(expr)) {
                if ((un->op == UnaryExpression::Operator::Increment ||
                    un->op == UnaryExpression::Operator::Decrement) &&
                    targets_source(un->operand.get())) {
                    report_source_write(un->location);
                }
                scan_expression(un->operand.get());
                return;
            }
            if (auto* e = dynamic_cast<const LogicalOrExpression*>(expr)) {
                scan_expression(e->left.get());
                scan_expression(e->right.get());
                return;
            }
            if (auto* e = dynamic_cast<const LogicalAndExpression*>(expr)) {
                scan_expression(e->left.get());
                scan_expression(e->right.get());
                return;
            }
            if (auto* e = dynamic_cast<const ComparisonExpression*>(expr)) {
                scan_expression(e->left.get());
                scan_expression(e->right.get());
                return;
            }
            if (auto* e = dynamic_cast<const AdditiveExpression*>(expr)) {
                scan_expression(e->left.get());
                scan_expression(e->right.get());
                return;
            }
            if (auto* e = dynamic_cast<const MultiplicativeExpression*>(expr)) {
                scan_expression(e->left.get());
                scan_expression(e->right.get());
                return;
            }
            if (auto* e = dynamic_cast<const PowerExpression*>(expr)) {
                scan_expression(e->left.get());
                scan_expression(e->right.get());
                return;
            }
            if (auto* e = dynamic_cast<const BitwiseExpression*>(expr)) {
                scan_expression(e->left.get());
                scan_expression(e->right.get());
                return;
            }
            if (auto* e = dynamic_cast<const ShiftExpression*>(expr)) {
                scan_expression(e->left.get());
                scan_expression(e->right.get());
                return;
            }
            if (auto* e = dynamic_cast<const ConditionalExpression*>(expr)) {
                scan_expression(e->condition.get());
                scan_expression(e->then_expr.get());
                scan_expression(e->else_expr.get());
                return;
            }
            if (auto* e = dynamic_cast<const CompileTimePropertyExpression*>(expr)) {
                scan_expression(e->receiver.get());
                for (const auto& arg : e->arguments) scan_expression(arg.get());
                return;
            }
        };

        std::function<void(const Initializer*)> scan_initializer =
            [&](const Initializer* init) {
            if (init == nullptr) return;
            if (auto* expr_init = dynamic_cast<const ExpressionInitializer*>(init)) {
                scan_expression(expr_init->expr.get());
                return;
            }
            if (auto* arr_init = dynamic_cast<const ArrayInitializer*>(init)) {
                for (const auto& element : arr_init->elements) {
                    scan_initializer(element.get());
                }
            }
        };

        std::function<void(const Statement*)> scan_statement =
            [&](const Statement* stmt) {
            if (stmt == nullptr) return;
            if (auto* block = dynamic_cast<const Block*>(stmt)) {
                std::vector<std::string> introduced;
                for (const auto& child : block->statements) {
                    if (auto* decl = dynamic_cast<const VariableDeclaration*>(child.get())) {
                        if (shadowed.insert(decl->name).second) {
                            introduced.push_back(decl->name);
                        }
                    }
                    scan_statement(child.get());
                }
                for (const std::string& name : introduced) shadowed.erase(name);
                return;
            }
            if (auto* decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
                scan_initializer(decl->initializer.get());
                scan_expression(decl->array_size_expr.get());
                return;
            }
            if (auto* expr_stmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
                scan_expression(expr_stmt->expr.get());
                return;
            }
            if (auto* if_stmt = dynamic_cast<const IfStatement*>(stmt)) {
                scan_expression(if_stmt->condition.get());
                scan_statement(if_stmt->then_block.get());
                scan_statement(if_stmt->else_block.get());
                return;
            }
            if (auto* for_stmt = dynamic_cast<const ForStatement*>(stmt)) {
                std::vector<std::string> introduced;
                if (for_stmt->init != nullptr) {
                    if (auto* init_decl =
                        dynamic_cast<const VariableDeclaration*>(for_stmt->init.get())) {
                        if (shadowed.insert(init_decl->name).second) {
                            introduced.push_back(init_decl->name);
                        }
                    }
                    scan_statement(for_stmt->init.get());
                }
                scan_expression(for_stmt->condition.get());
                scan_expression(for_stmt->step.get());
                scan_statement(for_stmt->body.get());
                for (const std::string& name : introduced) shadowed.erase(name);
                return;
            }
            if (auto* while_stmt = dynamic_cast<const WhileStatement*>(stmt)) {
                scan_expression(while_stmt->condition.get());
                scan_statement(while_stmt->body.get());
                return;
            }
            if (auto* return_stmt = dynamic_cast<const ReturnStatement*>(stmt)) {
                scan_expression(return_stmt->value.get());
                return;
            }
            if (auto* destruct_stmt = dynamic_cast<const DestructStatement*>(stmt)) {
                scan_expression(destruct_stmt->target.get());
                return;
            }
            if (auto* emit_stmt = dynamic_cast<const EmitStatement*>(stmt)) {
                for (const auto& piece : emit_stmt->pieces) scan_expression(piece.get());
                return;
            }
        };

        scan_statement(copy_constructor->body.get());
    }

    void LifecycleLowering::collect_declarations() {
        static const char* kNames[] = {
            "constructor", "destructor", "copy_constructor",
            "move_constructor", "copy_assignment", "move_assignment",
        };
        for (auto& top : program_->top_levels) {
            auto* func = dynamic_cast<FunctionDefinition*>(top.get());
            if (func == nullptr) continue;
            for (const char* special : kNames) {
                if (func->name == special) {
                    report_template(func->location, ErrorCode::SpecialMemberNameConflict,
                        { func->name });
                }
            }
        }
    }

    bool LifecycleLowering::run(AST::Program* program) {
        if (program == nullptr) return false;
        program_ = program;
        had_error_ = false;
        collect_structs();
        collect_declarations();
        validate_special_members();
        lower_special_members();
        for (auto& top : program_->top_levels) {
            if (dynamic_cast<FunctionDefinition*>(top.get()) != nullptr) {
                auto* func = static_cast<FunctionDefinition*>(top.get());
                if (func->name.rfind("__sgc_", 0) == 0) continue;
            }
            rewrite_top_level(top.get());
        }
        return !had_error_;
    }

}
