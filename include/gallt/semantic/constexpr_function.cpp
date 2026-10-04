#include "constexpr_function.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace gallt::AST;

namespace gallt {

    namespace {

        constexpr std::size_t kMaxCallDepth = 48;
        constexpr std::size_t kMaxSteps = 200000;
        constexpr long long kMaxIntegerExponent = 64;

        bool decode_escaped_text(std::string_view lexeme, char quote,
            std::string& out) {
            if (is_literal_string_lexeme(lexeme)) {
                out.assign(literal_string_content(lexeme));
                return true;
            }

            std::string_view content = lexeme;
            if (content.size() >= 2 && content.front() == quote &&
                content.back() == quote) {
                content = content.substr(1, content.size() - 2);
            }

            out.clear();

            for (std::size_t i = 0; i < content.size(); ++i) {
                const char c = content[i];

                if (c != '\\') {
                    out.push_back(c);
                    continue;
                }

                if (++i >= content.size()) { return false; }
                const char escape = content[i];

                switch (escape) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'v': out.push_back('\v'); break;
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                case '\'': out.push_back('\''); break;
                case 'x': {
                    int value = 0;
                    int digits = 0;

                    while (i + 1 < content.size() && digits < 2) {
                        const char h = content[i + 1];
                        int digit = -1;
                        if (h >= '0' && h <= '9') {
                            digit = h - '0';
                        } else if (h >= 'a' && h <= 'f') {
                            digit = h - 'a' + 10;
                        } else if (h >= 'A' && h <= 'F') {
                            digit = h - 'A' + 10;
                        }
                        if (digit < 0) { break; }
                        ++i;
                        value = value * 16 + digit;
                        ++digits;
                    }

                    if (digits == 0) { return false; }
                    out.push_back(static_cast<char>(value));
                    break;
                }
                default:
                    if (escape >= '0' && escape <= '7') {
                        int value = escape - '0';
                        int digits = 1;

                        while (i + 1 < content.size() && digits < 3 &&
                            content[i + 1] >= '0' && content[i + 1] <= '7') {
                            ++i;
                            value = value * 8 + (content[i] - '0');
                            ++digits;
                        }

                        out.push_back(static_cast<char>(value));
                    } else {
                        out.push_back(escape);
                    }
                    break;
                }
            }

            return true;
        }

        bool decode_integer_literal(std::string_view lexeme, long long& out) {
            std::string_view text = Type::strip_integer_suffix(lexeme);
            int base = 10;

            if (text.size() > 2 && text[0] == '0' &&
                (text[1] == 'x' || text[1] == 'X')) {
                base = 16;
                text = text.substr(2);
            } else if (text.size() > 1 && text[0] == '0') {
                base = 8;
                text = text.substr(1);
            }

            if (text.empty()) { return false; }
            long long value = 0;
            auto result = std::from_chars(text.data(), text.data() + text.size(),
                value, base);
            if (result.ec != std::errc()) { return false; }
            out = value;
            return true;
        }

        bool decode_float_literal(std::string_view lexeme, double& out) {
            std::string text(lexeme);
            if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) {
                text.pop_back();
            }

            if (text.empty()) { return false; }
            std::size_t consumed = 0;

            try {
                out = std::stod(text, &consumed);
            } catch (...) {
                return false;
            }

