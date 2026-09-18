#include "lifecycle.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <functional>

using namespace gallt::AST;

namespace gallt {

    namespace {
        constexpr int kArgumentRankUnknown = 1 << 20;
    }

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
        for (auto& top : program_->top_levels) {
            if (auto* def = dynamic_cast<StructDefinition*>(top.get())) {
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

    void LifecycleLowering::rewrite_member_references(Statement* stmt,
        const std::unordered_set<std::string>& members,
        const std::unordered_set<std::string>& locals) {
        std::unordered_set<std::string> shadowed = locals;
        std::vector<std::string> introduced;
        auto declare_local = [&](const std::string& name) {
            if (name.empty()) return;
            if (shadowed.insert(name).second) introduced.push_back(name);
        };
        auto leave_scope = [&](std::size_t mark) {
            while (introduced.size() > mark) {
                shadowed.erase(introduced.back());
                introduced.pop_back();
            }
        };
        std::function<std::unique_ptr<Expression>(std::unique_ptr<Expression>)> rewrite_expr;
        rewrite_expr = [&](std::unique_ptr<Expression> expr) -> std::unique_ptr<Expression> {
            if (expr == nullptr) return nullptr;
            if (auto* prim = dynamic_cast<PrimaryExpression*>(expr.get())) {
                if (prim->kind == PrimaryExpression::Kind::Identifier &&
                    members.count(prim->identifier) != 0 &&
                    shadowed.count(prim->identifier) == 0) {
                    auto base = std::make_unique<PrimaryExpression>(prim->location, "__this");
                    auto arrow = std::make_unique<PostfixExpression>(prim->location,
                        std::move(base), PostfixExpression::Operator::Arrow);
                    arrow->member_name = prim->identifier;
                    return arrow;
                }
                if (prim->kind == PrimaryExpression::Kind::Parens) {
                    prim->paren_expr = rewrite_expr(std::move(prim->paren_expr));
                }
                else if (prim->kind == PrimaryExpression::Kind::Heap) {
                    prim->heap_size = rewrite_expr(std::move(prim->heap_size));
                }
                else if (prim->kind == PrimaryExpression::Kind::Construct ||
                    prim->kind == PrimaryExpression::Kind::PlacementConstruct) {
                    for (auto& arg : prim->construct_args) arg = rewrite_expr(std::move(arg));
                    prim->placement_target = rewrite_expr(std::move(prim->placement_target));
                }
                else if (prim->kind == PrimaryExpression::Kind::CopyMove) {
                    prim->paren_expr = rewrite_expr(std::move(prim->paren_expr));
                }
                return expr;
            }
            if (auto* assign = dynamic_cast<AssignmentExpression*>(expr.get())) {
                assign->left = rewrite_expr(std::move(assign->left));
                assign->right = rewrite_expr(std::move(assign->right));
                return expr;
            }
            auto rewrite_binary = [&](Expression* lhs, Expression* rhs) {
                (void)lhs; (void)rhs;
            };
            (void)rewrite_binary;
            if (auto* e = dynamic_cast<LogicalOrExpression*>(expr.get())) {
                e->left = rewrite_expr(std::move(e->left));
                e->right = rewrite_expr(std::move(e->right));
                return expr;
            }
            if (auto* e = dynamic_cast<LogicalAndExpression*>(expr.get())) {
                e->left = rewrite_expr(std::move(e->left));
                e->right = rewrite_expr(std::move(e->right));
                return expr;
            }
            if (auto* e = dynamic_cast<ComparisonExpression*>(expr.get())) {
                e->left = rewrite_expr(std::move(e->left));
                e->right = rewrite_expr(std::move(e->right));
                return expr;
            }
            if (auto* e = dynamic_cast<AdditiveExpression*>(expr.get())) {
                e->left = rewrite_expr(std::move(e->left));
                e->right = rewrite_expr(std::move(e->right));
                return expr;
            }
            if (auto* e = dynamic_cast<MultiplicativeExpression*>(expr.get())) {
                e->left = rewrite_expr(std::move(e->left));
                e->right = rewrite_expr(std::move(e->right));
                return expr;
            }
            if (auto* e = dynamic_cast<PowerExpression*>(expr.get())) {
                e->left = rewrite_expr(std::move(e->left));
                e->right = rewrite_expr(std::move(e->right));
                return expr;
            }
            if (auto* e = dynamic_cast<UnaryExpression*>(expr.get())) {
                e->operand = rewrite_expr(std::move(e->operand));
                return expr;
            }
            if (auto* e = dynamic_cast<PostfixExpression*>(expr.get())) {
                e->base = rewrite_expr(std::move(e->base));
                e->subscript_expr = rewrite_expr(std::move(e->subscript_expr));
                for (auto& arg : e->arguments) arg = rewrite_expr(std::move(arg));
                return expr;
            }
            return expr;
        };

        std::function<void(Statement*)> walk = [&](Statement* s) {
            if (s == nullptr) return;
            if (auto* vd = dynamic_cast<VariableDeclaration*>(s)) {
                if (vd->initializer) {
                    if (auto* ei = dynamic_cast<ExpressionInitializer*>(vd->initializer.get())) {
                        ei->expr = rewrite_expr(std::move(ei->expr));
                    }
                    else if (auto* ai = dynamic_cast<ArrayInitializer*>(vd->initializer.get())) {
                        std::function<void(Initializer*)> init_walk = [&](Initializer* init) {
                            if (auto* ie = dynamic_cast<ExpressionInitializer*>(init)) {
                                ie->expr = rewrite_expr(std::move(ie->expr));
                            }
                            else if (auto* ia = dynamic_cast<ArrayInitializer*>(init)) {
                                for (auto& element : ia->elements) init_walk(element.get());
                            }
                        };
                        init_walk(ai);
                    }
                }
                declare_local(vd->name);
                return;
            }
            if (auto* es = dynamic_cast<ExpressionStatement*>(s)) {
                es->expr = rewrite_expr(std::move(es->expr));
                return;
            }
            if (auto* rs = dynamic_cast<ReturnStatement*>(s)) {
                rs->value = rewrite_expr(std::move(rs->value));
                return;
            }
            if (auto* ds = dynamic_cast<DestructStatement*>(s)) {
                ds->target = rewrite_expr(std::move(ds->target));
                return;
            }
            if (auto* block = dynamic_cast<Block*>(s)) {
                std::size_t mark = introduced.size();
                for (auto& child : block->statements) walk(child.get());
                leave_scope(mark);
                return;
            }
            if (auto* ifs = dynamic_cast<IfStatement*>(s)) {
                ifs->condition = rewrite_expr(std::move(ifs->condition));
                walk(ifs->then_block.get());
                walk(ifs->else_block.get());
                return;
            }
            if (auto* fors = dynamic_cast<ForStatement*>(s)) {
                std::size_t mark = introduced.size();
                walk(fors->init.get());
                fors->condition = rewrite_expr(std::move(fors->condition));
                fors->step = rewrite_expr(std::move(fors->step));
                walk(fors->body.get());
                leave_scope(mark);
                return;
            }
            if (auto* whiles = dynamic_cast<WhileStatement*>(s)) {
                whiles->condition = rewrite_expr(std::move(whiles->condition));
                walk(whiles->body.get());
                return;
            }
        };
        walk(stmt);
    }

    std::unique_ptr<FunctionDefinition> LifecycleLowering::lower_special_member(
        StructDefinition* def, SpecialMemberFunction* member, const std::string& name) {
        std::unordered_set<std::string> members;
        for (const auto& m : def->members) members.insert(m.name);
        std::unordered_set<std::string> locals;
        for (const auto& pname : member->parameter_names) {
            if (!pname.empty()) locals.insert(pname);
        }
        if (!member->parameter_name.empty()) locals.insert(member->parameter_name);
        locals.insert("__this");
        rewrite_member_references(member->body.get(), members, locals);

        std::vector<Type> params;
        std::vector<std::string> param_names;
        params.push_back(Type::make_pointer(std::make_shared<Type>(Type::make_struct(def->name))));
        param_names.push_back("__this");
        if (member->kind == SpecialMemberFunction::Kind::Constructor) {
            for (std::size_t i = 0; i < member->parameters.size(); ++i) {
                params.push_back(member->parameters[i]);
                param_names.push_back(i < member->parameter_names.size() &&
                    !member->parameter_names[i].empty()
                    ? member->parameter_names[i]
                    : "_param" + std::to_string(i));
            }
        }
        else if (member->kind != SpecialMemberFunction::Kind::Destructor) {
            params.push_back(Type::make_pointer(
                std::make_shared<Type>(Type::make_struct(def->name))));
            param_names.push_back(member->parameter_name.empty()
                ? std::string("__other") : member->parameter_name);
        }
        auto func = std::make_unique<FunctionDefinition>(member->location,
            Type::make_void(), name, params, param_names, std::move(member->body));
        if (member->kind == SpecialMemberFunction::Kind::Constructor) {
            func->param_defaults.push_back(nullptr);
            for (auto& default_value : member->parameter_defaults) {
                func->param_defaults.push_back(std::move(default_value));
            }
        }
        return func;
    }

    void LifecycleLowering::lower_special_members() {
        std::vector<std::unique_ptr<TopLevel>> generated;
        for (auto& pair : structs_) {
            StructInfo& info = pair.second;
            StructDefinition* def = info.def;
            for (std::size_t i = 0; i < info.constructors.size(); ++i) {
                auto func = lower_special_member(def, info.constructors[i],
                    info.ctor_names[i]);
                generated.push_back(std::move(func));
            }
            if (info.destructor != nullptr) {
                auto func = lower_special_member(def, info.destructor, info.dtor_name);
                generated.push_back(std::move(func));
            }
            if (info.copy_constructor != nullptr) {
                auto func = lower_special_member(def, info.copy_constructor,
                    info.copy_ctor_name);
                generated.push_back(std::move(func));
            }
            if (info.move_constructor != nullptr) {
                auto func = lower_special_member(def, info.move_constructor,
                    info.move_ctor_name);
                generated.push_back(std::move(func));
            }
            if (info.copy_assignment != nullptr) {
                auto func = lower_special_member(def, info.copy_assignment,
                    info.copy_assign_name);
                generated.push_back(std::move(func));
            }
            if (info.move_assignment != nullptr) {
                auto func = lower_special_member(def, info.move_assignment,
                    info.move_assign_name);
                generated.push_back(std::move(func));
            }
            def->destructor_name = (info.destructor != nullptr) ? info.dtor_name : std::string();
            def->copy_constructor_name = info.copy_ctor_name;
            def->move_constructor_name = info.move_ctor_name;
            def->copy_assignment_name = info.copy_assign_name;
            def->move_assignment_name = info.move_assign_name;
            def->constructor_names = info.ctor_names;
            def->ctor_overload_name = info.ctor_names.empty() ? std::string()
                : info.ctor_names.front();
            def->constructor_param_types.clear();
            for (AST::SpecialMemberFunction* ctor : info.constructors) {
                def->constructor_param_types.push_back(ctor->parameters);
            }
        }
        std::vector<std::unique_ptr<TopLevel>> merged;
        merged.reserve(program_->top_levels.size() + generated.size());
        for (auto& node : generated) merged.push_back(std::move(node));
        for (auto& node : program_->top_levels) merged.push_back(std::move(node));
        program_->top_levels = std::move(merged);
    }

    AST::Type LifecycleLowering::infer_literal_type(const AST::Expression* expr) const {
        if (auto* prim = dynamic_cast<const PrimaryExpression*>(expr)) {
            if (prim->kind == PrimaryExpression::Kind::Literal) {
                switch (prim->literal_token.type) {
                case TokenType::IntegerLiteral:
                    return Type::integer_literal_type(prim->literal_token.lexeme);
                case TokenType::FloatLiteral:
                    return Type::float_literal_type(prim->literal_token.lexeme);
                case TokenType::CharLiteral: return Type::make_char();
                case TokenType::BoolLiteral: return Type::make_bool();
                case TokenType::StringLiteral: return Type::make_string();
                default: break;
                }
            }
            if (prim->kind == PrimaryExpression::Kind::Null) {
                return Type::make_pointer(std::make_shared<Type>(Type::make_void()));
            }
        }
        return Type::make_void();
    }

    bool LifecycleLowering::type_matches_argument(const AST::Type& param,
        const AST::Expression* arg) const {
        AST::Type arg_type = infer_literal_type(arg);
        if (arg_type.kind == TypeKind::Void) return true;   
        if (param.kind == TypeKind::Pointer && arg_type.kind == TypeKind::Pointer) return true;
        if (param == arg_type) return true;
        if (param.is_floating() && arg_type.is_arithmetic()) return true;
        if (param.kind == TypeKind::Double && arg_type.kind == TypeKind::Float) return true;
        if (param.kind == TypeKind::Int && arg_type.is_integer()) return true;
        if (param.kind == TypeKind::Lint && arg_type.is_integer()) return true;
        if (param.kind == TypeKind::Uint && arg_type.is_integer()) return true;
        if (param.kind == TypeKind::Luint && arg_type.is_integer()) return true;
        if (param.kind == TypeKind::Uchar && arg_type.is_integer()) return true;
        if (param.kind == TypeKind::Bool && arg_type.is_integer()) return true;
        if (param.kind == TypeKind::Char && arg_type.is_integer()) return true;
        return false;
    }

    int LifecycleLowering::argument_rank(const AST::Type& param,
        const AST::Expression* arg) const {
        constexpr int kExact = 0;
        constexpr int kPromotion = 100;
        constexpr int kConversion = 200;
        AST::Type arg_type = infer_literal_type(arg);
        if (arg_type.kind == TypeKind::Void) return kArgumentRankUnknown;
        auto position = [](const AST::Type& t) -> int {
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
        if (param == arg_type) return kExact;
        if (param.kind == TypeKind::Pointer && arg_type.kind == TypeKind::Pointer) {
            return kConversion;
        }
        const int from_pos = position(arg_type);
        const int to_pos = position(param);
        if (from_pos >= 0 && to_pos >= 0) {
            const bool small_integer = arg_type.kind == TypeKind::Bool ||
                arg_type.kind == TypeKind::Char || arg_type.kind == TypeKind::Uchar;
            bool promotion = (small_integer && (param.kind == TypeKind::Int ||
                param.kind == TypeKind::Uint)) ||
                (arg_type.kind == TypeKind::Float && param.kind == TypeKind::Double);
            int distance = std::abs(to_pos - from_pos);
            if (distance == 0) distance = 1;
            return (promotion ? kPromotion : kConversion) + distance;
        }
        if (!type_matches_argument(param, arg)) return -1;
        return kConversion;
    }

    AST::SpecialMemberFunction* LifecycleLowering::select_constructor(const StructInfo& info,
        const std::vector<AST::Expression*>& args, bool* ambiguous) {
        *ambiguous = false;
        struct Candidate {
            AST::SpecialMemberFunction* ctor = nullptr;
            std::vector<int> ranks;
        };
        std::vector<Candidate> viable;
        for (AST::SpecialMemberFunction* ctor : info.constructors) {
            std::size_t required = ctor->parameters.size();
            for (std::size_t i = ctor->parameter_defaults.size(); i > 0; --i) {
                if (ctor->parameter_defaults[i - 1] != nullptr) required = i - 1;
                else break;
            }
            if (args.size() > ctor->parameters.size() || args.size() < required) continue;
            Candidate candidate;
            candidate.ctor = ctor;
            bool ok = true;
            for (std::size_t i = 0; i < args.size(); ++i) {
                int rank = argument_rank(ctor->parameters[i], args[i]);
                if (rank < 0) {
                    ok = false;
                    break;
                }
                candidate.ranks.push_back(rank);
            }
            if (ok) viable.push_back(std::move(candidate));
        }
        if (viable.empty()) return nullptr;
        auto better = [](const std::vector<int>& a, const std::vector<int>& b) {
            bool strictly_better = false;
            for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) {
                if (a[k] == kArgumentRankUnknown || b[k] == kArgumentRankUnknown) continue;
                if (a[k] > b[k]) return false;
                if (a[k] < b[k]) strictly_better = true;
            }
            return strictly_better;
        };
        std::size_t best = 0;
        std::size_t best_count = 0;
        bool best_has_unknown = false;
        for (std::size_t i = 0; i < viable.size(); ++i) {
            bool is_best = true;
            for (std::size_t j = 0; j < viable.size(); ++j) {
                if (i == j) continue;
                if (better(viable[j].ranks, viable[i].ranks)) {
                    is_best = false;
                    break;
                }
            }
            if (is_best) {
                best = i;
                ++best_count;
                for (int rank : viable[i].ranks) {
                    if (rank == kArgumentRankUnknown) best_has_unknown = true;
                }
            }
        }
        if (best_count > 1) {
            if (best_has_unknown) {
                return viable[best].ctor;
            }
            *ambiguous = true;
            return nullptr;
        }
        return viable[best].ctor;
    }

   void LifecycleLowering::rewrite_declaration(VariableDeclaration* decl,
       std::vector<std::unique_ptr<Statement>>& insert_after) {
        if (decl == nullptr) return;
        if (auto* expr_init = dynamic_cast<ExpressionInitializer*>(decl->initializer.get())) {
            rewrite_expression(expr_init->expr.get());
        }
        if (decl->type.kind != TypeKind::Struct) return;
        auto info_it = structs_.find(decl->type.struct_name);
        if (info_it == structs_.end()) return;
        StructInfo& info = info_it->second;
        const std::string& struct_name = decl->type.struct_name;

        auto make_self_address = [&]() {
            auto target = std::make_unique<PrimaryExpression>(decl->location, decl->name);
            return std::make_unique<UnaryExpression>(decl->location,
                UnaryExpression::Operator::AddressOf, std::move(target));
        };
        auto emit_self_call = [&](const std::string& func_name,
            std::unique_ptr<Expression> extra, SourceLocation loc) {
            auto call = std::make_unique<PostfixExpression>(loc,
                std::make_unique<PrimaryExpression>(loc, func_name),
                PostfixExpression::Operator::FunctionCall);
            call->arguments.push_back(make_self_address());
            if (extra != nullptr) call->arguments.push_back(std::move(extra));
            decl->initializer.reset();
            insert_after.push_back(std::make_unique<ExpressionStatement>(loc,
                std::move(call)));
        };

        auto* expr_init2 = dynamic_cast<ExpressionInitializer*>(decl->initializer.get());
        if (expr_init2 == nullptr) {
            if (!info.constructors.empty()) {
                for (std::size_t i = 0; i < info.constructors.size(); ++i) {
                    std::size_t required = info.constructors[i]->parameters.size();
                    for (std::size_t k = info.constructors[i]->parameter_defaults.size(); k > 0; --k) {
                        if (info.constructors[i]->parameter_defaults[k - 1] != nullptr) {
                            required = k - 1;
                        }
                        else {
                            break;
                        }
                    }
                    if (required == 0) {
                        emit_self_call(info.ctor_names[i], nullptr, decl->location);
                        return;
                    }
                }
                report(decl->location, ErrorCode::FunctionArgCountMismatch,
                    "type '" + struct_name + "' has no default constructor");
            }
            return;
        }


        auto* call = dynamic_cast<PostfixExpression*>(expr_init2->expr.get());
        if (auto* cm = dynamic_cast<PrimaryExpression*>(expr_init2->expr.get())) {
            if (cm->kind == PrimaryExpression::Kind::CopyMove) {
                bool is_move = cm->copy_move_kind == PrimaryExpression::CopyMoveKind::Move;
                bool is_copy = cm->copy_move_kind == PrimaryExpression::CopyMoveKind::Copy;
                const std::string* special = nullptr;
                if (is_move && info.move_constructor != nullptr) special = &info.move_ctor_name;
                if (is_copy && info.copy_constructor != nullptr) special = &info.copy_ctor_name;
                if (special != nullptr) {
                    std::unique_ptr<Expression> source_operand = std::move(cm->paren_expr);
                    if (auto* src_prim = dynamic_cast<PrimaryExpression*>(source_operand.get())) {
                        if (src_prim->kind == PrimaryExpression::Kind::Identifier) {
                            source_operand = std::make_unique<UnaryExpression>(cm->location,
                                UnaryExpression::Operator::AddressOf,
                                std::move(source_operand));
                        }
                    }
                    emit_self_call(*special, std::move(source_operand), cm->location);
                    return;
                }
                return;
            }
        }
        if (call == nullptr || call->op != PostfixExpression::Operator::FunctionCall) return;
        auto* callee = dynamic_cast<PrimaryExpression*>(call->base.get());
        if (callee == nullptr || callee->kind != PrimaryExpression::Kind::Identifier) return;

        bool is_type_construction = (callee->identifier == struct_name);
        bool is_copy = (callee->identifier == "copy");
        bool is_move = (callee->identifier == "move");
        bool is_deep = (callee->identifier == "deep_copy");
        bool is_shallow = (callee->identifier == "shallow_copy");
        if (!is_type_construction && !is_copy && !is_move && !is_deep && !is_shallow) return;

        if (is_copy || is_move) {
        }

        if (is_type_construction) {
            bool ambiguous = false;
            std::vector<AST::Expression*> args;
            for (auto& a : call->arguments) args.push_back(a.get());
            AST::SpecialMemberFunction* ctor = select_constructor(info, args, &ambiguous);
            if (ambiguous) {
                report_template(call->location, ErrorCode::SpecialMemberAmbiguous,
                    { struct_name });
                return;
            }
            if (ctor == nullptr) {
                if (info.constructors.empty()) {
                    std::vector<std::unique_ptr<Initializer>> elements;
                    for (auto& a : call->arguments) {
                        elements.push_back(std::make_unique<ExpressionInitializer>(
                            a->location, std::move(a)));
                    }
                    decl->initializer = std::make_unique<ArrayInitializer>(
                        call->location, std::move(elements));
                }
                return;
            }
            std::size_t index = static_cast<std::size_t>(
                std::find(info.constructors.begin(), info.constructors.end(), ctor)
                - info.constructors.begin());
            auto call_stmt = std::make_unique<PostfixExpression>(call->location,
                std::make_unique<PrimaryExpression>(call->location, info.ctor_names[index]),
                PostfixExpression::Operator::FunctionCall);
            call_stmt->arguments.push_back(make_self_address());
            for (auto& a : call->arguments) call_stmt->arguments.push_back(std::move(a));
            decl->initializer.reset();
            insert_after.push_back(std::make_unique<ExpressionStatement>(call->location,
                std::move(call_stmt)));
            return;
        }

        if (call->arguments.empty()) return;
        std::unique_ptr<Expression> source = std::move(call->arguments[0]);
        const std::string* special = nullptr;
        if (is_move && info.move_constructor != nullptr) special = &info.move_ctor_name;
        if (is_copy && info.copy_constructor != nullptr) special = &info.copy_ctor_name;
        if (special != nullptr) {
            emit_self_call(*special, std::move(source), call->location);
            return;
        }
        expr_init2->expr = std::move(source);
    }

    void LifecycleLowering::rewrite_assignment_call(ExpressionStatement* stmt,
        AssignmentExpression* assign, const std::string& func_name) {
        std::unique_ptr<Expression> target = std::move(assign->left);
        auto address = [](std::unique_ptr<Expression> operand) -> std::unique_ptr<Expression> {
            if (auto* prim = dynamic_cast<PrimaryExpression*>(operand.get())) {
                if (prim->kind == PrimaryExpression::Kind::Identifier) {
                    return std::make_unique<UnaryExpression>(operand->location,
                        UnaryExpression::Operator::AddressOf, std::move(operand));
                }
            }
            return operand;
        };
        std::unique_ptr<Expression> source = std::move(assign->right);
        if (auto* cm = dynamic_cast<PrimaryExpression*>(source.get())) {
            if (cm->kind == PrimaryExpression::Kind::CopyMove) {
                source = std::move(cm->paren_expr);
            }
        }
        auto call = std::make_unique<PostfixExpression>(stmt->location,
            std::make_unique<PrimaryExpression>(stmt->location, func_name),
            PostfixExpression::Operator::FunctionCall);
        call->arguments.push_back(address(std::move(target)));
        call->arguments.push_back(address(std::move(source)));
        stmt->expr = std::move(call);
    }

    void LifecycleLowering::rewrite_expression(AST::Expression* expr) {
        if (expr == nullptr) return;
        if (auto* prim = dynamic_cast<PrimaryExpression*>(expr)) {
            if (prim->kind == PrimaryExpression::Kind::Construct ||
                prim->kind == PrimaryExpression::Kind::PlacementConstruct) {
                const std::string& struct_name = prim->construct_type.struct_name;
                auto it = structs_.find(struct_name);
                if (prim->construct_type.kind != TypeKind::Struct) {
                    report_template(prim->location, ErrorCode::SpecialMemberOnNonStruct,
                        { prim->construct_type.to_string() });
                    return;
                }
                if (it == structs_.end()) return;
                StructInfo& info = it->second;
                bool ambiguous = false;
                std::vector<AST::Expression*> args;
                for (auto& a : prim->construct_args) args.push_back(a.get());
                AST::SpecialMemberFunction* ctor = select_constructor(info, args, &ambiguous);
                if (ambiguous) {
                    report_template(prim->location, ErrorCode::SpecialMemberAmbiguous,
                        { struct_name });
                    return;
                }
                if (ctor != nullptr) {
                    std::size_t index = static_cast<std::size_t>(
                        std::find(info.constructors.begin(), info.constructors.end(), ctor)
                        - info.constructors.begin());
                    prim->lowered_ctor = info.ctor_names[index];
                }
                else if (info.constructors.empty() && !prim->construct_args.empty()) {
                    report_template(prim->location, ErrorCode::FunctionArgCountMismatch,
                        { "0", std::to_string(prim->construct_args.size()) });
                }
                if (prim->kind == PrimaryExpression::Kind::PlacementConstruct) {
                    auto* target = dynamic_cast<PrimaryExpression*>(prim->placement_target.get());
                    if (target != nullptr && target->kind == PrimaryExpression::Kind::Null) {
                        report_template(prim->location, ErrorCode::PlacementTargetInvalid,
                            std::vector<std::string>{});
                    }
                }
                return;
            }
            if (prim->kind == PrimaryExpression::Kind::Parens) {
                rewrite_expression(prim->paren_expr.get());
            }
            else if (prim->kind == PrimaryExpression::Kind::Heap) {
                rewrite_expression(prim->heap_size.get());
            }
            else if (prim->kind == PrimaryExpression::Kind::CopyMove) {
                rewrite_expression(prim->paren_expr.get());
            }
            return;
        }
        if (auto* post = dynamic_cast<PostfixExpression*>(expr)) {
            if (post->op == PostfixExpression::Operator::Dot ||
                post->op == PostfixExpression::Operator::Arrow) {
                static const std::unordered_set<std::string> kSpecials = {
                    "constructor", "copy_constructor", "move_constructor",
                    "copy_assignment", "move_assignment",
                };
                if (kSpecials.count(post->member_name) != 0) {
                    report_template(post->location, ErrorCode::SpecialMemberDirectCall,
                        { post->member_name });
                }
            }
            rewrite_expression(post->base.get());
            rewrite_expression(post->subscript_expr.get());
            for (auto& arg : post->arguments) rewrite_expression(arg.get());
            return;
        }
        if (auto* assign = dynamic_cast<AssignmentExpression*>(expr)) {
            rewrite_expression(assign->left.get());
            rewrite_expression(assign->right.get());
            return;
        }
        if (auto* e = dynamic_cast<LogicalOrExpression*>(expr)) {
            rewrite_expression(e->left.get()); rewrite_expression(e->right.get()); return;
        }
        if (auto* e = dynamic_cast<LogicalAndExpression*>(expr)) {
            rewrite_expression(e->left.get()); rewrite_expression(e->right.get()); return;
        }
        if (auto* e = dynamic_cast<ComparisonExpression*>(expr)) {
            rewrite_expression(e->left.get()); rewrite_expression(e->right.get()); return;
        }
        if (auto* e = dynamic_cast<AdditiveExpression*>(expr)) {
            rewrite_expression(e->left.get()); rewrite_expression(e->right.get()); return;
        }
        if (auto* e = dynamic_cast<MultiplicativeExpression*>(expr)) {
            rewrite_expression(e->left.get()); rewrite_expression(e->right.get()); return;
        }
        if (auto* e = dynamic_cast<PowerExpression*>(expr)) {
            rewrite_expression(e->left.get()); rewrite_expression(e->right.get()); return;
        }
        if (auto* e = dynamic_cast<UnaryExpression*>(expr)) {
            rewrite_expression(e->operand.get());
            return;
        }
    }

    void LifecycleLowering::track_pointer_source(const std::string& name,
        const Expression* expr) {
        if (expr == nullptr) return;
        if (auto* prim = dynamic_cast<const PrimaryExpression*>(expr)) {
            switch (prim->kind) {
            case PrimaryExpression::Kind::Construct:
            case PrimaryExpression::Kind::PlacementConstruct:
                constructed_pointers_.insert(name);
                alias_group_[name] = next_alias_group_++;
                return;
            case PrimaryExpression::Kind::Heap:
                non_construct_pointers_.insert(name);
                return;
            case PrimaryExpression::Kind::Null:
                null_pointers_.insert(name);
                return;
            case PrimaryExpression::Kind::Identifier: {
                const std::string& source = prim->identifier;
                if (null_pointers_.count(source) != 0) {
                    null_pointers_.insert(name);
                    return;
                }
                if (non_construct_pointers_.count(source) != 0) {
                    non_construct_pointers_.insert(name);
                    return;
                }
                auto group = alias_group_.find(source);
                if (group != alias_group_.end()) {
                    constructed_pointers_.insert(name);
                    alias_group_[name] = group->second;
                }
                return;
            }
            default:
                return;
            }
        }
        if (auto* un = dynamic_cast<const UnaryExpression*>(expr)) {
            if (un->op == UnaryExpression::Operator::AddressOf ||
                un->op == UnaryExpression::Operator::Dereference ||
                un->op == UnaryExpression::Operator::Increment ||
                un->op == UnaryExpression::Operator::Decrement) {
                non_construct_pointers_.insert(name);
            }
            return;
        }
        if (dynamic_cast<const PostfixExpression*>(expr) != nullptr) {
            non_construct_pointers_.insert(name);
        }
    }

    void LifecycleLowering::rewrite_statement(Statement* stmt) {
        if (stmt == nullptr) return;
        if (auto* block = dynamic_cast<Block*>(stmt)) {
            std::vector<std::unique_ptr<Statement>> rewritten;
            for (auto& child : block->statements) {
                std::vector<std::unique_ptr<Statement>> insert_after;
                if (auto* vd = dynamic_cast<VariableDeclaration*>(child.get())) {
                    local_types_[vd->name] = vd->type;
                    if (vd->type.kind == TypeKind::Pointer) {
                        if (auto* expr_init =
                            dynamic_cast<ExpressionInitializer*>(vd->initializer.get())) {
                            track_pointer_source(vd->name, expr_init->expr.get());
                        }
                    }
                    rewrite_declaration(vd, insert_after);
                }
                else {
                    rewrite_statement(child.get());
                }
                rewritten.push_back(std::move(child));
                for (auto& extra : insert_after) rewritten.push_back(std::move(extra));
            }
            block->statements = std::move(rewritten);
            return;
        }
        if (auto* vd = dynamic_cast<VariableDeclaration*>(stmt)) {
            std::vector<std::unique_ptr<Statement>> ignored;
            rewrite_declaration(vd, ignored);
            return;
        }
        if (auto* ifs = dynamic_cast<IfStatement*>(stmt)) {
            rewrite_expression(ifs->condition.get());
            rewrite_statement(ifs->then_block.get());
            rewrite_statement(ifs->else_block.get());
            return;
        }
        if (auto* fors = dynamic_cast<ForStatement*>(stmt)) {
            rewrite_statement(fors->init.get());
            rewrite_expression(fors->condition.get());
            rewrite_expression(fors->step.get());
            rewrite_statement(fors->body.get());
            return;
        }
        if (auto* whiles = dynamic_cast<WhileStatement*>(stmt)) {
            rewrite_expression(whiles->condition.get());
            rewrite_statement(whiles->body.get());
            return;
        }
        if (auto* rs = dynamic_cast<ReturnStatement*>(stmt)) {
            rewrite_expression(rs->value.get());
            return;
        }
        if (auto* es = dynamic_cast<ExpressionStatement*>(stmt)) {
            if (auto* assign = dynamic_cast<AssignmentExpression*>(es->expr.get())) {
                if (assign->op == AssignmentExpression::Operator::Assign) {
                    auto* left_id = dynamic_cast<PrimaryExpression*>(assign->left.get());
                    if (left_id != nullptr &&
                        left_id->kind == PrimaryExpression::Kind::Identifier) {
                        auto type_it = local_types_.find(left_id->identifier);
                        if (type_it != local_types_.end() &&
                            type_it->second.kind == TypeKind::Pointer) {
                            track_pointer_source(left_id->identifier, assign->right.get());
                        }
                    }
                    if (left_id != nullptr && left_id->kind == PrimaryExpression::Kind::Identifier) {
                        auto type_it = local_types_.find(left_id->identifier);
                        if (type_it != local_types_.end() &&
                            type_it->second.kind == TypeKind::Struct) {
                            auto info_it = structs_.find(type_it->second.struct_name);
                            if (info_it != structs_.end()) {
                                StructInfo& info = info_it->second;
                                bool is_move = false;
                                if (auto* cm = dynamic_cast<PrimaryExpression*>(
                                    assign->right.get())) {
                                    if (cm->kind == PrimaryExpression::Kind::CopyMove &&
                                        cm->copy_move_kind == PrimaryExpression::CopyMoveKind::Move) {
                                        is_move = true;
                                    }
                                }
                                if (is_move && info.move_assignment != nullptr) {
                                    rewrite_assignment_call(es, assign, info.move_assign_name);
                                    return;
                                }
                                if (!is_move && info.copy_assignment != nullptr) {
                                    rewrite_assignment_call(es, assign, info.copy_assign_name);
                                    return;
                                }
                            }
                        }
                    }
                }
            }
            rewrite_expression(es->expr.get());
            return;
        }
        if (auto* ds = dynamic_cast<DestructStatement*>(stmt)) {
            rewrite_expression(ds->target.get());
            if (auto* prim = dynamic_cast<PrimaryExpression*>(ds->target.get())) {
                if (prim->kind == PrimaryExpression::Kind::Identifier) {
                    const std::string& name = prim->identifier;
                    if (null_pointers_.count(name) != 0) {
                    }
                    else if (non_construct_pointers_.count(name) != 0) {
                        report(ds->location, ErrorCode::DestructNonConstructed, std::string());
                    }
                    else if (constructed_pointers_.count(name) != 0) {
                        auto group = alias_group_.find(name);
                        if (group != alias_group_.end()) {
                            if (destructed_groups_.count(group->second) != 0) {
                                report(ds->location, ErrorCode::DestructTwice, std::string());
                            }
                            else {
                                destructed_groups_.insert(group->second);
                            }
                        }
                    }
                }
                else if (prim->kind == PrimaryExpression::Kind::Null) {
                }
                else {
                    report(ds->location, ErrorCode::DestructNonConstructed, std::string());
                }
            }
            else {
                report(ds->location, ErrorCode::DestructNonConstructed, std::string());
            }
            return;
        }
        if (auto* sd = dynamic_cast<StructDefinition*>(stmt)) {
            for (auto& member : sd->members) {
                if (auto* ei = dynamic_cast<ExpressionInitializer*>(member.initializer.get())) {
                    rewrite_expression(ei->expr.get());
                }
            }
            return;
        }
    }

    void LifecycleLowering::rewrite_top_level(TopLevel* node) {
        if (auto* def = dynamic_cast<StructDefinition*>(node)) {
            for (auto& member : def->members) {
                if (auto* ei = dynamic_cast<ExpressionInitializer*>(member.initializer.get())) {
                    rewrite_expression(ei->expr.get());
                }
            }
            return;
        }
        if (auto* func = dynamic_cast<FunctionDefinition*>(node)) {
            if (func->body) rewrite_statement(func->body.get());
            return;
        }
        if (auto* vd = dynamic_cast<VariableDeclaration*>(node)) {
            std::vector<std::unique_ptr<Statement>> initializers;
            rewrite_declaration(vd, initializers);
            for (auto& stmt : initializers) {
                program_->global_initializers.push_back(std::move(stmt));
            }
            return;
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
