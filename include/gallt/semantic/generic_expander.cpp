// semantic/generic_expander.cpp
// 编译期泛型展开（单态化）实现 —— Gallt 0.3.txt §19
// Compile-time generic expansion (monomorphization) — Gallt 0.3.txt §19

#include "generic_expander.hpp"
#include "constant_folding.hpp"
#include "../lexer/lexer.hpp"
#include "../parser/parser.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

using namespace gallt::AST;

namespace gallt {

    namespace {
        bool is_builtin_type_name(const std::string& name) {
            // 0.4.1 §2：新增 lint / uint / luint / uchar
            // 0.4.1 §2: lint / uint / luint / uchar are added
            return name == "int" || name == "lint" || name == "uint" ||
                name == "luint" || name == "float" || name == "double" ||
                name == "char" || name == "uchar" || name == "bool" ||
                name == "string" || name == "file" || name == "void";
        }

        // 规范化实参列表文本，用于诊断与命名修饰
        // Normalized argument list text, for diagnostics and mangling
        std::string arguments_text(const GenericRef& ref) {
            std::string out;
            for (std::size_t i = 0; i < ref.arguments.size(); ++i) {
                if (i != 0) out += ", ";
                out += ref.arguments[i].normalize();
            }
            return out;
        }

        // 实参种类签名：用于检测“相同规范化结果、不同底层构成”(ER 0079)
        // Argument-kind signature, used to detect normalization collisions (ER 0079)
        std::string arguments_kind_signature(const GenericRef& ref) {
            std::string out;
            for (std::size_t i = 0; i < ref.arguments.size(); ++i) {
                const GenericArgument& arg = ref.arguments[i];
                out += "|";
                if (arg.is_type) {
                    out += "T:" + arg.type.to_string();
                }
                else if (arg.is_string_constant) {
                    // 字符串编译期常量实参（Gallt 0.4.txt §19 修正后）
                    // String compile-time constant argument (§19 as corrected)
                    out += "CS";
                }
                else if (arg.float_constant) {
                    out += "CF";
                }
                else {
                    out += "CI";
                }
            }
            return out;
        }
    } // anonymous namespace

    GenericExpander::GenericExpander(DiagnosticEngine& diag) : diag_(diag) {}

    void GenericExpander::report(SourceLocation loc, ErrorCode code,
        const std::vector<std::string>& values) {
        diag_.report_error_template(loc, code, values);
        had_error_ = true;
    }

    void GenericExpander::report(SourceLocation loc, ErrorCode code, const std::string& message) {
        diag_.report_error(loc, code, message);
        had_error_ = true;
    }

    // ============================================================================
    // 作用域与短名
    // ============================================================================

    void GenericExpander::push_scope() {
        declared_scopes_.emplace_back();
        short_scopes_.emplace_back();
        instance_scopes_.emplace_back();
    }

    void GenericExpander::pop_scope() {
        declared_scopes_.pop_back();
        short_scopes_.pop_back();
        instance_scopes_.pop_back();
    }

    void GenericExpander::declare_name(const std::string& name) {
        if (!declared_scopes_.empty()) {
            declared_scopes_.back().insert(name);
        }
    }

    bool GenericExpander::name_declared_in_current_scope(const std::string& name) const {
        if (declared_scopes_.empty()) return false;
        return declared_scopes_.back().count(name) != 0;
    }

    void GenericExpander::register_short_name(const std::string& name,
        const ShortBinding& binding, const std::string& owning_generic, SourceLocation loc) {
        if (short_scopes_.empty()) return;
        auto& shorts = short_scopes_.back();
        auto it = shorts.find(name);
        if (it != shorts.end()) {
            if (it->second.instance == binding.instance) {
                return;  // 幂等：相同实参组合重复实例化
            }
            if (it->second.target != binding.target) {
                // ER 0073：同一作用域中引用同名短名，产生二义性
                report(loc, ErrorCode::GenericShortNameAmbiguous, std::vector<std::string>{ name });
            }
            return;
        }
        // 泛型名本身不构成冲突：实例化短名是该泛型的具象化
        // The generic's own name is not a conflict: the short name realizes that generic
        if (name == owning_generic) {
            shorts[name] = binding;
            return;
        }
        if (name_declared_in_current_scope(name)) {
            // ER 0074：短名与当前作用域中已有普通声明冲突
            report(loc, ErrorCode::GenericShortNameConflict, std::vector<std::string>{ name });
            return;
        }
        shorts[name] = binding;
        declare_name(name);
    }

    void GenericExpander::add_top_level(std::unique_ptr<TopLevel> node) {
        if (output_ != nullptr) {
            output_->push_back(std::move(node));
        }
        else {
            program_->top_levels.push_back(std::move(node));
        }
    }

    // ============================================================================
    // 阶段一：收集泛型与声明
    // ============================================================================

