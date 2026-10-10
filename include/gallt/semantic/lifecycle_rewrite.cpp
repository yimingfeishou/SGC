#include "lifecycle.hpp"
#include "lifecycle_detail.hpp"
#include "../parser/ast_visitor.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <functional>

using namespace gallt::AST;

namespace gallt {
    using namespace lifecycle_detail;

namespace {

    class MemberReferenceRewriter : public AstRewriter {
    public:
        MemberReferenceRewriter(const std::unordered_set<std::string>& members,
            const std::unordered_set<std::string>& locals)
            : members_(members), shadowed_(locals) {
        }

    protected:
        bool EnterExpression(std::unique_ptr<Expression>& node) override {
            auto* prim = dynamic_cast<PrimaryExpression*>(node.get());

            if (prim != nullptr &&
                prim->kind == PrimaryExpression::Kind::Identifier &&
                members_.count(prim->identifier) != 0 &&
                shadowed_.count(prim->identifier) == 0) {
                auto base = std::make_unique<PrimaryExpression>(prim->location,
                    "__this");
                auto arrow = std::make_unique<PostfixExpression>(prim->location,
                    std::move(base), PostfixExpression::Operator::Arrow);
                arrow->member_name = prim->identifier;
                node = std::move(arrow);
                return false;
            }

            return true;
        }

        bool EnterPrimaryExpression(PrimaryExpression* node) override {
            switch (node->kind) {
            case PrimaryExpression::Kind::Parens:
                rewrite_expression(node->paren_expr);
                return false;
            case PrimaryExpression::Kind::Heap:
                rewrite_expression(node->heap_size);
                return false;
            case PrimaryExpression::Kind::Construct:
            case PrimaryExpression::Kind::PlacementConstruct:
                for (std::unique_ptr<Expression>& argument :
                    node->construct_args) {
                    rewrite_expression(argument);
                }
                rewrite_expression(node->placement_target);
                return false;
            case PrimaryExpression::Kind::CopyMove:
                rewrite_expression(node->paren_expr);
                return false;
            default:
                return false;
            }
        }

        bool EnterCompileTimePropertyExpression(
            CompileTimePropertyExpression*) override {
            return false;
        }

        bool EnterBlock(Block*) override {
            marks_.push_back(introduced_.size());
            return true;
        }

        void LeaveBlock(Block*) override { leave_scope(); }

        bool EnterVariableDeclaration(VariableDeclaration* node) override {
            rewrite_initializer(node->initializer);
            return false;
        }

        bool EnterForStatement(ForStatement*) override {
            marks_.push_back(introduced_.size());
            return true;
        }

        void LeaveForStatement(ForStatement*) override { leave_scope(); }

        bool EnterSwitchCaseStatement(SwitchCaseStatement*) override {
            marks_.push_back(introduced_.size());
            return true;
        }

        void LeaveSwitchCaseStatement(SwitchCaseStatement*) override { leave_scope(); }

        void LeaveVariableDeclaration(VariableDeclaration* node) override {
            declare_local(node->name);
        }

        bool EnterStructDefinition(StructDefinition*) override { return false; }

        bool EnterGenericDefinition(GenericDefinition*) override { return false; }

        bool EnterEmitStatement(EmitStatement*) override { return false; }

        bool EnterType(Type&) override { return false; }

    private:
        void declare_local(const std::string& name) {
            if (name.empty()) { return; }
            if (shadowed_.insert(name).second) { introduced_.push_back(name); }
        }

        void leave_scope() {
            if (marks_.empty()) { return; }
            const std::size_t mark = marks_.back();
            marks_.pop_back();

            while (introduced_.size() > mark) {
                shadowed_.erase(introduced_.back());
                introduced_.pop_back();
            }
        }

        const std::unordered_set<std::string>& members_;
        std::unordered_set<std::string> shadowed_;
        std::vector<std::string> introduced_;
        std::vector<std::size_t> marks_;
    };

}

    void LifecycleLowering::rewrite_member_references(Statement* stmt,
        const std::unordered_set<std::string>& members,
        const std::unordered_set<std::string>& locals) {
        if (stmt == nullptr) {
            return;
        }

        MemberReferenceRewriter rewriter(members, locals);
        std::unique_ptr<Statement> holder(stmt);
        rewriter.rewrite_statement(holder);
        holder.release();
    }

