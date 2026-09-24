#include "generic_expander.hpp"
#include "generic_expander_detail.hpp"
#include <string>
#include <vector>

using namespace gallt::AST;

namespace gallt {
    using namespace generic_expander_detail;

    GenericExpander::ConstantValue GenericExpander::pack_element_zero(
        const AST::Type& type) {
        ConstantValue value;
        if (type.kind == AST::TypeKind::String) {
            value.is_string = true;
            value.string_value.clear();
        } else if (type.kind == AST::TypeKind::Float ||
            type.kind == AST::TypeKind::Double) {
            value.is_float = true;
            value.float_value = 0.0;
        }
        return value;
    }

    std::unique_ptr<Expression> GenericExpander::make_bool_literal(
        SourceLocation loc, bool value) {
        lexeme_pool_.push_back(value ? std::string("true") : std::string("false"));
        Token literal(TokenType::BoolLiteral, loc, lexeme_pool_.back());
        return std::make_unique<PrimaryExpression>(loc, literal);
    }

    bool GenericExpander::is_pack_name(const Substitution& sub,
        const std::string& name) const {
        return sub.type_packs.find(name) != sub.type_packs.end() ||
            sub.const_packs.find(name) != sub.const_packs.end();
    }

    bool GenericExpander::pack_identifier_name(const AST::Expression* expr,
        std::string& out) {
        auto* prim = dynamic_cast<const PrimaryExpression*>(expr);
        if (prim == nullptr ||
            prim->kind != PrimaryExpression::Kind::Identifier) {
            return false;
        }
        out = prim->identifier;
        return true;
    }

    bool GenericExpander::pack_type_projection(const PostfixExpression* access,
        const Substitution& sub, std::string& out) {
        if (access == nullptr) {
            return false;
        }
        std::string pack_name;
        const Expression* element_index = nullptr;
        std::string element_property;
        bool is_element_access = false;
        if (access->op == PostfixExpression::Operator::Subscript) {
            is_element_access = pack_identifier_name(access->base.get(), pack_name);
            element_index = access->subscript_expr.get();
        } else if (access->op == PostfixExpression::Operator::Dot &&
            (access->member_name == "first" || access->member_name == "last")) {
            is_element_access = pack_identifier_name(access->base.get(), pack_name);
            element_property = access->member_name;
        }
        if (!is_element_access) {
            return false;
        }
        auto type_pack = sub.type_packs.find(pack_name);
        auto const_pack = sub.const_packs.find(pack_name);
        if (type_pack == sub.type_packs.end() &&
            const_pack == sub.const_packs.end()) {
            return false;
        }
        std::size_t index = 0;
        bool resolved = true;
        if (element_index != nullptr) {
            ConstantValue index_value;
            resolved = evaluate_with_substitution(element_index, sub, index_value);
            if (resolved) {
                const long long raw = index_value.is_float
                    ? static_cast<long long>(index_value.float_value)
                    : index_value.int_value;
                resolved = raw >= 0;
                index = resolved ? static_cast<std::size_t>(raw) : 0;
            }
        } else if (element_property == "last") {
            const std::size_t length = type_pack != sub.type_packs.end()
                ? type_pack->second.size() : const_pack->second.size();
            resolved = length != 0;
            index = length == 0 ? 0 : length - 1;
        } else if (element_property == "first") {
            const std::size_t length = type_pack != sub.type_packs.end()
                ? type_pack->second.size() : const_pack->second.size();
            resolved = length != 0;
        }
        if (type_pack != sub.type_packs.end()) {
            if (resolved && index < type_pack->second.size()) {
                out = type_pack->second[index].to_string();
                return true;
            }
        } else {
            auto declared = sub.const_pack_element.find(pack_name);
            if (resolved && declared != sub.const_pack_element.end()) {
                out = declared->second.to_string();
                return true;
            }
        }
        report(element_index != nullptr ? element_index->location
            : access->location, ErrorCode::ParameterPackPropertyNotApplicable,
            std::vector<std::string>{ "typename", pack_name });
        return false;
    }

