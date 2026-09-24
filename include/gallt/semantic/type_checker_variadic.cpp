#include "type_checker.hpp"
#include "type_checker_detail.hpp"
#include <string>
#include <vector>

using namespace gallt::AST;

namespace gallt {

    const TypeChecker::VariadicPack* TypeChecker::find_variadic_pack(
        const std::string& name) const {
        for (auto it = variadic_packs_.rbegin(); it != variadic_packs_.rend(); ++it) {
            if (it->name == name) {
                return &(*it);
            }
        }

        return nullptr;
    }

    bool TypeChecker::pack_base_identifier(const AST::Expression* expr,
        std::string& out) {
        auto* prim = dynamic_cast<const AST::PrimaryExpression*>(expr);

        if (prim == nullptr ||
            prim->kind != AST::PrimaryExpression::Kind::Identifier) {
            return false;
        }

        out = prim->identifier;
        return true;
    }

    bool TypeChecker::pack_expansion_target(const AST::Expression* expr,
        const VariadicPack*& out, SourceLocation& loc) const {
        loc = expr != nullptr ? expr->location : SourceLocation{};
        std::string name;

        if (expr != nullptr) {
            if (auto* post = dynamic_cast<const AST::PostfixExpression*>(expr)) {
                if (post->op == AST::PostfixExpression::Operator::PackExpand) {
                    if (pack_base_identifier(post->base.get(), name)) {
                        out = find_variadic_pack(name);
                        return out != nullptr;
                    }

                    return false;
                }
            }

            if (pack_base_identifier(expr, name)) {
                out = find_variadic_pack(name);
                return out != nullptr;
            }
        }

        return false;
    }

    AST::Type TypeChecker::check_pack_property(AST::PostfixExpression* expr,
        const VariadicPack& pack) {
        const std::string& property = expr->member_name;
        if (property == "length") {
            return AST::Type::make_int();
        }
        if (property == "empty") {
            return AST::Type::make_bool();
        }
        if (property == "data") {
            return AST::Type::make_pointer(
                std::make_shared<AST::Type>(pack.element));
        }

        if (property == "first" || property == "last") {
            return pack.element;
        }

        diag_.report_error_template(expr->location,
            ErrorCode::ParameterPackPropertyNotApplicable,
            { property });
        return AST::Type::make_void();
    }

    AST::Type TypeChecker::check_pack_subscript(AST::PostfixExpression* expr,
        const VariadicPack& pack) {
        if (expr->subscript_expr != nullptr) {
            AST::Type index_type = check_expression(expr->subscript_expr.get());

            if (!is_integer_type(index_type)) {
                diag_.report_error_template(expr->subscript_expr->location,
                    ErrorCode::PackSubscriptNotInteger,
                    { index_type.to_string() });
            }
        }

        return pack.element;
    }

    AST::Type TypeChecker::check_pack_get(AST::PostfixExpression* call,
        const VariadicPack& pack) {
        if (call->arguments.size() != 1) {
            report_error(call->location, ErrorCode::FunctionArgCountMismatch,
                "parameter pack get expects exactly one argument");

            for (auto& arg : call->arguments) {
                check_expression(arg.get());
            }

            return pack.element;
        }

        AST::Type index_type = check_expression(call->arguments[0].get());

        if (!is_integer_type(index_type)) {
            diag_.report_error_template(call->arguments[0]->location,
                ErrorCode::PackSubscriptNotInteger,
                { index_type.to_string() });
        }

        return pack.element;
    }

    void TypeChecker::check_variadic_argument_list(AST::PostfixExpression* call,
        const std::vector<AST::Type>& fixed_types, const AST::Type& element,
        const AST::FunctionDefinition* node) {
        for (std::size_t i = 0; i < call->arguments.size(); ++i) {
            if (i < fixed_types.size()) {
                continue;
            }

            auto* post = dynamic_cast<AST::PostfixExpression*>(
                call->arguments[i].get());

            if (post != nullptr &&
                post->op == AST::PostfixExpression::Operator::PackExpand) {
                const VariadicPack* source = nullptr;
                SourceLocation loc = post->location;

                if (!pack_expansion_target(post, source, loc)) {
                    std::string name;
                    pack_base_identifier(post->base.get(), name);
                    diag_.report_error_template(post->location,
                        ErrorCode::PackExpansionTargetNotPack, { name });
                    continue;
                }

                if (!(source->element == element)) {
                    diag_.report_error_template(post->location,
                        ErrorCode::VariadicArgumentTypeMismatch,
                        { std::to_string(i + 1), element.to_string(),
                          source->element.to_string() });
                }

                continue;
            }

            AST::Type arg_type = check_expression(call->arguments[i].get());

            if (!can_implicit_convert(arg_type, element)) {
                diag_.report_error_template(call->arguments[i]->location,
                    ErrorCode::VariadicArgumentTypeMismatch,
                    { std::to_string(i + 1), element.to_string(),
                      arg_type.to_string() });
            }
        }

        (void)node;
    }

