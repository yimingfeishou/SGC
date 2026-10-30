#include "generic_expander.hpp"
#include "generic_expander_detail.hpp"
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
    using namespace generic_expander_detail;

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

            if (evaluate_constant_with_members(member.array_size_expr.get(), sub, value)) {
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
                if (auto* expr_init =
                        dynamic_cast<const ExpressionInitializer*>(element.get())) {
                    auto* post = dynamic_cast<const PostfixExpression*>(
                        expr_init->expr.get());
                    std::string pack_name;

                    if (post != nullptr &&
                        post->op == PostfixExpression::Operator::PackExpand &&
                        pack_identifier_name(post->base.get(), pack_name)) {
                        auto const_pack = sub.const_packs.find(pack_name);

                        if (const_pack != sub.const_packs.end()) {
                            for (const ConstantValue& value : const_pack->second) {
                                elements.push_back(
                                    std::make_unique<ExpressionInitializer>(
                                        element->location,
                                        make_constant_literal(element->location,
                                            value)));
                            }

                            continue;
                        }

                        if (sub.type_packs.count(pack_name) != 0) {
                            report(element->location,
                                ErrorCode::ParameterPackPropertyNotApplicable,
                                std::vector<std::string>{ "value", pack_name });
                            continue;
                        }
                    }
                }

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
        } else if (value.is_float) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%g", value.float_value);
            text = buffer;
            type = TokenType::FloatLiteral;
        } else {
            text = std::to_string(value.int_value);
        }
        lexeme_pool_.push_back(text);
        Token literal(type, loc, lexeme_pool_.back());
        return std::make_unique<PrimaryExpression>(loc, literal);
    }

    std::unique_ptr<Expression> GenericExpander::clone_expression(
        const Expression* expr, const Substitution& sub) {
        if (expr == nullptr) { return nullptr; }
        if (auto* e = dynamic_cast<const PostfixExpression*>(expr)) {
            if (auto rewritten = rewrite_pack_expression(e, sub)) {
                return rewritten;
            }
            if (auto rewritten = rewrite_property_expression(e, sub)) {
                return rewritten;
            }
        }
        if (auto* e = dynamic_cast<const CompileTimePropertyExpression*>(expr)) {
            std::vector<const Expression*> args;
            for (const auto& arg : e->arguments) { args.push_back(arg.get()); }
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
            std::unique_ptr<Expression> left =
                clone_expression(e->left.get(), sub);
            std::unique_ptr<Expression> right =
                clone_expression(e->right.get(), sub);
            return std::make_unique<AssignmentExpression>(e->location,
                std::move(left), e->op, std::move(right));
        }
        if (auto* e = dynamic_cast<const LogicalOrExpression*>(expr)) {
            std::unique_ptr<Expression> left =
                clone_expression(e->left.get(), sub);
            std::unique_ptr<Expression> right =
                clone_expression(e->right.get(), sub);
            return std::make_unique<LogicalOrExpression>(e->location,
                std::move(left), std::move(right));
        }
        if (auto* e = dynamic_cast<const LogicalAndExpression*>(expr)) {
            std::unique_ptr<Expression> left =
                clone_expression(e->left.get(), sub);
            std::unique_ptr<Expression> right =
                clone_expression(e->right.get(), sub);
            return std::make_unique<LogicalAndExpression>(e->location,
                std::move(left), std::move(right));
        }
        if (auto* e = dynamic_cast<const ComparisonExpression*>(expr)) {
            std::unique_ptr<Expression> left =
                clone_expression(e->left.get(), sub);
            std::unique_ptr<Expression> right =
                clone_expression(e->right.get(), sub);
            return std::make_unique<ComparisonExpression>(e->location,
                std::move(left), e->op, std::move(right));
        }
        if (auto* e = dynamic_cast<const AdditiveExpression*>(expr)) {
            std::unique_ptr<Expression> left =
                clone_expression(e->left.get(), sub);
            std::unique_ptr<Expression> right =
                clone_expression(e->right.get(), sub);
            return std::make_unique<AdditiveExpression>(e->location,
                std::move(left), e->op, std::move(right));
        }
        if (auto* e = dynamic_cast<const MultiplicativeExpression*>(expr)) {
            std::unique_ptr<Expression> left =
                clone_expression(e->left.get(), sub);
            std::unique_ptr<Expression> right =
                clone_expression(e->right.get(), sub);
            return std::make_unique<MultiplicativeExpression>(e->location,
                std::move(left), e->op, std::move(right));
        }
        if (auto* e = dynamic_cast<const PowerExpression*>(expr)) {
            std::unique_ptr<Expression> left =
                clone_expression(e->left.get(), sub);
            std::unique_ptr<Expression> right =
                clone_expression(e->right.get(), sub);
            return std::make_unique<PowerExpression>(e->location,
                std::move(left), std::move(right));
        }
        if (auto* e = dynamic_cast<const BitwiseExpression*>(expr)) {
            std::unique_ptr<Expression> left =
                clone_expression(e->left.get(), sub);
            std::unique_ptr<Expression> right =
                clone_expression(e->right.get(), sub);
            return std::make_unique<BitwiseExpression>(e->location,
                std::move(left), e->op, std::move(right));
        }
        if (auto* e = dynamic_cast<const ShiftExpression*>(expr)) {
            std::unique_ptr<Expression> left =
                clone_expression(e->left.get(), sub);
            std::unique_ptr<Expression> right =
                clone_expression(e->right.get(), sub);
            return std::make_unique<ShiftExpression>(e->location,
                std::move(left), e->op, std::move(right));
        }
        if (auto* e = dynamic_cast<const ConditionalExpression*>(expr)) {
            std::unique_ptr<Expression> condition =
                clone_expression(e->condition.get(), sub);
            std::unique_ptr<Expression> then_expr =
                clone_expression(e->then_expr.get(), sub);
            std::unique_ptr<Expression> else_expr =
                clone_expression(e->else_expr.get(), sub);
            return std::make_unique<ConditionalExpression>(e->location,
                std::move(condition), std::move(then_expr),
                std::move(else_expr));
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
                if (is_pack_name(sub, e->identifier) ||
                    sub.fixed_pack_members.count(e->identifier) != 0) {
                    report(e->location, ErrorCode::ParameterPackUsedAsValue,
                        std::vector<std::string>{ e->identifier });
                    return make_constant_literal(e->location,
                        ConstantValue{ 0LL, 0.0, false, false, std::string() });
                }
                if (sub.locals.count(e->identifier) != 0) {
                    return std::make_unique<PrimaryExpression>(
                        e->location, e->identifier);
                }
                auto found = sub.types.find(e->identifier);
                if (found != sub.types.end()) {
                    return std::make_unique<PrimaryExpression>(e->location,
                        found->second.to_source_string());
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
                    if (!defer_instantiation_dependency(dependency)) {
                        ensure_instantiation(dependency, false);
                    }
                    return std::make_unique<PrimaryExpression>(e->location, member->second);
                }
                for (auto it = short_scopes_.rbegin(); it != short_scopes_.rend(); ++it) {
                    auto binding = it->find(e->identifier);
                    if (binding != it->end()) {
                        if (binding->second.ambiguous) {
                            report(e->location, ErrorCode::GenericShortNameAmbiguous,
                                std::vector<std::string>{ e->identifier });
                        }
                        return std::make_unique<PrimaryExpression>(e->location,
                            binding->second.target);
                    }
                }
                {
                    return std::make_unique<PrimaryExpression>(e->location,
                        e->identifier);
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
                    substitute_generic_argument(arg, sub);
                }
                expand_pack_arguments(ref.arguments, sub, e->location);
                if (defer_instantiation_dependency(ref)) {
                    return std::make_unique<PrimaryExpression>(e->location,
                        ref.mangle());
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
        return clone_statement_impl(stmt, sub);
    }

    std::unique_ptr<Statement> GenericExpander::clone_substatement(
        const Statement* stmt, const Substitution& sub) {
        if (stmt == nullptr) {
            return nullptr;
        }

        return clone_statement_impl(stmt, sub);
    }

    std::unique_ptr<Statement> GenericExpander::clone_statement_impl(
        const Statement* stmt, const Substitution& sub) {
        if (stmt == nullptr) { return nullptr; }

        if (auto* s = dynamic_cast<const StructDefinition*>(stmt)) {
            return clone_struct_definition(s, sub);
        }

        if (auto* s = dynamic_cast<const EmitStatement*>(stmt)) {
            std::vector<std::unique_ptr<Statement>> emitted;
            std::vector<const EmitStatement*> emits{ s };

            if (!expand_statement_emit_run(emits, sub, emitted)) {
                return std::make_unique<Block>(s->location,
                    std::vector<std::unique_ptr<Statement>>{});
            }

            return std::make_unique<Block>(s->location, std::move(emitted));
        }

        if (auto* s = dynamic_cast<const Block*>(stmt)) {
            std::vector<std::unique_ptr<Statement>> statements;
            Substitution block_sub = sub;

            for (std::size_t index = 0; index < s->statements.size();) {
                std::vector<const EmitStatement*> emits;

                while (index < s->statements.size() &&
                    dynamic_cast<const EmitStatement*>(s->statements[index].get()) != nullptr) {
                    emits.push_back(static_cast<const EmitStatement*>(
                        s->statements[index].get()));
                    ++index;
                }

                if (!emits.empty()) {
                    if (!expand_statement_emit_run(emits, sub, statements)) {
                        return std::make_unique<Block>(s->location,
                            std::vector<std::unique_ptr<Statement>>{});
                    }
                    continue;
                }

                if (auto* instantiation =
                    dynamic_cast<const InstantiationStatement*>(
                        s->statements[index].get())) {
                    GenericRef reference = instantiation->reference;
                    for (GenericArgument& argument : reference.arguments) {
                        substitute_generic_argument(argument, sub);
                    }
                    expand_pack_arguments(reference.arguments, sub,
                        instantiation->location);
                    ensure_instantiation(reference,
                        reference.member.empty());
                    ++index;
                    continue;
                }

                if (auto* declaration =
                    dynamic_cast<const VariableDeclaration*>(
                        s->statements[index].get())) {
                    block_sub.locals.insert(declaration->name);
                } else if (auto* definition =
                    dynamic_cast<const StructDefinition*>(
                        s->statements[index].get())) {
                    block_sub.locals.insert(definition->name);
                }

                statements.push_back(
                    clone_substatement(s->statements[index].get(), block_sub));
                ++index;
            }

            return std::make_unique<Block>(s->location, std::move(statements));
        }

        if (auto* s = dynamic_cast<const VariableDeclaration*>(stmt)) {
            Type type = Type::make_void();
            substitute_type(s->type, sub, type);
            auto clone = std::make_unique<VariableDeclaration>(s->location, std::move(type),
                s->name, s->array_size, std::nullopt,
                s->initializer ? clone_initializer(s->initializer.get(), sub) : nullptr);
            if (s->function_pointer_type.has_value()) {
                Type fp = Type::make_void();
                substitute_type(*s->function_pointer_type, sub, fp);
                clone->function_pointer_type = fp;
            }

            if (s->array_size_expr) {
                ConstantValue value;

                if (evaluate_constant_with_members(s->array_size_expr.get(), sub, value)) {
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
            if (s->is_compile_time) {
                bool taken = false;
                const std::size_t condition_errors_before = diag_.error_count();

                if (!eval_compile_time_condition(s->condition.get(), sub, taken)) {
                    if (diag_.error_count() == condition_errors_before) {
                        report(s->location, ErrorCode::CompileTimeConditionNotBoolean,
                            std::vector<std::string>{ expression_text(s->condition.get()) });
                    }
                    return std::make_unique<Block>(s->location,
                        std::vector<std::unique_ptr<Statement>>{});
                }

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

            std::unique_ptr<Expression> condition =
                clone_expression(s->condition.get(), sub);
            std::unique_ptr<Statement> then_block =
                clone_substatement(s->then_block.get(), sub);
            std::unique_ptr<Statement> else_block =
                clone_substatement(s->else_block.get(), sub);
            return std::make_unique<IfStatement>(s->location,
                std::move(condition), std::move(then_block),
                std::move(else_block));
        }

        if (auto* s = dynamic_cast<const ForStatement*>(stmt)) {
            Substitution loop_sub = sub;
            if (auto* declaration =
                dynamic_cast<const VariableDeclaration*>(s->init.get())) {
                loop_sub.locals.insert(declaration->name);
            }
            std::unique_ptr<Statement> init =
                clone_substatement(s->init.get(), loop_sub);
            std::unique_ptr<Expression> condition =
                clone_expression(s->condition.get(), loop_sub);
            std::unique_ptr<Expression> step =
                clone_expression(s->step.get(), loop_sub);
            std::unique_ptr<Statement> body =
                clone_substatement(s->body.get(), loop_sub);
            return std::make_unique<ForStatement>(s->location,
                std::move(init), std::move(condition), std::move(step),
                std::move(body));
        }

        if (auto* s = dynamic_cast<const WhileStatement*>(stmt)) {
            std::unique_ptr<Expression> condition =
                clone_expression(s->condition.get(), sub);
            std::unique_ptr<Statement> body =
                clone_substatement(s->body.get(), sub);
            return std::make_unique<WhileStatement>(s->location,
                std::move(condition), std::move(body));
        }

        if (auto* s = dynamic_cast<const SwitchCaseStatement*>(stmt)) {
            auto clone = std::make_unique<SwitchCaseStatement>(s->location,
                clone_expression(s->condition.get(), sub));

            for (const SwitchCaseStatement::Clause& clause : s->clauses) {
                SwitchCaseStatement::Clause copied;
                Substitution clause_sub = sub;
                copied.is_default = clause.is_default;
                copied.condition = clone_expression(clause.condition.get(), sub);
                copied.location = clause.location;

                for (const std::unique_ptr<Statement>& child : clause.statements) {
                    if (auto* declaration =
                        dynamic_cast<const VariableDeclaration*>(child.get())) {
                        clause_sub.locals.insert(declaration->name);
                    } else if (auto* definition =
                        dynamic_cast<const StructDefinition*>(child.get())) {
                        clause_sub.locals.insert(definition->name);
                    }

                    copied.statements.push_back(
                        clone_substatement(child.get(), clause_sub));
                }

                clone->clauses.push_back(std::move(copied));
            }

            return clone;
        }

        if (auto* s = dynamic_cast<const BreakStatement*>(stmt)) {
            return std::make_unique<BreakStatement>(s->location);
        }

        if (auto* s = dynamic_cast<const ContinueStatement*>(stmt)) {
            return std::make_unique<ContinueStatement>(s->location);
        }

        if (auto* s = dynamic_cast<const FallthroughStatement*>(stmt)) {
            return std::make_unique<FallthroughStatement>(s->location);
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
            GenericRef reference = s->reference;

            for (GenericArgument& arg : reference.arguments) {
                substitute_generic_argument(arg, sub);
            }

            expand_pack_arguments(reference.arguments, sub, s->location);
            return std::make_unique<InstantiationStatement>(s->location, reference);
        }

        if (auto* s = dynamic_cast<const CondDefinition*>(stmt)) {
            return std::make_unique<CondDefinition>(s->location, s->name,
                clone_expression(s->value.get(), sub));
        }

        if (auto* s = dynamic_cast<const UncondDefinition*>(stmt)) {
            return std::make_unique<UncondDefinition>(s->location, s->name);
        }

        if (auto* s = dynamic_cast<const ConditionalBlock*>(stmt)) {
            std::unique_ptr<Expression> condition =
                clone_expression(s->condition.get(), sub);
            std::unique_ptr<Statement> then_block =
                clone_substatement(s->then_block.get(), sub);
            std::unique_ptr<Statement> else_block =
                clone_substatement(s->else_block.get(), sub);
            return std::make_unique<ConditionalBlock>(s->location,
                std::move(condition), std::move(then_block),
                std::move(else_block));
        }

        return nullptr;
    }

    std::unique_ptr<FunctionDefinition> GenericExpander::clone_function(
        const FunctionDefinition* func, const Substitution& sub, const std::string& name) {
        std::vector<std::string> param_names = func->param_names;
        Substitution body_sub = sub;
        register_local_struct_names(func->body.get(), body_sub);
        for (const std::string& name : param_names) {
            if (!name.empty()) { body_sub.locals.insert(name); }
        }
        Type ret = Type::make_void();
        substitute_type(func->return_type, sub, ret);
        std::vector<Type> params;
        bool variadic = func->is_variadic;
        bool expanded_pack = false;

        if (variadic && !func->parameters.empty()) {
            const Type& last = func->parameters.back();
            std::string pack_type_name;
            if (last.kind == TypeKind::Struct && !last.generic_ref) {
                pack_type_name = last.struct_name;
            }
            auto type_pack = sub.type_packs.find(pack_type_name);
            if (!pack_type_name.empty() && type_pack != sub.type_packs.end()) {
                const std::size_t limit = func->parameters.size() - 1;
                variadic = false;
                expanded_pack = true;
                const std::string base = param_names.size() ==
                        func->parameters.size()
                    ? param_names.back() : std::string();
                std::vector<std::string> members;
                members.reserve(type_pack->second.size());

                for (std::size_t i = 0; i < type_pack->second.size(); ++i) {
                    members.push_back(base + "$" + std::to_string(i + 1));
                }

                if (!base.empty()) {
                    body_sub.fixed_pack_members[base] = members;
                }

                for (const Type& p : func->parameters) {
                    Type substituted = Type::make_void();
                    substitute_type(p, sub, substituted);
                    params.push_back(std::move(substituted));
                }
                params.resize(limit);
                param_names.resize(limit);

                for (std::size_t i = 0; i < type_pack->second.size(); ++i) {
                    params.push_back(type_pack->second[i]);
                    param_names.push_back(members[i]);
                }
            }
        }

        if (!expanded_pack) {
            for (const Type& p : func->parameters) {
                Type substituted = Type::make_void();
                substitute_type(p, sub, substituted);
                params.push_back(std::move(substituted));
            }
        }

        auto clone = std::make_unique<FunctionDefinition>(func->location, std::move(ret), name,
            params, param_names, clone_statement(func->body.get(), body_sub));
        clone->is_variadic = variadic;
        clone->is_operator = func->is_operator;
        clone->overloaded_operator = func->overloaded_operator;
        clone->is_conversion_operator = func->is_conversion_operator;
        clone->is_constexpr_function = func->is_constexpr_function;
        clone->is_export = func->is_export;

        if (func->is_conversion_operator) {
            Type target = Type::make_void();
            substitute_type(func->conversion_target_type, sub, target);
            clone->conversion_target_type = std::move(target);
        } else {
            clone->conversion_target_type = func->conversion_target_type;
        }

        clone->param_defaults.reserve(func->param_defaults.size());

        for (const auto& default_value : func->param_defaults) {
            if (default_value == nullptr) {
                clone->param_defaults.push_back(nullptr);
            } else {
                clone->param_defaults.push_back(
                    clone_expression(default_value.get(), body_sub));
            }
        }

        return clone;
    }

    std::unique_ptr<SpecialMemberFunction> GenericExpander::clone_special_member(
        const SpecialMemberFunction* member, const Substitution& sub) {
        Substitution body_sub = sub;
        register_local_struct_names(member->body.get(), body_sub);
        if (!member->parameter_name.empty()) {
            body_sub.locals.insert(member->parameter_name);
        }
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
            } else {
                clone->parameter_defaults.push_back(clone_expression(default_value.get(), sub));
            }
        }

        Type source = Type::make_void();
        substitute_type(member->parameter_type, sub, source);
        clone->parameter_type = std::move(source);
        clone->parameter_name = member->parameter_name;
        clone->body = clone_statement(member->body.get(), body_sub);
        return clone;
    }

    std::unique_ptr<StructDefinition> GenericExpander::clone_struct_definition(
        const StructDefinition* def, const Substitution& sub) {
        if (def == nullptr) { return nullptr; }

        std::string name = def->name;
        auto bound = sub.types.find(def->name);

        if (bound != sub.types.end() && bound->second.kind == TypeKind::Struct) {
            name = bound->second.struct_name;
        }

        auto clone = std::make_unique<StructDefinition>(def->location, name,
            std::vector<StructDefinition::Member>{});
        clone->no_copy = def->no_copy;
        clone->no_move = def->no_move;

        for (const auto& m : def->members) {
            clone->members.push_back(clone_member(m, sub));
        }

        for (const auto& special : def->special_members) {
            clone->special_members.push_back(clone_special_member(special.get(), sub));
        }

        return clone;
    }

    void GenericExpander::register_local_struct_names(const Statement* stmt,
        Substitution& sub) const {
        if (stmt == nullptr) { return; }

        if (auto* def = dynamic_cast<const StructDefinition*>(stmt)) {
            if (sub.types.find(def->name) == sub.types.end() &&
                !sub.instance.generic_name.empty()) {
                sub.types[def->name] = Type::make_struct(
                    sub.instance.mangle() + "$" + def->name);
            }
            return;
        }

        if (auto* block = dynamic_cast<const Block*>(stmt)) {
            for (const auto& inner : block->statements) {
                register_local_struct_names(inner.get(), sub);
            }
            return;
        }

        if (auto* top_block = dynamic_cast<const TopLevelBlock*>(stmt)) {
            for (const auto& item : top_block->items) {
                register_local_struct_names(
                    dynamic_cast<const Statement*>(item.get()), sub);
            }
            return;
        }

        if (auto* if_stmt = dynamic_cast<const IfStatement*>(stmt)) {
            register_local_struct_names(if_stmt->then_block.get(), sub);
            register_local_struct_names(if_stmt->else_block.get(), sub);
            return;
        }

        if (auto* for_stmt = dynamic_cast<const ForStatement*>(stmt)) {
            register_local_struct_names(for_stmt->init.get(), sub);
            register_local_struct_names(for_stmt->body.get(), sub);
            return;
        }

        if (auto* while_stmt = dynamic_cast<const WhileStatement*>(stmt)) {
            register_local_struct_names(while_stmt->body.get(), sub);
            return;
        }

        if (auto* switch_stmt = dynamic_cast<const SwitchCaseStatement*>(stmt)) {
            for (const SwitchCaseStatement::Clause& clause : switch_stmt->clauses) {
                for (const auto& child : clause.statements) {
                    register_local_struct_names(child.get(), sub);
                }
            }
        }
    }

    bool GenericExpander::expand_statement_emit_run(
        const std::vector<const EmitStatement*>& emits, const Substitution& sub,
        std::vector<std::unique_ptr<Statement>>& out) {
        std::string text;
        SourceLocation text_loc;

        auto flush_text = [&]() -> bool {
            if (text.empty()) { return true; }

            std::vector<LineSplice> splices;
            const std::string body_text = splice_line_continuations(text, splices);
            text.clear();

            if (text_contains_compile_time_condition(body_text)) {
                report(text_loc, ErrorCode::CompileTimeConditionOutsideGenericBlock,
                    std::vector<std::string>());
                return false;
            }

            emit_source_pool_.push_back(
                "void __glt_emit_body() {\n" + body_text + "\n}\n");
            std::string& wrapper = emit_source_pool_.back();
            std::string name = "<emit>";
            std::string_view source_view(wrapper);
            std::string_view name_view(name);
            DiagnosticEngine emit_diag;
            Lexer lexer(source_view, name_view, emit_diag);
            Parser parser(lexer, emit_diag, true);
            auto program = parser.parse();
            auto* function = program != nullptr && program->top_levels.size() == 1
                ? dynamic_cast<FunctionDefinition*>(program->top_levels.front().get())
                : nullptr;
            auto* body = function != nullptr
                ? dynamic_cast<Block*>(function->body.get()) : nullptr;

            if (emit_diag.has_errors() || body == nullptr) {
                report(text_loc, ErrorCode::EmitBlockNotExpandable,
                    std::vector<std::string>());
                return false;
            }

            for (const auto& stmt : body->statements) {
                if (!namespaces_.rewrite_new_generated_statement(stmt.get())) {
                    return false;
                }

                auto clone = clone_statement(stmt.get(), sub);

                if (clone == nullptr) {
                    report(stmt->location, ErrorCode::EmitBlockNotExpandable,
                        std::vector<std::string>());
                    return false;
                }

                out.push_back(std::move(clone));
            }

            return true;
        };

        for (const EmitStatement* emit : emits) {
            if (emit == nullptr) { continue; }
            if (!validate_emit_generated_node(emit)) { return false; }

            if (emit->is_block) {
                if (!flush_text()) { return false; }

                for (const auto& node : emit->block_items) {
                    auto* statement = dynamic_cast<Statement*>(node.get());

                    if (statement == nullptr) {
                        report(node != nullptr ? node->location : SourceLocation(),
                            ErrorCode::EmitBlockNotExpandable,
                            std::vector<std::string>());
                        return false;
                    }

                    if (!namespaces_.rewrite_new_generated_statement(statement)) {
                        return false;
                    }

                    auto clone = clone_statement(statement, sub);

                    if (clone == nullptr) {
                        report(statement->location, ErrorCode::EmitBlockNotExpandable,
                            std::vector<std::string>());
                        return false;
                    }

                    out.push_back(std::move(clone));
                }

                continue;
            }

            if (text.empty()) { text_loc = emit->location; }

            for (const auto& piece : emit->pieces) {
                std::string piece_text;

                if (!eval_emit_piece(piece.get(), sub, piece_text)) {
                    report(piece->location, ErrorCode::EmitStringNotConstant,
                        std::vector<std::string>{ expression_text(piece.get()) });
                    return false;
                }

                text += piece_text;
            }
        }

        return flush_text();
    }

}
