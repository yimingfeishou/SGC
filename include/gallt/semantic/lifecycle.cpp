// semantic/lifecycle.cpp
// 对象生命周期降低实现 —— Gallt 0.3.txt §20
// Object lifetime lowering — Gallt 0.3.txt §20

#include "lifecycle.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <functional>

using namespace gallt::AST;

namespace gallt {

    namespace {
        // 无法在降低阶段静态判定的实参类型（第 18 章：交由类型检查器复核）
        // Argument type that lowering cannot settle statically (§18: re-checked later)
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

    // ============================================================================
    // 收集与校验
    // ============================================================================

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
                // 命名
                // 第 18/20 章：所有构造函数降低到同一个重载名，由类型检查器按实参类型
                // 做重载决议（含默认参数与二义性 ER 0096）
                // §18/§20: every constructor lowers to one overloaded name so the type checker
                // resolves them by argument type (defaults included, ambiguity -> ER 0096)
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
        // 传播“需要析构”标记：成员含析构函数的结构体也需要析构
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
            return false;   // 字符串由已有的清理机制处理
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

    // 成员级可拷贝判定：任意成员不可拷贝（[nocopy]）则外层类型的默认拷贝函数不生成
    // A type is copyable only when every member is copyable; a [nocopy] member disables the
    // default copy members of the enclosing type (Gallt 0.3.txt §20)
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
            // 拷贝/移动构造与赋值：参数必须是本类型的指针（ER 0086）
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

            // 构造函数重复定义（相同参数列表）→ ER 0085
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

            // [nocopy] / [nomove] 与已定义特殊成员冲突（ER 0090 / ER 0091）
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