    bool TypeChecker::expression_mentions_variadic_pack(
        const AST::Expression* expr) const {
        if (expr == nullptr) { return false; }

        if (auto* prim = dynamic_cast<const AST::PrimaryExpression*>(expr)) {
            if (prim->kind == AST::PrimaryExpression::Kind::Identifier &&
                find_variadic_pack(prim->identifier) != nullptr) {
                return true;
            }

            return expression_mentions_variadic_pack(prim->paren_expr.get()) ||
                expression_mentions_variadic_pack(prim->heap_size.get()) ||
                expression_mentions_variadic_pack(prim->placement_target.get());
        }

        if (auto* post = dynamic_cast<const AST::PostfixExpression*>(expr)) {
            if (post->op == AST::PostfixExpression::Operator::PackExpand) {
                return true;
            }

            if (expression_mentions_variadic_pack(post->base.get()) ||
                expression_mentions_variadic_pack(post->subscript_expr.get())) {
                return true;
            }

            for (const auto& arg : post->arguments) {
                if (expression_mentions_variadic_pack(arg.get())) { return true; }
            }

            return false;
        }

        if (auto* bin = dynamic_cast<const AdditiveExpression*>(expr)) {
            return expression_mentions_variadic_pack(bin->left.get()) ||
                expression_mentions_variadic_pack(bin->right.get());
        }

        if (auto* bin = dynamic_cast<const MultiplicativeExpression*>(expr)) {
            return expression_mentions_variadic_pack(bin->left.get()) ||
                expression_mentions_variadic_pack(bin->right.get());
        }

        if (auto* bin = dynamic_cast<const BitwiseExpression*>(expr)) {
            return expression_mentions_variadic_pack(bin->left.get()) ||
                expression_mentions_variadic_pack(bin->right.get());
        }

        if (auto* bin = dynamic_cast<const ShiftExpression*>(expr)) {
            return expression_mentions_variadic_pack(bin->left.get()) ||
                expression_mentions_variadic_pack(bin->right.get());
        }

        if (auto* un = dynamic_cast<const UnaryExpression*>(expr)) {
            return expression_mentions_variadic_pack(un->operand.get());
        }

        if (auto* cast = dynamic_cast<const CompileTimePropertyExpression*>(expr)) {
            return expression_mentions_variadic_pack(cast->receiver.get());
        }

        return false;
    }

    AST::Type TypeChecker::function_type_of(const Symbol& symbol) {
        if (symbol.is_variadic && !symbol.param_types.empty()) {
            std::vector<AST::Type> fixed(symbol.param_types.begin(),
                symbol.param_types.end() - 1);
            return AST::Type::make_function(
                std::make_shared<AST::Type>(symbol.type), fixed, true,
                std::make_shared<AST::Type>(symbol.param_types.back()));
        }

        return AST::Type::make_function(
            std::make_shared<AST::Type>(symbol.type), symbol.param_types);
    }

    bool TypeChecker::report_expansion_against_fixed_signature(
        AST::PostfixExpression* call, const std::vector<AST::Type>& parameter_types) {
        bool reported = false;

        for (std::size_t i = 0; i < call->arguments.size(); ++i) {
            auto* post = dynamic_cast<AST::PostfixExpression*>(call->arguments[i].get());
            if (post == nullptr ||
                post->op != AST::PostfixExpression::Operator::PackExpand) {
                continue;
            }

            std::string name;
            const AST::Expression* base = post->base.get();
            pack_base_identifier(base, name);
            const VariadicPack* source = find_variadic_pack(name);

            if (source != nullptr && i < parameter_types.size() &&
                !can_implicit_convert(source->element, parameter_types[i])) {
                diag_.report_error_template(post->location,
                    ErrorCode::FunctionArgTypeMismatch,
                    { std::to_string(i + 1), parameter_types[i].to_string(),
                      source->element.to_string() });
                reported = true;
                continue;
            }

            if (!reported) {
                report_error(call->location, ErrorCode::FunctionArgCountMismatch,
                    "a parameter pack expansion cannot fill the fixed parameter list of this function");
                reported = true;
            }
        }

        return reported;
    }

    bool TypeChecker::report_runtime_pack_expansion_in_initializer(
        const AST::ArrayInitializer* init) {
        if (init == nullptr) { return false; }
        bool reported = false;

        for (const auto& item : init->elements) {
            auto* expression = dynamic_cast<const AST::ExpressionInitializer*>(item.get());
            if (expression == nullptr) {
                continue;
            }

            auto* post = dynamic_cast<const AST::PostfixExpression*>(
                expression->expr.get());
            if (post == nullptr ||
                post->op != AST::PostfixExpression::Operator::PackExpand) {
                continue;
            }

            if (reported) {
                continue;
            }

            std::string name;
            pack_base_identifier(post->base.get(), name);

            if (find_variadic_pack(name) == nullptr) {
                diag_.report_error_template(post->location,
                    ErrorCode::PackExpansionTargetNotPack, { name });
                reported = true;
                continue;
            }

            report_error(post->location,
                ErrorCode::ParameterPackInConstantExpression,
                "a runtime parameter pack expansion cannot initialize a fixed-length initializer list");
            reported = true;
        }

        return reported;
    }

}
