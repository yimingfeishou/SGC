#include "lifecycle.hpp"
#include "lifecycle_detail.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <functional>

using namespace gallt::AST;

namespace gallt {
    using namespace lifecycle_detail;

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
            if (auto* e = dynamic_cast<BitwiseExpression*>(expr.get())) {
                e->left = rewrite_expr(std::move(e->left));
                e->right = rewrite_expr(std::move(e->right));
                return expr;
            }
            if (auto* e = dynamic_cast<ShiftExpression*>(expr.get())) {
                e->left = rewrite_expr(std::move(e->left));
                e->right = rewrite_expr(std::move(e->right));
                return expr;
            }
            if (auto* e = dynamic_cast<ConditionalExpression*>(expr.get())) {
                e->condition = rewrite_expr(std::move(e->condition));
                e->then_expr = rewrite_expr(std::move(e->then_expr));
                e->else_expr = rewrite_expr(std::move(e->else_expr));
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
            decl->constructed_by_lowering = true;
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
                    auto aggregate = std::make_unique<ArrayInitializer>(
                        call->location, std::move(elements));
                    aggregate->from_paren_call = true;
                    decl->initializer = std::move(aggregate);
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
            decl->constructed_by_lowering = true;
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
        if (auto* e = dynamic_cast<BitwiseExpression*>(expr)) {
            rewrite_expression(e->left.get()); rewrite_expression(e->right.get()); return;
        }
        if (auto* e = dynamic_cast<ShiftExpression*>(expr)) {
            rewrite_expression(e->left.get()); rewrite_expression(e->right.get()); return;
        }
        if (auto* e = dynamic_cast<ConditionalExpression*>(expr)) {
            rewrite_expression(e->condition.get());
            rewrite_expression(e->then_expr.get());
            rewrite_expression(e->else_expr.get());
            return;
        }
        if (auto* e = dynamic_cast<UnaryExpression*>(expr)) {
            rewrite_expression(e->operand.get());
            return;
        }
    }

    void LifecycleLowering::track_pointer_source(const std::string& name,
        const Expression* expr) {
        if (expr == nullptr) return;
        int resolved_group = -1;
        if (resolve_pointer_origin(expr, resolved_group)) {
            constructed_pointers_.insert(name);
            alias_group_[name] = resolved_group >= 0
                ? resolved_group : next_alias_group_++;
            return;
        }
        if (auto* prim = dynamic_cast<const PrimaryExpression*>(expr)) {
            switch (prim->kind) {
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

    bool LifecycleLowering::resolve_pointer_origin(const Expression* expr, int& group) {
        group = -1;
        if (expr == nullptr) return false;
        if (auto* prim = dynamic_cast<const PrimaryExpression*>(expr)) {
            switch (prim->kind) {
            case PrimaryExpression::Kind::Construct:
            case PrimaryExpression::Kind::PlacementConstruct:
                return true;
            case PrimaryExpression::Kind::Identifier: {
                auto found = alias_group_.find(prim->identifier);
                if (found != alias_group_.end()) {
                    group = found->second;
                    return true;
                }
                return false;
            }
            case PrimaryExpression::Kind::CopyMove:
                return prim->paren_expr != nullptr
                    ? resolve_pointer_origin(prim->paren_expr.get(), group)
                    : false;
            default:
                return false;
            }
        }
        if (auto* call = dynamic_cast<const PostfixExpression*>(expr)) {
            if (call->op != PostfixExpression::Operator::FunctionCall) return false;
            auto* callee = dynamic_cast<const PrimaryExpression*>(call->base.get());
            if (callee == nullptr ||
                callee->kind != PrimaryExpression::Kind::Identifier) {
                return false;
            }
            auto construct_it = function_returns_construct_.find(callee->identifier);
            if (construct_it != function_returns_construct_.end() &&
                construct_it->second) {
                return true;
            }
            auto forward_it = function_forwards_parameter_.find(callee->identifier);
            if (forward_it != function_forwards_parameter_.end() &&
                forward_it->second >= 0 &&
                static_cast<std::size_t>(forward_it->second) <
                    call->arguments.size()) {
                return resolve_pointer_origin(
                    call->arguments[forward_it->second].get(), group);
            }
        }
        return false;
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
            if (!current_function_name_.empty() && rs->value != nullptr) {
                bool returns_construct = false;
                int forwards_parameter = -1;
                if (auto* prim = dynamic_cast<PrimaryExpression*>(rs->value.get())) {
                    if (prim->kind == PrimaryExpression::Kind::Construct ||
                        prim->kind == PrimaryExpression::Kind::PlacementConstruct) {
                        returns_construct = true;
                    }
                    else if (prim->kind == PrimaryExpression::Kind::Identifier &&
                        constructed_pointers_.count(prim->identifier) != 0) {
                        returns_construct = true;
                    }
                    else if (prim->kind == PrimaryExpression::Kind::Identifier) {
                        for (std::size_t i = 0;
                            i < current_function_parameters_.size(); ++i) {
                            if (current_function_parameters_[i] == prim->identifier) {
                                forwards_parameter = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                    else if (prim->kind == PrimaryExpression::Kind::CopyMove &&
                        prim->paren_expr != nullptr) {
                        auto* inner = dynamic_cast<PrimaryExpression*>(
                            prim->paren_expr.get());
                        if (inner != nullptr &&
                            inner->kind == PrimaryExpression::Kind::Identifier &&
                            constructed_pointers_.count(inner->identifier) != 0) {
                            returns_construct = true;
                        }
                    }
                }
                else if (auto* call = dynamic_cast<PostfixExpression*>(rs->value.get())) {
                    if (call->op == PostfixExpression::Operator::FunctionCall) {
                        auto* callee = dynamic_cast<PrimaryExpression*>(
                            call->base.get());
                        if (callee != nullptr &&
                            callee->kind == PrimaryExpression::Kind::Identifier) {
                            auto found = function_returns_construct_.find(
                                callee->identifier);
                            returns_construct =
                                found != function_returns_construct_.end() &&
                                found->second;
                        }
                    }
                }
                function_returns_construct_[current_function_name_] =
                    returns_construct;
                function_forwards_parameter_[current_function_name_] =
                    forwards_parameter;
            }
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
            current_function_name_ = func->name;
            current_function_parameters_ = func->param_names;
            if (func->body) rewrite_statement(func->body.get());
            current_function_name_.clear();
            current_function_parameters_.clear();
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

}
