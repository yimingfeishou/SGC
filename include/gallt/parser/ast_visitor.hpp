#ifndef GALLT_PARSER_AST_VISITOR_HPP
#define GALLT_PARSER_AST_VISITOR_HPP

#include "ast.hpp"
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gallt {
    namespace AST {

        template <typename... Ts>
        struct AstTypeList {
        };

        using AstTopLevelTypes = AstTypeList<
            GuideStatement, ClibStatement, ExternDeclaration, FunctionDefinition,
            StructDefinition, GenericDefinition, InstantiationStatement,
            NamespaceDefinition, AccessNamespaceStatement, AdditionNamespaceStatement,
            CondDefinition, UncondDefinition, ConditionalBlock, TopLevelBlock,
            VariableDeclaration>;

        using AstStatementTypes = AstTypeList<
            EmptyStatement, Block, VariableDeclaration, IfStatement, ForStatement,
            WhileStatement, BreakStatement, ReturnStatement, ExpressionStatement,
            DestructStatement, EmitStatement, StructDefinition, GenericDefinition,
            InstantiationStatement, NamespaceDefinition, AccessNamespaceStatement,
            AdditionNamespaceStatement, CondDefinition, UncondDefinition,
            ConditionalBlock, TopLevelBlock>;

        using AstExpressionTypes = AstTypeList<
            AssignmentExpression, LogicalOrExpression, ConditionalExpression,
            LogicalAndExpression, BitwiseExpression, ComparisonExpression,
            ShiftExpression, AdditiveExpression, MultiplicativeExpression,
            PowerExpression, UnaryExpression, PostfixExpression,
            CompileTimePropertyExpression, PrimaryExpression>;

        using AstInitializerTypes = AstTypeList<ExpressionInitializer, ArrayInitializer>;

        template <typename T>
        inline constexpr bool ast_always_false = false;

        template <typename Base, typename T>
        using AstCastTarget = std::conditional_t<std::is_const_v<Base>,
            const T, T>;

        template <typename Base, typename... Ts>
        struct AstRuntimeCast;

        template <typename Base>
        struct AstRuntimeCast<Base> {
            template <typename Fn>
            static bool apply(Base*, Fn&&) { return false; }
        };

        template <typename Base, typename T, typename... Ts>
        struct AstRuntimeCast<Base, T, Ts...> {
            template <typename Fn>
            static bool apply(Base* node, Fn&& fn) {
                if (auto* typed = dynamic_cast<AstCastTarget<Base, T>*>(node)) {
                    fn(typed);
                    return true;
                }
                return AstRuntimeCast<Base, Ts...>::apply(
                    node, std::forward<Fn>(fn));
            }
        };

        template <typename Base, typename List>
        struct AstDispatch;

        template <typename Base, typename... Ts>
        struct AstDispatch<Base, AstTypeList<Ts...>> {
            template <typename Fn>
            static bool apply(Base* node, Fn&& fn) {
                return AstRuntimeCast<Base, Ts...>::apply(
                    node, std::forward<Fn>(fn));
            }
        };

        class AstScope {
        public:
            void enter() { ++depth_; }

            void leave() {
                if (depth_ > 0) {
                    --depth_;
                }
            }

            std::size_t depth() const { return depth_; }

            void clear() { depth_ = 0; }

        private:
            std::size_t depth_ = 0;
        };

        class AstScopeGuard {
        public:
            explicit AstScopeGuard(AstScope& scope) : scope_(scope) {
                scope_.enter();
            }

            ~AstScopeGuard() { scope_.leave(); }

            AstScopeGuard(const AstScopeGuard&) = delete;
            AstScopeGuard& operator=(const AstScopeGuard&) = delete;

        private:
            AstScope& scope_;
        };

        class AstPoison {
        public:
            void poison(const Node* node) {
                if (node != nullptr) {
                    nodes_.insert(node);
                }
            }

            bool poisoned(const Node* node) const {
                return node != nullptr && nodes_.count(node) != 0;
            }

            void clear() { nodes_.clear(); }

            bool empty() const { return nodes_.empty(); }

            std::size_t size() const { return nodes_.size(); }

        private:
            std::unordered_set<const Node*> nodes_;
        };

        class AstVisitor {
        public:
            virtual ~AstVisitor() = default;

            bool visit_program(const Program* program) {
                if (program == nullptr) {
                    return true;
                }
                if (!visit_top_levels(program->top_levels)) {
                    return false;
                }
                return visit_statements(program->global_initializers);
            }

            bool visit_top_levels(const std::vector<std::unique_ptr<TopLevel>>& nodes) {
                for (const std::unique_ptr<TopLevel>& node : nodes) {
                    if (!visit_top_level(node.get())) {
                        return false;
                    }
                }
                return true;
            }

            bool visit_statements(const std::vector<std::unique_ptr<Statement>>& nodes) {
                for (const std::unique_ptr<Statement>& node : nodes) {
                    if (!visit_statement(node.get())) {
                        return false;
                    }
                }
                return true;
            }

            bool visit_expressions(
                const std::vector<std::unique_ptr<Expression>>& nodes) {
                for (const std::unique_ptr<Expression>& node : nodes) {
                    if (!visit_expression(node.get())) {
                        return false;
                    }
                }
                return true;
            }

            bool visit_initializers(
                const std::vector<std::unique_ptr<Initializer>>& nodes) {
                for (const std::unique_ptr<Initializer>& node : nodes) {
                    if (!visit_initializer(node.get())) {
                        return false;
                    }
                }
                return true;
            }

            bool visit_top_level(const TopLevel* node) {
                if (stopped_ || node == nullptr) {
                    return !stopped_;
                }
                bool proceed = true;

                AstDispatch<const TopLevel, AstTopLevelTypes>::apply(node,
                    [&](const auto* typed) {
                        proceed = visit_top_level_typed(typed);
                    });

                if (!proceed || stopped_) {
                    return false;
                }
                return true;
            }

            bool visit_statement(const Statement* node) {
                if (stopped_ || node == nullptr) {
                    return !stopped_;
                }
                bool proceed = true;

                AstDispatch<const Statement, AstStatementTypes>::apply(node,
                    [&](const auto* typed) {
                        proceed = visit_statement_typed(typed);
                    });

                if (!proceed || stopped_) {
                    return false;
                }
                return true;
            }

            bool visit_expression(const Expression* node) {
                if (stopped_ || node == nullptr) {
                    return !stopped_;
                }
                bool proceed = true;

                AstDispatch<const Expression, AstExpressionTypes>::apply(node,
                    [&](const auto* typed) {
                        proceed = visit_expression_typed(typed);
                    });

                if (!proceed || stopped_) {
                    return false;
                }
                return true;
            }

            bool visit_initializer(const Initializer* node) {
                if (stopped_ || node == nullptr) {
                    return !stopped_;
                }
                bool proceed = true;

                AstDispatch<const Initializer, AstInitializerTypes>::apply(node,
                    [&](const auto* typed) {
                        proceed = visit_initializer_typed(typed);
                    });

                if (!proceed || stopped_) {
                    return false;
                }
                return true;
            }

            bool visit_expression_shared(const std::shared_ptr<Expression>& node) {
                if (node == nullptr) {
                    return !stopped_;
                }
                return visit_expression(node.get());
            }

            bool visit_type(const Type& type) {
                if (stopped_) {
                    return false;
                }
                if (!EnterType(type)) {
                    return !stopped_;
                }

                if (type.element_type != nullptr &&
                    !visit_type(*type.element_type)) {
                    return false;
                }
                if (type.pointee_type != nullptr &&
                    !visit_type(*type.pointee_type)) {
                    return false;
                }
                if (type.return_type != nullptr &&
                    !visit_type(*type.return_type)) {
                    return false;
                }
                for (const Type& parameter : type.parameter_types) {
                    if (!visit_type(parameter)) {
                        return false;
                    }
                }
                if (type.variadic_element_type != nullptr &&
                    !visit_type(*type.variadic_element_type)) {
                    return false;
                }
                if (type.generic_ref != nullptr &&
                    !visit_generic_ref(*type.generic_ref)) {
                    return false;
                }

                LeaveType(type);
                return !stopped_;
            }

            bool visit_generic_ref(const GenericRef& ref) {
                for (const GenericArgument& argument : ref.arguments) {
                    if (!visit_generic_argument(argument)) {
                        return false;
                    }
                }
                return !stopped_;
            }

            bool visit_generic_argument(const GenericArgument& argument) {
                if (argument.is_type && !visit_type(argument.type)) {
                    return false;
                }
                if (!visit_type(argument.constant_actual_type)) {
                    return false;
                }
                for (const Type& parameter : argument.expr_param_types) {
                    if (!visit_type(parameter)) {
                        return false;
                    }
                }
                if (!visit_type(argument.expr_return_type)) {
                    return false;
                }
                if (argument.expr_body != nullptr &&
                    !visit_statements(argument.expr_body->statements)) {
                    return false;
                }
                return visit_expression_shared(argument.expression);
            }

            void stop() { stopped_ = true; }

            bool stopped() const { return stopped_; }

            void reset() { stopped_ = false; }

        protected:
            virtual bool EnterGuideStatement(const GuideStatement*) { return true; }
            virtual void LeaveGuideStatement(const GuideStatement*) {}
            virtual bool EnterClibStatement(const ClibStatement*) { return true; }
            virtual void LeaveClibStatement(const ClibStatement*) {}
            virtual bool EnterExternDeclaration(const ExternDeclaration*) { return true; }
            virtual void LeaveExternDeclaration(const ExternDeclaration*) {}
            virtual bool EnterFunctionDefinition(const FunctionDefinition*) { return true; }
            virtual void LeaveFunctionDefinition(const FunctionDefinition*) {}
            virtual bool EnterStructDefinition(const StructDefinition*) { return true; }
            virtual void LeaveStructDefinition(const StructDefinition*) {}
            virtual bool EnterGenericDefinition(const GenericDefinition*) { return true; }
            virtual void LeaveGenericDefinition(const GenericDefinition*) {}
            virtual bool EnterInstantiationStatement(const InstantiationStatement*) { return true; }
            virtual void LeaveInstantiationStatement(const InstantiationStatement*) {}
            virtual bool EnterNamespaceDefinition(const NamespaceDefinition*) { return true; }
            virtual void LeaveNamespaceDefinition(const NamespaceDefinition*) {}
            virtual bool EnterAccessNamespaceStatement(const AccessNamespaceStatement*) { return true; }
            virtual void LeaveAccessNamespaceStatement(const AccessNamespaceStatement*) {}
            virtual bool EnterAdditionNamespaceStatement(const AdditionNamespaceStatement*) { return true; }
            virtual void LeaveAdditionNamespaceStatement(const AdditionNamespaceStatement*) {}
            virtual bool EnterCondDefinition(const CondDefinition*) { return true; }
            virtual void LeaveCondDefinition(const CondDefinition*) {}
            virtual bool EnterUncondDefinition(const UncondDefinition*) { return true; }
            virtual void LeaveUncondDefinition(const UncondDefinition*) {}
            virtual bool EnterConditionalBlock(const ConditionalBlock*) { return true; }
            virtual void LeaveConditionalBlock(const ConditionalBlock*) {}
            virtual bool EnterTopLevelBlock(const TopLevelBlock*) { return true; }
            virtual void LeaveTopLevelBlock(const TopLevelBlock*) {}
            virtual bool EnterVariableDeclaration(const VariableDeclaration*) { return true; }
            virtual void LeaveVariableDeclaration(const VariableDeclaration*) {}

            virtual bool EnterEmptyStatement(const EmptyStatement*) { return true; }
            virtual void LeaveEmptyStatement(const EmptyStatement*) {}
            virtual bool EnterBlock(const Block*) { return true; }
            virtual void LeaveBlock(const Block*) {}
            virtual bool EnterIfStatement(const IfStatement*) { return true; }
            virtual void LeaveIfStatement(const IfStatement*) {}
            virtual bool EnterForStatement(const ForStatement*) { return true; }
            virtual void LeaveForStatement(const ForStatement*) {}
            virtual bool EnterWhileStatement(const WhileStatement*) { return true; }
            virtual void LeaveWhileStatement(const WhileStatement*) {}
            virtual bool EnterBreakStatement(const BreakStatement*) { return true; }
            virtual void LeaveBreakStatement(const BreakStatement*) {}
            virtual bool EnterReturnStatement(const ReturnStatement*) { return true; }
            virtual void LeaveReturnStatement(const ReturnStatement*) {}
            virtual bool EnterExpressionStatement(const ExpressionStatement*) { return true; }
            virtual void LeaveExpressionStatement(const ExpressionStatement*) {}
            virtual bool EnterDestructStatement(const DestructStatement*) { return true; }
            virtual void LeaveDestructStatement(const DestructStatement*) {}
            virtual bool EnterEmitStatement(const EmitStatement*) { return true; }
            virtual void LeaveEmitStatement(const EmitStatement*) {}

            virtual bool EnterAssignmentExpression(const AssignmentExpression*) { return true; }
            virtual void LeaveAssignmentExpression(const AssignmentExpression*) {}
            virtual bool EnterLogicalOrExpression(const LogicalOrExpression*) { return true; }
            virtual void LeaveLogicalOrExpression(const LogicalOrExpression*) {}
            virtual bool EnterLogicalAndExpression(const LogicalAndExpression*) { return true; }
            virtual void LeaveLogicalAndExpression(const LogicalAndExpression*) {}
            virtual bool EnterConditionalExpression(const ConditionalExpression*) { return true; }
            virtual void LeaveConditionalExpression(const ConditionalExpression*) {}
            virtual bool EnterBitwiseExpression(const BitwiseExpression*) { return true; }
            virtual void LeaveBitwiseExpression(const BitwiseExpression*) {}
            virtual bool EnterComparisonExpression(const ComparisonExpression*) { return true; }
            virtual void LeaveComparisonExpression(const ComparisonExpression*) {}
            virtual bool EnterShiftExpression(const ShiftExpression*) { return true; }
            virtual void LeaveShiftExpression(const ShiftExpression*) {}
            virtual bool EnterAdditiveExpression(const AdditiveExpression*) { return true; }
            virtual void LeaveAdditiveExpression(const AdditiveExpression*) {}
            virtual bool EnterMultiplicativeExpression(const MultiplicativeExpression*) { return true; }
            virtual void LeaveMultiplicativeExpression(const MultiplicativeExpression*) {}
            virtual bool EnterPowerExpression(const PowerExpression*) { return true; }
            virtual void LeavePowerExpression(const PowerExpression*) {}
            virtual bool EnterUnaryExpression(const UnaryExpression*) { return true; }
            virtual void LeaveUnaryExpression(const UnaryExpression*) {}
            virtual bool EnterPostfixExpression(const PostfixExpression*) { return true; }
            virtual void LeavePostfixExpression(const PostfixExpression*) {}
            virtual bool EnterCompileTimePropertyExpression(
                const CompileTimePropertyExpression*) { return true; }
            virtual void LeaveCompileTimePropertyExpression(
                const CompileTimePropertyExpression*) {}
            virtual bool EnterPrimaryExpression(const PrimaryExpression*) { return true; }
            virtual void LeavePrimaryExpression(const PrimaryExpression*) {}

            virtual bool EnterExpressionInitializer(const ExpressionInitializer*) { return true; }
            virtual void LeaveExpressionInitializer(const ExpressionInitializer*) {}
            virtual bool EnterArrayInitializer(const ArrayInitializer*) { return true; }
            virtual void LeaveArrayInitializer(const ArrayInitializer*) {}

            virtual bool EnterType(const Type&) { return true; }
            virtual void LeaveType(const Type&) {}

            virtual bool EnterUnknownNode(const Node*) { return true; }
            virtual void LeaveUnknownNode(const Node*) {}

        private:
            template <typename T>
            bool visit_top_level_typed(const T* node) {
                if constexpr (std::is_same_v<T, GuideStatement>) {
                    if (!EnterGuideStatement(node)) { return true; }
                    LeaveGuideStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ClibStatement>) {
                    if (!EnterClibStatement(node)) { return true; }
                    LeaveClibStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ExternDeclaration>) {
                    if (!EnterExternDeclaration(node)) { return true; }
                    if (!visit_type(node->return_type)) { return false; }
                    for (const Type& parameter : node->parameters) {
                        if (!visit_type(parameter)) { return false; }
                    }
                    LeaveExternDeclaration(node);
                    return true;
                } else if constexpr (std::is_same_v<T, FunctionDefinition>) {
                    if (!EnterFunctionDefinition(node)) { return true; }
                    if (!visit_type(node->return_type)) { return false; }
                    for (const Type& parameter : node->parameters) {
                        if (!visit_type(parameter)) { return false; }
                    }
                    if (!visit_type(node->conversion_target_type)) { return false; }
                    if (!visit_expressions(node->param_defaults)) { return false; }
                    if (!visit_statement(node->body.get())) { return false; }
                    LeaveFunctionDefinition(node);
                    return true;
                } else if constexpr (std::is_same_v<T, StructDefinition>) {
                    if (!EnterStructDefinition(node)) { return true; }
                    for (const StructDefinition::Member& member : node->members) {
                        if (!visit_type(member.type)) { return false; }
                        if (member.function_pointer_type.has_value() &&
                            !visit_type(*member.function_pointer_type)) {
                            return false;
                        }
                        if (!visit_expression(member.array_size_expr.get())) {
                            return false;
                        }
                        if (!visit_initializer(member.initializer.get())) {
                            return false;
                        }
                    }
                    for (const std::unique_ptr<SpecialMemberFunction>& special :
                        node->special_members) {
                        if (special == nullptr) { continue; }
                        if (!visit_type(special->parameter_type)) { return false; }
                        for (const Type& parameter : special->parameters) {
                            if (!visit_type(parameter)) { return false; }
                        }
                        if (!visit_expressions(special->parameter_defaults)) {
                            return false;
                        }
                        if (!visit_statement(special->body.get())) { return false; }
                    }
                    LeaveStructDefinition(node);
                    return true;
                } else if constexpr (std::is_same_v<T, GenericDefinition>) {
                    if (!EnterGenericDefinition(node)) { return true; }
                    for (const GenericParameter& parameter : node->parameters) {
                        if (!visit_type(parameter.constant_type)) { return false; }
                        for (const Type& declared : parameter.expr_param_types) {
                            if (!visit_type(declared)) { return false; }
                        }
                        if (!visit_type(parameter.expr_return_type)) { return false; }
                    }
                    for (const GenericPatternArg& pattern : node->patterns) {
                        if (!visit_type(pattern.type)) { return false; }
                    }
                    if (!visit_top_levels(node->members)) { return false; }
                    if (!visit_statements(node->compile_time_items)) { return false; }
                    LeaveGenericDefinition(node);
                    return true;
                } else if constexpr (std::is_same_v<T, InstantiationStatement>) {
                    if (!EnterInstantiationStatement(node)) { return true; }
                    if (!visit_generic_ref(node->reference)) { return false; }
                    LeaveInstantiationStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, NamespaceDefinition>) {
                    if (!EnterNamespaceDefinition(node)) { return true; }
                    if (!visit_top_levels(node->members)) { return false; }
                    LeaveNamespaceDefinition(node);
                    return true;
                } else if constexpr (std::is_same_v<T, AccessNamespaceStatement>) {
                    if (!EnterAccessNamespaceStatement(node)) { return true; }
                    LeaveAccessNamespaceStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, AdditionNamespaceStatement>) {
                    if (!EnterAdditionNamespaceStatement(node)) { return true; }
                    if (!visit_top_levels(node->members)) { return false; }
                    LeaveAdditionNamespaceStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, CondDefinition>) {
                    if (!EnterCondDefinition(node)) { return true; }
                    if (!visit_expression(node->value.get())) { return false; }
                    LeaveCondDefinition(node);
                    return true;
                } else if constexpr (std::is_same_v<T, UncondDefinition>) {
                    if (!EnterUncondDefinition(node)) { return true; }
                    LeaveUncondDefinition(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ConditionalBlock>) {
                    if (!EnterConditionalBlock(node)) { return true; }
                    if (!visit_expression(node->condition.get())) { return false; }
                    if (!visit_statement(node->then_block.get())) { return false; }
                    if (!visit_statement(node->else_block.get())) { return false; }
                    LeaveConditionalBlock(node);
                    return true;
                } else if constexpr (std::is_same_v<T, TopLevelBlock>) {
                    if (!EnterTopLevelBlock(node)) { return true; }
                    if (!visit_top_levels(node->items)) { return false; }
                    LeaveTopLevelBlock(node);
                    return true;
                } else if constexpr (std::is_same_v<T, VariableDeclaration>) {
                    return visit_variable_declaration(node);
                } else {
                    static_assert(ast_always_false<T>,
                        "unhandled AST TopLevel node type in AstVisitor");
                    return true;
                }
            }

            template <typename T>
            bool visit_statement_typed(const T* node) {
                if constexpr (std::is_same_v<T, EmptyStatement>) {
                    if (!EnterEmptyStatement(node)) { return true; }
                    LeaveEmptyStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, Block>) {
                    if (!EnterBlock(node)) { return true; }
                    if (!visit_statements(node->statements)) { return false; }
                    LeaveBlock(node);
                    return true;
                } else if constexpr (std::is_same_v<T, IfStatement>) {
                    if (!EnterIfStatement(node)) { return true; }
                    if (!visit_expression(node->condition.get())) { return false; }
                    if (!visit_statement(node->then_block.get())) { return false; }
                    if (!visit_statement(node->else_block.get())) { return false; }
                    LeaveIfStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ForStatement>) {
                    if (!EnterForStatement(node)) { return true; }
                    if (!visit_statement(node->init.get())) { return false; }
                    if (!visit_expression(node->condition.get())) { return false; }
                    if (!visit_expression(node->step.get())) { return false; }
                    if (!visit_statement(node->body.get())) { return false; }
                    LeaveForStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, WhileStatement>) {
                    if (!EnterWhileStatement(node)) { return true; }
                    if (!visit_expression(node->condition.get())) { return false; }
                    if (!visit_statement(node->body.get())) { return false; }
                    LeaveWhileStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, BreakStatement>) {
                    if (!EnterBreakStatement(node)) { return true; }
                    LeaveBreakStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ReturnStatement>) {
                    if (!EnterReturnStatement(node)) { return true; }
                    if (!visit_expression(node->value.get())) { return false; }
                    LeaveReturnStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ExpressionStatement>) {
                    if (!EnterExpressionStatement(node)) { return true; }
                    if (!visit_expression(node->expr.get())) { return false; }
                    LeaveExpressionStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, DestructStatement>) {
                    if (!EnterDestructStatement(node)) { return true; }
                    if (!visit_expression(node->target.get())) { return false; }
                    LeaveDestructStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, EmitStatement>) {
                    if (!EnterEmitStatement(node)) { return true; }
                    if (!visit_expressions(node->pieces)) { return false; }
                    if (!visit_node_pointers(node->block_items)) { return false; }
                    LeaveEmitStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, VariableDeclaration>) {
                    return visit_variable_declaration(node);
                } else if constexpr (std::is_same_v<T, StructDefinition>) {
                    return visit_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, GenericDefinition>) {
                    return visit_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, InstantiationStatement>) {
                    return visit_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, NamespaceDefinition>) {
                    return visit_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, AccessNamespaceStatement>) {
                    return visit_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, AdditionNamespaceStatement>) {
                    return visit_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, CondDefinition>) {
                    return visit_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, UncondDefinition>) {
                    return visit_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, ConditionalBlock>) {
                    return visit_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, TopLevelBlock>) {
                    return visit_top_level_typed(node);
                } else {
                    static_assert(ast_always_false<T>,
                        "unhandled AST Statement node type in AstVisitor");
                    return true;
                }
            }

            template <typename T>
            bool visit_variable_declaration(const T* node) {
                if (!EnterVariableDeclaration(node)) { return true; }
                if (!visit_type(node->type)) { return false; }
                if (node->function_pointer_type.has_value() &&
                    !visit_type(*node->function_pointer_type)) {
                    return false;
                }
                if (!visit_expression(node->array_size_expr.get())) { return false; }
                if (!visit_initializer(node->initializer.get())) { return false; }
                LeaveVariableDeclaration(node);
                return true;
            }

            bool visit_node_pointers(const std::vector<std::unique_ptr<Node>>& nodes) {
                for (const std::unique_ptr<Node>& node : nodes) {
                    if (node == nullptr) { continue; }
                    if (const TopLevel* top = dynamic_cast<const TopLevel*>(node.get())) {
                        if (!visit_top_level(top)) { return false; }
                        continue;
                    }
                    if (const Statement* statement =
                        dynamic_cast<const Statement*>(node.get())) {
                        if (!visit_statement(statement)) { return false; }
                        continue;
                    }
                    if (!EnterUnknownNode(node.get())) { continue; }
                    LeaveUnknownNode(node.get());
                }
                return !stopped_;
            }

            template <typename T>
            bool visit_expression_typed(const T* node) {
                if constexpr (std::is_same_v<T, AssignmentExpression>) {
                    if (!EnterAssignmentExpression(node)) { return true; }
                    if (!visit_expression(node->left.get())) { return false; }
                    if (!visit_expression(node->right.get())) { return false; }
                    LeaveAssignmentExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, LogicalOrExpression>) {
                    if (!EnterLogicalOrExpression(node)) { return true; }
                    if (!visit_expression(node->left.get())) { return false; }
                    if (!visit_expression(node->right.get())) { return false; }
                    LeaveLogicalOrExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, LogicalAndExpression>) {
                    if (!EnterLogicalAndExpression(node)) { return true; }
                    if (!visit_expression(node->left.get())) { return false; }
                    if (!visit_expression(node->right.get())) { return false; }
                    LeaveLogicalAndExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ConditionalExpression>) {
                    if (!EnterConditionalExpression(node)) { return true; }
                    if (!visit_expression(node->condition.get())) { return false; }
                    if (!visit_expression(node->then_expr.get())) { return false; }
                    if (!visit_expression(node->else_expr.get())) { return false; }
                    LeaveConditionalExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, BitwiseExpression>) {
                    if (!EnterBitwiseExpression(node)) { return true; }
                    if (!visit_expression(node->left.get())) { return false; }
                    if (!visit_expression(node->right.get())) { return false; }
                    LeaveBitwiseExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ComparisonExpression>) {
                    if (!EnterComparisonExpression(node)) { return true; }
                    if (!visit_expression(node->left.get())) { return false; }
                    if (!visit_expression(node->right.get())) { return false; }
                    LeaveComparisonExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ShiftExpression>) {
                    if (!EnterShiftExpression(node)) { return true; }
                    if (!visit_expression(node->left.get())) { return false; }
                    if (!visit_expression(node->right.get())) { return false; }
                    LeaveShiftExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, AdditiveExpression>) {
                    if (!EnterAdditiveExpression(node)) { return true; }
                    if (!visit_expression(node->left.get())) { return false; }
                    if (!visit_expression(node->right.get())) { return false; }
                    LeaveAdditiveExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, MultiplicativeExpression>) {
                    if (!EnterMultiplicativeExpression(node)) { return true; }
                    if (!visit_expression(node->left.get())) { return false; }
                    if (!visit_expression(node->right.get())) { return false; }
                    LeaveMultiplicativeExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, PowerExpression>) {
                    if (!EnterPowerExpression(node)) { return true; }
                    if (!visit_expression(node->left.get())) { return false; }
                    if (!visit_expression(node->right.get())) { return false; }
                    LeavePowerExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, UnaryExpression>) {
                    if (!EnterUnaryExpression(node)) { return true; }
                    if (!visit_expression(node->operand.get())) { return false; }
                    LeaveUnaryExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, PostfixExpression>) {
                    if (!EnterPostfixExpression(node)) { return true; }
                    if (!visit_type(node->cast_type)) { return false; }
                    if (!visit_expression(node->base.get())) { return false; }
                    if (!visit_expression(node->subscript_expr.get())) { return false; }
                    if (!visit_expressions(node->arguments)) { return false; }
                    LeavePostfixExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, CompileTimePropertyExpression>) {
                    if (!EnterCompileTimePropertyExpression(node)) { return true; }
                    if (!visit_expression(node->receiver.get())) { return false; }
                    if (!visit_expressions(node->arguments)) { return false; }
                    LeaveCompileTimePropertyExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, PrimaryExpression>) {
                    if (!EnterPrimaryExpression(node)) { return true; }
                    if (!visit_type(node->heap_type)) { return false; }
                    if (!visit_type(node->construct_type)) { return false; }
                    if (!visit_expression(node->paren_expr.get())) { return false; }
                    if (!visit_expression(node->heap_size.get())) { return false; }
                    if (!visit_expression(node->placement_target.get())) { return false; }
                    if (!visit_expressions(node->construct_args)) { return false; }
                    if (node->generic_ref != nullptr &&
                        !visit_generic_ref(*node->generic_ref)) {
                        return false;
                    }
                    LeavePrimaryExpression(node);
                    return true;
                } else {
                    static_assert(ast_always_false<T>,
                        "unhandled AST Expression node type in AstVisitor");
                    return true;
                }
            }

            template <typename T>
            bool visit_initializer_typed(const T* node) {
                if constexpr (std::is_same_v<T, ExpressionInitializer>) {
                    if (!EnterExpressionInitializer(node)) { return true; }
                    if (!visit_expression(node->expr.get())) { return false; }
                    LeaveExpressionInitializer(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ArrayInitializer>) {
                    if (!EnterArrayInitializer(node)) { return true; }
                    if (!visit_initializers(node->elements)) { return false; }
                    LeaveArrayInitializer(node);
                    return true;
                } else {
                    static_assert(ast_always_false<T>,
                        "unhandled AST Initializer node type in AstVisitor");
                    return true;
                }
            }

            bool stopped_ = false;
        };

        class AstRewriter {
        public:
            virtual ~AstRewriter() = default;

            bool rewrite_program(Program* program) {
                if (program == nullptr) {
                    return true;
                }
                if (!rewrite_top_levels(program->top_levels)) {
                    return false;
                }
                return rewrite_statements(program->global_initializers);
            }

            bool rewrite_top_levels(std::vector<std::unique_ptr<TopLevel>>& nodes) {
                for (std::unique_ptr<TopLevel>& node : nodes) {
                    if (!rewrite_top_level(node)) {
                        return false;
                    }
                }
                return true;
            }

            bool rewrite_statements(std::vector<std::unique_ptr<Statement>>& nodes) {
                for (std::unique_ptr<Statement>& node : nodes) {
                    if (!rewrite_statement(node)) {
                        return false;
                    }
                }
                return true;
            }

            bool rewrite_expressions(std::vector<std::unique_ptr<Expression>>& nodes) {
                for (std::unique_ptr<Expression>& node : nodes) {
                    if (!rewrite_expression(node)) {
                        return false;
                    }
                }
                return true;
            }

            bool rewrite_initializers(std::vector<std::unique_ptr<Initializer>>& nodes) {
                for (std::unique_ptr<Initializer>& node : nodes) {
                    if (!rewrite_initializer(node)) {
                        return false;
                    }
                }
                return true;
            }

            bool rewrite_top_level(std::unique_ptr<TopLevel>& node) {
                if (stopped_) {
                    return false;
                }
                if (node == nullptr) {
                    return true;
                }
                if (!EnterTopLevel(node)) {
                    return !stopped_;
                }
                if (node == nullptr) {
                    return !stopped_;
                }
                if (!rewrite_top_level_body(node.get())) {
                    return false;
                }
                if (node != nullptr) {
                    LeaveTopLevel(node);
                }
                return !stopped_;
            }

            bool rewrite_statement(std::unique_ptr<Statement>& node) {
                if (stopped_) {
                    return false;
                }
                if (node == nullptr) {
                    return true;
                }
                if (!EnterStatement(node)) {
                    return !stopped_;
                }
                if (node == nullptr) {
                    return !stopped_;
                }
                if (!rewrite_statement_body(node.get())) {
                    return false;
                }
                if (node != nullptr) {
                    LeaveStatement(node);
                }
                return !stopped_;
            }

            bool rewrite_expression(std::unique_ptr<Expression>& node) {
                if (stopped_) {
                    return false;
                }
                if (node == nullptr) {
                    return true;
                }
                if (!EnterExpression(node)) {
                    return !stopped_;
                }
                if (node == nullptr) {
                    return !stopped_;
                }
                if (!rewrite_expression_body(node.get())) {
                    return false;
                }
                if (node != nullptr) {
                    LeaveExpression(node);
                }
                return !stopped_;
            }

            bool rewrite_expression_shared(std::shared_ptr<Expression>& node) {
                if (stopped_) {
                    return false;
                }
                if (node == nullptr) {
                    return true;
                }
                if (!EnterSharedExpression(node)) {
                    return !stopped_;
                }
                if (node == nullptr) {
                    return !stopped_;
                }
                if (!rewrite_expression_body(node.get())) {
                    return false;
                }
                if (node != nullptr) {
                    LeaveSharedExpression(node);
                }
                return !stopped_;
            }

            bool rewrite_initializer(std::unique_ptr<Initializer>& node) {
                if (stopped_) {
                    return false;
                }
                if (node == nullptr) {
                    return true;
                }
                if (!EnterInitializer(node)) {
                    return !stopped_;
                }
                if (node == nullptr) {
                    return !stopped_;
                }
                if (!rewrite_initializer_body(node.get())) {
                    return false;
                }
                if (node != nullptr) {
                    LeaveInitializer(node);
                }
                return !stopped_;
            }

            bool rewrite_type(Type& type) {
                if (stopped_) {
                    return false;
                }
                if (!EnterType(type)) {
                    return !stopped_;
                }
                if (type.element_type != nullptr &&
                    !rewrite_type(*type.element_type)) {
                    return false;
                }
                if (type.pointee_type != nullptr &&
                    !rewrite_type(*type.pointee_type)) {
                    return false;
                }
                if (type.return_type != nullptr &&
                    !rewrite_type(*type.return_type)) {
                    return false;
                }
                for (Type& parameter : type.parameter_types) {
                    if (!rewrite_type(parameter)) {
                        return false;
                    }
                }
                if (type.variadic_element_type != nullptr &&
                    !rewrite_type(*type.variadic_element_type)) {
                    return false;
                }
                if (type.generic_ref != nullptr &&
                    !rewrite_generic_ref(*type.generic_ref)) {
                    return false;
                }
                LeaveType(type);
                return !stopped_;
            }

            bool rewrite_generic_ref(GenericRef& ref) {
                for (GenericArgument& argument : ref.arguments) {
                    if (!rewrite_generic_argument(argument)) {
                        return false;
                    }
                }
                return !stopped_;
            }

            bool rewrite_generic_argument(GenericArgument& argument) {
                if (argument.is_type && !rewrite_type(argument.type)) {
                    return false;
                }
                if (!rewrite_type(argument.constant_actual_type)) {
                    return false;
                }
                for (Type& parameter : argument.expr_param_types) {
                    if (!rewrite_type(parameter)) {
                        return false;
                    }
                }
                if (!rewrite_type(argument.expr_return_type)) {
                    return false;
                }
                if (argument.expr_body != nullptr &&
                    !rewrite_statements(argument.expr_body->statements)) {
                    return false;
                }
                return rewrite_expression_shared(argument.expression);
            }

            void stop() { stopped_ = true; }

            bool stopped() const { return stopped_; }

            void reset() { stopped_ = false; }

        protected:
            virtual bool EnterTopLevel(std::unique_ptr<TopLevel>&) { return true; }
            virtual void LeaveTopLevel(std::unique_ptr<TopLevel>&) {}
            virtual bool EnterStatement(std::unique_ptr<Statement>&) { return true; }
            virtual void LeaveStatement(std::unique_ptr<Statement>&) {}
            virtual bool EnterExpression(std::unique_ptr<Expression>&) { return true; }
            virtual void LeaveExpression(std::unique_ptr<Expression>&) {}
            virtual bool EnterSharedExpression(std::shared_ptr<Expression>&) { return true; }
            virtual void LeaveSharedExpression(std::shared_ptr<Expression>&) {}
            virtual bool EnterInitializer(std::unique_ptr<Initializer>&) { return true; }
            virtual void LeaveInitializer(std::unique_ptr<Initializer>&) {}
            virtual bool EnterType(Type&) { return true; }
            virtual void LeaveType(Type&) {}

            virtual bool EnterGuideStatement(GuideStatement*) { return true; }
            virtual void LeaveGuideStatement(GuideStatement*) {}
            virtual bool EnterClibStatement(ClibStatement*) { return true; }
            virtual void LeaveClibStatement(ClibStatement*) {}
            virtual bool EnterExternDeclaration(ExternDeclaration*) { return true; }
            virtual void LeaveExternDeclaration(ExternDeclaration*) {}
            virtual bool EnterFunctionDefinition(FunctionDefinition*) { return true; }
            virtual void LeaveFunctionDefinition(FunctionDefinition*) {}
            virtual bool EnterStructDefinition(StructDefinition*) { return true; }
            virtual void LeaveStructDefinition(StructDefinition*) {}
            virtual bool EnterGenericDefinition(GenericDefinition*) { return true; }
            virtual void LeaveGenericDefinition(GenericDefinition*) {}
            virtual bool EnterInstantiationStatement(InstantiationStatement*) { return true; }
            virtual void LeaveInstantiationStatement(InstantiationStatement*) {}
            virtual bool EnterNamespaceDefinition(NamespaceDefinition*) { return true; }
            virtual void LeaveNamespaceDefinition(NamespaceDefinition*) {}
            virtual bool EnterAccessNamespaceStatement(AccessNamespaceStatement*) { return true; }
            virtual void LeaveAccessNamespaceStatement(AccessNamespaceStatement*) {}
            virtual bool EnterAdditionNamespaceStatement(AdditionNamespaceStatement*) { return true; }
            virtual void LeaveAdditionNamespaceStatement(AdditionNamespaceStatement*) {}
            virtual bool EnterCondDefinition(CondDefinition*) { return true; }
            virtual void LeaveCondDefinition(CondDefinition*) {}
            virtual bool EnterUncondDefinition(UncondDefinition*) { return true; }
            virtual void LeaveUncondDefinition(UncondDefinition*) {}
            virtual bool EnterConditionalBlock(ConditionalBlock*) { return true; }
            virtual void LeaveConditionalBlock(ConditionalBlock*) {}
            virtual bool EnterTopLevelBlock(TopLevelBlock*) { return true; }
            virtual void LeaveTopLevelBlock(TopLevelBlock*) {}
            virtual bool EnterVariableDeclaration(VariableDeclaration*) { return true; }
            virtual void LeaveVariableDeclaration(VariableDeclaration*) {}
            virtual bool EnterEmptyStatement(EmptyStatement*) { return true; }
            virtual void LeaveEmptyStatement(EmptyStatement*) {}
            virtual bool EnterBlock(Block*) { return true; }
            virtual void LeaveBlock(Block*) {}
            virtual bool EnterIfStatement(IfStatement*) { return true; }
            virtual void LeaveIfStatement(IfStatement*) {}
            virtual bool EnterForStatement(ForStatement*) { return true; }
            virtual void LeaveForStatement(ForStatement*) {}
            virtual bool EnterWhileStatement(WhileStatement*) { return true; }
            virtual void LeaveWhileStatement(WhileStatement*) {}
            virtual bool EnterBreakStatement(BreakStatement*) { return true; }
            virtual void LeaveBreakStatement(BreakStatement*) {}
            virtual bool EnterReturnStatement(ReturnStatement*) { return true; }
            virtual void LeaveReturnStatement(ReturnStatement*) {}
            virtual bool EnterExpressionStatement(ExpressionStatement*) { return true; }
            virtual void LeaveExpressionStatement(ExpressionStatement*) {}
            virtual bool EnterDestructStatement(DestructStatement*) { return true; }
            virtual void LeaveDestructStatement(DestructStatement*) {}
            virtual bool EnterEmitStatement(EmitStatement*) { return true; }
            virtual void LeaveEmitStatement(EmitStatement*) {}

            virtual bool EnterAssignmentExpression(AssignmentExpression*) { return true; }
            virtual void LeaveAssignmentExpression(AssignmentExpression*) {}
            virtual bool EnterLogicalOrExpression(LogicalOrExpression*) { return true; }
            virtual void LeaveLogicalOrExpression(LogicalOrExpression*) {}
            virtual bool EnterLogicalAndExpression(LogicalAndExpression*) { return true; }
            virtual void LeaveLogicalAndExpression(LogicalAndExpression*) {}
            virtual bool EnterConditionalExpression(ConditionalExpression*) { return true; }
            virtual void LeaveConditionalExpression(ConditionalExpression*) {}
            virtual bool EnterBitwiseExpression(BitwiseExpression*) { return true; }
            virtual void LeaveBitwiseExpression(BitwiseExpression*) {}
            virtual bool EnterComparisonExpression(ComparisonExpression*) { return true; }
            virtual void LeaveComparisonExpression(ComparisonExpression*) {}
            virtual bool EnterShiftExpression(ShiftExpression*) { return true; }
            virtual void LeaveShiftExpression(ShiftExpression*) {}
            virtual bool EnterAdditiveExpression(AdditiveExpression*) { return true; }
            virtual void LeaveAdditiveExpression(AdditiveExpression*) {}
            virtual bool EnterMultiplicativeExpression(MultiplicativeExpression*) { return true; }
            virtual void LeaveMultiplicativeExpression(MultiplicativeExpression*) {}
            virtual bool EnterPowerExpression(PowerExpression*) { return true; }
            virtual void LeavePowerExpression(PowerExpression*) {}
            virtual bool EnterUnaryExpression(UnaryExpression*) { return true; }
            virtual void LeaveUnaryExpression(UnaryExpression*) {}
            virtual bool EnterPostfixExpression(PostfixExpression*) { return true; }
            virtual void LeavePostfixExpression(PostfixExpression*) {}
            virtual bool EnterCompileTimePropertyExpression(
                CompileTimePropertyExpression*) { return true; }
            virtual void LeaveCompileTimePropertyExpression(
                CompileTimePropertyExpression*) {}
            virtual bool EnterPrimaryExpression(PrimaryExpression*) { return true; }
            virtual void LeavePrimaryExpression(PrimaryExpression*) {}

            virtual bool EnterExpressionInitializer(ExpressionInitializer*) { return true; }
            virtual void LeaveExpressionInitializer(ExpressionInitializer*) {}
            virtual bool EnterArrayInitializer(ArrayInitializer*) { return true; }
            virtual void LeaveArrayInitializer(ArrayInitializer*) {}

            virtual bool EnterUnknownNode(std::unique_ptr<Node>&) { return true; }
            virtual void LeaveUnknownNode(std::unique_ptr<Node>&) {}

        private:
            bool rewrite_top_level_body(TopLevel* node) {
                bool proceed = true;
                AstDispatch<TopLevel, AstTopLevelTypes>::apply(node,
                    [&](auto* typed) {
                        using T = std::remove_cv_t<
                            std::remove_pointer_t<decltype(typed)>>;
                        proceed = rewrite_top_level_typed<T>(typed);
                    });
                return proceed;
            }

            template <typename T>
            bool rewrite_top_level_typed(T* node) {
                if constexpr (std::is_same_v<T, GuideStatement>) {
                    if (!EnterGuideStatement(node)) { return true; }
                    LeaveGuideStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ClibStatement>) {
                    if (!EnterClibStatement(node)) { return true; }
                    LeaveClibStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ExternDeclaration>) {
                    if (!EnterExternDeclaration(node)) { return true; }
                    if (!rewrite_type(node->return_type)) { return false; }
                    for (Type& parameter : node->parameters) {
                        if (!rewrite_type(parameter)) { return false; }
                    }
                    LeaveExternDeclaration(node);
                    return true;
                } else if constexpr (std::is_same_v<T, FunctionDefinition>) {
                    if (!EnterFunctionDefinition(node)) { return true; }
                    if (!rewrite_type(node->return_type)) { return false; }
                    for (Type& parameter : node->parameters) {
                        if (!rewrite_type(parameter)) { return false; }
                    }
                    if (!rewrite_type(node->conversion_target_type)) { return false; }
                    if (!rewrite_expressions(node->param_defaults)) { return false; }
                    if (!rewrite_statement(node->body)) { return false; }
                    LeaveFunctionDefinition(node);
                    return true;
                } else if constexpr (std::is_same_v<T, StructDefinition>) {
                    if (!EnterStructDefinition(node)) { return true; }
                    for (StructDefinition::Member& member : node->members) {
                        if (!rewrite_type(member.type)) { return false; }
                        if (member.function_pointer_type.has_value() &&
                            !rewrite_type(*member.function_pointer_type)) {
                            return false;
                        }
                        if (!rewrite_expression(member.array_size_expr)) {
                            return false;
                        }
                        if (!rewrite_initializer(member.initializer)) {
                            return false;
                        }
                    }
                    for (std::unique_ptr<SpecialMemberFunction>& special :
                        node->special_members) {
                        if (special == nullptr) { continue; }
                        if (!rewrite_type(special->parameter_type)) { return false; }
                        for (Type& parameter : special->parameters) {
                            if (!rewrite_type(parameter)) { return false; }
                        }
                        if (!rewrite_expressions(special->parameter_defaults)) {
                            return false;
                        }
                        if (!rewrite_statement(special->body)) { return false; }
                    }
                    LeaveStructDefinition(node);
                    return true;
                } else if constexpr (std::is_same_v<T, GenericDefinition>) {
                    if (!EnterGenericDefinition(node)) { return true; }
                    for (GenericParameter& parameter : node->parameters) {
                        if (!rewrite_type(parameter.constant_type)) { return false; }
                        for (Type& declared : parameter.expr_param_types) {
                            if (!rewrite_type(declared)) { return false; }
                        }
                        if (!rewrite_type(parameter.expr_return_type)) { return false; }
                    }
                    for (GenericPatternArg& pattern : node->patterns) {
                        if (!rewrite_type(pattern.type)) { return false; }
                    }
                    if (!rewrite_top_levels(node->members)) { return false; }
                    if (!rewrite_statements(node->compile_time_items)) { return false; }
                    LeaveGenericDefinition(node);
                    return true;
                } else if constexpr (std::is_same_v<T, InstantiationStatement>) {
                    if (!EnterInstantiationStatement(node)) { return true; }
                    if (!rewrite_generic_ref(node->reference)) { return false; }
                    LeaveInstantiationStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, NamespaceDefinition>) {
                    if (!EnterNamespaceDefinition(node)) { return true; }
                    if (!rewrite_top_levels(node->members)) { return false; }
                    LeaveNamespaceDefinition(node);
                    return true;
                } else if constexpr (std::is_same_v<T, AccessNamespaceStatement>) {
                    if (!EnterAccessNamespaceStatement(node)) { return true; }
                    LeaveAccessNamespaceStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, AdditionNamespaceStatement>) {
                    if (!EnterAdditionNamespaceStatement(node)) { return true; }
                    if (!rewrite_top_levels(node->members)) { return false; }
                    LeaveAdditionNamespaceStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, CondDefinition>) {
                    if (!EnterCondDefinition(node)) { return true; }
                    if (!rewrite_expression(node->value)) { return false; }
                    LeaveCondDefinition(node);
                    return true;
                } else if constexpr (std::is_same_v<T, UncondDefinition>) {
                    if (!EnterUncondDefinition(node)) { return true; }
                    LeaveUncondDefinition(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ConditionalBlock>) {
                    if (!EnterConditionalBlock(node)) { return true; }
                    if (!rewrite_expression(node->condition)) { return false; }
                    if (!rewrite_statement(node->then_block)) { return false; }
                    if (!rewrite_statement(node->else_block)) { return false; }
                    LeaveConditionalBlock(node);
                    return true;
                } else if constexpr (std::is_same_v<T, TopLevelBlock>) {
                    if (!EnterTopLevelBlock(node)) { return true; }
                    if (!rewrite_top_levels(node->items)) { return false; }
                    LeaveTopLevelBlock(node);
                    return true;
                } else if constexpr (std::is_same_v<T, VariableDeclaration>) {
                    return rewrite_variable_declaration(node);
                } else {
                    static_assert(ast_always_false<T>,
                        "unhandled AST TopLevel node type in AstRewriter");
                    return true;
                }
            }

            bool rewrite_statement_body(Statement* node) {
                bool proceed = true;
                AstDispatch<Statement, AstStatementTypes>::apply(node,
                    [&](auto* typed) {
                        using T = std::remove_cv_t<
                            std::remove_pointer_t<decltype(typed)>>;
                        proceed = rewrite_statement_typed<T>(typed);
                    });
                return proceed;
            }

            template <typename T>
            bool rewrite_statement_typed(T* node) {
                if constexpr (std::is_same_v<T, EmptyStatement>) {
                    if (!EnterEmptyStatement(node)) { return true; }
                    LeaveEmptyStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, Block>) {
                    if (!EnterBlock(node)) { return true; }
                    if (!rewrite_statements(node->statements)) { return false; }
                    LeaveBlock(node);
                    return true;
                } else if constexpr (std::is_same_v<T, IfStatement>) {
                    if (!EnterIfStatement(node)) { return true; }
                    if (!rewrite_expression(node->condition)) { return false; }
                    if (!rewrite_statement(node->then_block)) { return false; }
                    if (!rewrite_statement(node->else_block)) { return false; }
                    LeaveIfStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ForStatement>) {
                    if (!EnterForStatement(node)) { return true; }
                    if (!rewrite_statement(node->init)) { return false; }
                    if (!rewrite_expression(node->condition)) { return false; }
                    if (!rewrite_expression(node->step)) { return false; }
                    if (!rewrite_statement(node->body)) { return false; }
                    LeaveForStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, WhileStatement>) {
                    if (!EnterWhileStatement(node)) { return true; }
                    if (!rewrite_expression(node->condition)) { return false; }
                    if (!rewrite_statement(node->body)) { return false; }
                    LeaveWhileStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, BreakStatement>) {
                    if (!EnterBreakStatement(node)) { return true; }
                    LeaveBreakStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ReturnStatement>) {
                    if (!EnterReturnStatement(node)) { return true; }
                    if (!rewrite_expression(node->value)) { return false; }
                    LeaveReturnStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ExpressionStatement>) {
                    if (!EnterExpressionStatement(node)) { return true; }
                    if (!rewrite_expression(node->expr)) { return false; }
                    LeaveExpressionStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, DestructStatement>) {
                    if (!EnterDestructStatement(node)) { return true; }
                    if (!rewrite_expression(node->target)) { return false; }
                    LeaveDestructStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, EmitStatement>) {
                    if (!EnterEmitStatement(node)) { return true; }
                    if (!rewrite_expressions(node->pieces)) { return false; }
                    if (!rewrite_node_pointers(node->block_items)) { return false; }
                    LeaveEmitStatement(node);
                    return true;
                } else if constexpr (std::is_same_v<T, VariableDeclaration>) {
                    return rewrite_variable_declaration(node);
                } else if constexpr (std::is_same_v<T, StructDefinition>) {
                    return rewrite_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, GenericDefinition>) {
                    return rewrite_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, InstantiationStatement>) {
                    return rewrite_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, NamespaceDefinition>) {
                    return rewrite_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, AccessNamespaceStatement>) {
                    return rewrite_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, AdditionNamespaceStatement>) {
                    return rewrite_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, CondDefinition>) {
                    return rewrite_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, UncondDefinition>) {
                    return rewrite_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, ConditionalBlock>) {
                    return rewrite_top_level_typed(node);
                } else if constexpr (std::is_same_v<T, TopLevelBlock>) {
                    return rewrite_top_level_typed(node);
                } else {
                    static_assert(ast_always_false<T>,
                        "unhandled AST Statement node type in AstRewriter");
                    return true;
                }
            }

            template <typename T>
            bool rewrite_variable_declaration(T* node) {
                if (!EnterVariableDeclaration(node)) { return true; }
                if (!rewrite_type(node->type)) { return false; }
                if (node->function_pointer_type.has_value() &&
                    !rewrite_type(*node->function_pointer_type)) {
                    return false;
                }
                if (!rewrite_expression(node->array_size_expr)) { return false; }
                if (!rewrite_initializer(node->initializer)) { return false; }
                LeaveVariableDeclaration(node);
                return true;
            }

            bool rewrite_node_pointers(std::vector<std::unique_ptr<Node>>& nodes) {
                for (std::unique_ptr<Node>& node : nodes) {
                    if (stopped_) { return false; }
                    if (node == nullptr) { continue; }
                    if (auto* top = dynamic_cast<TopLevel*>(node.get())) {
                        node.release();
                        std::unique_ptr<TopLevel> holder(top);
                        const bool proceed = rewrite_top_level(holder);
                        node.reset(holder.release());

                        if (!proceed) { return false; }
                        continue;
                    }
                    if (auto* statement = dynamic_cast<Statement*>(node.get())) {
                        node.release();
                        std::unique_ptr<Statement> holder(statement);
                        const bool proceed = rewrite_statement(holder);
                        node.reset(holder.release());

                        if (!proceed) { return false; }
                        continue;
                    }
                    if (!EnterUnknownNode(node)) { continue; }
                    LeaveUnknownNode(node);
                }
                return !stopped_;
            }

            bool rewrite_expression_body(Expression* node) {
                bool proceed = true;
                AstDispatch<Expression, AstExpressionTypes>::apply(node,
                    [&](auto* typed) {
                        using T = std::remove_cv_t<
                            std::remove_pointer_t<decltype(typed)>>;
                        proceed = rewrite_expression_typed<T>(typed);
                    });
                return proceed;
            }

            template <typename T>
            bool rewrite_expression_typed(T* node) {
                if constexpr (std::is_same_v<T, AssignmentExpression>) {
                    if (!EnterAssignmentExpression(node)) { return true; }
                    if (!rewrite_expression(node->left)) { return false; }
                    if (!rewrite_expression(node->right)) { return false; }
                    LeaveAssignmentExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, LogicalOrExpression>) {
                    if (!EnterLogicalOrExpression(node)) { return true; }
                    if (!rewrite_expression(node->left)) { return false; }
                    if (!rewrite_expression(node->right)) { return false; }
                    LeaveLogicalOrExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, LogicalAndExpression>) {
                    if (!EnterLogicalAndExpression(node)) { return true; }
                    if (!rewrite_expression(node->left)) { return false; }
                    if (!rewrite_expression(node->right)) { return false; }
                    LeaveLogicalAndExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ConditionalExpression>) {
                    if (!EnterConditionalExpression(node)) { return true; }
                    if (!rewrite_expression(node->condition)) { return false; }
                    if (!rewrite_expression(node->then_expr)) { return false; }
                    if (!rewrite_expression(node->else_expr)) { return false; }
                    LeaveConditionalExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, BitwiseExpression>) {
                    if (!EnterBitwiseExpression(node)) { return true; }
                    if (!rewrite_expression(node->left)) { return false; }
                    if (!rewrite_expression(node->right)) { return false; }
                    LeaveBitwiseExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ComparisonExpression>) {
                    if (!EnterComparisonExpression(node)) { return true; }
                    if (!rewrite_expression(node->left)) { return false; }
                    if (!rewrite_expression(node->right)) { return false; }
                    LeaveComparisonExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ShiftExpression>) {
                    if (!EnterShiftExpression(node)) { return true; }
                    if (!rewrite_expression(node->left)) { return false; }
                    if (!rewrite_expression(node->right)) { return false; }
                    LeaveShiftExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, AdditiveExpression>) {
                    if (!EnterAdditiveExpression(node)) { return true; }
                    if (!rewrite_expression(node->left)) { return false; }
                    if (!rewrite_expression(node->right)) { return false; }
                    LeaveAdditiveExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, MultiplicativeExpression>) {
                    if (!EnterMultiplicativeExpression(node)) { return true; }
                    if (!rewrite_expression(node->left)) { return false; }
                    if (!rewrite_expression(node->right)) { return false; }
                    LeaveMultiplicativeExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, PowerExpression>) {
                    if (!EnterPowerExpression(node)) { return true; }
                    if (!rewrite_expression(node->left)) { return false; }
                    if (!rewrite_expression(node->right)) { return false; }
                    LeavePowerExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, UnaryExpression>) {
                    if (!EnterUnaryExpression(node)) { return true; }
                    if (!rewrite_expression(node->operand)) { return false; }
                    LeaveUnaryExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, PostfixExpression>) {
                    if (!EnterPostfixExpression(node)) { return true; }
                    if (!rewrite_type(node->cast_type)) { return false; }
                    if (!rewrite_expression(node->base)) { return false; }
                    if (!rewrite_expression(node->subscript_expr)) { return false; }
                    if (!rewrite_expressions(node->arguments)) { return false; }
                    LeavePostfixExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, CompileTimePropertyExpression>) {
                    if (!EnterCompileTimePropertyExpression(node)) { return true; }
                    if (!rewrite_expression(node->receiver)) { return false; }
                    if (!rewrite_expressions(node->arguments)) { return false; }
                    LeaveCompileTimePropertyExpression(node);
                    return true;
                } else if constexpr (std::is_same_v<T, PrimaryExpression>) {
                    if (!EnterPrimaryExpression(node)) { return true; }
                    if (!rewrite_type(node->heap_type)) { return false; }
                    if (!rewrite_type(node->construct_type)) { return false; }
                    if (!rewrite_expression(node->paren_expr)) { return false; }
                    if (!rewrite_expression(node->heap_size)) { return false; }
                    if (!rewrite_expression(node->placement_target)) { return false; }
                    if (!rewrite_expressions(node->construct_args)) { return false; }
                    if (node->generic_ref != nullptr &&
                        !rewrite_generic_ref(*node->generic_ref)) {
                        return false;
                    }
                    LeavePrimaryExpression(node);
                    return true;
                } else {
                    static_assert(ast_always_false<T>,
                        "unhandled AST Expression node type in AstRewriter");
                    return true;
                }
            }

            bool rewrite_initializer_body(Initializer* node) {
                bool proceed = true;
                AstDispatch<Initializer, AstInitializerTypes>::apply(node,
                    [&](auto* typed) {
                        using T = std::remove_cv_t<
                            std::remove_pointer_t<decltype(typed)>>;
                        proceed = rewrite_initializer_typed<T>(typed);
                    });
                return proceed;
            }

            template <typename T>
            bool rewrite_initializer_typed(T* node) {
                if constexpr (std::is_same_v<T, ExpressionInitializer>) {
                    if (!EnterExpressionInitializer(node)) { return true; }
                    if (!rewrite_expression(node->expr)) { return false; }
                    LeaveExpressionInitializer(node);
                    return true;
                } else if constexpr (std::is_same_v<T, ArrayInitializer>) {
                    if (!EnterArrayInitializer(node)) { return true; }
                    if (!rewrite_initializers(node->elements)) { return false; }
                    LeaveArrayInitializer(node);
                    return true;
                } else {
                    static_assert(ast_always_false<T>,
                        "unhandled AST Initializer node type in AstRewriter");
                    return true;
                }
            }

            bool stopped_ = false;
        };

    }
}

#endif