    std::unique_ptr<Expression> GenericExpander::rewrite_pack_expression(
        const PostfixExpression* expr, const Substitution& sub) {
        if (expr == nullptr) { return nullptr; }
        if (expr->op == PostfixExpression::Operator::Dot &&
            expr->member_name == "typename") {
            if (auto* access = dynamic_cast<const PostfixExpression*>(
                    expr->base.get())) {
                std::string projected;
                if (pack_type_projection(access, sub, projected)) {
                    ConstantValue value;
                    value.is_string = true;
                    value.string_value = projected;
                    return make_constant_literal(expr->location, value);
                }
            }
        }
        std::string name;
        auto* prim = dynamic_cast<const PrimaryExpression*>(expr->base.get());
        if (expr->op == PostfixExpression::Operator::FunctionCall) {
            auto* member = dynamic_cast<const PostfixExpression*>(expr->base.get());
            if (member == nullptr ||
                member->op != PostfixExpression::Operator::Dot ||
                member->member_name != "get") {
                return nullptr;
            }
            prim = dynamic_cast<const PrimaryExpression*>(member->base.get());
        }
        if (prim == nullptr ||
            prim->kind != PrimaryExpression::Kind::Identifier) {
            return nullptr;
        }
        name = prim->identifier;
        auto fixed = sub.fixed_pack_members.find(name);
        if (fixed != sub.fixed_pack_members.end()) {
            const std::vector<std::string>& members = fixed->second;
            if (expr->op == PostfixExpression::Operator::Dot) {
                const std::string& property = expr->member_name;
                if (property == "length") {
                    return make_constant_literal(expr->location,
                        ConstantValue{ static_cast<long long>(members.size()), 0.0,
                            false, false, std::string() });
                }
            if (property == "empty") {
                return make_bool_literal(expr->location, members.empty());
            }
                if ((property == "first" || property == "last") &&
                    !members.empty()) {
                    const std::string& member = property == "first"
                        ? members.front() : members.back();
                    return std::make_unique<PrimaryExpression>(expr->location,
                        member);
                }
            }
            const Expression* index_expr =
                expr->op == PostfixExpression::Operator::Subscript
                ? expr->subscript_expr.get()
                : (expr->op == PostfixExpression::Operator::FunctionCall &&
                        expr->arguments.size() == 1
                    ? expr->arguments[0].get() : nullptr);
            if (index_expr != nullptr) {
                ConstantValue index;
                if (evaluate_with_substitution(index_expr, sub, index)) {
                    long long raw = index.is_float
                        ? static_cast<long long>(index.float_value)
                        : index.int_value;
                    if (raw >= 0 &&
                        static_cast<std::size_t>(raw) < members.size()) {
                        return std::make_unique<PrimaryExpression>(expr->location,
                            members[static_cast<std::size_t>(raw)]);
                    }
                }
            }
            report(expr->location, ErrorCode::ParameterPackPropertyNotApplicable,
                std::vector<std::string>{ expr->op ==
                        PostfixExpression::Operator::Dot
                    ? expr->member_name : std::string("element"), name });
            return make_constant_literal(expr->location,
                ConstantValue{ 0LL, 0.0, false, false, std::string() });
        }
        if (!is_pack_name(sub, name)) { return nullptr; }

        auto type_pack = sub.type_packs.find(name);
        auto const_pack = sub.const_packs.find(name);
        const std::size_t length = type_pack != sub.type_packs.end()
            ? type_pack->second.size()
            : (const_pack != sub.const_packs.end() ? const_pack->second.size() : 0);

        if (expr->op == PostfixExpression::Operator::Dot) {
            const std::string& property = expr->member_name;
            if (property == "length") {
                return make_constant_literal(expr->location,
                    ConstantValue{ static_cast<long long>(length), 0.0, false,
                        false, std::string() });
            }
            if (property == "empty") {
                return make_bool_literal(expr->location, length == 0);
            }
            if (property == "typename") {
                ConstantValue value;
                value.is_string = true;
                if (type_pack != sub.type_packs.end()) {
                    for (std::size_t i = 0; i < type_pack->second.size(); ++i) {
                        if (i != 0) { value.string_value += ", "; }
                        value.string_value += type_pack->second[i].to_string();
                    }
                } else {
                    auto element = sub.const_pack_element.find(name);
                    if (element != sub.const_pack_element.end()) {
                        value.string_value = element->second.to_string();
                    }
                }
                return make_constant_literal(expr->location, value);
            }
            if (property == "first" || property == "last") {
                if (const_pack == sub.const_packs.end()) {
                    report(expr->location, ErrorCode::ParameterPackPropertyNotApplicable,
                        std::vector<std::string>{ property, name });
                    return make_constant_literal(expr->location,
                        ConstantValue{ 0LL, 0.0, false, false, std::string() });
                }
                if (length == 0) {
                    AST::Type element;
                    auto found = sub.const_pack_element.find(name);
                    if (found != sub.const_pack_element.end()) {
                        element = found->second;
                    }
                    return make_constant_literal(expr->location,
                        pack_element_zero(element));
                }
                const std::size_t index = property == "first" ? 0 : length - 1;
                return make_constant_literal(expr->location,
                    const_pack->second[index]);
            }
            return nullptr;
        }

        if (expr->op == PostfixExpression::Operator::Subscript ||
            expr->op == PostfixExpression::Operator::FunctionCall) {
            const Expression* index_expr = expr->op == PostfixExpression::Operator::Subscript
                ? expr->subscript_expr.get()
                : (expr->arguments.size() == 1 ? expr->arguments[0].get() : nullptr);
            if (index_expr == nullptr) {
                report(expr->location, ErrorCode::ParameterPackPropertyNotApplicable,
                    std::vector<std::string>{ "get", name });
                return make_constant_literal(expr->location,
                    ConstantValue{ 0LL, 0.0, false, false, std::string() });
            }
            ConstantValue index;
            if (!evaluate_with_substitution(index_expr, sub, index)) {
                report(index_expr->location, ErrorCode::PackSubscriptNotInteger,
                    std::vector<std::string>{ expression_text(index_expr) });
                return make_constant_literal(expr->location,
                    ConstantValue{ 0LL, 0.0, false, false, std::string() });
            }
            long long raw = index.is_float
                ? static_cast<long long>(index.float_value) : index.int_value;
            if (type_pack != sub.type_packs.end()) {
                report(expr->location, ErrorCode::ParameterPackPropertyNotApplicable,
                    std::vector<std::string>{ "element", name });
                return make_constant_literal(expr->location,
                    ConstantValue{ 0LL, 0.0, false, false, std::string() });
            }
            if (const_pack == sub.const_packs.end()) {
                report(expr->location, ErrorCode::ParameterPackPropertyNotApplicable,
                    std::vector<std::string>{ "element", name });
                return make_constant_literal(expr->location,
                    ConstantValue{ 0LL, 0.0, false, false, std::string() });
            }
            if (raw < 0 || static_cast<std::size_t>(raw) >= length) {
                AST::Type element;
                auto found = sub.const_pack_element.find(name);
                if (found != sub.const_pack_element.end()) {
                    element = found->second;
                }
                return make_constant_literal(expr->location,
                    pack_element_zero(element));
            }
            return make_constant_literal(expr->location,
                const_pack->second[static_cast<std::size_t>(raw)]);
        }
        return nullptr;
    }