            // 特殊成员函数体内不允许 return 表达式（ER 0089）
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
        }
    }

    void LifecycleLowering::collect_declarations() {
        // 特殊成员函数不得与普通函数同名（ER 0098）
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

    // ============================================================================
    // 降低特殊成员为普通函数
    // ============================================================================

    void LifecycleLowering::rewrite_member_references(Statement* stmt,
        const std::unordered_set<std::string>& members,
        const std::unordered_set<std::string>& locals) {
        // 用 __this->member 取代直接书写的成员名。遮蔽必须**按声明位置**生效：
        // 第 5 章规定内层声明遮蔽外层同名声明，因此局部变量只有在它的声明语句之后
        // 才遮蔽同名成员（此前按整个函数体预收集，导致“先写成员、后声明同名局部”无法编译）
        // Replace a bare member name with __this->member. Shadowing is position-aware:
        // §5 makes an inner declaration shadow an outer one, so a local only hides the
        // member after its declaration (a body-wide pre-pass broke "member use before a
        // same-named local declaration")
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
                // 声明语句之后该名字才遮蔽同名成员（其初始化器仍按外层含义解析）
                // The name shadows the member only after this declaration; its initializer
                // still resolves to the outer meaning
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
                // for 初始化中的声明在条件 / 步进 / 循环体内可见，循环结束后失效
                // A for-init declaration is visible in condition/step/body and ends there
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
        // 构造函数默认参数（第 8 章 / 第 20 章）：随降低后的构造函数一起保留，
        // 这样 `T v`、`construct T(...)`、`T(args)` 等调用点都能补全省略的默认实参
        // Constructor default arguments (§8/§20) travel with the lowered constructor so
        // call sites (declaration, construct, T(args)) can fill omitted default arguments
        if (member->kind == SpecialMemberFunction::Kind::Constructor) {
            // 降低后的构造函数形参为 [this, ...构造函数形参]，
            // 因此默认参数表需要为 this 位置补一个空位
            // The lowered parameter list is [this, ...ctor params], so the defaults table
            // needs one leading empty slot for `this`
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
            // 构造函数列表（供 T(args) 值临时对象按实参选择）
            // Constructor list so T(args) temporaries can select by arguments
            def->constructor_names = info.ctor_names;
            def->ctor_overload_name = info.ctor_names.empty() ? std::string()
                : info.ctor_names.front();
            def->constructor_param_types.clear();
            for (AST::SpecialMemberFunction* ctor : info.constructors) {
                def->constructor_param_types.push_back(ctor->parameters);
            }
        }
        // 生成的函数必须位于使用点之前：插入到程序最前面（除 guide/clib 之外）
        std::vector<std::unique_ptr<TopLevel>> merged;
        merged.reserve(program_->top_levels.size() + generated.size());
        for (auto& node : generated) merged.push_back(std::move(node));
        for (auto& node : program_->top_levels) merged.push_back(std::move(node));
        program_->top_levels = std::move(merged);
    }

    // ============================================================================
    // 使用点改写
    // ============================================================================

    AST::Type LifecycleLowering::infer_literal_type(const AST::Expression* expr) const {
        if (auto* prim = dynamic_cast<const PrimaryExpression*>(expr)) {
            if (prim->kind == PrimaryExpression::Kind::Literal) {
                switch (prim->literal_token.type) {
                case TokenType::IntegerLiteral: return Type::make_int();
                case TokenType::FloatLiteral: {
                    std::string_view lexeme = prim->literal_token.lexeme;
                    bool is_float = !lexeme.empty() &&
                        (lexeme.back() == 'f' || lexeme.back() == 'F');
                    return is_float ? Type::make_float() : Type::make_double();
                }
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
        if (arg_type.kind == TypeKind::Void) return true;   // 无法静态判断时保守接受
        if (param.kind == TypeKind::Pointer && arg_type.kind == TypeKind::Pointer) return true;
        if (param == arg_type) return true;
        if (param.is_floating() && arg_type.is_arithmetic()) return true;
        if (param.kind == TypeKind::Double && arg_type.kind == TypeKind::Float) return true;
        if (param.kind == TypeKind::Int && arg_type.is_integer()) return true;
        if (param.kind == TypeKind::Bool && arg_type.is_integer()) return true;
        if (param.kind == TypeKind::Char && arg_type.is_integer()) return true;
        return false;
    }

    int LifecycleLowering::argument_rank(const AST::Type& param,
        const AST::Expression* arg) const {
        // 第 18 章：与类型检查器一致的转换等级——等级（0 精确 / 100 提升 / 200 转换）×
        // 100 的距离项，数值越小越优
        // §18: same conversion ordering the type checker uses — (exact 0 / promotion 100 /
        // conversion 200) plus an arithmetic distance term, smaller is better
        constexpr int kExact = 0;
        constexpr int kPromotion = 100;
        constexpr int kConversion = 200;
        AST::Type arg_type = infer_literal_type(arg);
        if (arg_type.kind == TypeKind::Void) return kArgumentRankUnknown;
        auto position = [](const AST::Type& t) -> int {
            switch (t.kind) {
            case TypeKind::Char:
            case TypeKind::Bool:
            case TypeKind::Int: return 0;
            case TypeKind::Float: return 1;
            case TypeKind::Double: return 2;
            default: return -1;
            }
        };
        if (param == arg_type) return kExact;
        if (param.kind == TypeKind::Pointer && arg_type.kind == TypeKind::Pointer) {
            // 空指针字面量与指针形参视为一次转换
            // A null pointer literal to a pointer parameter counts as one conversion
            return kConversion;
        }
        const int from_pos = position(arg_type);
        const int to_pos = position(param);
        if (from_pos >= 0 && to_pos >= 0) {
            // char/bool → int 属于整数提升；float → double 属于浮点提升
            // char/bool -> int is an integral promotion; float -> double a floating one
            bool promotion = (from_pos == 0 && to_pos == 0) || (from_pos == 1 && to_pos == 2);
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
        // 第 18/20 章：构造函数按实参类型做重载决议——精确匹配优先，转换后更接近形参类型者更优；
        // 仅当存在多个互不可比较的最优候选时才报二义性（ER 0096）
        // §18/§20: constructors resolve by argument type (exact match first, the conversion
        // landing closer to the parameter wins); ambiguity is reported only when several
        // mutually incomparable best candidates exist (ER 0096)
        struct Candidate {
            AST::SpecialMemberFunction* ctor = nullptr;
            std::vector<int> ranks;
        };
        std::vector<Candidate> viable;
        for (AST::SpecialMemberFunction* ctor : info.constructors) {
            // 第 8 章：构造函数也遵循默认参数规则——省略的尾部实参由默认值补齐，
            // 因此只需实参个数落在 [必需个数, 形参总数] 区间内即可作为候选
            // §8: constructors honour default arguments, so a candidate only needs the
            // argument count to fall inside [required, total]
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
        // 偏序：a 优于 b 当且仅当 a 在每个实参上都不差，且至少在一个实参上更好
        // Partial order: a beats b when it is never worse and strictly better somewhere
        auto better = [](const std::vector<int>& a, const std::vector<int>& b) {
            bool strictly_better = false;
            for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) {
                // 无法静态判定的实参不参与比较（保守），最终由类型检查器复核
                // Statically unknown arguments take no part; the type checker re-checks
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
                // 并列原因含“降低阶段无法判定的实参类型”：所有构造函数共享同一重载体名，
                // 因此直接降低为对该重载体的调用，由类型检查器按真实类型复核
                // （真二义性仍在类型检查阶段报 ER 0096，避免在此产生伪二义性）
                // The tie involves arguments lowering cannot type: all constructors share one
                // overloaded name, so lower to that overload set and let the type checker
                // resolve by real types (true ambiguity still reports ER 0096 there)
                return viable[best].ctor;
            }
            // 多个互不更优的候选 → 第 20 章特殊成员重载二义性（ER 0096）
            // Several mutually incomparable best candidates → §20 ER 0096
            *ambiguous = true;
            return nullptr;
        }
        return viable[best].ctor;
    }

   void LifecycleLowering::rewrite_declaration(VariableDeclaration* decl,
       std::vector<std::unique_ptr<Statement>>& insert_after) {
        if (decl == nullptr) return;
        // 初始化表达式中的 construct / placement 始终需要降低
        // construct / placement inside the initializer must always be lowered
        if (auto* expr_init = dynamic_cast<ExpressionInitializer*>(decl->initializer.get())) {
            rewrite_expression(expr_init->expr.get());
        }
        if (decl->type.kind != TypeKind::Struct) return;
        auto info_it = structs_.find(decl->type.struct_name);
        if (info_it == structs_.end()) return;
        StructInfo& info = info_it->second;
        const std::string& struct_name = decl->type.struct_name;

        // 构造目标地址：&decl->name
        auto make_self_address = [&]() {
            auto target = std::make_unique<PrimaryExpression>(decl->location, decl->name);
            return std::make_unique<UnaryExpression>(decl->location,
                UnaryExpression::Operator::AddressOf, std::move(target));
        };
        // 生成 `func(&decl, extra...)` 语句
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
            // 无初始化器：若类型定义了构造函数，则必须存在默认构造函数
            if (!info.constructors.empty()) {
                for (std::size_t i = 0; i < info.constructors.size(); ++i) {
                    // 第 8 章：形参全部带默认值的构造函数也可以零实参调用
                    // §8: a constructor whose parameters all have defaults is callable with none
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

        // 说明：[nocopy]/[nomove] 的诊断统一由类型检查器给出（覆盖声明、赋值、
        // 实参传递、返回值等所有上下文），这里只负责降低语义，避免重复诊断
        // Note: [nocopy]/[nomove] diagnostics come from the type checker (covering
        // declarations, assignments, arguments and returns); this pass only lowers code

        auto* call = dynamic_cast<PostfixExpression*>(expr_init2->expr.get());
        // copy/move/deep_copy/shallow_copy 形式（解析为 CopyMove 主表达式）
        // copy/move/deep_copy/shallow_copy parse to a CopyMove primary expression
        if (auto* cm = dynamic_cast<PrimaryExpression*>(expr_init2->expr.get())) {
            if (cm->kind == PrimaryExpression::Kind::CopyMove) {
                bool is_move = cm->copy_move_kind == PrimaryExpression::CopyMoveKind::Move;
                bool is_copy = cm->copy_move_kind == PrimaryExpression::CopyMoveKind::Copy;
                const std::string* special = nullptr;
                if (is_move && info.move_constructor != nullptr) special = &info.move_ctor_name;
                if (is_copy && info.copy_constructor != nullptr) special = &info.copy_ctor_name;
                // [nocopy]/[nomove] 诊断由类型检查器统一给出
                // [nocopy]/[nomove] diagnostics are emitted by the type checker
                if (special != nullptr) {
                    // 源对象按地址传给拷贝/移动构造函数
                    // The source object is passed by address to the copy/move constructor
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
                // 没有自定义特殊成员时保留 copy/move/deep_copy/shallow_copy 节点，
                // 由代码生成按第 20 章的默认语义处理（默认拷贝/移动构造、深拷贝、浅拷贝）
                // Without a user special member the CopyMove node is kept so codegen can
                // apply the §20 default semantics for each variant
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
            // [nocopy]/[nomove] 诊断由类型检查器统一给出（见 rewrite_declaration 的说明）
            // [nocopy]/[nomove] diagnostics are emitted by the type checker
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
                // 无匹配的构造函数：若用户一个构造函数都没有定义，保持聚合初始化语义
                // （Point p = Point(10, 20) 等价于 Point p = {10, 20}）
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
            // 构造函数实参：把 call 的实参移动到插入语句中
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

        // copy / move / deep_copy / shallow_copy
        if (call->arguments.empty()) return;
        std::unique_ptr<Expression> source = std::move(call->arguments[0]);
        const std::string* special = nullptr;
        if (is_move && info.move_constructor != nullptr) special = &info.move_ctor_name;
        if (is_copy && info.copy_constructor != nullptr) special = &info.copy_ctor_name;
        if (special != nullptr) {
            emit_self_call(*special, std::move(source), call->location);
            return;
        }
        // 默认拷贝语义：直接使用源对象（深拷贝由代码生成处理）
        expr_init2->expr = std::move(source);
    }

    void LifecycleLowering::rewrite_assignment_call(ExpressionStatement* stmt,
        AssignmentExpression* assign, const std::string& func_name) {
        // x = y            -> copy_assignment(&x, &y)
        // x = move(y)      -> move_assignment(&x, &y)
        // 目标始终是本地对象，源可以是对象（取地址）或指针（直接传递）
        // The target is always a local object; the source is an object (address-of) or a
        // pointer (passed as-is)
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
                    // 第 20 章：类型无构造函数时 construct 执行默认初始化，不接受实参
                    // §20: without any constructor `construct` performs default initialization
                    // and takes no arguments
                    report_template(prim->location, ErrorCode::FunctionArgCountMismatch,
                        { "0", std::to_string(prim->construct_args.size()) });
                }
                if (prim->kind == PrimaryExpression::Kind::PlacementConstruct) {
                    // Placement 构造：指针必须非空（ER 0094）
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
            // 直接调用特殊成员（constructor/destructor/...）→ ER 0095
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
        // 第 20 章：destruct 只能作用于 construct 分配的对象；对 null 无操作；
        // 别名（q = p）与源对象共享别名组，便于检测重复 destruct
        // §20: destruct only applies to construct-allocated objects, null is a no-op, and
        // aliases share a group so repeated destruction is detectable
        if (expr == nullptr) return;
        if (auto* prim = dynamic_cast<const PrimaryExpression*>(expr)) {
            switch (prim->kind) {
            case PrimaryExpression::Kind::Construct:
            case PrimaryExpression::Kind::PlacementConstruct:
                constructed_pointers_.insert(name);
                alias_group_[name] = next_alias_group_++;
                return;
            case PrimaryExpression::Kind::Heap:
                // heap 分配的对象不是 construct 分配（ER 0092）
                // A heap allocation is not a construct-allocated object (ER 0092)
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
            // 下标/成员访问得到的内层指针不是 construct 分配的对象
            // A pointer obtained through subscript/member access is not construct-allocated
            non_construct_pointers_.insert(name);
        }
        // 其它来源（函数返回、参数等）未知：不登记，避免误报
        // Unknown origins (call results, parameters) are not tracked to avoid false positives
    }

    void LifecycleLowering::rewrite_statement(Statement* stmt) {
        if (stmt == nullptr) return;
        if (auto* block = dynamic_cast<Block*>(stmt)) {
            std::vector<std::unique_ptr<Statement>> rewritten;
            for (auto& child : block->statements) {
                std::vector<std::unique_ptr<Statement>> insert_after;
                if (auto* vd = dynamic_cast<VariableDeclaration*>(child.get())) {
                    // 变量声明的构造/拷贝改写只做一次，避免初始化器被吞掉
                    // Rewrite each declaration exactly once so its initializer is not consumed twice
                    local_types_[vd->name] = vd->type;
                    if (vd->type.kind == TypeKind::Pointer) {
                        if (auto* expr_init =
                            dynamic_cast<ExpressionInitializer*>(vd->initializer.get())) {
                            // 记录指针来源与别名（第 20 章 ER 0092 / ER 0093）
                            // Track the pointer's origin and aliases (§20 ER 0092/ER 0093)
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
            // 拷贝/移动赋值：改写为对特殊成员函数的调用（Gallt 0.3.txt §20）
            // Copy/move assignment is rewritten into a special-member call
            if (auto* assign = dynamic_cast<AssignmentExpression*>(es->expr.get())) {
                if (assign->op == AssignmentExpression::Operator::Assign) {
                    auto* left_id = dynamic_cast<PrimaryExpression*>(assign->left.get());
                    // 指针赋值：传播来源与别名（q = p → 与 p 同组；q = null → 可安全 destruct）
                    // Pointer assignment: propagate origin/alias (q = p shares p's group;
                    // q = null makes destruct a no-op)
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
                                // [nocopy]/[nomove] 诊断由类型检查器统一给出（覆盖所有上下文）
                                // [nocopy]/[nomove] diagnostics come from the type checker
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
            // ER 0092 / ER 0093：destruct 只能作用于 construct 分配的对象，且不可重复
            // ER 0092 / ER 0093: destruct only applies to construct-allocated objects, once
            if (auto* prim = dynamic_cast<PrimaryExpression*>(ds->target.get())) {
                if (prim->kind == PrimaryExpression::Kind::Identifier) {
                    const std::string& name = prim->identifier;
                    if (null_pointers_.count(name) != 0) {
                        // 第 20 章：对 null 执行 destruct 无操作
                        // §20: destruct on null is a no-op
                    }
                    else if (non_construct_pointers_.count(name) != 0) {
                        // 明确不是 construct 分配（heap/取地址/数组）→ ER 0092
                        report(ds->location, ErrorCode::DestructNonConstructed, std::string());
                    }
                    else if (constructed_pointers_.count(name) != 0) {
                        auto group = alias_group_.find(name);
                        if (group != alias_group_.end()) {
                            if (destructed_groups_.count(group->second) != 0) {
                                // 同一对象（含别名）重复 destruct → ER 0093
                                report(ds->location, ErrorCode::DestructTwice, std::string());
                            }
                            else {
                                destructed_groups_.insert(group->second);
                            }
                        }
                    }
                    // 未知来源（参数、函数返回值等）不报错，避免误报
                    // Unknown origins are not diagnosed to avoid false positives
                }
                else if (prim->kind == PrimaryExpression::Kind::Null) {
                    // destruct null：无操作
                    // destruct null: no-op
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
            // Gallt 0.3.txt §20：全局对象在 main 之前构造（glt_global_init），
            // 因此构造调用被收集到 Program::global_initializers
            // Gallt 0.3.txt §20: global objects are constructed before main, so their
            // constructor calls are collected into Program::global_initializers
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
        // 使用点改写（跳过刚生成的函数：它们已经完成成员引用改写）
        for (auto& top : program_->top_levels) {
            if (dynamic_cast<FunctionDefinition*>(top.get()) != nullptr) {
                auto* func = static_cast<FunctionDefinition*>(top.get());
                if (func->name.rfind("__sgc_", 0) == 0) continue;
            }
            rewrite_top_level(top.get());
        }
        return !had_error_;
    }

} // namespace gallt