            return consumed == text.size();
        }

        long long load_integer(const ConstexprValue& value) {
            return value.type.is_floating()
                ? static_cast<long long>(value.float_value) : value.int_value;
        }

        double load_floating(const ConstexprValue& value) {
            return value.type.is_floating()
                ? value.float_value : static_cast<double>(value.int_value);
        }

        std::string display_text_of(const AST::Expression* expr) {
            if (expr == nullptr) { return std::string(); }
            if (auto* primary = dynamic_cast<const PrimaryExpression*>(expr)) {
                if (primary->kind == PrimaryExpression::Kind::Identifier) {
                    return primary->identifier;
                }
            }
            return std::string();
        }

    }

    ConstexprValue ConstexprValue::invalid() {
        ConstexprValue value;
        value.type = Type::make_error();
        return value;
    }

    ConstexprValue ConstexprValue::make_void() {
        ConstexprValue value;
        value.type = Type::make_void();
        return value;
    }

    ConstexprValue ConstexprValue::make_integer(long long number,
        const AST::Type& type) {
        ConstexprValue value;
        value.type = type;
        value.int_value = number;
        value.float_value = static_cast<double>(number);
        return value;
    }

    ConstexprValue ConstexprValue::make_floating(double number,
        const AST::Type& type) {
        ConstexprValue value;
        value.type = type;
        value.float_value = number;
        value.int_value = static_cast<long long>(number);
        return value;
    }

    ConstexprValue ConstexprValue::make_bool(bool flag) {
        return make_integer(flag ? 1 : 0, Type::make_bool());
    }

    ConstexprValue ConstexprValue::make_string(std::string text) {
        ConstexprValue value;
        value.type = Type::make_string();
        value.string_value = std::move(text);
        return value;
    }

    ConstexprValue ConstexprValue::make_struct(std::string name,
        std::vector<ConstexprValue> members) {
        ConstexprValue value;
        value.type = Type::make_struct(name);
        value.elements = std::move(members);
        return value;
    }

    ConstexprValue ConstexprValue::make_array(const AST::Type& element_type,
        std::vector<ConstexprValue> elements) {
        ConstexprValue value;
        value.type = Type::make_array(
            std::make_shared<AST::Type>(element_type), elements.size());
        value.elements = std::move(elements);
        return value;
    }

    bool ConstexprValue::as_bool() const {
        if (type.is_floating()) { return float_value != 0.0; }
        if (type.kind == TypeKind::String) { return !string_value.empty(); }
        return int_value != 0;
    }

    long long ConstexprValue::as_integer() const {
        return type.is_floating() ? static_cast<long long>(float_value)
                                  : int_value;
    }

    double ConstexprValue::as_floating() const {
        return type.is_floating() ? float_value
                                  : static_cast<double>(int_value);
    }

    ConstexprInterpreter::ConstexprInterpreter(Host host) : host_(std::move(host)) {
    }

    bool ConstexprInterpreter::fail(ConstexprFailure failure) {
        if (failure_ == ConstexprFailure::None ||
            failure == ConstexprFailure::LimitExceeded) {
            failure_ = failure;
        }
        return false;
    }

    bool ConstexprInterpreter::step() {
        if (steps_ >= kMaxSteps) {
            return fail(ConstexprFailure::LimitExceeded);
        }
        ++steps_;
        return true;
    }

    long long ConstexprInterpreter::wrap_integer(long long value,
        AST::TypeKind kind) {
        switch (kind) {
        case TypeKind::Int:
            return static_cast<long long>(static_cast<int>(value));
        case TypeKind::Uint:
            return static_cast<long long>(static_cast<unsigned int>(value));
        case TypeKind::Luint:
            return static_cast<long long>(
                static_cast<unsigned long long>(value));
        case TypeKind::Char:
        case TypeKind::Uchar:
            return static_cast<long long>(static_cast<unsigned char>(value));
        case TypeKind::Bool:
            return value != 0 ? 1 : 0;
        case TypeKind::Lint:
        default:
            return value;
        }
    }

    ConstexprValue ConstexprInterpreter::wrap_numeric(const ConstexprValue& value) {
        if (value.type.is_floating()) {
            const double number = value.type.kind == TypeKind::Float
                ? static_cast<double>(static_cast<float>(value.float_value))
                : value.float_value;
            return ConstexprValue::make_floating(number, value.type);
        }

        if (!value.type.is_integer()) { return value; }
        ConstexprValue out = ConstexprValue::make_integer(
            wrap_integer(value.int_value, value.type.kind), value.type);
        out.float_value = static_cast<double>(out.int_value);
        return out;
    }

    AST::Type ConstexprInterpreter::arithmetic_result_type(
        const ConstexprValue& left, const ConstexprValue& right) {
        AST::Type result = left.type.promotion_rank() >= right.type.promotion_rank()
            ? left.type : right.type;
        result.is_const = false;
        return result;
    }

    bool ConstexprInterpreter::coerce(const ConstexprValue& value,
        const AST::Type& target, ConstexprValue& out) {
        AST::Type type = target;
        type.is_const = false;

        if (type.kind == TypeKind::Void) {
            out = ConstexprValue::make_void();
            return true;
        }

        if (type.kind == TypeKind::String) {
            if (value.type.kind != TypeKind::String) { return false; }
            out = value;
            out.type = type;
            return true;
        }

        if (type.kind == TypeKind::Struct || type.kind == TypeKind::Array) {
            if (value.type.kind != type.kind) { return false; }
            if (type.kind == TypeKind::Struct &&
                value.type.struct_name != type.struct_name) {
                return false;
            }
            out = value;
            out.type = type;
            return true;
        }

        if (type.is_floating()) {
            if (!value.type.is_arithmetic()) { return false; }
            const double number = value.as_floating();
            out = ConstexprValue::make_floating(number, type);
            return true;
        }

        if (type.is_integer()) {
            if (value.type.kind == TypeKind::String) { return false; }
            if (!value.type.is_arithmetic()) { return false; }
            const long long number = value.type.is_floating()
                ? static_cast<long long>(value.float_value) : value.int_value;
            out = ConstexprValue::make_integer(wrap_integer(number, type.kind), type);
            return true;
        }

        return false;
    }

    bool ConstexprInterpreter::is_compilable_type(const AST::Type& type,
        const std::function<const AST::StructDefinition*(const std::string&)>&
            lookup) {
        switch (type.kind) {
        case TypeKind::Int:
        case TypeKind::Lint:
        case TypeKind::Uint:
        case TypeKind::Luint:
        case TypeKind::Float:
        case TypeKind::Double:
        case TypeKind::Char:
        case TypeKind::Uchar:
        case TypeKind::Bool:
        case TypeKind::String:
        case TypeKind::Void:
            return true;
        case TypeKind::Array:
            if (!type.array_size.has_value() || type.element_type == nullptr) {
                return false;
            }
            return is_compilable_type(*type.element_type, lookup);
        case TypeKind::Struct: {
            const AST::StructDefinition* definition = lookup
                ? lookup(type.struct_name) : nullptr;
            if (definition == nullptr) { return false; }

            for (const StructDefinition::Member& member : definition->members) {
                if (!is_compilable_type(member.type, lookup)) { return false; }
            }

            return true;
        }
        default:
            return false;
        }
    }

    bool ConstexprInterpreter::default_value(const AST::Type& type,
        ConstexprValue& out) {
        switch (type.kind) {
        case TypeKind::Int:
        case TypeKind::Lint:
        case TypeKind::Uint:
        case TypeKind::Luint:
        case TypeKind::Char:
        case TypeKind::Uchar:
        case TypeKind::Bool:
            out = ConstexprValue::make_integer(0, type);
            return true;
        case TypeKind::Float:
        case TypeKind::Double:
            out = ConstexprValue::make_floating(0.0, type);
            return true;
        case TypeKind::String:
            out = ConstexprValue::make_string(std::string());
            out.type = type;
            return true;
        case TypeKind::Array: {
            if (type.element_type == nullptr) { return false; }
            const std::size_t count = type.array_size.value_or(0);
            std::vector<ConstexprValue> elements;
            elements.reserve(count);

            for (std::size_t i = 0; i < count; ++i) {
                ConstexprValue element;
                if (!default_value(*type.element_type, element)) { return false; }
                elements.push_back(std::move(element));
            }

            out = ConstexprValue::make_array(*type.element_type,
                std::move(elements));
            out.type = type;
            return true;
        }
        case TypeKind::Struct: {
            const AST::StructDefinition* definition = host_.lookup_struct
                ? host_.lookup_struct(type.struct_name) : nullptr;
            if (definition == nullptr) { return false; }
            std::vector<ConstexprValue> members;
            members.reserve(definition->members.size());

            for (const StructDefinition::Member& member : definition->members) {
                ConstexprValue value;
                if (!default_value(member.type, value)) { return false; }
                members.push_back(std::move(value));
            }

            out = ConstexprValue::make_struct(type.struct_name,
                std::move(members));
            out.type = type;
            return true;
        }
        case TypeKind::Void:
            out = ConstexprValue::make_void();
            return true;
        default:
            return false;
        }
    }

    ConstexprValue* ConstexprInterpreter::lookup_local(const std::string& name) {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            auto found = scope->find(name);
            if (found != scope->end()) { return &found->second; }
        }

        return nullptr;
    }

    ConstexprValue* ConstexprInterpreter::struct_member(ConstexprValue& container,
        const std::string& member) {
        if (container.type.kind != TypeKind::Struct) { return nullptr; }
        const AST::StructDefinition* definition = host_.lookup_struct
            ? host_.lookup_struct(container.type.struct_name) : nullptr;
        if (definition == nullptr) { return nullptr; }

        for (std::size_t i = 0; i < definition->members.size(); ++i) {
            if (definition->members[i].name == member) {
                if (i >= container.elements.size()) { return nullptr; }
                return &container.elements[i];
            }
        }

        return nullptr;
    }

    ConstexprValue* ConstexprInterpreter::lvalue(const AST::Expression* expr) {
        if (expr == nullptr) { return nullptr; }

        if (auto* primary = dynamic_cast<const PrimaryExpression*>(expr)) {
            if (primary->kind == PrimaryExpression::Kind::Identifier) {
                return lookup_local(primary->identifier);
            }
            if (primary->kind == PrimaryExpression::Kind::Parens) {
                return lvalue(primary->paren_expr.get());
            }
            return nullptr;
        }

        if (auto* postfix = dynamic_cast<const PostfixExpression*>(expr)) {
            if (postfix->op == PostfixExpression::Operator::Dot) {
                ConstexprValue* base = lvalue(postfix->base.get());
                if (base == nullptr) { return nullptr; }
                return struct_member(*base, postfix->member_name);
            }
            if (postfix->op == PostfixExpression::Operator::Subscript) {
                ConstexprValue* base = lvalue(postfix->base.get());
                if (base == nullptr || base->type.kind != TypeKind::Array) {
                    return nullptr;
                }
                ConstexprValue index;
                if (!eval(postfix->subscript_expr.get(), index)) { return nullptr; }
                const long long position = index.as_integer();
                if (position < 0 ||
                    static_cast<std::size_t>(position) >= base->elements.size()) {
                    return nullptr;
                }
                return &base->elements[static_cast<std::size_t>(position)];
            }
        }

        return nullptr;
    }

    bool ConstexprInterpreter::apply_arithmetic(ArithmeticOp op,
        const ConstexprValue& left, const ConstexprValue& right,
        ConstexprValue& out) {
        const AST::Type result_type = arithmetic_result_type(left, right);

        if (op == ArithmeticOp::Add && left.type.kind == TypeKind::String &&
            right.type.kind == TypeKind::String) {
            ConstexprValue value = ConstexprValue::make_string(
                left.string_value + right.string_value);
            value.type = result_type;
            out = value;
            return true;
        }

        if (op == ArithmeticOp::Add && (left.type.kind == TypeKind::String ||
            right.type.kind == TypeKind::String)) {
            return fail(ConstexprFailure::Unsupported);
        }

        if (!left.type.is_arithmetic() || !right.type.is_arithmetic()) {
            return fail(ConstexprFailure::Unsupported);
        }

        if (result_type.is_floating()) {
            const double a = left.as_floating();
            const double b = right.as_floating();
            double result = 0.0;

            switch (op) {
            case ArithmeticOp::Add: result = a + b; break;
            case ArithmeticOp::Subtract: result = a - b; break;
            case ArithmeticOp::Multiply: result = a * b; break;
            case ArithmeticOp::Divide:
                if (b == 0.0) { return fail(ConstexprFailure::Unsupported); }
                result = a / b;
                break;
            case ArithmeticOp::Remainder:
                if (b == 0.0) { return fail(ConstexprFailure::Unsupported); }
                result = std::fmod(a, b);
                break;
            case ArithmeticOp::Power:
                result = std::pow(a, b);
                break;
            }

            out = ConstexprValue::make_floating(result, result_type);
            return true;
        }

        const long long a = left.as_integer();
        const long long b = right.as_integer();
        long long result = 0;

        switch (op) {
        case ArithmeticOp::Add: result = a + b; break;
        case ArithmeticOp::Subtract: result = a - b; break;
        case ArithmeticOp::Multiply: result = a * b; break;
        case ArithmeticOp::Divide:
            if (b == 0) { return fail(ConstexprFailure::Unsupported); }
            if (result_type.is_unsigned_integer()) {
                result = static_cast<long long>(
                    static_cast<unsigned long long>(a) /
                    static_cast<unsigned long long>(b));
            } else {
                result = a / b;
            }
            break;
        case ArithmeticOp::Remainder:
            if (b == 0) { return fail(ConstexprFailure::Unsupported); }
            result = a % b;
            break;
        case ArithmeticOp::Power: {
            if (b < 0 || b > kMaxIntegerExponent) {
                return fail(ConstexprFailure::Unsupported);
            }
            result = 1;

            for (long long i = 0; i < b; ++i) {
                result = wrap_integer(result * a, result_type.kind);
            }
            break;
        }
        }

        out = ConstexprValue::make_integer(wrap_integer(result, result_type.kind),
            result_type);
        return true;
    }

    bool ConstexprInterpreter::apply_comparison(CompareOp op,
        const ConstexprValue& left, const ConstexprValue& right,
        ConstexprValue& out) {
        bool result = false;

        if (left.type.kind == TypeKind::String ||
            right.type.kind == TypeKind::String) {
            if (left.type.kind != TypeKind::String ||
                right.type.kind != TypeKind::String) {
                return fail(ConstexprFailure::Unsupported);
            }

            const int order = left.string_value.compare(right.string_value);

            switch (op) {
            case CompareOp::Equal: result = order == 0; break;
            case CompareOp::NotEqual: result = order != 0; break;
            case CompareOp::Greater: result = order > 0; break;
            case CompareOp::Less: result = order < 0; break;
            case CompareOp::GreaterEqual: result = order >= 0; break;
            case CompareOp::LessEqual: result = order <= 0; break;
            }

            out = ConstexprValue::make_bool(result);
            return true;
        }

        if (!left.type.is_arithmetic() || !right.type.is_arithmetic()) {
            return fail(ConstexprFailure::Unsupported);
        }

        if (left.type.is_floating() || right.type.is_floating()) {
            const double a = left.as_floating();
            const double b = right.as_floating();

            switch (op) {
            case CompareOp::Equal: result = a == b; break;
            case CompareOp::NotEqual: result = a != b; break;
            case CompareOp::Greater: result = a > b; break;
            case CompareOp::Less: result = a < b; break;
            case CompareOp::GreaterEqual: result = a >= b; break;
            case CompareOp::LessEqual: result = a <= b; break;
            }
        } else {
            const long long a = left.int_value;
            const long long b = right.int_value;

            switch (op) {
            case CompareOp::Equal: result = a == b; break;
            case CompareOp::NotEqual: result = a != b; break;
            case CompareOp::Greater: result = a > b; break;
            case CompareOp::Less: result = a < b; break;
            case CompareOp::GreaterEqual: result = a >= b; break;
            case CompareOp::LessEqual: result = a <= b; break;
            }
        }

        out = ConstexprValue::make_bool(result);
        return true;
    }

    bool ConstexprInterpreter::apply_bitwise(BitwiseOp op,
        const ConstexprValue& left, const ConstexprValue& right,
        ConstexprValue& out) {
        if (!left.type.is_integer() || !right.type.is_integer()) {
            return fail(ConstexprFailure::Unsupported);
        }

        const AST::Type result_type = arithmetic_result_type(left, right);
        const long long a = left.int_value;
        const long long b = right.int_value;
        long long result = 0;

        switch (op) {
        case BitwiseOp::And: result = a & b; break;
        case BitwiseOp::Xor: result = a ^ b; break;
        case BitwiseOp::Or: result = a | b; break;
        }

        out = ConstexprValue::make_integer(wrap_integer(result, result_type.kind),
            result_type);
        return true;
    }

    bool ConstexprInterpreter::apply_shift(ShiftOp op,
        const ConstexprValue& left, const ConstexprValue& right,
        ConstexprValue& out) {
        if (!left.type.is_integer() || !right.type.is_integer()) {
            return fail(ConstexprFailure::Unsupported);
        }

        const long long count = right.int_value;
        if (count < 0 || count >= 64) {
            return fail(ConstexprFailure::Unsupported);
        }

        AST::Type result_type = left.type;
        result_type.is_const = false;

        unsigned long long base = 0;
        if (left.type.is_unsigned_integer()) {
            base = static_cast<unsigned long long>(left.int_value);
        } else {
            base = static_cast<unsigned long long>(left.int_value);
        }

        unsigned long long result = op == ShiftOp::Left
            ? (base << count)
            : (left.type.is_unsigned_integer()
                ? (base >> count)
                : static_cast<unsigned long long>(
                    static_cast<long long>(left.int_value) >> count));

        out = ConstexprValue::make_integer(
            wrap_integer(static_cast<long long>(result), result_type.kind),
            result_type);
        return true;
    }

    bool ConstexprInterpreter::apply_unary(int op, const ConstexprValue& operand,
        ConstexprValue& out) {
        switch (static_cast<UnaryExpression::Operator>(op)) {
        case UnaryExpression::Operator::UnaryPlus:
            if (!operand.type.is_arithmetic()) {
                return fail(ConstexprFailure::Unsupported);
            }
            out = wrap_numeric(operand);
            return true;
        case UnaryExpression::Operator::UnaryMinus: {
            if (!operand.type.is_arithmetic()) {
                return fail(ConstexprFailure::Unsupported);
            }
            if (operand.type.is_floating()) {
                out = ConstexprValue::make_floating(-operand.as_floating(),
                    operand.type);
            } else {
                out = ConstexprValue::make_integer(
                    wrap_integer(-operand.int_value, operand.type.kind),
                    operand.type);
            }
            return true;
        }
        case UnaryExpression::Operator::LogicalNot:
            if (!operand.type.is_arithmetic() &&
                operand.type.kind != TypeKind::String) {
                return fail(ConstexprFailure::Unsupported);
            }
            out = ConstexprValue::make_bool(!operand.as_bool());
            return true;
        case UnaryExpression::Operator::BitwiseNot:
            if (!operand.type.is_integer()) {
                return fail(ConstexprFailure::Unsupported);
            }
            out = ConstexprValue::make_integer(
                wrap_integer(~operand.int_value, operand.type.kind),
                operand.type);
            return true;
        default:
            return fail(ConstexprFailure::Unsupported);
        }
    }

    bool ConstexprInterpreter::evaluate(const AST::Expression* expr,
        ConstexprValue& out) {
        scopes_.clear();
        steps_ = 0;
        return eval(expr, out);
    }

    bool ConstexprInterpreter::evaluate_initializer(const AST::Initializer* init,
        const AST::Type& type, ConstexprValue& out) {
        scopes_.clear();
        steps_ = 0;
        return eval_initializer(init, type, out);
    }

    bool ConstexprInterpreter::call(const AST::FunctionDefinition* function,
        const std::vector<ConstexprValue>& arguments, ConstexprValue& out) {
        if (function == nullptr || function->body == nullptr) {
            return fail(ConstexprFailure::Unsupported);
        }

        if (call_depth_ >= kMaxCallDepth) {
            return fail(ConstexprFailure::LimitExceeded);
        }

        if (call_depth_ == 0) {
            scopes_.clear();
            steps_ = 0;
        }

        ++call_depth_;
        scopes_.emplace_back();

        for (std::size_t i = 0; i < function->parameters.size(); ++i) {
            const std::string name = i < function->param_names.size()
                ? function->param_names[i] : std::string();
            if (name.empty()) { continue; }

            ConstexprValue converted;
            if (i >= arguments.size() ||
                !coerce(arguments[i], function->parameters[i], converted)) {
                scopes_.pop_back();
                --call_depth_;
                return fail(ConstexprFailure::Unsupported);
            }

            scopes_.back()[name] = std::move(converted);
        }

        Control control = exec_statement(function->body.get());
        scopes_.pop_back();
        --call_depth_;

        if (failure_ != ConstexprFailure::None) { return false; }

        if (control.kind == Control::Kind::Return) {
            ConstexprValue converted;
            if (coerce(control.value, function->return_type, converted)) {
                out = std::move(converted);
                return true;
            }
            return fail(ConstexprFailure::Unsupported);
        }

        if (function->return_type.kind == TypeKind::Void) {
            out = ConstexprValue::make_void();
            return true;
        }

        return fail(ConstexprFailure::Unsupported);
    }

    bool ConstexprInterpreter::eval(const AST::Expression* expr,
        ConstexprValue& out) {
        if (expr == nullptr) { return fail(ConstexprFailure::Unsupported); }
        if (!step()) { return false; }

        if (auto* primary = dynamic_cast<const PrimaryExpression*>(expr)) {
            return eval_primary(primary, out);
        }

        if (auto* assignment = dynamic_cast<const AssignmentExpression*>(expr)) {
            ConstexprValue right;
            if (!eval(assignment->right.get(), right)) { return false; }
            ConstexprValue* target = lvalue(assignment->left.get());
            if (target == nullptr) { return fail(ConstexprFailure::Unsupported); }

            if (assignment->op == AssignmentExpression::Operator::Assign) {
                ConstexprValue converted;
                if (!coerce(right, target->type, converted)) {
                    return fail(ConstexprFailure::Unsupported);
                }
                *target = std::move(converted);
                out = *target;
                return true;
            }

            ConstexprValue result;
            switch (assignment->op) {
            case AssignmentExpression::Operator::PlusAssign:
                if (!apply_arithmetic(ArithmeticOp::Add, *target, right, result)) {
                    return false;
                }
                break;
            case AssignmentExpression::Operator::MinusAssign:
                if (!apply_arithmetic(ArithmeticOp::Subtract, *target, right,
                    result)) {
                    return false;
                }
                break;
            case AssignmentExpression::Operator::AndAssign:
                if (!apply_bitwise(BitwiseOp::And, *target, right, result)) {
                    return false;
                }
                break;
            case AssignmentExpression::Operator::OrAssign:
                if (!apply_bitwise(BitwiseOp::Or, *target, right, result)) {
                    return false;
                }
                break;
            case AssignmentExpression::Operator::XorAssign:
                if (!apply_bitwise(BitwiseOp::Xor, *target, right, result)) {
                    return false;
                }
                break;
            case AssignmentExpression::Operator::ShiftLeftAssign:
                if (!apply_shift(ShiftOp::Left, *target, right, result)) {
                    return false;
                }
                break;
            case AssignmentExpression::Operator::ShiftRightAssign:
                if (!apply_shift(ShiftOp::Right, *target, right, result)) {
                    return false;
                }
                break;
            case AssignmentExpression::Operator::Assign:
                break;
            }

            ConstexprValue converted;
            if (!coerce(result, target->type, converted)) {
                return fail(ConstexprFailure::Unsupported);
            }
            *target = std::move(converted);
            out = *target;
            return true;
        }

        if (auto* expression = dynamic_cast<const LogicalOrExpression*>(expr)) {
            bool left = false;
            if (!evaluate_condition(expression->left.get(), left)) { return false; }
            if (left) { out = ConstexprValue::make_bool(true); return true; }
            bool right = false;
            if (!evaluate_condition(expression->right.get(), right)) { return false; }
            out = ConstexprValue::make_bool(right);
            return true;
        }

        if (auto* expression = dynamic_cast<const LogicalAndExpression*>(expr)) {
            bool left = false;
            if (!evaluate_condition(expression->left.get(), left)) { return false; }
            if (!left) { out = ConstexprValue::make_bool(false); return true; }
            bool right = false;
            if (!evaluate_condition(expression->right.get(), right)) { return false; }
            out = ConstexprValue::make_bool(right);
            return true;
        }

        if (auto* expression = dynamic_cast<const ConditionalExpression*>(expr)) {
            bool condition = false;
            if (!evaluate_condition(expression->condition.get(), condition)) {
                return false;
            }
            return eval(condition ? expression->then_expr.get()
                                  : expression->else_expr.get(), out);
        }

        if (auto* expression = dynamic_cast<const AdditiveExpression*>(expr)) {
            ConstexprValue left;
            ConstexprValue right;
            if (!eval(expression->left.get(), left) ||
                !eval(expression->right.get(), right)) {
                return false;
            }
            return apply_arithmetic(
                expression->op == AdditiveExpression::Operator::Plus
                    ? ArithmeticOp::Add : ArithmeticOp::Subtract,
                left, right, out);
        }

        if (auto* expression = dynamic_cast<const MultiplicativeExpression*>(expr)) {
            ConstexprValue left;
            ConstexprValue right;
            if (!eval(expression->left.get(), left) ||
                !eval(expression->right.get(), right)) {
                return false;
            }

            switch (expression->op) {
            case MultiplicativeExpression::Operator::Multiply:
                return apply_arithmetic(ArithmeticOp::Multiply, left, right, out);
            case MultiplicativeExpression::Operator::Divide:
                return apply_arithmetic(ArithmeticOp::Divide, left, right, out);
            case MultiplicativeExpression::Operator::Remainder:
                return apply_arithmetic(ArithmeticOp::Remainder, left, right, out);
            }
            return fail(ConstexprFailure::Unsupported);
        }

        if (auto* expression = dynamic_cast<const PowerExpression*>(expr)) {
            ConstexprValue left;
            ConstexprValue right;
            if (!eval(expression->left.get(), left) ||
                !eval(expression->right.get(), right)) {
                return false;
            }
            return apply_arithmetic(ArithmeticOp::Power, left, right, out);
        }

        if (auto* expression = dynamic_cast<const ComparisonExpression*>(expr)) {
            ConstexprValue left;
            ConstexprValue right;
            if (!eval(expression->left.get(), left) ||
                !eval(expression->right.get(), right)) {
                return false;
            }

            CompareOp op = CompareOp::Equal;
            switch (expression->op) {
            case ComparisonExpression::Operator::Greater: op = CompareOp::Greater; break;
            case ComparisonExpression::Operator::Less: op = CompareOp::Less; break;
            case ComparisonExpression::Operator::Equal: op = CompareOp::Equal; break;
            case ComparisonExpression::Operator::NotEqual: op = CompareOp::NotEqual; break;
            case ComparisonExpression::Operator::GreaterEqual:
                op = CompareOp::GreaterEqual; break;
            case ComparisonExpression::Operator::LessEqual:
                op = CompareOp::LessEqual; break;
            }
            return apply_comparison(op, left, right, out);
        }

        if (auto* expression = dynamic_cast<const BitwiseExpression*>(expr)) {
            ConstexprValue left;
            ConstexprValue right;
            if (!eval(expression->left.get(), left) ||
                !eval(expression->right.get(), right)) {
                return false;
            }

            BitwiseOp op = BitwiseOp::And;
            switch (expression->op) {
            case BitwiseExpression::Operator::And: op = BitwiseOp::And; break;
            case BitwiseExpression::Operator::Xor: op = BitwiseOp::Xor; break;
            case BitwiseExpression::Operator::Or: op = BitwiseOp::Or; break;
            }
            return apply_bitwise(op, left, right, out);
        }

        if (auto* expression = dynamic_cast<const ShiftExpression*>(expr)) {
            ConstexprValue left;
            ConstexprValue right;
            if (!eval(expression->left.get(), left) ||
                !eval(expression->right.get(), right)) {
                return false;
            }
            return apply_shift(expression->op == ShiftExpression::Operator::Left
                ? ShiftOp::Left : ShiftOp::Right, left, right, out);
        }

        if (auto* expression = dynamic_cast<const UnaryExpression*>(expr)) {
            if (expression->op == UnaryExpression::Operator::Increment ||
                expression->op == UnaryExpression::Operator::Decrement) {
                ConstexprValue* target = lvalue(expression->operand.get());
                if (target == nullptr) { return fail(ConstexprFailure::Unsupported); }
                ConstexprValue result;
                if (!apply_arithmetic(
                    expression->op == UnaryExpression::Operator::Increment
                        ? ArithmeticOp::Add : ArithmeticOp::Subtract,
                    *target, ConstexprValue::make_integer(1, Type::make_int()),
                    result)) {
                    return false;
                }
                ConstexprValue converted;
                if (!coerce(result, target->type, converted)) {
                    return fail(ConstexprFailure::Unsupported);
                }
                *target = std::move(converted);
                out = *target;
                return true;
            }

            ConstexprValue operand;
            if (!eval(expression->operand.get(), operand)) { return false; }
            return apply_unary(static_cast<int>(expression->op), operand, out);
        }

        if (auto* postfix = dynamic_cast<const PostfixExpression*>(expr)) {
            return eval_postfix(postfix, out);
        }

        return fail(ConstexprFailure::Unsupported);
    }

    bool ConstexprInterpreter::eval_primary(const PrimaryExpression* expr,
        ConstexprValue& out) {
        switch (expr->kind) {
        case PrimaryExpression::Kind::Literal: {
            const Token& token = expr->literal_token;
            switch (token.type) {
            case TokenType::IntegerLiteral: {
                long long value = 0;
                if (!decode_integer_literal(token.lexeme, value)) {
                    return fail(ConstexprFailure::Unsupported);
                }
                const AST::Type type = Type::integer_literal_type(token.lexeme);
                out = ConstexprValue::make_integer(
                    wrap_integer(value, type.kind), type);
                return true;
            }
            case TokenType::FloatLiteral: {
                double value = 0.0;
                if (!decode_float_literal(token.lexeme, value)) {
                    return fail(ConstexprFailure::Unsupported);
                }
                const AST::Type type = Type::float_literal_type(token.lexeme);
                out = ConstexprValue::make_floating(value, type);
                return true;
            }
            case TokenType::CharLiteral: {
                std::string text;
                if (!decode_escaped_text(token.lexeme, '\'', text)) {
                    return fail(ConstexprFailure::Unsupported);
                }
                const long long value = text.empty()
                    ? 0 : static_cast<unsigned char>(text.front());
                out = ConstexprValue::make_integer(value, Type::make_char());
                return true;
            }
            case TokenType::BoolLiteral:
                out = ConstexprValue::make_bool(token.lexeme == "true");
                return true;
            case TokenType::StringLiteral: {
                std::string text;
                if (!decode_escaped_text(token.lexeme, '"', text)) {
                    return fail(ConstexprFailure::Unsupported);
                }
                out = ConstexprValue::make_string(std::move(text));
                return true;
            }
            default:
                return fail(ConstexprFailure::Unsupported);
            }
        }
        case PrimaryExpression::Kind::Identifier: {
            if (ConstexprValue* local = lookup_local(expr->identifier)) {
                out = *local;
                return true;
            }
            if (host_.lookup_constant &&
                host_.lookup_constant(expr->identifier, out)) {
                return true;
            }
            return fail(ConstexprFailure::Unsupported);
        }
        case PrimaryExpression::Kind::Parens:
            return eval(expr->paren_expr.get(), out);
        default:
            return fail(ConstexprFailure::Unsupported);
        }
    }

    bool ConstexprInterpreter::eval_postfix(const PostfixExpression* expr,
        ConstexprValue& out) {
        switch (expr->op) {
        case PostfixExpression::Operator::Subscript: {
            ConstexprValue base;
            ConstexprValue index;
            if (!eval(expr->base.get(), base)) { return false; }
            if (!eval(expr->subscript_expr.get(), index)) { return false; }

            if (base.type.kind != TypeKind::Array) {
                return fail(ConstexprFailure::Unsupported);
            }

            const long long position = index.as_integer();
            if (position < 0 ||
                static_cast<std::size_t>(position) >= base.elements.size()) {
                return fail(ConstexprFailure::Unsupported);
            }

            out = base.elements[static_cast<std::size_t>(position)];
            return true;
        }
        case PostfixExpression::Operator::Dot: {
            ConstexprValue base;
            if (!eval(expr->base.get(), base)) { return false; }
            ConstexprValue* member = struct_member(base, expr->member_name);
            if (member == nullptr) { return fail(ConstexprFailure::Unsupported); }
            out = *member;
            return true;
        }
        case PostfixExpression::Operator::FunctionCall:
            return eval_call(expr, out);
        case PostfixExpression::Operator::Increment:
        case PostfixExpression::Operator::Decrement: {
            ConstexprValue* target = lvalue(expr->base.get());
            if (target == nullptr) { return fail(ConstexprFailure::Unsupported); }
            const ConstexprValue previous = *target;
            ConstexprValue result;
            if (!apply_arithmetic(
                expr->op == PostfixExpression::Operator::Increment
                    ? ArithmeticOp::Add : ArithmeticOp::Subtract,
                *target, ConstexprValue::make_integer(1, Type::make_int()),
                result)) {
                return false;
            }
            ConstexprValue converted;
            if (!coerce(result, target->type, converted)) {
                return fail(ConstexprFailure::Unsupported);
            }
            *target = std::move(converted);
            out = previous;
            return true;
        }
        case PostfixExpression::Operator::Cast: {
            ConstexprValue base;
            if (!eval(expr->base.get(), base)) { return false; }
            if (!coerce(base, expr->cast_type, out)) {
                return fail(ConstexprFailure::Unsupported);
            }
            return true;
        }
        default:
            return fail(ConstexprFailure::Unsupported);
        }
    }

    bool ConstexprInterpreter::eval_call(const PostfixExpression* expr,
        ConstexprValue& out) {
        const std::string name = display_text_of(expr->base.get());
        if (name.empty()) { return fail(ConstexprFailure::Unsupported); }
        return eval_named_call(name, expr, out);
    }

    bool ConstexprInterpreter::eval_named_call(const std::string& name,
        const PostfixExpression* call, ConstexprValue& out) {
        if (name == "size" || name == "align") {
            if (call->arguments.size() != 1) {
                return fail(ConstexprFailure::Unsupported);
            }

            std::size_t size = 0;
            std::size_t align = 0;
            bool resolved = false;

            if (auto* primary = dynamic_cast<const PrimaryExpression*>(
                call->arguments[0].get())) {
                if (primary->kind == PrimaryExpression::Kind::Identifier &&
                    host_.layout_of_name) {
                    resolved = host_.layout_of_name(primary->identifier, size,
                        align);
                }
            }

            if (!resolved) {
                ConstexprValue argument;
                if (!eval(call->arguments[0].get(), argument)) { return false; }
                if (!host_.layout_of_type ||
                    !host_.layout_of_type(argument.type, size, align)) {
                    return fail(ConstexprFailure::Unsupported);
                }
            }

            out = ConstexprValue::make_integer(
                static_cast<long long>(name == "size" ? size : align),
                Type::make_int());
            return true;
        }

        std::vector<ConstexprValue> arguments;
        arguments.reserve(call->arguments.size());

        for (const auto& argument : call->arguments) {
            ConstexprValue value;
            if (!eval(argument.get(), value)) { return false; }
            arguments.push_back(std::move(value));
        }

        if (name == "strlen" || name == "strconcat" || name == "strcopy" ||
            name == "strcompare" || name == "strcontains" ||
            name == "strsubstr" || name == "strfind" || name == "strreplace" ||
            name == "strupper" || name == "strlower" || name == "strtrim" ||
            name == "strcharat" || name == "strsplitcount" ||
            name == "strsplitget") {
            return eval_string_builtin(name, arguments, out);
        }

        const AST::FunctionDefinition* function = host_.resolve_function
            ? host_.resolve_function(name, arguments) : nullptr;
        if (function == nullptr) { return fail(ConstexprFailure::Unsupported); }
        return this->call(function, arguments, out);
    }

    bool ConstexprInterpreter::eval_string_builtin(const std::string& name,
        const std::vector<ConstexprValue>& arguments, ConstexprValue& out) {
        auto as_string = [&](std::size_t index, std::string& text) {
            if (index >= arguments.size()) { return false; }
            if (arguments[index].type.kind != TypeKind::String) { return false; }
            text = arguments[index].string_value;
            return true;
        };
        auto as_number = [&](std::size_t index, long long& value) {
            if (index >= arguments.size()) { return false; }
            if (!arguments[index].type.is_arithmetic()) { return false; }
            value = arguments[index].as_integer();
            return true;
        };

        if (name == "strlen") {
            std::string text;
            if (!as_string(0, text)) { return fail(ConstexprFailure::Unsupported); }
            out = ConstexprValue::make_integer(
                static_cast<long long>(text.size()), Type::make_int());
            return true;
        }

        if (name == "strcopy") {
            std::string text;
            if (!as_string(0, text)) { return fail(ConstexprFailure::Unsupported); }
            out = ConstexprValue::make_string(text);
            return true;
        }

        if (name == "strconcat") {
            std::string left;
            std::string right;
            if (!as_string(0, left) || !as_string(1, right)) {
                return fail(ConstexprFailure::Unsupported);
            }
            out = ConstexprValue::make_string(left + right);
            return true;
        }

        if (name == "strcompare") {
            std::string left;
            std::string right;
            if (!as_string(0, left) || !as_string(1, right)) {
                return fail(ConstexprFailure::Unsupported);
            }
            const int order = left.compare(right);
            out = ConstexprValue::make_integer(order < 0 ? -1 : (order > 0 ? 1 : 0),
                Type::make_int());
            return true;
        }

        if (name == "strcontains") {
            std::string text;
            std::string needle;
            if (!as_string(0, text) || !as_string(1, needle)) {
                return fail(ConstexprFailure::Unsupported);
            }
            out = ConstexprValue::make_bool(text.find(needle) != std::string::npos);
            return true;
        }

        if (name == "strsubstr") {
            std::string text;
            long long start = 0;
            long long count = 0;
            if (!as_string(0, text) || !as_number(1, start) || !as_number(2, count)) {
                return fail(ConstexprFailure::Unsupported);
            }
            if (start < 0) { start = 0; }
            if (count < 0) { count = 0; }
            const std::size_t begin = static_cast<std::size_t>(start);
            out = ConstexprValue::make_string(begin >= text.size()
                ? std::string()
                : text.substr(begin, static_cast<std::size_t>(count)));
            return true;
        }

        if (name == "strfind") {
            std::string text;
            std::string needle;
            if (!as_string(0, text) || !as_string(1, needle)) {
                return fail(ConstexprFailure::Unsupported);
            }
            const std::size_t position = text.find(needle);
            out = ConstexprValue::make_integer(
                position == std::string::npos
                    ? -1 : static_cast<long long>(position),
                Type::make_int());
            return true;
        }

        if (name == "strreplace") {
            std::string text;
            std::string from;
            std::string to;
            if (!as_string(0, text) || !as_string(1, from) || !as_string(2, to)) {
                return fail(ConstexprFailure::Unsupported);
            }
            if (from.empty()) {
                out = ConstexprValue::make_string(text);
                return true;
            }
            std::string result;
            std::size_t position = 0;

            while (position <= text.size()) {
                const std::size_t found = text.find(from, position);
                if (found == std::string::npos) {
                    result.append(text, position, std::string::npos);
                    break;
                }
                result.append(text, position, found - position);
                result += to;
                position = found + from.size();
            }

            out = ConstexprValue::make_string(std::move(result));
            return true;
        }

        if (name == "strupper" || name == "strlower") {
            std::string text;
            if (!as_string(0, text)) { return fail(ConstexprFailure::Unsupported); }

            for (char& c : text) {
                const unsigned char byte = static_cast<unsigned char>(c);
                if (name == "strupper") {
                    if (byte >= 'a' && byte <= 'z') {
                        c = static_cast<char>(byte - 'a' + 'A');
                    }
                } else if (byte >= 'A' && byte <= 'Z') {
                    c = static_cast<char>(byte - 'A' + 'a');
                }
            }

            out = ConstexprValue::make_string(std::move(text));
            return true;
        }

        if (name == "strtrim") {
            std::string text;
            if (!as_string(0, text)) { return fail(ConstexprFailure::Unsupported); }
            auto is_space = [](char c) {
                return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                    c == '\v' || c == '\f';
            };
            std::size_t begin = 0;
            std::size_t end = text.size();
            while (begin < end && is_space(text[begin])) { ++begin; }
            while (end > begin && is_space(text[end - 1])) { --end; }
            out = ConstexprValue::make_string(text.substr(begin, end - begin));
            return true;
        }

        if (name == "strcharat") {
            std::string text;
            long long index = 0;
            if (!as_string(0, text) || !as_number(1, index)) {
                return fail(ConstexprFailure::Unsupported);
            }
            const char value = (index >= 0 &&
                static_cast<std::size_t>(index) < text.size())
                ? text[static_cast<std::size_t>(index)] : '\0';
            out = ConstexprValue::make_integer(
                static_cast<unsigned char>(value), Type::make_char());
            return true;
        }

        if (name == "strsplitcount" || name == "strsplitget") {
            std::string text;
            std::string separator;
            if (!as_string(0, text) || !as_string(1, separator)) {
                return fail(ConstexprFailure::Unsupported);
            }

            std::vector<std::string> fields;

            if (separator.empty()) {
                fields.push_back(text);
            } else {
                std::size_t position = 0;

                while (position <= text.size()) {
                    const std::size_t found = text.find(separator, position);
                    if (found == std::string::npos) {
                        fields.push_back(text.substr(position));
                        break;
                    }
                    fields.push_back(text.substr(position, found - position));
                    position = found + separator.size();
                }
            }

            if (name == "strsplitcount") {
                out = ConstexprValue::make_integer(
                    static_cast<long long>(fields.size()), Type::make_int());
                return true;
            }

            long long index = 0;
            if (!as_number(2, index)) { return fail(ConstexprFailure::Unsupported); }
            out = ConstexprValue::make_string(index < 0 ||
                static_cast<std::size_t>(index) >= fields.size()
                ? std::string() : fields[static_cast<std::size_t>(index)]);
            return true;
        }

        return fail(ConstexprFailure::Unsupported);
    }

    bool ConstexprInterpreter::evaluate_condition(const AST::Expression* expr,
        bool& out) {
        ConstexprValue value;
        if (!eval(expr, value)) { return false; }

        if (!value.type.is_arithmetic() &&
            value.type.kind != TypeKind::String) {
            return fail(ConstexprFailure::Unsupported);
        }

        out = value.as_bool();
        return true;
    }

    bool ConstexprInterpreter::eval_initializer(const AST::Initializer* init,
        const AST::Type& type, ConstexprValue& out) {
        if (init == nullptr) { return default_value(type, out); }

        if (auto* expression = dynamic_cast<const ExpressionInitializer*>(init)) {
            ConstexprValue value;
            if (!eval(expression->expr.get(), value)) { return false; }
            return coerce(value, type, out);
        }

        if (auto* array = dynamic_cast<const ArrayInitializer*>(init)) {
            return eval_array_initializer(array, type, out);
        }

        return fail(ConstexprFailure::Unsupported);
    }

    bool ConstexprInterpreter::eval_array_initializer(
        const ArrayInitializer* init, const AST::Type& type, ConstexprValue& out) {
        if (type.kind == TypeKind::Array) {
            if (type.element_type == nullptr) {
                return fail(ConstexprFailure::Unsupported);
            }
            const std::size_t count = type.array_size.value_or(init->elements.size());
            std::vector<ConstexprValue> elements;
            elements.reserve(count);

            for (std::size_t i = 0; i < count; ++i) {
                ConstexprValue element;

                if (i < init->elements.size()) {
                    if (!eval_initializer(init->elements[i].get(),
                        *type.element_type, element)) {
                        return false;
                    }
                } else if (!default_value(*type.element_type, element)) {
                    return fail(ConstexprFailure::Unsupported);
                }

                elements.push_back(std::move(element));
            }

            out = ConstexprValue::make_array(*type.element_type,
                std::move(elements));
            out.type = type;
            return true;
        }

        if (type.kind == TypeKind::Struct) {
            const AST::StructDefinition* definition = host_.lookup_struct
                ? host_.lookup_struct(type.struct_name) : nullptr;
            if (definition == nullptr) { return fail(ConstexprFailure::Unsupported); }

            std::vector<ConstexprValue> members;
            members.reserve(definition->members.size());

            for (std::size_t i = 0; i < definition->members.size(); ++i) {
                const StructDefinition::Member& member = definition->members[i];
                ConstexprValue value;

                if (i < init->elements.size()) {
                    if (!eval_initializer(init->elements[i].get(), member.type,
                        value)) {
                        return false;
                    }
                } else if (member.initializer) {
                    if (!eval_initializer(member.initializer.get(), member.type,
                        value)) {
                        return false;
                    }
                } else if (!default_value(member.type, value)) {
                    return fail(ConstexprFailure::Unsupported);
                }

                members.push_back(std::move(value));
            }

            out = ConstexprValue::make_struct(type.struct_name, std::move(members));
            out.type = type;
            return true;
        }

        if (init->elements.size() == 1) {
            return eval_initializer(init->elements[0].get(), type, out);
        }

        return fail(ConstexprFailure::Unsupported);
    }

    ConstexprInterpreter::Control ConstexprInterpreter::exec_statement(
        const AST::Statement* stmt) {
        Control control;

        if (stmt == nullptr) { return control; }
        if (!step()) {
            control.kind = Control::Kind::Break;
            return control;
        }

        if (auto* block = dynamic_cast<const Block*>(stmt)) {
            return exec_block(block);
        }

        if (auto* declaration = dynamic_cast<const VariableDeclaration*>(stmt)) {
            return exec_variable_declaration(declaration);
        }

        if (auto* statement = dynamic_cast<const IfStatement*>(stmt)) {
            return exec_if(statement);
        }

        if (auto* statement = dynamic_cast<const ForStatement*>(stmt)) {
            return exec_for(statement);
        }

        if (auto* statement = dynamic_cast<const WhileStatement*>(stmt)) {
            return exec_while(statement);
        }

        if (dynamic_cast<const EmptyStatement*>(stmt) != nullptr) {
            return control;
        }

        if (dynamic_cast<const BreakStatement*>(stmt) != nullptr) {
            control.kind = Control::Kind::Break;
            return control;
        }

        if (auto* statement = dynamic_cast<const ReturnStatement*>(stmt)) {
            control.kind = Control::Kind::Return;

            if (statement->value != nullptr) {
                if (!eval(statement->value.get(), control.value)) {
                    control.kind = Control::Kind::Break;
                }
            } else {
                control.value = ConstexprValue::make_void();
            }

            return control;
        }

        if (auto* statement = dynamic_cast<const ExpressionStatement*>(stmt)) {
            ConstexprValue value;
            if (!eval(statement->expr.get(), value)) {
                control.kind = Control::Kind::Break;
            }
            return control;
        }

        fail(ConstexprFailure::Unsupported);
        control.kind = Control::Kind::Break;
        return control;
    }

    ConstexprInterpreter::Control ConstexprInterpreter::exec_block(
        const Block* block) {
        scopes_.emplace_back();
        Control control;

        for (const auto& statement : block->statements) {
            control = exec_statement(statement.get());
            if (failure_ != ConstexprFailure::None) { break; }
            if (control.kind != Control::Kind::Normal) { break; }
        }

        scopes_.pop_back();
        return control;
    }

    ConstexprInterpreter::Control ConstexprInterpreter::exec_variable_declaration(
        const VariableDeclaration* decl) {
        Control control;
        if (decl == nullptr) { return control; }

        AST::Type type = decl->type;

        if (type.kind == TypeKind::Array && !type.array_size.has_value()) {
            if (decl->array_size.has_value()) {
                type.array_size = decl->array_size;
            } else if (auto* array = dynamic_cast<const ArrayInitializer*>(
                decl->initializer.get())) {
                type.array_size = array->elements.size();
            } else if (decl->array_size_expr != nullptr) {
                ConstexprValue size;
                if (!eval(decl->array_size_expr.get(), size)) {
                    control.kind = Control::Kind::Break;
                    return control;
                }
                if (size.as_integer() < 0) {
                    fail(ConstexprFailure::Unsupported);
                    control.kind = Control::Kind::Break;
                    return control;
                }
                type.array_size = static_cast<std::size_t>(size.as_integer());
            }
        }

        ConstexprValue value;
        if (!eval_initializer(decl->initializer.get(), type, value)) {
            control.kind = Control::Kind::Break;
            return control;
        }

        value.type = type;
        scopes_.back()[decl->name] = std::move(value);
        return control;
    }

    ConstexprInterpreter::Control ConstexprInterpreter::exec_if(
        const IfStatement* stmt) {
        Control control;
        bool condition = false;

        if (!evaluate_condition(stmt->condition.get(), condition)) {
            control.kind = Control::Kind::Break;
            return control;
        }

        const AST::Statement* branch = condition ? stmt->then_block.get()
                                                 : stmt->else_block.get();
        if (branch == nullptr) { return control; }
        return exec_statement(branch);
    }

    ConstexprInterpreter::Control ConstexprInterpreter::exec_for(
        const ForStatement* stmt) {
        Control control;
        scopes_.emplace_back();

        if (stmt->init != nullptr) {
            control = exec_statement(stmt->init.get());
        }

        while (control.kind == Control::Kind::Normal &&
            failure_ == ConstexprFailure::None) {
            if (!step()) {
                control.kind = Control::Kind::Break;
                break;
            }

            bool condition = true;
            if (stmt->condition != nullptr &&
                !evaluate_condition(stmt->condition.get(), condition)) {
                control.kind = Control::Kind::Break;
                break;
            }

            if (!condition) { break; }

            control = exec_statement(stmt->body.get());
            if (control.kind == Control::Kind::Break) {
                control.kind = Control::Kind::Normal;
                break;
            }
            if (control.kind == Control::Kind::Return) { break; }

            if (stmt->step != nullptr) {
                ConstexprValue value;
                if (!eval(stmt->step.get(), value)) {
                    control.kind = Control::Kind::Break;
                    break;
                }
            }
        }

        if (control.kind == Control::Kind::Break &&
            failure_ == ConstexprFailure::None) {
            control.kind = Control::Kind::Normal;
        }

        scopes_.pop_back();
        return control;
    }

    ConstexprInterpreter::Control ConstexprInterpreter::exec_while(
        const WhileStatement* stmt) {
        Control control;

        while (true) {
            if (failure_ != ConstexprFailure::None) {
                control.kind = Control::Kind::Break;
                break;
            }

            if (!step()) {
                control.kind = Control::Kind::Break;
                break;
            }

            bool condition = false;
            if (!evaluate_condition(stmt->condition.get(), condition)) {
                control.kind = Control::Kind::Break;
                break;
            }

            if (!condition) { break; }

            control = exec_statement(stmt->body.get());
            if (control.kind == Control::Kind::Break) {
                control.kind = Control::Kind::Normal;
                break;
            }
            if (control.kind == Control::Kind::Return) { break; }
        }

        return control;
    }

}