    std::unique_ptr<FunctionDefinition> LifecycleLowering::lower_special_member(
        StructDefinition* def, SpecialMemberFunction* member, const std::string& name) {
        std::unordered_set<std::string> members;
        for (const auto& m : def->members) { members.insert(m.name); }
        std::unordered_set<std::string> locals;

        for (const auto& pname : member->parameter_names) {
            if (!pname.empty()) { locals.insert(pname); }
        }

        if (!member->parameter_name.empty()) { locals.insert(member->parameter_name); }
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
        } else if (member->kind != SpecialMemberFunction::Kind::Destructor) {
            const Type& declared = member->parameter_type;
            const bool declared_is_object_pointer = declared.kind == TypeKind::Pointer &&
                declared.pointee_type && declared.pointee_type->kind == TypeKind::Struct &&
                declared.pointee_type->struct_name == def->name;
            const bool keep_declared = !declared_is_object_pointer &&
                declared.kind != TypeKind::Void && declared.kind != TypeKind::Error;

            params.push_back(keep_declared ? declared : Type::make_pointer(
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
        std::unordered_map<const AST::TopLevel*,
            std::vector<std::unique_ptr<TopLevel>>> generated;
        std::vector<std::unique_ptr<TopLevel>> orphaned;

        for (AST::StructDefinition* ordered : struct_order_) {
            auto found = structs_.find(ordered->name);
            if (found == structs_.end()) { continue; }
            StructInfo& info = found->second;
            StructDefinition* def = info.def;
            std::vector<std::unique_ptr<TopLevel>>* bucket = nullptr;

            if (info.anchor != nullptr) {
                bucket = &generated[info.anchor];
            } else {
                bucket = &orphaned;
            }

            auto append = [&](std::unique_ptr<FunctionDefinition> func) {
                bucket->push_back(std::move(func));
            };

            for (std::size_t i = 0; i < info.constructors.size(); ++i) {
                auto func = lower_special_member(def, info.constructors[i],
                    info.ctor_names[i]);
                append(std::move(func));
            }

            if (info.destructor != nullptr) {
                auto func = lower_special_member(def, info.destructor, info.dtor_name);
                append(std::move(func));
            }

            if (info.copy_constructor != nullptr) {
                auto func = lower_special_member(def, info.copy_constructor,
                    info.copy_ctor_name);
                append(std::move(func));
            }

            if (info.move_constructor != nullptr) {
                auto func = lower_special_member(def, info.move_constructor,
                    info.move_ctor_name);
                append(std::move(func));
            }

            if (info.copy_assignment != nullptr) {
                auto func = lower_special_member(def, info.copy_assignment,
                    info.copy_assign_name);
                append(std::move(func));
            }

            if (info.move_assignment != nullptr) {
                auto func = lower_special_member(def, info.move_assignment,
                    info.move_assign_name);
                append(std::move(func));
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
        merged.reserve(program_->top_levels.size() + orphaned.size());

        for (auto& node : program_->top_levels) {
            AST::TopLevel* anchor = node.get();
            merged.push_back(std::move(node));

            auto found = generated.find(anchor);

            if (found == generated.end()) { continue; }

            for (auto& extra : found->second) {
                merged.push_back(std::move(extra));
            }
        }

        for (auto& extra : orphaned) {
            merged.push_back(std::move(extra));
        }

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
        if (arg_type.kind == TypeKind::Void) { return true; }
        if (param.kind == TypeKind::Pointer && arg_type.kind == TypeKind::Pointer) { return true; }
        if (param == arg_type) { return true; }
        if (param.is_floating() && arg_type.is_arithmetic()) { return true; }
        if (param.kind == TypeKind::Double && arg_type.kind == TypeKind::Float) { return true; }
        if (param.kind == TypeKind::Int && arg_type.is_integer()) { return true; }
        if (param.kind == TypeKind::Lint && arg_type.is_integer()) { return true; }
        if (param.kind == TypeKind::Uint && arg_type.is_integer()) { return true; }
        if (param.kind == TypeKind::Luint && arg_type.is_integer()) { return true; }
        if (param.kind == TypeKind::Uchar && arg_type.is_integer()) { return true; }
        if (param.kind == TypeKind::Bool && arg_type.is_integer()) { return true; }
        if (param.kind == TypeKind::Char && arg_type.is_integer()) { return true; }
        return false;
    }

    int LifecycleLowering::argument_rank(const AST::Type& param,
        const AST::Expression* arg) const {
        constexpr int kExact = 0;
        constexpr int kPromotion = 100;
        constexpr int kConversion = 200;
        AST::Type arg_type = infer_literal_type(arg);
        if (arg_type.kind == TypeKind::Void) { return kArgumentRankUnknown; }

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

        if (param == arg_type) { return kExact; }

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

            if (distance == 0) { distance = 1; }

            return (promotion ? kPromotion : kConversion) + distance;
        }

        if (!type_matches_argument(param, arg)) { return -1; }
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
                if (ctor->parameter_defaults[i - 1] != nullptr) {
                    required = i - 1;
                } else {
                    break;
                }
            }

            if (args.size() > ctor->parameters.size() || args.size() < required) { continue; }
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

            if (ok) { viable.push_back(std::move(candidate)); }
        }

        if (viable.empty()) { return nullptr; }
        auto better = [](const std::vector<int>& a, const std::vector<int>& b) {
            bool strictly_better = false;
            for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) {
                if (a[k] == kArgumentRankUnknown || b[k] == kArgumentRankUnknown) { continue; }
                if (a[k] > b[k]) { return false; }
                if (a[k] < b[k]) { strictly_better = true; }
            }
            return strictly_better;
        };
        std::size_t best = 0;
        std::size_t best_count = 0;
        bool best_has_unknown = false;

        for (std::size_t i = 0; i < viable.size(); ++i) {
            bool is_best = true;

            for (std::size_t j = 0; j < viable.size(); ++j) {
                if (i == j) { continue; }

                if (better(viable[j].ranks, viable[i].ranks)) {
                    is_best = false;
                    break;
                }
            }

            if (is_best) {
                best = i;
                ++best_count;

                for (int rank : viable[i].ranks) {
                    if (rank == kArgumentRankUnknown) { best_has_unknown = true; }
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
        if (decl == nullptr) { return; }
        if (auto* expr_init = dynamic_cast<ExpressionInitializer*>(decl->initializer.get())) {
            rewrite_expression(expr_init->expr.get());
        }

        if (decl->type.kind != TypeKind::Struct) { return; }
        auto info_it = structs_.find(decl->type.struct_name);
        if (info_it == structs_.end()) { return; }
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

            if (extra != nullptr) { call->arguments.push_back(std::move(extra)); }
            decl->initializer.reset();
            decl->constructed_by_lowering = true;
            insert_after.push_back(std::make_unique<ExpressionStatement>(loc,
                std::move(call)));
        };

        auto* expr_init2 = dynamic_cast<ExpressionInitializer*>(decl->initializer.get());
        if (expr_init2 == nullptr) {
            auto* brace = dynamic_cast<ArrayInitializer*>(decl->initializer.get());
            const bool has_brace_elements = brace != nullptr && !brace->elements.empty();

            if (has_brace_elements) {
                if (!info.constructors.empty()) {
                    std::vector<Expression*> args;
                    bool all_expressions = true;

                    for (auto& element : brace->elements) {
                        auto* expression =
                            dynamic_cast<ExpressionInitializer*>(element.get());

                        if (expression == nullptr) {
                            all_expressions = false;
                            break;
                        }

                        args.push_back(expression->expr.get());
                    }

                    if (all_expressions) {
                        bool ambiguous = false;
                        SpecialMemberFunction* ctor =
                            select_constructor(info, args, &ambiguous);

                        if (ambiguous) {
                            report_template(decl->location,
                                ErrorCode::SpecialMemberAmbiguous, { struct_name });
                            return;
                        }

                        if (ctor != nullptr) {
                            std::size_t index = static_cast<std::size_t>(
                                std::find(info.constructors.begin(),
                                    info.constructors.end(), ctor)
                                - info.constructors.begin());
                            auto call = std::make_unique<PostfixExpression>(
                                decl->location,
                                std::make_unique<PrimaryExpression>(decl->location,
                                    info.ctor_names[index]),
                                PostfixExpression::Operator::FunctionCall);
                            call->arguments.push_back(make_self_address());

                            for (auto& element : brace->elements) {
                                auto* expression =
                                    static_cast<ExpressionInitializer*>(element.get());
                                call->arguments.push_back(std::move(expression->expr));
                            }

                            decl->initializer.reset();
                            decl->constructed_by_lowering = true;
                            insert_after.push_back(std::make_unique<ExpressionStatement>(
                                decl->location, std::move(call)));
                            return;
                        }
                    }
                }

                return;
            }

            if (!info.constructors.empty()) {
                for (std::size_t i = 0; i < info.constructors.size(); ++i) {
                    std::size_t required = info.constructors[i]->parameters.size();

                    for (std::size_t k = info.constructors[i]->parameter_defaults.size(); k > 0; --k) {
                        if (info.constructors[i]->parameter_defaults[k - 1] != nullptr) {
                            required = k - 1;
                        } else {
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

        if (call == nullptr || call->op != PostfixExpression::Operator::FunctionCall) { return; }
        auto* callee = dynamic_cast<PrimaryExpression*>(call->base.get());
        if (callee == nullptr || callee->kind != PrimaryExpression::Kind::Identifier) { return; }

        bool is_type_construction = (callee->identifier == struct_name);
        bool is_copy = (callee->identifier == "copy");
        bool is_move = (callee->identifier == "move");
        bool is_deep = (callee->identifier == "deep_copy");
        bool is_shallow = (callee->identifier == "shallow_copy");
        if (!is_type_construction && !is_copy && !is_move && !is_deep && !is_shallow) { return; }

        if (is_type_construction) {
            bool ambiguous = false;
            std::vector<AST::Expression*> args;

            for (auto& a : call->arguments) { args.push_back(a.get()); }

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

            for (auto& a : call->arguments) { call_stmt->arguments.push_back(std::move(a)); }

            const SourceLocation call_location = call->location;
            decl->initializer.reset();
            decl->constructed_by_lowering = true;
            insert_after.push_back(std::make_unique<ExpressionStatement>(call_location,
                std::move(call_stmt)));
            return;
        }

        if (call->arguments.empty()) { return; }
        std::unique_ptr<Expression> source = std::move(call->arguments[0]);
        const std::string* special = nullptr;
        if (is_move && info.move_constructor != nullptr) { special = &info.move_ctor_name; }
        if (is_copy && info.copy_constructor != nullptr) { special = &info.copy_ctor_name; }

        if (special != nullptr) {
            emit_self_call(*special, std::move(source), call->location);
            return;
        }
        expr_init2->expr = std::move(source);
    }

    class LifecycleLowering::ExpressionRewriter : public AstRewriter {
    public:
        explicit ExpressionRewriter(LifecycleLowering& owner) : owner_(owner) {
        }

    protected:
        bool EnterPrimaryExpression(PrimaryExpression* node) override {
            if (node->kind != PrimaryExpression::Kind::Construct &&
                node->kind != PrimaryExpression::Kind::PlacementConstruct) {
                return true;
            }

            const std::string& struct_name = node->construct_type.struct_name;
            auto found = owner_.structs_.find(struct_name);

            if (node->construct_type.kind != TypeKind::Struct) {
                owner_.report_template(node->location,
                    ErrorCode::SpecialMemberOnNonStruct,
                    { node->construct_type.to_string() });
                return false;
            }

            if (found == owner_.structs_.end()) {
                return false;
            }

            StructInfo& info = found->second;
            bool ambiguous = false;
            std::vector<Expression*> args;

            for (std::unique_ptr<Expression>& argument : node->construct_args) {
                args.push_back(argument.get());
            }

            SpecialMemberFunction* ctor =
                owner_.select_constructor(info, args, &ambiguous);

            if (ambiguous) {
                owner_.report_template(node->location,
                    ErrorCode::SpecialMemberAmbiguous, { struct_name });
                return false;
            }

            if (ctor != nullptr) {
                std::size_t index = static_cast<std::size_t>(
                    std::find(info.constructors.begin(), info.constructors.end(),
                        ctor) - info.constructors.begin());
                node->lowered_ctor = info.ctor_names[index];
            } else if (info.constructors.empty() &&
                !node->construct_args.empty()) {
                owner_.report_template(node->location,
                    ErrorCode::FunctionArgCountMismatch,
                    { "0", std::to_string(node->construct_args.size()) });
            }

            if (node->kind == PrimaryExpression::Kind::PlacementConstruct) {
                auto* target = dynamic_cast<PrimaryExpression*>(
                    node->placement_target.get());

                if (target != nullptr &&
                    target->kind == PrimaryExpression::Kind::Null) {
                    owner_.report_template(node->location,
                        ErrorCode::PlacementTargetInvalid,
                        std::vector<std::string>{});
                }
            }

            return false;
        }

        bool EnterPostfixExpression(PostfixExpression* node) override {
            if (node->op == PostfixExpression::Operator::Dot ||
                node->op == PostfixExpression::Operator::Arrow) {
                static const std::unordered_set<std::string> kSpecials = {
                    "constructor", "copy_constructor", "move_constructor",
                    "copy_assignment", "move_assignment",
                };

                if (kSpecials.count(node->member_name) != 0) {
                    owner_.report_template(node->location,
                        ErrorCode::SpecialMemberDirectCall,
                        { node->member_name });
                }
            }

            return true;
        }

        bool EnterCompileTimePropertyExpression(
            CompileTimePropertyExpression*) override {
            return false;
        }

        bool EnterType(AST::Type&) override { return false; }

        bool EnterSharedExpression(std::shared_ptr<Expression>&) override {
            return false;
        }

    private:
        LifecycleLowering& owner_;
    };

    void LifecycleLowering::rewrite_expression(AST::Expression* expr) {
        if (expr == nullptr) { return; }
        if (poisoned_node(expr)) { return; }
        PoisonGuard guard(*this, expr);

        std::unique_ptr<Expression> holder(expr);
        ExpressionRewriter rewriter(*this);
        rewriter.rewrite_expression(holder);
        holder.release();
    }

    void LifecycleLowering::track_pointer_source(const std::string& name,
        const Expression* expr) {
        if (expr == nullptr) { return; }
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
        if (expr == nullptr) { return false; }

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
            if (call->op != PostfixExpression::Operator::FunctionCall) { return false; }
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

    class LifecycleLowering::StatementRewriter : public AstRewriter {
    public:
        explicit StatementRewriter(LifecycleLowering& owner) : owner_(owner) {
        }

    protected:
        bool EnterTopLevel(std::unique_ptr<TopLevel>&) override {
            ++top_level_entry_;
            return true;
        }

        void LeaveTopLevel(std::unique_ptr<TopLevel>&) override {
            --top_level_entry_;
        }

        bool EnterBlock(Block* node) override {
            std::vector<std::unique_ptr<Statement>> rewritten;

            for (std::unique_ptr<Statement>& child : node->statements) {
                std::vector<std::unique_ptr<Statement>> insert_after;

                if (auto* vd = dynamic_cast<VariableDeclaration*>(child.get())) {
                    owner_.local_types_[vd->name] = vd->type;

                    if (vd->type.kind == TypeKind::Pointer) {
                        if (auto* expr_init = dynamic_cast<ExpressionInitializer*>(
                            vd->initializer.get())) {
                            owner_.track_pointer_source(vd->name,
                                expr_init->expr.get());
                        }
                    }

                    owner_.rewrite_declaration(vd, insert_after);
                } else {
                    owner_.rewrite_statement(child.get());
                }

                rewritten.push_back(std::move(child));

                for (std::unique_ptr<Statement>& extra : insert_after) {
                    rewritten.push_back(std::move(extra));
                }
            }

            node->statements = std::move(rewritten);
            return false;
        }

        bool EnterVariableDeclaration(VariableDeclaration* node) override {
            std::vector<std::unique_ptr<Statement>> ignored;
            owner_.rewrite_declaration(node, ignored);
            return false;
        }

        bool EnterIfStatement(IfStatement* node) override {
            owner_.rewrite_expression(node->condition.get());
            owner_.rewrite_statement(node->then_block.get());
            owner_.rewrite_statement(node->else_block.get());
            return false;
        }

        bool EnterForStatement(ForStatement* node) override {
            owner_.rewrite_statement(node->init.get());
            owner_.rewrite_expression(node->condition.get());
            owner_.rewrite_expression(node->step.get());
            owner_.rewrite_statement(node->body.get());
            return false;
        }

        bool EnterWhileStatement(WhileStatement* node) override {
            owner_.rewrite_expression(node->condition.get());
            owner_.rewrite_statement(node->body.get());
            return false;
        }

        bool EnterSwitchCaseStatement(SwitchCaseStatement* node) override {
            owner_.rewrite_expression(node->condition.get());

            for (SwitchCaseStatement::Clause& clause : node->clauses) {
                owner_.rewrite_expression(clause.condition.get());
                std::vector<std::unique_ptr<Statement>> rewritten;

                for (std::unique_ptr<Statement>& child : clause.statements) {
                    std::vector<std::unique_ptr<Statement>> insert_after;

                    if (auto* vd = dynamic_cast<VariableDeclaration*>(child.get())) {
                        owner_.local_types_[vd->name] = vd->type;

                        if (vd->type.kind == TypeKind::Pointer) {
                            if (auto* expr_init = dynamic_cast<ExpressionInitializer*>(
                                vd->initializer.get())) {
                                owner_.track_pointer_source(vd->name,
                                    expr_init->expr.get());
                            }
                        }

                        owner_.rewrite_declaration(vd, insert_after);
                    } else {
                        owner_.rewrite_statement(child.get());
                    }

                    rewritten.push_back(std::move(child));

                    for (std::unique_ptr<Statement>& extra : insert_after) {
                        rewritten.push_back(std::move(extra));
                    }
                }

                clause.statements = std::move(rewritten);
            }

            return false;
        }

        bool EnterReturnStatement(ReturnStatement* node) override {
            owner_.rewrite_expression(node->value.get());

            if (!owner_.current_function_name_.empty() && node->value != nullptr) {
                bool returns_construct = false;
                int forwards_parameter = -1;

                if (auto* prim = dynamic_cast<PrimaryExpression*>(node->value.get())) {
                    if (prim->kind == PrimaryExpression::Kind::Construct ||
                        prim->kind == PrimaryExpression::Kind::PlacementConstruct) {
                        returns_construct = true;
                    } else if (prim->kind == PrimaryExpression::Kind::Identifier &&
                        owner_.constructed_pointers_.count(prim->identifier) != 0) {
                        returns_construct = true;
                    } else if (prim->kind == PrimaryExpression::Kind::Identifier) {
                        for (std::size_t i = 0;
                            i < owner_.current_function_parameters_.size(); ++i) {
                            if (owner_.current_function_parameters_[i] ==
                                prim->identifier) {
                                forwards_parameter = static_cast<int>(i);
                                break;
                            }
                        }
                    } else if (prim->kind == PrimaryExpression::Kind::CopyMove &&
                        prim->paren_expr != nullptr) {
                        auto* inner = dynamic_cast<PrimaryExpression*>(
                            prim->paren_expr.get());

                        if (inner != nullptr &&
                            inner->kind == PrimaryExpression::Kind::Identifier &&
                            owner_.constructed_pointers_.count(inner->identifier) != 0) {
                            returns_construct = true;
                        }
                    }
                } else if (auto* call = dynamic_cast<PostfixExpression*>(
                    node->value.get())) {
                    if (call->op == PostfixExpression::Operator::FunctionCall) {
                        auto* callee = dynamic_cast<PrimaryExpression*>(
                            call->base.get());

                        if (callee != nullptr &&
                            callee->kind == PrimaryExpression::Kind::Identifier) {
                            auto found = owner_.function_returns_construct_.find(
                                callee->identifier);
                            returns_construct =
                                found != owner_.function_returns_construct_.end() &&
                                found->second;
                        }
                    }
                }

                owner_.function_returns_construct_[owner_.current_function_name_] =
                    returns_construct;
                owner_.function_forwards_parameter_[owner_.current_function_name_] =
                    forwards_parameter;
            }

            return false;
        }

        bool EnterExpressionStatement(ExpressionStatement* node) override {
            if (auto* assign = dynamic_cast<AssignmentExpression*>(
                node->expr.get())) {
                if (assign->op == AssignmentExpression::Operator::Assign) {
                    auto* left_id = dynamic_cast<PrimaryExpression*>(
                        assign->left.get());

                    if (left_id != nullptr &&
                        left_id->kind == PrimaryExpression::Kind::Identifier) {
                        auto type_it = owner_.local_types_.find(left_id->identifier);

                        if (type_it != owner_.local_types_.end() &&
                            type_it->second.kind == TypeKind::Pointer) {
                            owner_.track_pointer_source(left_id->identifier,
                                assign->right.get());
                        }
                    }
                }
            }

            owner_.rewrite_expression(node->expr.get());
            return false;
        }

        bool EnterDestructStatement(DestructStatement* node) override {
            owner_.rewrite_expression(node->target.get());

            if (auto* prim = dynamic_cast<PrimaryExpression*>(node->target.get())) {
                if (prim->kind == PrimaryExpression::Kind::Identifier) {
                    const std::string& name = prim->identifier;

                    if (owner_.null_pointers_.count(name) != 0) {
                    } else if (owner_.non_construct_pointers_.count(name) != 0) {
                        owner_.report_template(node->location,
                            ErrorCode::DestructNonConstructed, {});
                    } else if (owner_.constructed_pointers_.count(name) != 0) {
                        auto group = owner_.alias_group_.find(name);

                        if (group != owner_.alias_group_.end()) {
                            if (owner_.destructed_groups_.count(group->second) != 0) {
                                owner_.report_template(node->location,
                                    ErrorCode::DestructTwice, {});
                            } else {
                                owner_.destructed_groups_.insert(group->second);
                            }
                        }
                    }
                } else if (prim->kind == PrimaryExpression::Kind::Null) {
                } else {
                    owner_.report_template(node->location,
                        ErrorCode::DestructNonConstructed, {});
                }
            } else {
                owner_.report_template(node->location,
                    ErrorCode::DestructNonConstructed, {});
            }

            return false;
        }

        bool EnterStructDefinition(StructDefinition* node) override {
            for (StructDefinition::Member& member : node->members) {
                if (auto* init = dynamic_cast<ExpressionInitializer*>(
                    member.initializer.get())) {
                    owner_.rewrite_expression(init->expr.get());
                }
            }

            return false;
        }

        bool EnterFunctionDefinition(FunctionDefinition* node) override {
            if (top_level_entry_ == 0) {
                return false;
            }

            owner_.current_function_name_ = node->name;
            owner_.current_function_parameters_ = node->param_names;

            if (node->body) {
                owner_.rewrite_statement(node->body.get());
            }

            owner_.current_function_name_.clear();
            owner_.current_function_parameters_.clear();
            return false;
        }

        bool EnterExternDeclaration(ExternDeclaration*) override { return false; }

        bool EnterEmitStatement(EmitStatement*) override { return false; }

        bool EnterGenericDefinition(GenericDefinition*) override { return false; }

        bool EnterInstantiationStatement(InstantiationStatement*) override {
            return false;
        }

        bool EnterNamespaceDefinition(NamespaceDefinition*) override { return false; }

        bool EnterAdditionNamespaceStatement(AdditionNamespaceStatement*) override {
            return false;
        }

        bool EnterCondDefinition(CondDefinition*) override { return false; }

        bool EnterConditionalBlock(ConditionalBlock*) override { return false; }

        bool EnterTopLevelBlock(TopLevelBlock*) override { return false; }

    private:
        LifecycleLowering& owner_;
        int top_level_entry_ = 0;
    };

    void LifecycleLowering::rewrite_statement(Statement* stmt) {
        if (stmt == nullptr) { return; }
        if (poisoned_node(stmt)) { return; }
        PoisonGuard guard(*this, stmt);

        std::unique_ptr<Statement> holder(stmt);
        StatementRewriter rewriter(*this);
        rewriter.rewrite_statement(holder);
        holder.release();
    }

    void LifecycleLowering::rewrite_top_level(TopLevel* node) {
        if (node == nullptr) { return; }
        if (poisoned_node(node)) { return; }
        PoisonGuard guard(*this, node);

        if (auto* vd = dynamic_cast<VariableDeclaration*>(node)) {
            std::vector<std::unique_ptr<Statement>> initializers;
            rewrite_declaration(vd, initializers);

            for (std::unique_ptr<Statement>& stmt : initializers) {
                program_->global_initializers.push_back(std::move(stmt));
            }

            return;
        }

        std::unique_ptr<TopLevel> holder(node);
        StatementRewriter rewriter(*this);
        rewriter.rewrite_top_level(holder);
        holder.release();
    }

}