    void GenericExpander::collect_generics() {
        std::map<std::string, std::vector<GenericDefinition*>> by_name;
        for (auto& top : program_->top_levels) {
            if (auto* g = dynamic_cast<GenericDefinition*>(top.get())) {
                by_name[g->name].push_back(g);
            }
        }
        for (auto& pair : by_name) {
            const std::string& name = pair.first;
            std::vector<GenericDefinition*>& defs = pair.second;
            GenericEntry entry;
            std::vector<GenericDefinition*> before_primary;
            bool has_primary_shaped = false;
            for (GenericDefinition* def : defs) {
                if (def->primary_shaped) has_primary_shaped = true;
            }
            for (GenericDefinition* def : defs) {
                // 参数重复声明（ER 0063）
                std::set<std::string> seen_params;
                for (const GenericParameter& p : def->parameters) {
                    if (!seen_params.insert(p.name).second) {
                        report(def->location, ErrorCode::GenericParameterRedefined, std::vector<std::string>{ p.name });
                    }
                }
                // 约束有效性（ER 0077）
                for (const GenericParameter& p : def->parameters) {
                    if (p.is_type && p.constraint.kind == GenericConstraint::Kind::Types) {
                        for (const Type& t : p.constraint.types) {
                            if (t.kind != TypeKind::Struct) continue;
                            // 泛型实例成员类型（如 Box<int>.Item）在展开阶段解析， здесь 跳过
                            // Instantiated generic member types are resolved during expansion
                            if (t.generic_ref) continue;
                            if (!is_builtin_type_name(t.struct_name) &&
                                struct_defs_.find(t.struct_name) == struct_defs_.end()) {
                                report(def->location, ErrorCode::GenericConstraintInvalid, std::vector<std::string>{ p.constraint.to_string() });
                            }
                        }
                    }
                }
                // 偏特化自由标识符结构冲突（ER 0072）
                if (!def->primary_shaped) {
                    std::map<std::string, std::string> structure;
                    // 递归扫描模式：记录每个自由标识符出现的结构（如 T、P(T)、P(P(T))、A(T)）
                    // 同一标识符以不同结构出现时，无法在实例化时同时推导一致类型 → ER 0072
                    // Scan patterns recursively: a free identifier that appears with different
                    // structures can never deduce one consistent type → ER 0072
                    std::function<std::string(const Type&)> structure_of =
                        [&](const Type& t) -> std::string {
                        switch (t.kind) {
                        case TypeKind::Struct:
                            if (!is_builtin_type_name(t.struct_name) &&
                                struct_defs_.find(t.struct_name) == struct_defs_.end()) {
                                return "T";
                            }
                            return "S:" + t.struct_name;
                        case TypeKind::Pointer:
                            return "P(" + (t.pointee_type ? structure_of(*t.pointee_type)
                                                          : std::string("?")) + ")";
                        case TypeKind::Array:
                            return "A(" + (t.element_type ? structure_of(*t.element_type)
                                                          : std::string("?")) + ")";
                        default:
                            return t.to_string();
                        }
                    };
                    std::function<void(const Type&)> scan = [&](const Type& t) {
                        // 逐个自由标识符记录其在模式中的结构位置
                        // Record the structural position of every free identifier
                        switch (t.kind) {
                        case TypeKind::Struct:
                            if (!is_builtin_type_name(t.struct_name) &&
                                struct_defs_.find(t.struct_name) == struct_defs_.end()) {
                                auto found = structure.find(t.struct_name);
                                if (found == structure.end()) {
                                    structure[t.struct_name] = "T";
                                }
                            }
                            return;
                        case TypeKind::Pointer:
                            if (t.pointee_type) scan(*t.pointee_type);
                            return;
                        case TypeKind::Array:
                            if (t.element_type) scan(*t.element_type);
                            return;
                        default:
                            return;
                        }
                    };
                    for (const GenericPatternArg& pattern : def->patterns) {
                        if (!pattern.is_constant) scan(pattern.type);
                    }
                    // 重新按“每个自由标识符的完整结构”核对一致性
                    // Re-check consistency using each identifier's full structure
                    std::map<std::string, std::string> full_structure;
                    std::function<void(const Type&)> verify = [&](const Type& t) {
                        if (t.kind == TypeKind::Struct &&
                            !is_builtin_type_name(t.struct_name) &&
                            struct_defs_.find(t.struct_name) == struct_defs_.end()) {
                            std::string shape = structure_of(t);
                            auto found = full_structure.find(t.struct_name);
                            if (found == full_structure.end()) {
                                full_structure[t.struct_name] = shape;
                            }
                            else if (found->second != shape) {
                                report(def->location,
                                    ErrorCode::GenericFreeIdentifierConflict, std::vector<std::string>{ t.struct_name });
                            }
                            return;
                        }
                        if (t.kind == TypeKind::Pointer && t.pointee_type) {
                            if (t.pointee_type->kind == TypeKind::Struct &&
                                !is_builtin_type_name(t.pointee_type->struct_name) &&
                                struct_defs_.find(t.pointee_type->struct_name) == struct_defs_.end()) {
                                // 自由标识符出现在指针内部：结构为 P(T)
                                // A free identifier inside a pointer has shape P(T)
                                const std::string& n = t.pointee_type->struct_name;
                                auto found = full_structure.find(n);
                                if (found == full_structure.end()) full_structure[n] = "P(T)";
                                else if (found->second != "P(T)") {
                                    report(def->location,
                                        ErrorCode::GenericFreeIdentifierConflict, std::vector<std::string>{ n });
                                }
                                return;
                            }
                            verify(*t.pointee_type);
                            return;
                        }
                        if (t.kind == TypeKind::Array && t.element_type) {
                            if (t.element_type->kind == TypeKind::Struct &&
                                !is_builtin_type_name(t.element_type->struct_name) &&
                                struct_defs_.find(t.element_type->struct_name) == struct_defs_.end()) {
                                const std::string& n = t.element_type->struct_name;
                                auto found = full_structure.find(n);
                                if (found == full_structure.end()) full_structure[n] = "A(T)";
                                else if (found->second != "A(T)") {
                                    report(def->location,
                                        ErrorCode::GenericFreeIdentifierConflict, std::vector<std::string>{ n });
                                }
                                return;
                            }
                            verify(*t.element_type);
                            return;
                        }
                    };
                    for (const GenericPatternArg& pattern : def->patterns) {
                        if (!pattern.is_constant) verify(pattern.type);
                    }
                }
                // 成员名与泛型参数冲突（ER 0083）
                std::set<std::string> member_names;
                for (auto& member : def->members) {
                    std::string member_name;
                    if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) member_name = sd->name;
                    else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) member_name = fd->name;
                    if (member_name.empty()) continue;
                    // 同一泛型块内同名成员（含函数重载形态）目前无法在实例化后共存：
                    // 命名修饰以“实例$成员名”为键，重载会互相覆盖。文档未定义泛型块内的
                    // 重载语义，这里给出明确诊断而不是静默丢弃其中一个成员。
                    // Same-named members (including function-overload shapes) cannot coexist
                    // after instantiation because member mangling is keyed by "instance$member".
                    // The document does not define overloading inside a generic block, so this is
                    // diagnosed explicitly instead of silently dropping a member.
                    if (!member_names.insert(member_name).second) {
                        report(def->location, ErrorCode::RedefinedFunction, std::vector<std::string>{ member_name });
                    }
                    for (const GenericParameter& p : def->parameters) {
                        if (p.name == member_name) {
                            report(def->location, ErrorCode::GenericMemberNameConflictsParameter, std::vector<std::string>{ member_name, p.name });
                        }
                    }
                }

                // 文档 §19：“同一泛型名第一次出现的 generics [标识符]<...> 声明为主泛型，
                // 主泛型之后出现的同名 generics [标识符]<...> 声明为特化”
                // 因此第二次及以后出现的、形如参数列表的定义按其自由标识符解释为
                // 偏特化（匹配一切），而不是重复主泛型；ER 0062 只用于“真正重复”的定义
                // （参数/模式列表结构完全相同的特化）。
                // The first occurrence is the primary; later same-name definitions are
                // specializations. A later primary-shaped list becomes a partial
                // specialization over its free identifiers, and ER 0062 is reserved for a
                // structurally identical duplicate definition.
                if (def->primary_shaped && entry.primary == nullptr) {
                    // 第一个参数列表形态的定义成为主泛型
                    // The first parameter-list-shaped definition becomes the primary
                    entry.primary = def;
                }
                else if (def->primary_shaped) {
                    // 主泛型之后再次出现的主泛型形态 → 按自由标识符转为偏特化
                    // （若解析阶段已按模式列表解析，则保持其结果）
                    // A later primary-shaped definition becomes a partial specialization;
                    // keep the parsed pattern list when the parser already produced one
                    def->is_specialization = true;
                    if (def->patterns.empty()) {
                        def->patterns = parameters_as_free_patterns(def);
                    }
                    if (duplicate_pattern_signature(entry, def)) {
                        report(def->location, ErrorCode::GenericPrimaryRedefined, std::vector<std::string>{ name });
                    }
                    else {
                        entry.specializations.push_back(def);
                    }
                }
                else if (entry.primary == nullptr) {
                    before_primary.push_back(def);
                }
                else {
                    if (duplicate_pattern_signature(entry, def)) {
                        report(def->location, ErrorCode::GenericPrimaryRedefined, std::vector<std::string>{ name });
                    }
                    else {
                        entry.specializations.push_back(def);
                    }
                }
            }
            if (entry.primary == nullptr) {
                if (!before_primary.empty()) {
                    if (has_primary_shaped) {
                        // ER 0081：特化必须出现在主泛型之后
                        for (GenericDefinition* def : before_primary) {
                            report(def->location, ErrorCode::GenericSpecializationBeforePrimary, std::vector<std::string>{ def->name, name });
                        }
                    }
                    else {
                        // ER 0076：特化缺少主泛型声明
                        report(before_primary.front()->location,
                            ErrorCode::GenericSpecializationWithoutPrimary, std::vector<std::string>{ name });
                    }
                }
            }
            else {
                for (GenericDefinition* def : before_primary) {
                    report(def->location, ErrorCode::GenericSpecializationBeforePrimary, std::vector<std::string>{ def->name, name });
                }
            }
            generics_[name] = std::move(entry);
        }
    }

    void GenericExpander::collect_declarations() {
        std::function<void(const std::vector<std::unique_ptr<TopLevel>>&)> scan_top;
        for (auto& top : program_->top_levels) {
            if (auto* sd = dynamic_cast<StructDefinition*>(top.get())) {
                struct_defs_[sd->name] = sd;
                declare_name(sd->name);
            }
            else if (auto* fd = dynamic_cast<FunctionDefinition*>(top.get())) {
                func_defs_[fd->name] = fd;
                declare_name(fd->name);
            }
            else if (auto* ed = dynamic_cast<ExternDeclaration*>(top.get())) {
                extern_defs_[ed->name] = ed;
                declare_name(ed->name);
            }
            else if (auto* vd = dynamic_cast<VariableDeclaration*>(top.get())) {
                declare_name(vd->name);
            }
            else if (auto* g = dynamic_cast<GenericDefinition*>(top.get())) {
                declare_name(g->name);
            }
        }
        (void)scan_top;
    }

    // ============================================================================
    // 类型替换与克隆
    // ============================================================================

    bool GenericExpander::substitute_type(const Type& in, const Substitution& sub, Type& out) {
        switch (in.kind) {
        case TypeKind::Array: {
            Type elem = Type::make_void();
            bool ok = in.element_type ? substitute_type(*in.element_type, sub, elem) : false;
            out = Type::make_array(std::make_shared<Type>(std::move(elem)), in.array_size);
            return ok;
        }
        case TypeKind::Pointer: {
            Type pointee = Type::make_void();
            bool ok = in.pointee_type ? substitute_type(*in.pointee_type, sub, pointee) : false;
            out = Type::make_pointer(std::make_shared<Type>(std::move(pointee)));
            return ok;
        }
        case TypeKind::Function: {
            Type ret = Type::make_void();
            bool ok = in.return_type ? substitute_type(*in.return_type, sub, ret) : true;
            std::vector<Type> params;
            for (const Type& p : in.parameter_types) {
                Type substituted = Type::make_void();
                if (!substitute_type(p, sub, substituted)) ok = false;
                params.push_back(std::move(substituted));
            }
            out = Type::make_function(std::make_shared<Type>(std::move(ret)), params);
            return ok;
        }
        case TypeKind::Struct: {
            if (in.generic_ref) {
                GenericRef ref = *in.generic_ref;
                for (GenericArgument& arg : ref.arguments) {
                    if (arg.is_type) {
                        Type substituted = Type::make_void();
                        if (!substitute_type(arg.type, sub, substituted)) return false;
                        arg.type = std::move(substituted);
                        arg.text = arg.type.to_string();
                    }
                }
                auto mangled = ensure_instantiation(ref, false);
                if (!mangled.has_value()) return false;
                out = Type::make_struct(*mangled);
                return true;
            }
            auto found = sub.types.find(in.struct_name);
            if (found != sub.types.end()) {
                out = found->second;
                return true;
            }
            out = in;
            return true;
        }
        default:
            out = in;
            return true;
        }
    }

    void GenericExpander::resolve_type(Type& type) {
        switch (type.kind) {
        case TypeKind::Array:
            if (type.element_type) resolve_type(*type.element_type);
            break;
        case TypeKind::Pointer:
            if (type.pointee_type) resolve_type(*type.pointee_type);
            break;
        case TypeKind::Function:
            if (type.return_type) resolve_type(*type.return_type);
            for (Type& p : type.parameter_types) resolve_type(p);
            break;
        case TypeKind::Struct: {
            if (type.generic_ref) {
                GenericRef ref = *type.generic_ref;
                if (ref.member.empty()) {
                    // 类型上下文中的整体实例化：若实例化块恰好只有一个结构体成员，
                    // 则自动选用该成员（文档示例均写作 Box<int>.Example；单成员时
                    // Box<int> 亦可，避免歧义；多成员时报 ER 0080 要求使用限定名）
                    // A bare Box<int> in a type context selects the only struct member when
                    // the block has exactly one; otherwise a qualified name is required
                    auto instance_key = ensure_instantiation(ref, true);
                    if (!instance_key.has_value()) {
                        type.generic_ref.reset();
                        break;
                    }
                    auto members = instance_struct_members_.find(*instance_key);
                    if (members != instance_struct_members_.end() && members->second.size() == 1) {
                        ref.member = members->second.front();
                    }
                    else {
                        report(type.generic_ref->location, ErrorCode::GenericMemberNotInInstantiation, std::vector<std::string>{ ref.generic_name, ref.generic_name });
                        type.generic_ref.reset();
                        break;
                    }
                }
                auto mangled = ensure_instantiation(ref, false);
                if (mangled.has_value()) {
                    type.struct_name = *mangled;
                }
                type.generic_ref.reset();
                break;
            }
            // 短名解析：泛型实例引入的类型短名
            for (auto it = short_scopes_.rbegin(); it != short_scopes_.rend(); ++it) {
                auto found = it->find(type.struct_name);
                if (found != it->end()) {
                    if (found->second.is_type) {
                        type.struct_name = found->second.target;
                    }
                    break;
                }
            }
            break;
        }
        default:
            break;
        }
    }

    bool GenericExpander::resolve_constant_argument(const GenericArgument& arg,
        const Substitution* sub, ConstantValue& out) {
        if (arg.is_type) return false;
        // Gallt 0.4.txt §19：字符串字面量形式的编译期常量实参
        // Gallt 0.4.txt §19: a string-literal compile-time constant argument
        if (arg.constant_actual_type.kind == TypeKind::String) {
            if (arg.is_string_constant) {
                out.is_string = true;
                out.is_float = false;
                out.string_value = arg.string_constant;
                return true;
            }
            if (arg.expression != nullptr) {
                if (auto* prim = dynamic_cast<const PrimaryExpression*>(arg.expression.get())) {
                    if (prim->kind == PrimaryExpression::Kind::Literal &&
                        prim->literal_token.type == TokenType::StringLiteral) {
                        out.is_string = true;
                        out.is_float = false;
                        if (!decode_string_literal(prim->literal_token.lexeme,
                            out.string_value)) {
                            return false;
                        }
                        return true;
                    }
                }
            }
            return false;
        }
        // 带表达式的常量实参：按当前替换环境重新求值（§19 编译期常量表达式）
        // A constant argument carrying an expression is re-evaluated under the substitution
        if (arg.expression != nullptr) {
            Substitution empty;
            const Substitution& env = sub != nullptr ? *sub : empty;
            if (evaluate_with_substitution(arg.expression.get(), env, out)) {
                return true;
            }
            // 无法求值时不静默取值：交由调用方按 ER 0069 报错
            // Not evaluable: let the caller report ER 0069 rather than guessing
            return false;
        }
        if (arg.float_constant) {
            out.is_float = true;
            out.float_value = arg.float_value;
            return true;
        }
        // 标识符形实参：可能引用外层泛型的编译期常量参数
        if (!arg.text.empty() && sub != nullptr) {
            auto found = sub->constants.find(arg.text);
            if (found != sub->constants.end()) {
                out = found->second;
                return true;
            }
        }
        out.is_float = false;
        out.int_value = arg.int_value;
        return true;
    }

    bool GenericExpander::evaluate_with_substitution(const AST::Expression* expr,
        const Substitution& sub, ConstantValue& out) {
        ConstantEvaluationContext context;
        context.lookup_constant = [&](const std::string& name, long long& int_value,
            double& float_value, bool& is_float) {
            auto found = sub.constants.find(name);
            if (found == sub.constants.end()) return false;
            int_value = found->second.int_value;
            float_value = found->second.float_value;
            is_float = found->second.is_float;
            return true;
        };
        context.type_layout = [&](const std::string& name, std::size_t& size,
            std::size_t& align) { return resolve_type_layout(name, size, align, sub); };
        long long int_value = 0;
        double float_value = 0.0;
        bool is_float = false;
        if (!evaluate_constant_expression(expr, int_value, float_value, is_float, context)) {
            return false;
        }
        out.int_value = int_value;
        out.float_value = float_value;
        out.is_float = is_float;
        return true;
    }

    bool GenericExpander::resolve_type_layout(const std::string& type_name, std::size_t& size,
        std::size_t& align, const Substitution& sub) const {
        // 内建类型（第 2 章）
        // Builtin types (§2)
        // 0.4.1 §2：int/uint 4 字节，lint/luint/double 8 字节，char/uchar/bool 1 字节
        // 0.4.1 §2: int/uint are 4 bytes, lint/luint/double 8, char/uchar/bool one byte
        if (type_name == "int" || type_name == "uint" || type_name == "float") {
            size = 4; align = 4; return true;
        }
        if (type_name == "lint" || type_name == "luint" || type_name == "double") {
            size = 8; align = 8; return true;
        }
        if (type_name == "char" || type_name == "uchar" || type_name == "bool") {
            size = 1; align = 1; return true;
        }
        if (type_name == "string") { size = 32; align = 8; return true; }
        if (type_name == "file") { size = 8; align = 8; return true; }
        // 泛型参数名 → 其绑定的具体类型
        // A generic parameter name resolves to its bound concrete type
        auto bound = sub.types.find(type_name);
        if (bound != sub.types.end()) {
            return resolve_type_layout(bound->second.struct_name, size, align, sub) ||
                resolve_type_layout(bound->second.to_string(), size, align, sub);
        }
        // 已实例化的结构体：按成员布局计算（与代码生成一致）
        // Instantiated structs: member layout identical to codegen
        auto it = struct_defs_.find(type_name);
        if (it == struct_defs_.end() || it->second == nullptr) return false;
        auto round_up = [](std::size_t value, std::size_t alignment) {
            if (alignment <= 1) return value;
            return (value + alignment - 1) / alignment * alignment;
        };
        std::function<bool(const AST::Type&, std::size_t&, std::size_t&)> type_layout =
            [&](const AST::Type& type, std::size_t& out_size, std::size_t& out_align) -> bool {
            switch (type.kind) {
            case TypeKind::Int: case TypeKind::Float: out_size = 4; out_align = 4; return true;
            case TypeKind::Double: out_size = 8; out_align = 8; return true;
            case TypeKind::Char: case TypeKind::Bool: out_size = 1; out_align = 1; return true;
            case TypeKind::String: out_size = 32; out_align = 8; return true;
            case TypeKind::File: out_size = 8; out_align = 8; return true;
            case TypeKind::Pointer:
            case TypeKind::Function: out_size = 8; out_align = 8; return true;
            case TypeKind::Array: {
                std::size_t element_size = 0;
                std::size_t element_align = 1;
                if (!type.element_type ||
                    !type_layout(*type.element_type, element_size, element_align)) {
                    return false;
                }
                out_size = element_size * type.array_size.value_or(0);
                out_align = element_align;
                return true;
            }
            case TypeKind::Struct: {
                auto def = struct_defs_.find(type.struct_name);
                if (def == struct_defs_.end()) return false;
                std::size_t offset = 0;
                std::size_t max_align = 1;
                for (const auto& member : def->second->members) {
                    std::size_t member_size = 0;
                    std::size_t member_align = 1;
                    if (!type_layout(member.type, member_size, member_align)) return false;
                    max_align = std::max(max_align, member_align);
                    offset = round_up(offset, member_align);
                    offset += member_size;
                }
                out_size = round_up(offset, max_align);
                out_align = max_align;
                return true;
            }
            default:
                return false;
            }
        };
        return type_layout(AST::Type::make_struct(type_name), size, align);
    }

    std::unique_ptr<Expression> GenericExpander::make_typed_constant_literal(SourceLocation loc,
        const ConstantValue& value, const AST::Type& type) {
        // 第 19 章：编译期常量参数在泛型体内作为“对应类型的常量”使用
        // §19: a constant parameter is used as a constant of its declared type
        if (value.is_float || type.kind == TypeKind::Float || type.kind == TypeKind::Double) {
            return make_constant_literal(loc, value);
        }
        return make_constant_literal(loc, value);
    }

    // ============================================================================
    // 特化匹配
    // ============================================================================

    namespace {
        // 判断某个类型名是否为模式中的自由标识符（既非内建类型也非已声明结构体）
        // Whether a type name is a free identifier of a pattern
        bool is_free_identifier(const std::string& name,
            const std::unordered_map<std::string, StructDefinition*>& structs) {
            if (is_builtin_type_name(name)) return false;
            return structs.find(name) == structs.end();
        }
    }

    // 把主泛型形态的参数列表转换为自由标识符模式
    // Convert a parameter list into free-identifier patterns
    std::vector<GenericPatternArg> GenericExpander::parameters_as_free_patterns(
        GenericDefinition* def) const {
        std::vector<GenericPatternArg> patterns;
        for (const GenericParameter& param : def->parameters) {
            GenericPatternArg pattern;
            if (param.is_type) {
                pattern.is_constant = false;
                pattern.type = Type::make_struct(param.name);
                pattern.text = param.name;
            }
            else {
                // 自由的编译期常量参数：匹配任意编译期常量
                // Free compile-time constant parameter: matches any compile-time constant
                pattern.is_constant = true;
                pattern.free_constant = true;
                pattern.text = param.name;
            }
            patterns.push_back(std::move(pattern));
        }
        return patterns;
    }

    // 模式结构签名：自由标识符归一化为 "F"，用于“真正重复定义”判定
    // Structural signature of a pattern, normalizing free identifiers to "F"
    std::string GenericExpander::pattern_signature(const GenericPatternArg& pattern) const {
        std::function<std::string(const Type&)> type_sig = [&](const Type& t) -> std::string {
            switch (t.kind) {
            case TypeKind::Struct:
                return is_free_identifier(t.struct_name, struct_defs_)
                    ? std::string("F") : ("S:" + t.struct_name);
            case TypeKind::Pointer:
                return "P(" + (t.pointee_type ? type_sig(*t.pointee_type) : std::string("?")) + ")";
            case TypeKind::Array:
                return "A(" + (t.element_type ? type_sig(*t.element_type) : std::string("?")) +
                    (t.array_size.has_value() ? "," + std::to_string(*t.array_size) : "") + ")";
            default:
                return t.to_string();
            }
        };
        if (pattern.is_constant) {
            if (pattern.free_constant) return "FC";
            if (pattern.string_constant) return "CS:" + pattern.string_value;
            if (pattern.float_constant) {
                char buffer[64];
                std::snprintf(buffer, sizeof(buffer), "%g", pattern.float_value);
                return std::string("C:") + buffer;
            }
            return "C:" + std::to_string(pattern.int_value);
        }
        return type_sig(pattern.type);
    }

    bool GenericExpander::duplicate_pattern_signature(const GenericEntry& entry,
        GenericDefinition* def) const {
        auto same_signature = [&](GenericDefinition* other) {
            if (other == def) return false;
            if (other->patterns.size() != def->patterns.size()) return false;
            for (std::size_t i = 0; i < def->patterns.size(); ++i) {
                if (pattern_signature(other->patterns[i]) != pattern_signature(def->patterns[i])) {
                    return false;
                }
            }
            return true;
        };
        if (entry.primary != nullptr && same_signature(entry.primary)) return true;
        for (GenericDefinition* spec : entry.specializations) {
            if (same_signature(spec)) return true;
        }
        return false;
    }

    // 单位置模式偏序（Gallt 0.3.txt §19“特化程度偏序定义”）
    // Single-position pattern order (Gallt 0.3.txt §19)
    GenericExpander::PatternOrder GenericExpander::compare_pattern_type(const Type& a,
        const Type& b) const {
        bool a_free = (a.kind == TypeKind::Struct &&
            is_free_identifier(a.struct_name, struct_defs_));
        bool b_free = (b.kind == TypeKind::Struct &&
            is_free_identifier(b.struct_name, struct_defs_));
        if (a_free && b_free) return PatternOrder::Equal;
        if (a_free) return PatternOrder::Worse;        // B 更特化
        if (b_free) return PatternOrder::Better;       // A 更特化
        // 两者均非裸自由标识符：可能是复合模式或具体类型
        bool a_composite = (a.kind == TypeKind::Pointer || a.kind == TypeKind::Array);
        bool b_composite = (b.kind == TypeKind::Pointer || b.kind == TypeKind::Array);
        if (!a_composite && !b_composite) {
            // 具体类型之间：相同类型一样特化；不同具体类型在实例化时不可能同时匹配
            // Concrete types are equally specialized (different ones cannot both match)
            return PatternOrder::Equal;
        }
        if (a_composite && !b_composite) return PatternOrder::Worse;
        if (!a_composite && b_composite) return PatternOrder::Better;
        // 相同结构的复合模式按内部模式递归比较；结构不同则不可比较
        // Same-shaped composites recurse; different shapes are incomparable
        if (a.kind != b.kind) return PatternOrder::Incomparable;
        if (a.kind == TypeKind::Pointer) {
            if (!a.pointee_type || !b.pointee_type) return PatternOrder::Incomparable;
            return compare_pattern_type(*a.pointee_type, *b.pointee_type);
        }
        if (!a.element_type || !b.element_type) return PatternOrder::Incomparable;
        if (a.array_size.has_value() != b.array_size.has_value()) {
            return PatternOrder::Incomparable;
        }
        if (a.array_size.has_value() && *a.array_size != *b.array_size) {
            return PatternOrder::Incomparable;
        }
        return compare_pattern_type(*a.element_type, *b.element_type);
    }

    GenericExpander::PatternOrder GenericExpander::compare_pattern(
        const GenericPatternArg& a, const GenericPatternArg& b) const {
        if (a.is_constant && b.is_constant) {
            if (a.free_constant && b.free_constant) return PatternOrder::Equal;
            if (a.free_constant) return PatternOrder::Worse;
            if (b.free_constant) return PatternOrder::Better;
            // Gallt 0.4.txt §19（修正后）：字符串常量模式与具体类型/常量同样特化；
            // 相同字面量一样特化，不同字面量不可能同时匹配同一实参
            // Gallt 0.4.txt §19 (corrected): string constant patterns are concrete
            // constants; equal literals are equally specialized and different literals
            // can never both match one argument
            if (a.string_constant || b.string_constant) {
                if (a.string_constant != b.string_constant) {
                    return PatternOrder::Incomparable;
                }
                return (a.string_value == b.string_value)
                    ? PatternOrder::Equal : PatternOrder::Incomparable;
            }
            if (a.float_constant != b.float_constant) return PatternOrder::Incomparable;
            bool same = a.float_constant ? (a.float_value == b.float_value)
                                         : (a.int_value == b.int_value);
            return same ? PatternOrder::Equal : PatternOrder::Equal;
        }
        // 常量模式与类型模式不可能匹配同一实参，故二者不可比较（第 19 章）
        // A constant pattern and a type pattern can never match the same argument, so they
        // are incomparable
        if (a.is_constant || b.is_constant) return PatternOrder::Incomparable;
        return compare_pattern_type(a.type, b.type);
    }

    bool GenericExpander::at_least_as_specialized(GenericDefinition* a,
        GenericDefinition* b) const {
        if (a->patterns.size() != b->patterns.size()) return false;
        for (std::size_t i = 0; i < a->patterns.size(); ++i) {
            // 只有当 B 在该位置严格更特化时，A 才“不至少与 B 一样特化”
            // A fails to be at least as specialized only when B is strictly better here
            if (compare_pattern(a->patterns[i], b->patterns[i]) == PatternOrder::Worse) {
                return false;
            }
        }
        return true;
    }

    bool GenericExpander::more_specialized(GenericDefinition* a,
        GenericDefinition* b) const {
        return at_least_as_specialized(a, b) && !at_least_as_specialized(b, a);
    }

    GenericExpander::MatchResult GenericExpander::match_specialization(
        GenericDefinition* def, const GenericRef& ref) {
        MatchResult result;
        if (def->patterns.size() != ref.arguments.size()) return result;

        std::function<bool(const Type&, const Type&)> match;
        match = [&](const Type& pattern, const Type& actual) -> bool {
            switch (pattern.kind) {
            case TypeKind::Struct:
                if (is_free_identifier(pattern.struct_name, struct_defs_)) {
                    // 推导自由标识符：多次出现必须推导为一致类型（ER 0082）
                    auto found = result.substitution.types.find(pattern.struct_name);
                    if (found == result.substitution.types.end()) {
                        result.substitution.types[pattern.struct_name] = actual;
                    }
                    else if (!(found->second == actual)) {
                        report(def->location, ErrorCode::GenericPatternDeductionMismatch, std::vector<std::string>{ pattern.struct_name });
                        return false;
                    }
                    return true;
                }
                return pattern.struct_name == actual.struct_name && actual.kind == TypeKind::Struct;
            case TypeKind::Pointer:
                if (actual.kind != TypeKind::Pointer || !actual.pointee_type || !pattern.pointee_type) {
                    return false;
                }
                return match(*pattern.pointee_type, *actual.pointee_type);
            case TypeKind::Array:
                if (actual.kind != TypeKind::Array || !actual.element_type || !pattern.element_type) {
                    return false;
                }
                if (pattern.array_size.has_value() && actual.array_size.has_value() &&
                    pattern.array_size.value() != actual.array_size.value()) {
                    return false;
                }
                return match(*pattern.element_type, *actual.element_type);
            default:
                return pattern == actual;
            }
        };

        for (std::size_t i = 0; i < def->patterns.size(); ++i) {
            const GenericPatternArg& pattern = def->patterns[i];
            const GenericArgument& arg = ref.arguments[i];
            if (pattern.is_constant) {
                if (arg.is_type) return MatchResult{};
                // 自由的编译期常量模式匹配任意编译期常量
                // A free constant pattern matches any compile-time constant
                if (pattern.free_constant) continue;
                ConstantValue value;
                if (!resolve_constant_argument(arg, nullptr, value)) return MatchResult{};
                // Gallt 0.4.txt §19（修正后）：字符串常量模式按字符串内容精确匹配
                // Gallt 0.4.txt §19 (corrected): a string constant pattern matches by value
                if (pattern.string_constant) {
                    if (!value.is_string) return MatchResult{};
                    if (pattern.string_value != value.string_value) return MatchResult{};
                    continue;
                }
                if (value.is_string) return MatchResult{};
                if (pattern.float_constant != value.is_float) return MatchResult{};
                if (pattern.float_constant) {
                    if (pattern.float_value != value.float_value) return MatchResult{};
                }
                else if (pattern.int_value != value.int_value) {
                    return MatchResult{};
                }
            }
            else {
                if (!arg.is_type) return MatchResult{};
                if (!match(pattern.type, arg.type)) return MatchResult{};
            }
        }
        result.matched = true;
        return result;
    }

    bool GenericExpander::constraint_satisfied(const GenericConstraint& constraint,
        const Type& arg) {
        switch (constraint.kind) {
        case GenericConstraint::Kind::Any:
            return true;
        case GenericConstraint::Kind::Struct:
            return arg.kind == TypeKind::Struct;
        case GenericConstraint::Kind::Pointer:
            return arg.kind == TypeKind::Pointer;
        case GenericConstraint::Kind::Array:
            return arg.kind == TypeKind::Array;
        case GenericConstraint::Kind::File:
            return arg.kind == TypeKind::File;
        case GenericConstraint::Kind::Types:
            for (const Type& allowed : constraint.types) {
                if (allowed.kind == TypeKind::Struct && arg.kind == TypeKind::Struct) {
                    // 约束中的泛型实例成员类型（Box<int>.Item）先解析为具体类型再比较
                    // Resolve instantiated generic member types in the constraint first
                    Type resolved = allowed;
                    if (resolved.generic_ref) {
                        resolve_type(resolved);
                    }
                    if (resolved.struct_name == arg.struct_name) return true;
                    continue;
                }
                if (allowed == arg) return true;
            }
            return false;
        }
        return false;
    }

    // ============================================================================
    // 实例化
    // ============================================================================

    std::optional<std::string> GenericExpander::ensure_instantiation(const GenericRef& ref,
        bool whole_block) {
        auto entry_it = generics_.find(ref.generic_name);
        if (entry_it == generics_.end()) {
            report(ref.location, ErrorCode::GenericUndefined, std::vector<std::string>{ ref.generic_name });
            return std::nullopt;
        }
        GenericEntry& entry = entry_it->second;
        if (entry.primary == nullptr) {
            // 只有特化、没有可用主泛型（定义阶段已报 ER 0076）
            report(ref.location, ErrorCode::GenericNoMatch, std::vector<std::string>{ ref.generic_name, arguments_text(ref) });
            return std::nullopt;
        }
        std::size_t expected = entry.primary->parameters.size();
        if (ref.arguments.size() != expected) {
            report(ref.location, ErrorCode::GenericArgCountMismatch, std::vector<std::string>{ ref.generic_name, arguments_text(ref),
                  std::to_string(expected), std::to_string(ref.arguments.size()) });
            return std::nullopt;
        }

        // ---- 主泛型实参检查 ----
        Substitution primary_sub;
        for (std::size_t i = 0; i < expected; ++i) {
            const GenericParameter& param = entry.primary->parameters[i];
            const GenericArgument& arg = ref.arguments[i];
            if (param.is_type) {
                if (!arg.is_type) {
                    report(ref.location, ErrorCode::GenericNonTypeArgTypeMismatch, std::vector<std::string>{ param.name, arg.normalize() });
                    return std::nullopt;
                }
                if (!constraint_satisfied(param.constraint, arg.type)) {
                    // 说明：arg.type 若为泛型实例成员类型，先解析为具体类型再比较
                    // (instantiated generic member argument types are resolved first)
                    Type resolved_arg = arg.type;
                    if (resolved_arg.generic_ref) resolve_type(resolved_arg);
                    if (constraint_satisfied(param.constraint, resolved_arg)) {
                        primary_sub.types[param.name] = resolved_arg;
                        continue;
                    }
                    report(ref.location, ErrorCode::GenericConstraintViolated, std::vector<std::string>{ ref.generic_name, arguments_text(ref),
                          std::to_string(i + 1), param.constraint.to_string(),
                          arg.type.to_string() });
                    return std::nullopt;
                }
                primary_sub.types[param.name] = arg.type;
            }
            else {
                if (arg.is_type) {
                    report(ref.location, ErrorCode::GenericNonTypeArgTypeMismatch, std::vector<std::string>{ param.constant_type_is_parameter ? param.constant_type_parameter
                                                           : param.constant_type.to_string(),
                          arg.type.to_string() });
                    return std::nullopt;
                }
                // 实参类型无法转换为常量参数类型（ER 0070）：先于“是否编译期常量”判定，
                // 这样字符串等非数值常量实参报告的是 ER 0070 而不是 ER 0069
                // A non-convertible argument type is ER 0070, checked before the
                // "is it a compile-time constant" question
                if (arg.constant_actual_type.kind != TypeKind::Void) {
                    Type target = param.constant_type;
                    if (param.constant_type_is_parameter) {
                        auto found = primary_sub.types.find(param.constant_type_parameter);
                        if (found != primary_sub.types.end()) target = found->second;
                    }
                    // Gallt 0.4.txt §19：字符串常量实参只对 string 常量参数合法
                    // Gallt 0.4.txt §19: string constant arguments are only valid for
                    // string constant parameters
                    bool string_argument =
                        (arg.constant_actual_type.kind == TypeKind::String);
                    if (target.kind != TypeKind::Void &&
                        !(string_argument && target.kind == TypeKind::String)) {
                        report(ref.location, ErrorCode::GenericNonTypeArgTypeMismatch, std::vector<std::string>{ target.to_string(), arg.constant_actual_type.to_string() });
                        return std::nullopt;
                    }
                }
              ConstantValue value;
              if (!resolve_constant_argument(arg, nullptr, value)) {
                  // ER 0069：非类型实参必须是编译期常量表达式（占位符填实参源代码文本）
                  // ER 0069: a non-type argument must be a compile-time constant
                  report(ref.location, ErrorCode::GenericNonTypeArgNotConstant,
                      std::vector<std::string>{ arg.text.empty() ? arg.normalize()
                                                                 : arg.text });
                  return std::nullopt;
              }
                // 常量参数类型检查（ER 0070 / ER 0071）
                Type target = param.constant_type;
                if (param.constant_type_is_parameter) {
                    auto found = primary_sub.types.find(param.constant_type_parameter);
                    if (found != primary_sub.types.end()) target = found->second;
                }
                if (target.kind != TypeKind::Void) {
                    bool integral = (target.kind == TypeKind::Int || target.kind == TypeKind::Char ||
                        target.kind == TypeKind::Bool);
                    if (integral && value.is_float) {
                        // 允许截断，但必须符合转换规则
                        value.is_float = false;
                        value.int_value = static_cast<long long>(value.float_value);
                    }
                    else if (integral) {
                        long long limit = (target.kind == TypeKind::Char) ? 255
                            : (target.kind == TypeKind::Bool ? 1
                                : (long long)0x7fffffffLL);
                        if (value.int_value > limit || value.int_value < -limit - 1) {
                            report(ref.location, ErrorCode::GenericNonTypeArgOutOfRange, std::vector<std::string>{ target.to_string() });
                            return std::nullopt;
                        }
                    }
                    else if (target.kind == TypeKind::Float || target.kind == TypeKind::Double) {
                        if (!value.is_float) {
                            value.float_value = static_cast<double>(value.int_value);
                            value.is_float = true;
                        }
                    }
                }
                primary_sub.constants[param.name] = value;
                // 记录常量参数的声明类型，供泛型体内生成对应类型的字面量（§19）
                // Remember the declared type so the body can build a typed literal (§19)
                AST::Type declared = param.constant_type;
                if (param.constant_type_is_parameter) {
                    auto found = primary_sub.types.find(param.constant_type_parameter);
                    if (found != primary_sub.types.end()) declared = found->second;
                }
                primary_sub.constant_types[param.name] = declared;
            }
        }

        // ---- 特化匹配 ----
        std::vector<GenericDefinition*> candidates;
        std::vector<Substitution> candidate_subs;
        for (GenericDefinition* spec : entry.specializations) {
            MatchResult mr = match_specialization(spec, ref);
            if (!mr.matched) continue;
            candidates.push_back(spec);
            candidate_subs.push_back(mr.substitution);
        }
        GenericDefinition* block = entry.primary;
        Substitution sub = primary_sub;
        if (!candidates.empty()) {
            int best = -1;
            bool ambiguous = false;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                bool is_best = true;
                for (std::size_t j = 0; j < candidates.size(); ++j) {
                    if (i == j) continue;
                    // 候选 i 必须比所有其它候选更特化（第 19 章偏序定义）
                    // Candidate i must be more specialized than every other candidate
                    if (!more_specialized(candidates[i], candidates[j])) {
                        is_best = false;
                        break;
                    }
                }
                if (is_best) {
                    if (best != -1) ambiguous = true;
                    else best = static_cast<int>(i);
                }
            }
            // 多个候选但不存在唯一最特化者 → 二义性（ER 0067）
            // Several candidates without a unique most-specialized one is ambiguous
            if (ambiguous || best < 0) {
               report(ref.location, ErrorCode::GenericAmbiguousSpecialization, std::vector<std::string>{ ref.generic_name, arguments_text(ref) });
               return std::nullopt;
           }
            if (best >= 0) {
                block = candidates[static_cast<std::size_t>(best)];
                sub = candidate_subs[static_cast<std::size_t>(best)];
            }
        }

        // ---- 规范化与幂等键 ----
        GenericRef key_ref = ref;
        key_ref.member.clear();
        std::string instance_key = key_ref.mangle();
        auto kind_it = instantiated_kind_signatures_.find(instance_key);
        std::string kind_signature = arguments_kind_signature(ref);
        if (kind_it == instantiated_kind_signatures_.end()) {
            instantiated_kind_signatures_[instance_key] = kind_signature;
        }
        else if (kind_it->second != kind_signature) {
            // ER 0079：相同实参组合产生不同规范化结果
            report(ref.location, ErrorCode::GenericNormalizationConflict,
                std::vector<std::string>{});
            return std::nullopt;
        }

        // ---- 建立块内成员映射 ----
        sub.instance = key_ref;
        // Gallt 0.4.txt §19：先处理编译期代码生成项（emit / 编译期 if），
        // 得到的成员与泛型块内声明的成员一起参与命名修饰与实例化
        // Gallt 0.4.txt §19: run the compile-time code-generation items first; the
        // members they produce take part in mangling and instantiation like declared ones
        std::vector<TopLevel*> block_member_ptrs;
        std::vector<std::unique_ptr<TopLevel>> parsed_emit_members;
        for (auto& member : block->members) {
            block_member_ptrs.push_back(member.get());
        }
        if (!block->compile_time_items.empty()) {
            std::string emit_text;
            SourceLocation emit_loc = ref.location;
            if (!process_compile_time_items(block->compile_time_items, sub, emit_text,
                emit_loc, block_member_ptrs, parsed_emit_members)) {
                return std::nullopt;
            }
            if (!flush_emit_string(emit_text, emit_loc, block_member_ptrs,
                parsed_emit_members)) {
                return std::nullopt;
            }
        }
        // 记录本作用域可见的实例，并统计“主泛型有、特化块没有”的成员（ER 0075）
        // Remember the instance in this scope and collect members the specialization omits
        if (!instance_scopes_.empty()) {
            instance_scopes_.back().insert(instance_key);
        }
        if (block != entry.primary) {
            std::unordered_set<std::string> block_members;
            for (auto& member : block->members) {
                if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) {
                    block_members.insert(sd->name);
                }
                else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) {
                    block_members.insert(fd->name);
                }
            }
            for (auto& member : entry.primary->members) {
                std::string member_name;
                if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) member_name = sd->name;
                else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) member_name = fd->name;
                if (member_name.empty() || block_members.count(member_name) != 0) continue;
                missing_members_[instance_key].insert(member_name);
            }
        }
        {
            std::vector<std::string>& members = instance_struct_members_[instance_key];
            members.clear();
            for (TopLevel* member : block_member_ptrs) {
                if (auto* sd = dynamic_cast<StructDefinition*>(member)) {
                    members.push_back(sd->name);
                }
            }
        }
        std::unordered_map<std::string, std::string> member_types;
        std::unordered_map<std::string, std::string> member_functions;
        for (TopLevel* member : block_member_ptrs) {
           if (auto* sd = dynamic_cast<StructDefinition*>(member)) {
               std::string mangled = instance_key + "$" + sd->name;
               member_types[sd->name] = mangled;
               sub.types[sd->name] = Type::make_struct(mangled);
                sub.members[sd->name] = mangled;
           }
           else if (auto* fd = dynamic_cast<FunctionDefinition*>(member)) {
               member_functions[fd->name] = instance_key + "$" + fd->name;
                sub.members[fd->name] = instance_key + "$" + fd->name;
           }
        }

        // ---- 物化成员 ----
        auto materialize = [&](const std::string& member_name, bool register_short) -> std::string {
            for (TopLevel* member : block_member_ptrs) {
                if (auto* sd = dynamic_cast<StructDefinition*>(member)) {
                    if (!member_name.empty() && sd->name != member_name) continue;
                    std::string mangled = instance_key + "$" + sd->name;
                    if (instantiated_members_.insert(mangled).second) {
                        auto clone = std::make_unique<StructDefinition>(sd->location, mangled,
                            std::vector<StructDefinition::Member>{});
                        clone->no_copy = sd->no_copy;
                        clone->no_move = sd->no_move;
                        for (const auto& m : sd->members) {
                            clone->members.push_back(clone_member(m, sub));
                        }
                        // 第 19 章 + 第 20 章：实例化结构体必须一并复制特殊成员函数，
                        // 否则实例化类型（以及后续的生命周期降低）会丢失构造/析构/拷贝/移动语义
                        // Instantiated structs must carry their special members over, otherwise
                        // the instantiated type loses its constructor/destructor/copy/move semantics
                        for (const auto& special : sd->special_members) {
                            clone->special_members.push_back(clone_special_member(special.get(),
                                sub));
                        }
                        StructDefinition* raw = clone.get();
                        struct_defs_[mangled] = raw;
                        instantiated_structs_[mangled] = raw;
                        add_top_level(std::move(clone));
                    }
                    if (register_short) {
                        ShortBinding binding;
                        binding.target = mangled;
                        binding.is_type = true;
                        binding.instance = instance_key;
                        register_short_name(sd->name, binding, ref.generic_name, sd->location);
                    }
                    if (!member_name.empty()) return mangled;
                }
                else if (auto* fd = dynamic_cast<FunctionDefinition*>(member)) {
                    if (!member_name.empty() && fd->name != member_name) continue;
                    std::string mangled = instance_key + "$" + fd->name;
                    if (instantiated_members_.insert(mangled).second) {
                        auto clone = clone_function(fd, sub, mangled);
                        func_defs_[mangled] = clone.get();
                        add_top_level(std::move(clone));
                    }
                    if (register_short) {
                        ShortBinding binding;
                        binding.target = mangled;
                        binding.is_type = false;
                        binding.instance = instance_key;
                        register_short_name(fd->name, binding, ref.generic_name, fd->location);
                    }
                    if (!member_name.empty()) return mangled;
                }
            }
            return std::string();
        };

        if (whole_block) {
            if (instantiated_blocks_.insert(instance_key).second) {
                for (TopLevel* member : block_member_ptrs) {
                    if (auto* sd = dynamic_cast<StructDefinition*>(member)) {
                        materialize(sd->name, true);
                    }
                    else if (auto* fd = dynamic_cast<FunctionDefinition*>(member)) {
                        materialize(fd->name, true);
                    }
                }
            }
            else {
                // 幂等：重复实例化不重复引入短名
                for (TopLevel* member : block_member_ptrs) {
                    std::string member_name;
                    if (auto* sd = dynamic_cast<StructDefinition*>(member)) member_name = sd->name;
                    else if (auto* fd = dynamic_cast<FunctionDefinition*>(member)) member_name = fd->name;
                    if (!member_name.empty()) materialize(member_name, false);
                }
            }
            return instance_key;
        }

        // 按成员实例化
        if (ref.member.empty()) {
            return instance_key;
        }
        // Gallt 0.4.txt §19：`X<...>::m` 按成员实例化并引入短名，
        // `X<...>.m` 是限定名，只直接解析，不引入短名
        // Gallt 0.4.txt §19: `X<...>::m` instantiates the member and introduces its short
        // name; `X<...>.m` is a qualified name that resolves directly without one
        std::string target = materialize(ref.member, ref.member_scope_access);
        if (target.empty()) {
            // 成员不存在：若主泛型中存在该成员，说明特化块缺少该成员
            bool in_primary = false;
            if (entry.primary != block) {
                for (auto& member : entry.primary->members) {
                    std::string member_name;
                    if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) member_name = sd->name;
                    else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) member_name = fd->name;
                    if (member_name == ref.member) { in_primary = true; break; }
                }
            }
            if (in_primary) {
                report(ref.location, ErrorCode::GenericSpecializationMissingMember, std::vector<std::string>{ instance_key, ref.member });
            }
            else {
                report(ref.location, ErrorCode::GenericMemberNotInInstantiation, std::vector<std::string>{ ref.member, instance_key });
            }
            return std::nullopt;
        }
        return target;
    }

    // ============================================================================
    // 克隆
    // ============================================================================

    AST::StructDefinition::Member GenericExpander::clone_member(
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
            // 数组长度中的编译期常量表达式：N、N*2、N+1、int(N)、size(T) 等（§19）
            // Array-length compile-time constant expressions (§19)
            ConstantValue value;
            if (evaluate_with_substitution(member.array_size_expr.get(), sub, value)) {
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
                elements.push_back(clone_initializer(element.get(), sub));
            }
            return std::make_unique<ArrayInitializer>(init->location, std::move(elements));
        }
        return nullptr;
    }

    std::unique_ptr<Expression> GenericExpander::make_constant_literal(SourceLocation loc,
        const ConstantValue& value) {
        // 合成的字面量词素存放在 lexeme_pool_ 中以保证 string_view 生命周期
        // Synthesized literal spellings live in lexeme_pool_ so the string_view stays valid
        std::string text;
        TokenType type = TokenType::IntegerLiteral;
        if (value.is_string) {
            // Gallt 0.4.txt §19：字符串编译期常量参数替换为字符串字面量
            // Gallt 0.4.txt §19: a string constant parameter becomes a string literal
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
        }
        else if (value.is_float) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%g", value.float_value);
            text = buffer;
            type = TokenType::FloatLiteral;
        }
        else {
            text = std::to_string(value.int_value);
        }
        lexeme_pool_.push_back(text);
        Token literal(type, loc, lexeme_pool_.back());
        return std::make_unique<PrimaryExpression>(loc, literal);
    }

    std::unique_ptr<Expression> GenericExpander::clone_expression(
        const Expression* expr, const Substitution& sub) {
        if (expr == nullptr) return nullptr;
        // Gallt 0.4.txt §19：编译期属性（.size / .align / .typename / .is_*）与
        // 编译期属性比较在克隆时求值为常量
        // Gallt 0.4.txt §19: compile-time properties (.size/.align/.typename/.is_*) and
        // property comparisons evaluate to constants while cloning
        if (auto* e = dynamic_cast<const PostfixExpression*>(expr)) {
            if (auto rewritten = rewrite_property_expression(e, sub)) {
                return rewritten;
            }
        }
        if (auto* e = dynamic_cast<const CompileTimePropertyExpression*>(expr)) {
            std::vector<const Expression*> args;
            for (const auto& arg : e->arguments) args.push_back(arg.get());
            bool value = false;
            if (eval_bool_property(e->receiver.get(), e->property, args, sub, value)) {
                return make_constant_literal(e->location,
                    ConstantValue{ value ? 1LL : 0LL, 0.0, false, false, std::string() });
            }
            // 非布尔属性（如 typename）在表达式上下文中不适用
            // Non-boolean properties such as typename do not apply in expressions
            auto* receiver = dynamic_cast<const PrimaryExpression*>(e->receiver.get());
            report(e->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ e->property, receiver != nullptr ? receiver->identifier
                                                   : std::string("<expr>") });
            return make_constant_literal(e->location,
                ConstantValue{ 0LL, 0.0, false, false, std::string() });
        }
        if (auto* e = dynamic_cast<const AssignmentExpression*>(expr)) {
            // 第 19 章：编译期常量参数不允许作为左值（ER 0078）
            // §19: a constant parameter must not be used as an lvalue (ER 0078)
            if (auto* target = dynamic_cast<const PrimaryExpression*>(e->left.get())) {
                if (target->kind == PrimaryExpression::Kind::Identifier &&
                    sub.constants.count(target->identifier) != 0) {
                    report(target->location, ErrorCode::GenericConstantParameterRuntimeUse, std::vector<std::string>{ target->identifier });
                }
            }
            return std::make_unique<AssignmentExpression>(e->location,
                clone_expression(e->left.get(), sub), e->op,
                clone_expression(e->right.get(), sub));
        }
        if (auto* e = dynamic_cast<const LogicalOrExpression*>(expr)) {
            return std::make_unique<LogicalOrExpression>(e->location,
                clone_expression(e->left.get(), sub), clone_expression(e->right.get(), sub));
        }
        if (auto* e = dynamic_cast<const LogicalAndExpression*>(expr)) {
            return std::make_unique<LogicalAndExpression>(e->location,
                clone_expression(e->left.get(), sub), clone_expression(e->right.get(), sub));
        }
        if (auto* e = dynamic_cast<const ComparisonExpression*>(expr)) {
            return std::make_unique<ComparisonExpression>(e->location,
                clone_expression(e->left.get(), sub), e->op,
                clone_expression(e->right.get(), sub));
        }
        if (auto* e = dynamic_cast<const AdditiveExpression*>(expr)) {
            return std::make_unique<AdditiveExpression>(e->location,
                clone_expression(e->left.get(), sub), e->op,
                clone_expression(e->right.get(), sub));
        }
        if (auto* e = dynamic_cast<const MultiplicativeExpression*>(expr)) {
            return std::make_unique<MultiplicativeExpression>(e->location,
                clone_expression(e->left.get(), sub), e->op,
                clone_expression(e->right.get(), sub));
        }
        if (auto* e = dynamic_cast<const PowerExpression*>(expr)) {
            return std::make_unique<PowerExpression>(e->location,
                clone_expression(e->left.get(), sub), clone_expression(e->right.get(), sub));
        }
        if (auto* e = dynamic_cast<const UnaryExpression*>(expr)) {
            // 第 19 章：编译期常量参数不允许取地址或自增自减（ER 0078）
            // §19: address-of/inc/dec on a constant parameter is ER 0078
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
            // int(N) 形式：编译期常量参数允许通过类型转换参与常量表达式
            // int(N): a constant parameter may take part through a cast
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
                // 编译期常量参数：仅在类型/数组长度上下文中替换
                // Constant parameters are substituted only in type/array-length contexts
                auto found = sub.types.find(e->identifier);
                if (found != sub.types.end()) {
                    return std::make_unique<PrimaryExpression>(e->location,
                        found->second.to_string());
                }
                auto constant = sub.constants.find(e->identifier);
                if (constant != sub.constants.end()) {
                    // 第 19 章：编译期常量参数在泛型体内作为对应类型的常量使用，
                    // 因此这里直接替换为该类型的常量字面量（不再误报 ER 0078）
                    // §19: a constant parameter is used as a constant of its type, so replace
                    // it with a typed literal instead of reporting ER 0078
                    AST::Type declared = AST::Type::make_int();
                    auto declared_type = sub.constant_types.find(e->identifier);
                    if (declared_type != sub.constant_types.end()) {
                        declared = declared_type->second;
                    }
                    return make_typed_constant_literal(e->location, constant->second, declared);
                }
                // 泛型块内成员名：重写为命名修饰后的名字
                // Block member names are rewritten to their mangled names
                auto member = sub.members.find(e->identifier);
                if (member != sub.members.end()) {
                    // 依赖成员：按成员实例化需连带实例化被引用的成员，避免出现
                    // 已经命名修饰却找不到定义的情况
                    // Dependent member: instantiate the referenced member as well so the
                    // mangled name always has a definition
                    GenericRef dependency = sub.instance;
                    dependency.member = e->identifier;
                    dependency.location = e->location;
                    ensure_instantiation(dependency, false);
                    return std::make_unique<PrimaryExpression>(e->location, member->second);
                }
               // 短名（类型名）重写
                for (auto it = short_scopes_.rbegin(); it != short_scopes_.rend(); ++it) {
                    auto binding = it->find(e->identifier);
                    if (binding != it->end()) {
                        if (binding->second.is_type) {
                            return std::make_unique<PrimaryExpression>(e->location,
                                binding->second.target);
                        }
                        break;
                    }
                }
                return std::make_unique<PrimaryExpression>(e->location, e->identifier);
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
                    if (arg.is_type) {
                        Type substituted = Type::make_void();
                        substitute_type(arg.type, sub, substituted);
                        arg.type = std::move(substituted);
                        arg.text = arg.type.to_string();
                    }
                    else {
                        auto found = sub.constants.find(arg.text);
                        if (found != sub.constants.end()) {
                            if (found->second.is_float) {
                                arg.float_constant = true;
                                arg.float_value = found->second.float_value;
                            }
                            else {
                                arg.int_value = found->second.int_value;
                            }
                            arg.text = arg.normalize();
                        }
                    }
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
                // 命名空间限定名已在命名空间降低阶段解析（Gallt 0.4.txt §21）
                // Namespace-qualified names are resolved by the namespace lowering pass
                return std::make_unique<PrimaryExpression>(e->location, e->qualified_path);
            }
        }
        return nullptr;
    }

    std::unique_ptr<Statement> GenericExpander::clone_statement(
        const Statement* stmt, const Substitution& sub) {
        if (stmt == nullptr) return nullptr;
        if (auto* s = dynamic_cast<const Block*>(stmt)) {
            std::vector<std::unique_ptr<Statement>> statements;
            for (const auto& child : s->statements) {
                statements.push_back(clone_statement(child.get(), sub));
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
                // 局部数组长度中的编译期常量表达式（§19）
                // Compile-time constant expression in a local array length (§19)
                ConstantValue value;
                if (evaluate_with_substitution(s->array_size_expr.get(), sub, value)) {
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
            return std::make_unique<IfStatement>(s->location,
                clone_expression(s->condition.get(), sub),
                clone_statement(s->then_block.get(), sub),
                clone_statement(s->else_block.get(), sub));
        }
        if (auto* s = dynamic_cast<const ForStatement*>(stmt)) {
            return std::make_unique<ForStatement>(s->location,
                clone_statement(s->init.get(), sub),
                clone_expression(s->condition.get(), sub),
                clone_expression(s->step.get(), sub),
                clone_statement(s->body.get(), sub));
        }
        if (auto* s = dynamic_cast<const WhileStatement*>(stmt)) {
            return std::make_unique<WhileStatement>(s->location,
                clone_expression(s->condition.get(), sub),
                clone_statement(s->body.get(), sub));
        }
        if (auto* s = dynamic_cast<const BreakStatement*>(stmt)) {
            return std::make_unique<BreakStatement>(s->location);
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
            return std::make_unique<InstantiationStatement>(s->location, s->reference);
        }
        return nullptr;
    }

    std::unique_ptr<FunctionDefinition> GenericExpander::clone_function(
        const FunctionDefinition* func, const Substitution& sub, const std::string& name) {
        Type ret = Type::make_void();
        substitute_type(func->return_type, sub, ret);
        std::vector<Type> params;
        for (const Type& p : func->parameters) {
            Type substituted = Type::make_void();
            substitute_type(p, sub, substituted);
            params.push_back(std::move(substituted));
        }
        auto clone = std::make_unique<FunctionDefinition>(func->location, std::move(ret), name,
            params, func->param_names, clone_statement(func->body.get(), sub));
        // 默认参数（第 8 章）：实例化时按替换环境克隆，避免实例化后默认参数丢失
        // Default arguments (§8): cloned under the substitution so instantiations keep them
        clone->param_defaults.reserve(func->param_defaults.size());
        for (const auto& default_value : func->param_defaults) {
            if (default_value == nullptr) {
                clone->param_defaults.push_back(nullptr);
            }
            else {
                clone->param_defaults.push_back(clone_expression(default_value.get(), sub));
            }
        }
        return clone;
    }

    std::unique_ptr<SpecialMemberFunction> GenericExpander::clone_special_member(
        const SpecialMemberFunction* member, const Substitution& sub) {
        // 第 20 章特殊成员：按替换环境克隆类型、形参、默认参数与函数体
        // §20 special members: clone types, parameters, defaults and the body under `sub`
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
            }
            else {
                clone->parameter_defaults.push_back(clone_expression(default_value.get(), sub));
            }
        }
        // copy/move 系列：源对象指针类型按替换环境解析（例如 T* → int*）
        // copy/move members: the source pointer type is substituted (e.g. T* -> int*)
        Type source = Type::make_void();
        substitute_type(member->parameter_type, sub, source);
        clone->parameter_type = std::move(source);
        clone->parameter_name = member->parameter_name;
        clone->body = clone_statement(member->body.get(), sub);
        return clone;
    }

    // ============================================================================
    // 遍历
    // ============================================================================

    // ============================================================================
    // Gallt 0.4.txt §19：编译期代码生成与编译期逻辑
    // Gallt 0.4.txt §19: compile-time code generation and compile-time logic
    // ============================================================================

    bool GenericExpander::decode_string_literal(std::string_view lexeme,
        std::string& out) const {
        // 与第 9 章转义表一致（与代码生成阶段的解码规则保持同步）
        // Matches the §9 escape table (kept in sync with the codegen decoder)
        std::string_view content = lexeme;
        if (content.size() >= 2 && content.front() == '"' && content.back() == '"') {
            content = content.substr(1, content.size() - 2);
        }
        out.clear();
        for (std::size_t i = 0; i < content.size(); ++i) {
            char c = content[i];
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (++i >= content.size()) break;
            char e = content[i];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'v': out.push_back('\v'); break;
            case '0': out.push_back('\0'); break;
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            case '\'': out.push_back('\''); break;
            case 'x': {
                int value = 0;
                int digits = 0;
                while (i + 1 < content.size() && digits < 2) {
                    char h = content[i + 1];
                    int d = (h >= '0' && h <= '9') ? (h - '0')
                        : (h >= 'a' && h <= 'f') ? (h - 'a' + 10)
                        : (h >= 'A' && h <= 'F') ? (h - 'A' + 10) : -1;
                    if (d < 0) break;
                    ++i;
                    value = value * 16 + d;
                    ++digits;
                }
                out.push_back(static_cast<char>(value));
                break;
            }
            default:
                if (e >= '0' && e <= '7') {
                    int value = e - '0';
                    int digits = 1;
                    while (i + 1 < content.size() && digits < 3 &&
                        content[i + 1] >= '0' && content[i + 1] <= '7') {
                        ++i;
                        value = value * 8 + (content[i] - '0');
                        ++digits;
                    }
                    out.push_back(static_cast<char>(value));
                }
                else {
                    out.push_back(e);
                }
                break;
            }
        }
        return true;
    }

    std::string GenericExpander::expression_text(const Expression* expr) const {
        // 仅用于诊断占位符：给出表达式的近似源码文本
        // Diagnostics only: an approximate source spelling of the expression
        if (expr == nullptr) return "<empty>";
        if (auto* e = dynamic_cast<const PrimaryExpression*>(expr)) {
            switch (e->kind) {
            case PrimaryExpression::Kind::Literal:
                return std::string(e->literal_token.lexeme);
            case PrimaryExpression::Kind::Identifier:
                return e->identifier;
            default:
                return "<expression>";
            }
        }
        if (auto* e = dynamic_cast<const PostfixExpression*>(expr)) {
            if (e->op == PostfixExpression::Operator::Dot) {
                return expression_text(e->base.get()) + "." + e->member_name;
            }
            if (e->op == PostfixExpression::Operator::FunctionCall) {
                return expression_text(e->base.get()) + "(...)";
            }
            return "<expression>";
        }
        if (auto* e = dynamic_cast<const CompileTimePropertyExpression*>(expr)) {
            return expression_text(e->receiver.get()) + "." + e->property + "<...>";
        }
        if (auto* e = dynamic_cast<const AdditiveExpression*>(expr)) {
            return expression_text(e->left.get()) + " + " + expression_text(e->right.get());
        }
        if (auto* e = dynamic_cast<const ComparisonExpression*>(expr)) {
            const char* op = "==";
            switch (e->op) {
            case ComparisonExpression::Operator::Greater: op = ">"; break;
            case ComparisonExpression::Operator::Less: op = "<"; break;
            case ComparisonExpression::Operator::Equal: op = "=="; break;
            case ComparisonExpression::Operator::NotEqual: op = "!="; break;
            case ComparisonExpression::Operator::GreaterEqual: op = ">="; break;
            case ComparisonExpression::Operator::LessEqual: op = "<="; break;
            }
            return expression_text(e->left.get()) + " " + op + " " +
                expression_text(e->right.get());
        }
        if (auto* e = dynamic_cast<const LogicalAndExpression*>(expr)) {
            return expression_text(e->left.get()) + " && " +
                expression_text(e->right.get());
        }
        if (auto* e = dynamic_cast<const LogicalOrExpression*>(expr)) {
            return expression_text(e->left.get()) + " || " +
                expression_text(e->right.get());
        }
        return "<expression>";
    }

    std::unique_ptr<Expression> GenericExpander::rewrite_property_expression(
        const PostfixExpression* expr, const Substitution& sub) {
        if (expr == nullptr || expr->op != PostfixExpression::Operator::Dot) return nullptr;
        auto* prim = dynamic_cast<const PrimaryExpression*>(expr->base.get());
        if (prim == nullptr || prim->kind != PrimaryExpression::Kind::Identifier) {
            return nullptr;
        }
        const std::string& parameter = prim->identifier;
        bool is_type_param = (sub.types.find(parameter) != sub.types.end());
        bool is_constant_param =
            (sub.constant_types.find(parameter) != sub.constant_types.end());
        if (!is_type_param && !is_constant_param) return nullptr;

        // .size / .align：参数类型的大小与对齐（值和常量参数都适用）
        // .size / .align: layout of the parameter's type (both parameter kinds)
        if (expr->member_name == "size" || expr->member_name == "align") {
            long long value = 0;
            if (!eval_size_align_property(parameter, expr->member_name, sub, value)) {
                report(expr->location, ErrorCode::CompileTimePropertyNotApplicable,
                    std::vector<std::string>{ expr->member_name, parameter });
                value = 0;
            }
            return make_constant_literal(expr->location,
                ConstantValue{ value, 0.0, false, false, std::string() });
        }
        // .typename：仅适用于类型参数（Gallt 0.4.txt §19）
        // .typename: type parameters only (Gallt 0.4.txt §19)
        if (expr->member_name == "typename") {
            if (!is_type_param) {
                report(expr->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ expr->member_name, parameter });
                return make_constant_literal(expr->location,
                    ConstantValue{ 0LL, 0.0, false, false, std::string() });
            }
            ConstantValue value;
            value.is_string = true;
            value.string_value = sub.types.at(parameter).to_string();
            return make_constant_literal(expr->location, value);
        }
        // .is_* ：编译期类型判定（返回 bool）
        // .is_*: compile-time type predicates (return bool)
        if (expr->member_name.rfind("is_", 0) == 0) {
            bool value = false;
            if (eval_bool_property(expr->base.get(), expr->member_name, {}, sub, value)) {
                return make_constant_literal(expr->location,
                    ConstantValue{ value ? 1LL : 0LL, 0.0, false, false, std::string() });
            }
            report(expr->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ expr->member_name, parameter });
            return make_constant_literal(expr->location,
                ConstantValue{ 0LL, 0.0, false, false, std::string() });
        }
        return nullptr;
    }

    bool GenericExpander::param_type_of(const std::string& name,
        const Substitution& sub, AST::Type& out) const {
        auto type_param = sub.types.find(name);
        if (type_param != sub.types.end()) {
            out = type_param->second;
            return true;
        }
        auto constant_param = sub.constant_types.find(name);
        if (constant_param != sub.constant_types.end()) {
            out = constant_param->second;
            return true;
        }
        return false;
    }

    bool GenericExpander::base_type_of(const std::string& name, const Substitution& sub,
        AST::Type& out) const {
        // 泛型参数 / 编译期常量参数 / 内建类型 / 已声明或已实例化的结构体
        // Generic parameters, constant parameters, builtin types and known structs
        if (param_type_of(name, sub, out)) return true;
        if (name == "int") { out = AST::Type::make_int(); return true; }
        if (name == "lint") { out = AST::Type::make_lint(); return true; }
        if (name == "uint") { out = AST::Type::make_uint(); return true; }
        if (name == "luint") { out = AST::Type::make_luint(); return true; }
        if (name == "float") { out = AST::Type::make_float(); return true; }
        if (name == "double") { out = AST::Type::make_double(); return true; }
        if (name == "char") { out = AST::Type::make_char(); return true; }
        if (name == "uchar") { out = AST::Type::make_uchar(); return true; }
        if (name == "bool") { out = AST::Type::make_bool(); return true; }
        if (name == "string") { out = AST::Type::make_string(); return true; }
        if (name == "file") { out = AST::Type::make_file(); return true; }
        if (name == "void") { out = AST::Type::make_void(); return true; }
        if (struct_defs_.count(name) != 0 || instantiated_structs_.count(name) != 0) {
            out = AST::Type::make_struct(name);
            return true;
        }
        return false;
    }

    bool GenericExpander::property_argument_type(const std::string& text,
        const Substitution& sub, AST::Type& out) const {
        // 属性实参文本：`T`、`int`、`T*`、`T**`、`T[]`
        // Property argument spelling: `T`, `int`, `T*`, `T**`, `T[]`
        std::string base = text;
        int pointers = 0;
        bool array = false;
        for (;;) {
            if (base.size() >= 2 && base.compare(base.size() - 2, 2, "**") == 0) {
                pointers += 2;
                base.resize(base.size() - 2);
                continue;
            }
            if (!base.empty() && base.back() == '*') {
                ++pointers;
                base.pop_back();
                continue;
            }
            if (base.size() >= 2 && base.compare(base.size() - 2, 2, "[]") == 0) {
                array = true;
                base.resize(base.size() - 2);
                continue;
            }
            break;
        }
        AST::Type type;
        if (!base_type_of(base, sub, type)) return false;
        if (array) {
            type = AST::Type::make_array(std::make_shared<AST::Type>(type), std::nullopt);
        }
        for (int i = 0; i < pointers; ++i) {
            type = AST::Type::make_pointer(std::make_shared<AST::Type>(type));
        }
        out = type;
        return true;
    }

    bool GenericExpander::property_convertible(const AST::Type& from,
        const AST::Type& to) const {
        // Gallt 0.4.txt §19 is_convertible：数字提升与截断、相同类型、指针与 null
        // Gallt 0.4.txt §19 is_convertible: numeric promotion/truncation, identical
        // types, pointer to void* / null
        if (from == to) return true;
        auto numeric = [](const AST::Type& t) {
            return t.kind == TypeKind::Int || t.kind == TypeKind::Char ||
                t.kind == TypeKind::Bool || t.kind == TypeKind::Float ||
                t.kind == TypeKind::Double;
        };
        if (numeric(from) && numeric(to)) return true;
        if (from.kind == TypeKind::Pointer && to.kind == TypeKind::Pointer) return true;
        if (from.kind == TypeKind::Array && to.kind == TypeKind::Pointer) return true;
        return false;
    }

    bool GenericExpander::eval_size_align_property(const std::string& parameter,
        const std::string& property, const Substitution& sub, long long& out) {
        bool want_align = (property == "align");
        AST::Type type;
        if (!param_type_of(parameter, sub, type)) return false;
        std::size_t size = 0;
        std::size_t align = 0;
        std::string name = type.to_string();
        if (!resolve_type_layout(name, size, align, sub)) {
            if (!resolve_type_layout(type.struct_name, size, align, sub)) return false;
        }
        out = static_cast<long long>(want_align ? align : size);
        return true;
    }

    bool GenericExpander::eval_bool_property(const Expression* receiver,
        const std::string& property, const std::vector<const Expression*>& args,
        const Substitution& sub, bool& out) {
        // 接收者必须是泛型参数标识符（Gallt 0.4.txt §19）
        // The receiver must be a generic-parameter identifier (Gallt 0.4.txt §19)
        auto* prim = dynamic_cast<const PrimaryExpression*>(receiver);
        if (prim == nullptr || prim->kind != PrimaryExpression::Kind::Identifier) {
            return false;
        }
        const std::string& parameter = prim->identifier;
        AST::Type type;
        bool is_type_param = (sub.types.find(parameter) != sub.types.end());
        bool is_constant_param = (sub.constant_types.find(parameter) != sub.constant_types.end());
        if (!is_type_param && !is_constant_param) {
            return false;   // 不是泛型参数：由调用方报告 ER 0112 / ER 0107
        }
        if (!param_type_of(parameter, sub, type)) return false;

        // .is_same / .is_convertible / .has_member 需要类型参数
        // .is_same / .is_convertible / .has_member require a type parameter
        if (property == "is_same" || property == "is_convertible" ||
            property == "has_member") {
            if (!is_type_param) {
                report(receiver->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ property, parameter });
                return false;
            }
            if (args.empty()) {
                report(receiver->location, ErrorCode::CompileTimePropertyArgCountMismatch, std::vector<std::string>{ property, "1", "0" });
                return false;
            }
            if (property == "has_member") {
                const StructDefinition* def = nullptr;
                if (type.kind == TypeKind::Struct) {
                    auto found = struct_defs_.find(type.struct_name);
                    if (found != struct_defs_.end()) def = found->second;
                }
                for (std::size_t i = 0; i < args.size(); ++i) {
                    auto* arg_prim = dynamic_cast<const PrimaryExpression*>(args[i]);
                    if (arg_prim == nullptr ||
                        arg_prim->kind != PrimaryExpression::Kind::Literal ||
                        arg_prim->literal_token.type != TokenType::StringLiteral) {
                        report(receiver->location,
                            ErrorCode::CompileTimePropertyArgNotString, std::vector<std::string>{ property, std::to_string(i + 1) });
                        return false;
                    }
                    std::string member_name;
                    decode_string_literal(arg_prim->literal_token.lexeme, member_name);
                    // 参数不是结构体时返回 false（Gallt 0.4.txt §19）
                    // Non-struct arguments answer false (Gallt 0.4.txt §19)
                    if (def == nullptr) { out = false; return true; }
                    bool has = false;
                    for (const auto& member : def->members) {
                        if (member.name == member_name) { has = true; break; }
                    }
                    if (!has) { out = false; return true; }
                }
                out = true;
                return true;
            }
            // is_same<...> / is_convertible<...>：以接收者为基准逐一比较
            // is_same<...> / is_convertible<...>: compare the receiver against each argument
            for (const Expression* arg : args) {
                std::string arg_text = expression_text(arg);
                AST::Type arg_type;
                if (!property_argument_type(arg_text, sub, arg_type)) {
                    return false;
                }
                if (property == "is_same") {
                    if (!(type == arg_type)) { out = false; return true; }
                }
                else {
                    if (!property_convertible(type, arg_type)) { out = false; return true; }
                }
            }
            out = true;
            return true;
        }

        // 单一类型判定属性
        // Single type-predicate properties
        auto matches = [&](TypeKind kind) { out = (type.kind == kind); return true; };
        // 0.4.1 §2：is_integer 覆盖全部整数型（int / lint / uint / luint）。
        // char/uchar/bool 仍各自有 is_char / is_bool 判定，保持 0.4 的语义。
        // 0.4.1 §2: is_integer covers every integer type (int / lint / uint / luint);
        // char/uchar/bool keep their own is_char / is_bool predicates as in 0.4.
        if (property == "is_integer") {
            out = type.kind == TypeKind::Int || type.kind == TypeKind::Lint ||
                type.kind == TypeKind::Uint || type.kind == TypeKind::Luint;
            return true;
        }
        if (property == "is_float") return matches(TypeKind::Float);
        if (property == "is_double") return matches(TypeKind::Double);
        if (property == "is_char") return matches(TypeKind::Char);
        if (property == "is_bool") return matches(TypeKind::Bool);
        if (property == "is_struct") return matches(TypeKind::Struct);
        if (property == "is_string") return matches(TypeKind::String);
        if (property == "is_pointer") return matches(TypeKind::Pointer);
        if (property == "is_array") return matches(TypeKind::Array);
        return false;
    }

    bool GenericExpander::eval_compile_time_condition(const Expression* expr,
        const Substitution& sub, bool& out) {
        if (expr == nullptr) return false;
        // `!expr`、`a && b`、`a || b`
        if (auto* e = dynamic_cast<const UnaryExpression*>(expr)) {
            if (e->op == UnaryExpression::Operator::LogicalNot) {
                bool inner = false;
                if (!eval_compile_time_condition(e->operand.get(), sub, inner)) return false;
                out = !inner;
                return true;
            }
            return false;
        }
        if (auto* e = dynamic_cast<const LogicalAndExpression*>(expr)) {
            bool left = false;
            bool right = false;
            if (!eval_compile_time_condition(e->left.get(), sub, left)) return false;
            if (!eval_compile_time_condition(e->right.get(), sub, right)) return false;
            out = left && right;
            return true;
        }
        if (auto* e = dynamic_cast<const LogicalOrExpression*>(expr)) {
            bool left = false;
            bool right = false;
            if (!eval_compile_time_condition(e->left.get(), sub, left)) return false;
            if (!eval_compile_time_condition(e->right.get(), sub, right)) return false;
            out = left || right;
            return true;
        }

        // 属性比较：`T.is_integer == true` 等
        // Property comparisons such as `T.is_integer == true`
        auto property_side = [&](const Expression* side, bool& value,
            bool& is_property) -> bool {
            is_property = false;
            if (auto* e = dynamic_cast<const PostfixExpression*>(side)) {
                if (e->op == PostfixExpression::Operator::Dot) {
                    is_property = true;
                    return eval_bool_property(e->base.get(), e->member_name, {}, sub, value);
                }
            }
            if (auto* e = dynamic_cast<const CompileTimePropertyExpression*>(side)) {
                is_property = true;
                std::vector<const Expression*> args;
                for (const auto& arg : e->arguments) args.push_back(arg.get());
                return eval_bool_property(e->receiver.get(), e->property, args, sub, value);
            }
            return false;
        };
        auto literal_side = [&](const Expression* side, bool& value) -> bool {
            auto* prim = dynamic_cast<const PrimaryExpression*>(side);
            if (prim == nullptr || prim->kind != PrimaryExpression::Kind::Literal) return false;
            if (prim->literal_token.type != TokenType::BoolLiteral) return false;
            value = (prim->literal_token.lexeme == "true");
            return true;
        };

        if (auto* e = dynamic_cast<const ComparisonExpression*>(expr)) {
            // Gallt 0.4.txt §19（修正后）：字符串字面量与字符串编译期常量参数可参与
            // 编译期相等/不等比较（'=='、'!='）
            // Gallt 0.4.txt §19 (corrected): string literals and string constant
            // parameters take part in compile-time equality comparison
            auto string_operand = [&](const Expression* side, std::string& value) -> bool {
                auto* prim = dynamic_cast<const PrimaryExpression*>(side);
                if (prim == nullptr) return false;
                if (prim->kind == PrimaryExpression::Kind::Literal &&
                    prim->literal_token.type == TokenType::StringLiteral) {
                    return decode_string_literal(prim->literal_token.lexeme, value);
                }
                if (prim->kind == PrimaryExpression::Kind::Identifier) {
                    auto found = sub.constants.find(prim->identifier);
                    if (found != sub.constants.end() && found->second.is_string) {
                        value = found->second.string_value;
                        return true;
                    }
                }
                return false;
            };
            std::string left_text;
            std::string right_text;
            if (string_operand(e->left.get(), left_text) &&
                string_operand(e->right.get(), right_text)) {
                switch (e->op) {
                case ComparisonExpression::Operator::Equal:
                    out = (left_text == right_text);
                    return true;
                case ComparisonExpression::Operator::NotEqual:
                    out = (left_text != right_text);
                    return true;
                default:
                    // 字符串的次序比较在 0.4 文档中没有定义 → 非编译期布尔常量
                    // Ordering comparisons on strings are undefined in 0.4
                    return false;
                }
            }
            bool left_prop = false;
            bool left_value = false;
            bool left_is_property = false;
            bool right_value = false;
            bool right_is_property = false;
            bool left_ok = property_side(e->left.get(), left_value, left_is_property);
            bool right_ok = property_side(e->right.get(), right_value, right_is_property);
            if (!left_ok) {
                left_ok = literal_side(e->left.get(), left_value);
                left_is_property = false;
            }
            if (!right_ok) {
                right_ok = literal_side(e->right.get(), right_value);
                right_is_property = false;
            }
            if (!left_ok || !right_ok) return false;
            (void)left_prop;
            (void)left_is_property;
            (void)right_is_property;
            switch (e->op) {
            case ComparisonExpression::Operator::Equal:
                out = (left_value == right_value);
                return true;
            case ComparisonExpression::Operator::NotEqual:
                out = (left_value != right_value);
                return true;
            default:
                return false;
            }
        }
        // 裸属性或布尔字面量
        // A bare property or a boolean literal
        bool value = false;
        bool is_property = false;
        if (property_side(expr, value, is_property) && is_property) {
            out = value;
            return true;
        }
        if (literal_side(expr, value)) {
            out = value;
            return true;
        }
        return false;
    }

    bool GenericExpander::eval_emit_piece(const Expression* expr, const Substitution& sub,
        std::string& out) {
        if (expr == nullptr) return false;
        // 字符串拼接
        // String concatenation
        if (auto* e = dynamic_cast<const AdditiveExpression*>(expr)) {
            if (e->op != AdditiveExpression::Operator::Plus) return false;
            std::string left;
            std::string right;
            bool left_ok = eval_emit_piece(e->left.get(), sub, left);
            bool right_ok = eval_emit_piece(e->right.get(), sub, right);
            if (left_ok && right_ok) {
                out += left;
                out += right;
                return true;
            }
            // 数值算术（理论上不用于 Emit String，保守处理）
            // Numeric arithmetic (not the documented use, handled conservatively)
            ConstantValue value;
            if (evaluate_with_substitution(expr, sub, value)) {
                if (value.is_float) {
                    char buffer[64];
                    std::snprintf(buffer, sizeof(buffer), "%g", value.float_value);
                    out += buffer;
                }
                else {
                    out += std::to_string(value.int_value);
                }
                return true;
            }
            return false;
        }
        if (auto* e = dynamic_cast<const PrimaryExpression*>(expr)) {
            if (e->kind == PrimaryExpression::Kind::Literal) {
                switch (e->literal_token.type) {
                case TokenType::StringLiteral: {
                    std::string decoded;
                    decode_string_literal(e->literal_token.lexeme, decoded);
                    out += decoded;
                    return true;
                }
                case TokenType::IntegerLiteral:
                case TokenType::FloatLiteral:
                case TokenType::CharLiteral:
                case TokenType::BoolLiteral:
                    out += std::string(e->literal_token.lexeme);
                    return true;
                default:
                    return false;
                }
            }
            if (e->kind == PrimaryExpression::Kind::Identifier) {
                // 编译期常量参数：字符串常量取字符串值，数值常量取其字面量文本
                // A constant parameter: strings contribute their value, numbers their text
                auto constant = sub.constants.find(e->identifier);
                if (constant != sub.constants.end()) {
                    if (constant->second.is_string) {
                        out += constant->second.string_value;
                    }
                    else if (constant->second.is_float) {
                        char buffer[64];
                        std::snprintf(buffer, sizeof(buffer), "%g",
                            constant->second.float_value);
                        out += buffer;
                    }
                    else {
                        out += std::to_string(constant->second.int_value);
                    }
                    return true;
                }
                return false;
            }
            return false;
        }
        // 编译期属性：typename / size / align
        // Compile-time properties: typename / size / align
        if (auto* e = dynamic_cast<const PostfixExpression*>(expr)) {
            if (e->op != PostfixExpression::Operator::Dot) return false;
            auto* prim = dynamic_cast<const PrimaryExpression*>(e->base.get());
            if (prim == nullptr || prim->kind != PrimaryExpression::Kind::Identifier) {
                return false;
            }
            const std::string& parameter = prim->identifier;
            bool is_type_param = (sub.types.find(parameter) != sub.types.end());
            bool is_constant_param =
                (sub.constant_types.find(parameter) != sub.constant_types.end());
            if (!is_type_param && !is_constant_param) return false;
            if (e->member_name == "typename") {
                if (!is_type_param) {
                    // ER 0109：.typename 只适用于类型参数
                    // ER 0109: .typename only applies to type parameters
                    report(e->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ e->member_name, parameter });
                    return false;
                }
                out += sub.types.at(parameter).to_string();
                return true;
            }
            if (e->member_name == "size" || e->member_name == "align") {
                long long value = 0;
                if (!eval_size_align_property(parameter, e->member_name, sub, value)) {
                    report(e->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ e->member_name, parameter });
                    return false;
                }
                out += std::to_string(value);
                return true;
            }
            return false;
        }
        return false;
    }

    bool GenericExpander::flush_emit_string(const std::string& text, SourceLocation loc,
        std::vector<AST::TopLevel*>& member_ptrs,
        std::vector<std::unique_ptr<AST::TopLevel>>& owner) {
        // (see rewrite_property_expression below for property evaluation inside clones)
        if (text.empty()) return true;
        // 生成的源码必须一直存活：解析结果中的字符串字面量以 string_view 指向它
        // The generated source must stay alive: parsed string literals keep views into it
        emit_source_pool_.push_back(text);
        std::string& buffer = emit_source_pool_.back();
        std::string name = "<emit>";
        std::string_view source_view(buffer);
        std::string_view name_view(name);
        DiagnosticEngine emit_diag;
        Lexer lexer(source_view, name_view, emit_diag);
        Parser parser(lexer, emit_diag);
        auto program = parser.parse();
        if (program == nullptr || emit_diag.has_errors()) {
            // ER 0108：emit 生成的内容无法在编译期展开为合法语法单元
            // ER 0108: the emitted content is not a valid expandable syntax unit
            report(loc, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
            return false;
        }
        for (auto& top : program->top_levels) {
            if (dynamic_cast<GenericDefinition*>(top.get()) != nullptr) {
                report(loc, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
                return false;
            }
            member_ptrs.push_back(top.get());
            owner.push_back(std::move(top));
        }
        return true;
    }

    bool GenericExpander::process_compile_time_items(
        const std::vector<std::unique_ptr<AST::Statement>>& items,
        const Substitution& sub, std::string& text, SourceLocation& text_loc,
        std::vector<AST::TopLevel*>& member_ptrs,
        std::vector<std::unique_ptr<AST::TopLevel>>& owner) {
        for (const auto& item : items) {
            if (auto* emit = dynamic_cast<const EmitStatement*>(item.get())) {
                if (emit->is_block) {
                    if (!flush_emit_string(text, text_loc, member_ptrs, owner)) return false;
                    text.clear();
                    for (const auto& node : emit->block_items) {
                        // Emit Block 只允许插入成员定义（结构体 / 函数）
                        // An Emit Block may only insert member definitions
                        if (auto* fd = dynamic_cast<FunctionDefinition*>(node.get())) {
                            member_ptrs.push_back(fd);
                        }
                        else if (auto* sd = dynamic_cast<StructDefinition*>(node.get())) {
                            member_ptrs.push_back(sd);
                        }
                        else {
                            report(node->location, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
                            return false;
                        }
                    }
                }
                else {
                    if (text.empty()) text_loc = emit->location;
                    for (const auto& piece : emit->pieces) {
                        std::string piece_text;
                        if (!eval_emit_piece(piece.get(), sub, piece_text)) {
                            // ER 0107：emit 字符串表达式不是编译期字符串常量
                            // ER 0107: the emit string expression is not a compile-time
                            // string constant
                            report(piece->location, ErrorCode::EmitStringNotConstant, std::vector<std::string>{ expression_text(piece.get()) });
                            return false;
                        }
                        text += piece_text;
                    }
                }
                continue;
            }
            if (auto* ifs = dynamic_cast<const IfStatement*>(item.get())) {
                bool condition = false;
                if (!eval_compile_time_condition(ifs->condition.get(), sub, condition)) {
                    // ER 0112：编译期条件必须为编译期布尔常量
                    // ER 0112: the compile-time condition must be a compile-time boolean
                    report(ifs->location, ErrorCode::CompileTimeConditionNotBoolean, std::vector<std::string>{ expression_text(ifs->condition.get()) });
                    return false;
                }
                const Statement* branch = condition ? ifs->then_block.get()
                                                    : ifs->else_block.get();
                if (branch == nullptr) continue;
                if (auto* block = dynamic_cast<const Block*>(branch)) {
                    if (!process_compile_time_items(block->statements, sub, text, text_loc,
                        member_ptrs, owner)) {
                        return false;
                    }
                }
                else {
                    report(branch->location, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
                    return false;
                }
                continue;
            }
            report(item->location, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
            return false;
        }
        return true;
    }

    void GenericExpander::expand_expression(std::unique_ptr<Expression>& expr_holder) {
        Expression* expr = expr_holder.get();
        if (expr == nullptr) return;
        if (auto* e = dynamic_cast<AssignmentExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<LogicalOrExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<LogicalAndExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<ComparisonExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<AdditiveExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<MultiplicativeExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<PowerExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<UnaryExpression*>(expr)) {
            expand_expression(e->operand);
        }
        else if (auto* e = dynamic_cast<PostfixExpression*>(expr)) {
            expand_expression(e->base);
            expand_expression(e->subscript_expr);
            for (auto& arg : e->arguments) expand_expression(arg);
            resolve_type(e->cast_type);
        }
        else if (auto* e = dynamic_cast<PrimaryExpression*>(expr)) {
            switch (e->kind) {
            case PrimaryExpression::Kind::Parens:
                expand_expression(e->paren_expr);
                break;
            case PrimaryExpression::Kind::Heap:
                resolve_type(e->heap_type);
                expand_expression(e->heap_size);
                break;
            case PrimaryExpression::Kind::QualifiedName: {
                auto mangled = ensure_instantiation(*e->generic_ref, false);
                std::string name = mangled.has_value() ? *mangled : e->generic_ref->to_string();
                expr_holder = std::make_unique<PrimaryExpression>(e->location, name);
                break;
            }
            case PrimaryExpression::Kind::Construct:
            case PrimaryExpression::Kind::PlacementConstruct: {
                resolve_type(e->construct_type);
                for (auto& arg : e->construct_args) expand_expression(arg);
                expand_expression(e->placement_target);
                break;
            }
            case PrimaryExpression::Kind::CopyMove:
                expand_expression(e->paren_expr);
                break;
            case PrimaryExpression::Kind::Identifier: {
                // 泛型成员短名重写（Gallt 0.3.txt §5/§19）：短名视为在当前作用域声明
                // Short-name rewriting: instantiation short names live in the current scope
                std::string name = e->identifier;
                bool resolved_short_name = false;
                for (std::size_t depth = short_scopes_.size(); depth > 0; --depth) {
                    const auto& shorts = short_scopes_[depth - 1];
                    auto found = shorts.find(name);
                    if (found != shorts.end()) {
                        expr_holder = std::make_unique<PrimaryExpression>(e->location,
                            found->second.target);
                        resolved_short_name = true;
                        break;
                    }
                    const auto& declared = declared_scopes_[depth - 1];
                    if (declared.count(name) != 0) {
                        break;   // 内层同名普通声明遮蔽外层短名
                    }
                }
                // 第 19 章：整体实例化后引用特化块缺失（而主泛型存在）的成员 → ER 0075
                // §19: referencing a member the specialization omits is ER 0075
                if (!resolved_short_name) {
                    for (std::size_t depth = instance_scopes_.size(); depth > 0; --depth) {
                        for (const std::string& instance : instance_scopes_[depth - 1]) {
                            auto missing = missing_members_.find(instance);
                            if (missing != missing_members_.end() &&
                                missing->second.count(name) != 0) {
                                report(e->location, ErrorCode::GenericSpecializationMissingMember, std::vector<std::string>{ instance, name });
                                return;
                            }
                        }
                    }
                }
                break;
            }
            default:
                break;
            }
        }
    }

    void GenericExpander::expand_initializer(Initializer* init) {
        if (init == nullptr) return;
        if (auto* e = dynamic_cast<ExpressionInitializer*>(init)) {
            expand_expression(e->expr);
        }
        else if (auto* a = dynamic_cast<ArrayInitializer*>(init)) {
            for (auto& element : a->elements) expand_initializer(element.get());
        }
    }

    void GenericExpander::expand_statement(Statement* stmt) {
        if (stmt == nullptr) return;
        if (auto* block = dynamic_cast<Block*>(stmt)) {
            push_scope();
            for (auto& child : block->statements) {
                if (auto* vd = dynamic_cast<VariableDeclaration*>(child.get())) declare_name(vd->name);
                else if (auto* sd = dynamic_cast<StructDefinition*>(child.get())) declare_name(sd->name);
                else if (auto* def = dynamic_cast<FunctionDefinition*>(child.get())) declare_name(def->name);
            }
            for (auto& child : block->statements) expand_statement(child.get());
            // 实例化语句不是可执行语句：展开后从语句序列中移除
            // Instantiation statements are not executable statements; drop them afterwards
            std::vector<std::unique_ptr<Statement>> kept;
            kept.reserve(block->statements.size());
            for (auto& child : block->statements) {
                if (dynamic_cast<InstantiationStatement*>(child.get()) != nullptr) continue;
                kept.push_back(std::move(child));
            }
            block->statements = std::move(kept);
            pop_scope();
            return;
        }
        if (auto* vd = dynamic_cast<VariableDeclaration*>(stmt)) {
            resolve_type(vd->type);
            expand_initializer(vd->initializer.get());
            return;
        }
        if (auto* sd = dynamic_cast<StructDefinition*>(stmt)) {
            struct_defs_[sd->name] = sd;
            for (auto& member : sd->members) {
                resolve_type(member.type);
                if (member.array_size_expr) {
                    expand_expression(member.array_size_expr);
                }
                if (member.function_pointer_type.has_value()) {
                    resolve_type(*member.function_pointer_type);
                }
                expand_initializer(member.initializer.get());
            }
            for (auto& smf : sd->special_members) {
                for (Type& p : smf->parameters) resolve_type(p);
                resolve_type(smf->parameter_type);
                expand_statement(smf->body.get());
            }
            return;
        }
        if (auto* is = dynamic_cast<IfStatement*>(stmt)) {
            expand_expression(is->condition);
            expand_statement(is->then_block.get());
            expand_statement(is->else_block.get());
            return;
        }
        if (auto* fs = dynamic_cast<ForStatement*>(stmt)) {
            push_scope();
            if (fs->init) {
                if (auto* vd = dynamic_cast<VariableDeclaration*>(fs->init.get())) declare_name(vd->name);
                expand_statement(fs->init.get());
            }
            expand_expression(fs->condition);
            expand_expression(fs->step);
            expand_statement(fs->body.get());
            pop_scope();
            return;
        }
        if (auto* ws = dynamic_cast<WhileStatement*>(stmt)) {
            expand_expression(ws->condition);
            expand_statement(ws->body.get());
            return;
        }
        if (auto* rs = dynamic_cast<ReturnStatement*>(stmt)) {
            expand_expression(rs->value);
            return;
        }
        if (auto* es = dynamic_cast<ExpressionStatement*>(stmt)) {
            expand_expression(es->expr);
            return;
        }
        if (auto* ds = dynamic_cast<DestructStatement*>(stmt)) {
            expand_expression(ds->target);
            return;
        }
        if (auto* inst = dynamic_cast<InstantiationStatement*>(stmt)) {
            ensure_instantiation(inst->reference, inst->reference.member.empty());
            return;
        }
    }

    void GenericExpander::expand_top_level(TopLevel* node) {
        if (dynamic_cast<GenericDefinition*>(node) != nullptr) {
            return;   // 展开后会被移除
        }
        if (auto* inst = dynamic_cast<InstantiationStatement*>(node)) {
            ensure_instantiation(inst->reference, inst->reference.member.empty());
            return;
        }
        if (auto* sd = dynamic_cast<StructDefinition*>(node)) {
            struct_defs_[sd->name] = sd;
            for (auto& member : sd->members) {
                resolve_type(member.type);
                if (member.array_size_expr) expand_expression(member.array_size_expr);
                if (member.function_pointer_type.has_value()) {
                    resolve_type(*member.function_pointer_type);
                }
                expand_initializer(member.initializer.get());
            }
            for (auto& smf : sd->special_members) {
                for (Type& p : smf->parameters) resolve_type(p);
                resolve_type(smf->parameter_type);
                for (auto& d : smf->parameter_defaults) {
                    expand_expression(d);
                }
                push_scope();
                if (!smf->parameter_name.empty()) declare_name(smf->parameter_name);
                for (const std::string& pname : smf->parameter_names) declare_name(pname);
                expand_statement(smf->body.get());
                pop_scope();
            }
            return;
        }
        if (auto* fd = dynamic_cast<FunctionDefinition*>(node)) {
            func_defs_[fd->name] = fd;
            resolve_type(fd->return_type);
            for (Type& p : fd->parameters) resolve_type(p);
            for (auto& d : fd->param_defaults) {
                expand_expression(d);
            }
            push_scope();
            for (const std::string& pname : fd->param_names) {
                if (!pname.empty()) declare_name(pname);
            }
            expand_statement(fd->body.get());
            pop_scope();
            return;
        }
        if (auto* vd = dynamic_cast<VariableDeclaration*>(node)) {
            resolve_type(vd->type);
            if (vd->array_size_expr) expand_expression(vd->array_size_expr);
            expand_initializer(vd->initializer.get());
            return;
        }
        if (auto* ed = dynamic_cast<ExternDeclaration*>(node)) {
            resolve_type(ed->return_type);
            for (Type& p : ed->parameters) resolve_type(p);
            return;
        }
    }

    bool GenericExpander::expand(AST::Program* program) {
        if (program == nullptr) return false;
        program_ = program;
        had_error_ = false;
        push_scope();               // 全局作用域
        collect_declarations();
        collect_generics();
        // 顺序展开：泛型定义与实例化语句本身不作为具体声明保留，
        // 实例化产生的成员插入到触发它们的声明之前
        // Sequential expansion: generic definitions and instantiation statements are not
        // concrete declarations; instantiated members precede the triggering declaration
        std::vector<std::unique_ptr<TopLevel>> output;
        output.reserve(program_->top_levels.size());
        output_ = &output;
        for (auto& top : program_->top_levels) {
            if (dynamic_cast<GenericDefinition*>(top.get()) != nullptr) {
                continue;
            }
            if (auto* inst = dynamic_cast<InstantiationStatement*>(top.get())) {
                ensure_instantiation(inst->reference, inst->reference.member.empty());
                continue;
            }
            expand_top_level(top.get());
            output.push_back(std::move(top));
        }
        output_ = nullptr;
        pop_scope();
        program_->top_levels = std::move(output);
        return !had_error_;
    }

} // namespace gallt