    void GenericExpander::expand_pack_arguments(
        std::vector<AST::GenericArgument>& arguments, const Substitution& sub,
        SourceLocation loc) {
        bool has_expansion = false;
        for (const GenericArgument& arg : arguments) {
            if (arg.is_pack_expansion) {
                has_expansion = true;
                break;
            }
        }
        if (!has_expansion) { return; }
        std::vector<AST::GenericArgument> expanded;
        expanded.reserve(arguments.size());
        for (GenericArgument& arg : arguments) {
            if (!arg.is_pack_expansion) {
                expanded.push_back(std::move(arg));
                continue;
            }
            auto type_pack = sub.type_packs.find(arg.pack_name);
            if (type_pack != sub.type_packs.end()) {
                for (const AST::Type& element : type_pack->second) {
                    GenericArgument item;
                    item.is_type = true;
                    item.type = element;
                    item.text = element.to_string();
                    expanded.push_back(std::move(item));
                }
                continue;
            }
            auto const_pack = sub.const_packs.find(arg.pack_name);
            if (const_pack != sub.const_packs.end()) {
                for (const ConstantValue& value : const_pack->second) {
                    std::unique_ptr<Expression> literal =
                        make_constant_literal(loc, value);
                    GenericArgument item;
                    item.is_type = false;
                    if (value.is_string) {
                        item.is_string_constant = true;
                        item.string_constant = value.string_value;
                        item.constant_actual_type = AST::Type::make_string();
                    } else if (value.is_float) {
                        item.float_constant = true;
                        item.float_value = value.float_value;
                    } else {
                        item.int_value = value.int_value;
                    }
                    item.expression = std::shared_ptr<Expression>(literal.release());
                    item.text = item.normalize();
                    expanded.push_back(std::move(item));
                }
                continue;
            }
            report(loc, ErrorCode::PackExpansionTargetNotPack,
                std::vector<std::string>{ arg.pack_name });
        }
        arguments = std::move(expanded);
    }

}
