// semantic/namespace_lowering.cpp
// 命名空间降低实现 —— Gallt 0.4.txt §21
// Namespace lowering implementation — Gallt 0.4.txt §21

#include "namespace_lowering.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

using namespace gallt::AST;

namespace gallt {

    namespace {
        // 取顶层/成员声明的短名；无名字的声明返回空串
        // Short name of a top-level/member declaration; empty when it has none
        std::string declaration_name(TopLevel* node) {
            if (auto* sd = dynamic_cast<StructDefinition*>(node)) return sd->name;
            if (auto* fd = dynamic_cast<FunctionDefinition*>(node)) return fd->name;
            if (auto* vd = dynamic_cast<VariableDeclaration*>(node)) return vd->name;
            if (auto* gd = dynamic_cast<GenericDefinition*>(node)) return gd->name;
            if (auto* ed = dynamic_cast<ExternDeclaration*>(node)) return ed->name;
            if (auto* nd = dynamic_cast<NamespaceDefinition*>(node)) return nd->name;
            return std::string();
        }

        // 声明在 §5 查找中的类型文本（用于 ER 0105 的 '[type]' 占位符）
        // Declaration type text for the '[type]' placeholder of ER 0105
        std::string declaration_type_text(TopLevel* node) {
            if (auto* sd = dynamic_cast<StructDefinition*>(node)) return sd->name;
            if (auto* vd = dynamic_cast<VariableDeclaration*>(node)) return vd->type.to_string();
            if (auto* fd = dynamic_cast<FunctionDefinition*>(node)) {
                return fd->return_type.to_string() + "(...)";
            }
            if (dynamic_cast<GenericDefinition*>(node) != nullptr) return "generic";
            if (dynamic_cast<NamespaceDefinition*>(node) != nullptr) return "namespace";
            if (dynamic_cast<ExternDeclaration*>(node) != nullptr) return "extern function";
            return "expression";
        }

        // 声明种类（用于命名空间限定泛型实例化的查找）
        // Declaration kind (used by namespace-qualified generic instantiation lookup)
        const char* declaration_kind(TopLevel* node) {
            if (dynamic_cast<GenericDefinition*>(node) != nullptr) return "generic";
            if (dynamic_cast<StructDefinition*>(node) != nullptr) return "struct";
            if (dynamic_cast<FunctionDefinition*>(node) != nullptr) return "function";
            if (dynamic_cast<VariableDeclaration*>(node) != nullptr) return "variable";
            if (dynamic_cast<ExternDeclaration*>(node) != nullptr) return "extern";
            return "unknown";
        }

        std::string join_parts(const std::vector<std::string>& parts) {
            std::string out;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i != 0) out += "::";
                out += parts[i];
            }
            return out;
        }

        std::vector<std::string> split_qualified(const std::string& name) {
            std::vector<std::string> parts;
            std::size_t start = 0;
            while (start <= name.size()) {
                std::size_t pos = name.find("::", start);
                if (pos == std::string::npos) {
                    parts.push_back(name.substr(start));
                    break;
                }
                parts.push_back(name.substr(start, pos - start));
                start = pos + 2;
            }
            return parts;
        }

        // 形如 `A::B::C` 的纯标识符限定名（排除 Box<int>::X 这类泛型限定名）
        // A pure identifier-qualified name such as `A::B::C` (excludes generic
        // qualified names such as Box<int>::X)
        bool is_plain_qualified_identifier(const std::string& name) {
            if (name.find("::") == std::string::npos) return false;
            for (char c : name) {
                bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '$';
                if (!ok) return false;
            }
            return true;
        }

        bool is_plain_identifier(const std::string& name) {
            if (name.empty()) return false;
            if (name[0] >= '0' && name[0] <= '9') return false;
            for (char c : name) {
                bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '$';
                if (!ok) return false;
            }
            return true;
        }
    } // anonymous namespace

    NamespaceLowering::NamespaceLowering(DiagnosticEngine& diag) : diag_(diag) {}

    void NamespaceLowering::report(SourceLocation loc, ErrorCode code,
        const std::vector<std::string>& values) {
        diag_.report_error_template(loc, code, values);
        had_error_ = true;
    }

    void NamespaceLowering::report(SourceLocation loc, ErrorCode code,
        const std::string& message) {
        diag_.report_error(loc, code, message);
        had_error_ = true;
    }

    std::string NamespaceLowering::internal_name(const std::string& qualified) {
        std::string out = qualified;
        std::size_t pos = 0;
        while ((pos = out.find("::", pos)) != std::string::npos) {
            out.replace(pos, 2, "$");
            ++pos;
        }
        return out;
    }

    std::string NamespaceLowering::join_path(const std::string& prefix,
        const std::string& name) {
        return prefix.empty() ? name : prefix + "::" + name;
    }

    // ============================================================================
    // 阶段一：收集命名空间
    // Phase 1: collect namespaces
    // ============================================================================

    void NamespaceLowering::declare_namespace_member(const std::string& prefix,
        const std::string& name, const std::string& kind, bool addition,
        SourceLocation loc) {
        NamespaceInfo& info = namespaces_[prefix];
        auto found = info.members.find(name);
        if (found != info.members.end()) {
            if (addition) {
                // ER 0114：addition namespace 合并后成员与已有声明冲突
                // ER 0114: the merged member conflicts with an existing declaration
                report(loc, ErrorCode::AdditionNamespaceMemberConflict, std::vector<std::string>{ name });
            }
            // 非 addition：同一定义内的重复成员交由既有重复声明检查报告
            // (a duplicate inside one definition is left to the ordinary checks)
            return;
        }
        info.members[name] = internal_name(join_path(prefix, name));
        info.member_kinds[name] = kind;
    }

    void NamespaceLowering::collect_members(
        const std::vector<std::unique_ptr<TopLevel>>& nodes, const std::string& prefix,
        bool addition, SourceLocation loc) {
        (void)loc;
        for (const auto& node : nodes) {
            if (auto* nd = dynamic_cast<NamespaceDefinition*>(node.get())) {
                std::string full = join_path(prefix, nd->name);
                if (namespaces_.count(full) != 0) {
                    // ER 0101：命名空间重复定义
                    // ER 0101: duplicate namespace definition
                    report(nd->location, ErrorCode::NamespaceRedefined, std::vector<std::string>{ full });
                    continue;
                }
                namespaces_[full].location = nd->location;
                if (!prefix.empty()) {
                    namespaces_[prefix].child_namespaces.insert(nd->name);
                }
                collect_members(nd->members, full, false, nd->location);
                continue;
            }
            if (auto* ad = dynamic_cast<AdditionNamespaceStatement*>(node.get())) {
                std::string full = join_path(prefix, ad->name);
                if (namespaces_.count(full) == 0) {
                    // ER 0104：addition namespace 未找到可追加的命名空间
                    // ER 0104: no namespace to append to
                    report(ad->location, ErrorCode::AdditionNamespaceTargetMissing, std::vector<std::string>{ ad->name });
                    continue;
                }
                collect_members(ad->members, full, true, ad->location);
                continue;
            }
            if (dynamic_cast<AccessNamespaceStatement*>(node.get()) != nullptr) {
                // access namespace 在阶段二按作用域处理
                // access namespace is handled per scope in phase 2
                continue;
            }
            if (prefix.empty()) continue;   // 顶层普通声明的名字保持不变
            std::string name = declaration_name(node.get());
            if (name.empty()) continue;
            declare_namespace_member(prefix, name, declaration_kind(node.get()), addition,
                node->location);
        }
    }

    // ============================================================================
    // 作用域
    // Scopes
    // ============================================================================

    void NamespaceLowering::push_scope() { scopes_.emplace_back(); }

    void NamespaceLowering::pop_scope() {
        if (!scopes_.empty()) scopes_.pop_back();
    }

    void NamespaceLowering::declare_name(const std::string& name,
        const std::string& type_text) {
        if (scopes_.empty()) push_scope();
        scopes_.back().declared[name] = type_text;
    }

    bool NamespaceLowering::declared_in_current_scope(const std::string& name) const {
        if (scopes_.empty()) return false;
        return scopes_.back().declared.count(name) != 0 ||
            scopes_.back().aliases.count(name) != 0;
    }

    std::optional<std::string> NamespaceLowering::declared_type_of(
        const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->declared.find(name);
            if (found != it->declared.end()) return found->second;
        }
        return std::nullopt;
    }

    std::optional<std::string> NamespaceLowering::lookup_alias(
        const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (it->declared.count(name) != 0) return std::nullopt;
            auto found = it->aliases.find(name);
            if (found != it->aliases.end()) return found->second;
        }
        return std::nullopt;
    }

    std::optional<std::string> NamespaceLowering::rewrite_identifier(
        const std::string& name) const {
        return lookup_alias(name);
    }

    void NamespaceLowering::enter_namespace_scope(const std::string& full_name) {
        auto found = namespaces_.find(full_name);
        if (found == namespaces_.end()) return;
        if (scopes_.empty()) push_scope();
        Scope& scope = scopes_.back();
        // 命名空间成员与直接子命名空间在本体内以短名可见（§21）
        // Members and direct child namespaces are visible by short name inside the body
        for (const auto& kv : found->second.members) {
            scope.aliases[kv.first] = kv.second;
        }
        for (const auto& child : found->second.child_namespaces) {
            scope.aliases[child] = join_path(full_name, child);
        }
    }

    // ============================================================================
    // 限定名解析
    // Qualified-name resolution
    // ============================================================================

    std::optional<std::string> NamespaceLowering::resolve_qualified(
        const std::vector<std::string>& path, SourceLocation loc, bool type_context) {
        (void)type_context;
        if (path.size() < 2) return std::nullopt;
        // 注意：命名空间限定的**泛型**实例化 `ns::Box<int>` 不走本函数，而是在
        // rewrite_generic_ref 中处理（语法层已把 `Ident (:: Ident)* <` 解析为 GenericRef）。
        // Note: namespace-qualified *generic* instantiation is handled by
        // rewrite_generic_ref; the parser already folds `Ident (:: Ident)* <` into a
        // GenericRef, so this function only sees ordinary qualified names.

        std::vector<std::vector<std::string>> candidates;
        // (a) 当前命名空间前缀 + 路径
        // (a) current namespace prefix + path
        if (!namespace_prefix_.empty()) {
            std::vector<std::string> with_prefix = split_qualified(namespace_prefix_);
            with_prefix.insert(with_prefix.end(), path.begin(), path.end());
            candidates.push_back(std::move(with_prefix));
        }
        // (b) 第一分量经别名展开（命名空间别名）
        // (b) expand the first component through an alias (namespace alias)
        {
            std::vector<std::string> expanded = path;
            if (auto target = lookup_alias(path[0])) {
                if (namespaces_.count(*target) != 0) {
                    expanded = split_qualified(*target);
                    expanded.insert(expanded.end(), path.begin() + 1, path.end());
                }
            }
            candidates.push_back(std::move(expanded));
        }
        // (c) 原路径（绝对名）
        // (c) the path itself (absolute name)
        candidates.push_back(path);

        for (const std::vector<std::string>& candidate : candidates) {
            if (candidate.size() < 2) continue;
            for (std::size_t cut = candidate.size() - 1; cut >= 1; --cut) {
                std::vector<std::string> ns_parts(candidate.begin(),
                    candidate.begin() + static_cast<std::ptrdiff_t>(cut));
                std::string ns_name;
                for (std::size_t i = 0; i < ns_parts.size(); ++i) {
                    if (i != 0) ns_name += "::";
                    ns_name += ns_parts[i];
                }
                auto ns = namespaces_.find(ns_name);
                if (ns == namespaces_.end()) continue;
                if (candidate.size() - cut == 1) {
                    const std::string& member = candidate[cut];
                    auto found = ns->second.members.find(member);
                    if (found != ns->second.members.end()) {
                        return found->second;
                    }
                    // ER 0102：命名空间成员不存在
                    // ER 0102: the namespace member does not exist
                    report(loc, ErrorCode::NamespaceMemberNotFound, std::vector<std::string>{ member, ns_name });
                    return std::nullopt;
                }
                // 剩余多个分量：左操作数是命名空间成员而非命名空间/泛型实例
                // Several components remain: the left operand is a plain member
                report(loc, ErrorCode::ScopeOperatorOperandInvalid, std::vector<std::string>{ candidate[cut] });
                return std::nullopt;
            }
        }

        // 没有任何命名空间前缀：左操作数要么是普通声明（ER 0105），要么根本不存在（ER 0100）
        // No namespace prefix at all: the left operand is either an ordinary
        // declaration (ER 0105) or unknown (ER 0100)
        if (auto type_text = declared_type_of(path[0])) {
            report(loc, ErrorCode::ScopeOperatorOperandInvalid, std::vector<std::string>{ *type_text });
            return std::nullopt;
        }
        report(loc, ErrorCode::NamespaceUndefined, std::vector<std::string>{ path[0] });
        return std::nullopt;
    }

    // ============================================================================
    // 重写
    // Rewriting
    // ============================================================================

    std::optional<std::string> NamespaceLowering::resolve_namespace_path(
        const std::vector<std::string>& parts, SourceLocation loc) {
        // 候选：当前命名空间前缀 + 路径 / 别名展开后的路径 / 绝对路径
        // Candidates: current namespace prefix + path, alias-expanded path, absolute path
        std::vector<std::vector<std::string>> candidates;
        if (!namespace_prefix_.empty()) {
            std::vector<std::string> with_prefix = split_qualified(namespace_prefix_);
            with_prefix.insert(with_prefix.end(), parts.begin(), parts.end());
            candidates.push_back(std::move(with_prefix));
        }
        {
            std::vector<std::string> expanded = parts;
            if (!parts.empty()) {
                if (auto target = lookup_alias(parts[0])) {
                    if (namespaces_.count(*target) != 0) {
                        expanded = split_qualified(*target);
                        expanded.insert(expanded.end(), parts.begin() + 1, parts.end());
                    }
                }
            }
            candidates.push_back(std::move(expanded));
        }
        candidates.push_back(parts);

        for (const std::vector<std::string>& candidate : candidates) {
            std::string joined = join_parts(candidate);
            if (namespaces_.count(joined) != 0) return joined;
        }

        // 路径不存在：左操作数若是普通声明 → ER 0105，否则 → ER 0100
        // Unknown path: an ordinary declaration is ER 0105, otherwise ER 0100
        std::string source_path = join_parts(parts);
        if (!parts.empty()) {
            if (auto type_text = declared_type_of(parts[0])) {
                report(loc, ErrorCode::ScopeOperatorOperandInvalid,
                    std::vector<std::string>{ *type_text });
                return std::nullopt;
            }
        }
        report(loc, ErrorCode::NamespaceUndefined,
            std::vector<std::string>{ source_path });
        return std::nullopt;
    }

    void NamespaceLowering::rewrite_generic_ref(GenericRef& ref) {
        if (!ref.namespace_path.empty()) {
            // Gallt 0.4.txt §19/§21：命名空间限定泛型实例化
            //   1) 解析命名空间路径：不存在报 ER 0100，左操作数不是命名空间报 ER 0105
            //   2) 末段必须是该命名空间的成员，否则 ER 0102
            //   3) 该成员必须是泛型，否则 ER 0061
            // Gallt 0.4.txt §19/§21: namespace-qualified generic instantiation
            //   1) resolve the namespace path (ER 0100 / ER 0105)
            //   2) the final component must be a member of that namespace (ER 0102)
            //   3) that member must be a generic (ER 0061)
            std::string source_generic = join_parts(ref.namespace_path) + "::" +
                ref.generic_name;
            auto ns = resolve_namespace_path(ref.namespace_path, ref.location);
            if (!ns.has_value()) return;
            auto info = namespaces_.find(*ns);
            if (info == namespaces_.end()) {
                report(ref.location, ErrorCode::NamespaceUndefined,
                    std::vector<std::string>{ *ns });
                return;
            }
            auto member = info->second.members.find(ref.generic_name);
            if (member == info->second.members.end()) {
                report(ref.location, ErrorCode::NamespaceMemberNotFound,
                    std::vector<std::string>{ ref.generic_name, *ns });
                return;
            }
            auto kind = info->second.member_kinds.find(ref.generic_name);
            if (kind == info->second.member_kinds.end() || kind->second != "generic") {
                // ER 0061：命名空间成员存在但不是泛型
                // ER 0061: the member exists but is not a generic
                report(ref.location, ErrorCode::GenericUndefined,
                    std::vector<std::string>{ source_generic });
                return;
            }
            // 成功：内部名（已含命名空间路径）+ 源级限定名（诊断显示）
            // Success: internal name (encodes the namespace path) + source-level name
            ref.display_name = source_generic;
            ref.generic_name = member->second;
            ref.namespace_path.clear();
        }
        else if (auto target = lookup_alias(ref.generic_name)) {
            // access namespace 引入的泛型名（仅当别名不是命名空间时改写）
            // A generic imported through `access namespace` (rewrite unless it is a
            // namespace name)
            if (namespaces_.count(*target) == 0) {
                ref.generic_name = *target;
            }
        }
        for (GenericArgument& arg : ref.arguments) {
            rewrite_type(arg.type);
            rewrite_shared_expression(arg.expression);
        }
    }

    void NamespaceLowering::rewrite_type(AST::Type& type) {
        if (type.generic_ref) {
            rewrite_generic_ref(*type.generic_ref);
        }
        else if (type.kind == TypeKind::Struct && !type.struct_name.empty()) {
            if (is_plain_qualified_identifier(type.struct_name)) {
                std::vector<std::string> path = split_qualified(type.struct_name);
                if (auto resolved = resolve_qualified(path, type_location_hint_, false)) {
                    type.struct_name = *resolved;
                }
            }
            else if (is_plain_identifier(type.struct_name)) {
                if (auto target = lookup_alias(type.struct_name)) {
                    if (namespaces_.count(*target) == 0) type.struct_name = *target;
                }
            }
        }
        if (type.element_type) rewrite_type(*type.element_type);
        if (type.pointee_type) rewrite_type(*type.pointee_type);
        if (type.return_type) rewrite_type(*type.return_type);
        for (Type& param : type.parameter_types) rewrite_type(param);
    }

    std::optional<std::string> NamespaceLowering::expression_replacement(
        Expression* expr, SourceLocation& loc) {
        auto* primary = dynamic_cast<PrimaryExpression*>(expr);
        if (primary == nullptr) return std::nullopt;
        loc = primary->location;
        if (primary->kind == PrimaryExpression::Kind::NamespaceQualified) {
            return resolve_qualified(primary->qualified_path, primary->location, false);
        }
        if (primary->kind == PrimaryExpression::Kind::Identifier) {
            return rewrite_identifier(primary->identifier);
        }
        return std::nullopt;
    }

    void NamespaceLowering::rewrite_expression(std::unique_ptr<Expression>& holder) {
        if (holder == nullptr) return;
        SourceLocation loc;
        if (auto replacement = expression_replacement(holder.get(), loc)) {
            holder = std::make_unique<PrimaryExpression>(loc, *replacement);
            return;
        }
        rewrite_expression_nested(holder.get());
    }

    void NamespaceLowering::rewrite_shared_expression(std::shared_ptr<Expression>& holder) {
        if (holder == nullptr) return;
        SourceLocation loc;
        if (auto replacement = expression_replacement(holder.get(), loc)) {
            holder = std::make_shared<PrimaryExpression>(loc, *replacement);
            return;
        }
        rewrite_expression_nested(holder.get());
    }

    void NamespaceLowering::rewrite_expression_nested(Expression* expr) {
        if (expr == nullptr) return;
        type_location_hint_ = expr->location;
        if (auto* e = dynamic_cast<PrimaryExpression*>(expr)) {
            switch (e->kind) {
            case PrimaryExpression::Kind::QualifiedName: {
                if (e->generic_ref) {
                    rewrite_generic_ref(*e->generic_ref);
                }
                return;
            }
            case PrimaryExpression::Kind::Parens:
                rewrite_expression(e->paren_expr);
                return;
            case PrimaryExpression::Kind::Heap:
                rewrite_type(e->heap_type);
                rewrite_expression(e->heap_size);
                return;
            case PrimaryExpression::Kind::Construct:
            case PrimaryExpression::Kind::PlacementConstruct:
                rewrite_type(e->construct_type);
                for (auto& arg : e->construct_args) rewrite_expression(arg);
                rewrite_expression(e->placement_target);
                return;
            case PrimaryExpression::Kind::CopyMove:
                rewrite_expression(e->paren_expr);
                return;
            default:
                return;
            }
        }
        if (auto* e = dynamic_cast<CompileTimePropertyExpression*>(expr)) {
            rewrite_expression(e->receiver);
            for (auto& arg : e->arguments) rewrite_expression(arg);
            return;
        }
        if (auto* e = dynamic_cast<AssignmentExpression*>(expr)) {
            rewrite_expression(e->left);
            rewrite_expression(e->right);
            return;
        }
        if (auto* e = dynamic_cast<LogicalOrExpression*>(expr)) {
            rewrite_expression(e->left);
            rewrite_expression(e->right);
            return;
        }
        if (auto* e = dynamic_cast<LogicalAndExpression*>(expr)) {
            rewrite_expression(e->left);
            rewrite_expression(e->right);
            return;
        }
        if (auto* e = dynamic_cast<ComparisonExpression*>(expr)) {
            rewrite_expression(e->left);
            rewrite_expression(e->right);
            return;
        }
        if (auto* e = dynamic_cast<AdditiveExpression*>(expr)) {
            rewrite_expression(e->left);
            rewrite_expression(e->right);
            return;
        }
        if (auto* e = dynamic_cast<MultiplicativeExpression*>(expr)) {
            rewrite_expression(e->left);
            rewrite_expression(e->right);
            return;
        }
        if (auto* e = dynamic_cast<PowerExpression*>(expr)) {
            rewrite_expression(e->left);
            rewrite_expression(e->right);
            return;
        }
        if (auto* e = dynamic_cast<UnaryExpression*>(expr)) {
            rewrite_expression(e->operand);
            return;
        }
        if (auto* e = dynamic_cast<PostfixExpression*>(expr)) {
            rewrite_expression(e->base);
            rewrite_expression(e->subscript_expr);
            for (auto& arg : e->arguments) rewrite_expression(arg);
            rewrite_type(e->cast_type);
            return;
        }
    }

    void NamespaceLowering::rewrite_initializer(Initializer* init) {
        if (init == nullptr) return;
        if (auto* e = dynamic_cast<ExpressionInitializer*>(init)) {
            rewrite_expression(e->expr);
        }
        else if (auto* a = dynamic_cast<ArrayInitializer*>(init)) {
            for (auto& element : a->elements) rewrite_initializer(element.get());
        }
    }

    void NamespaceLowering::rewrite_statement(Statement* stmt) {
        if (stmt == nullptr) return;
        type_location_hint_ = stmt->location;
        if (auto* block = dynamic_cast<Block*>(stmt)) {
            push_scope();
            for (auto& child : block->statements) {
                if (auto* vd = dynamic_cast<VariableDeclaration*>(child.get())) {
                    declare_name(vd->name, vd->type.to_string());
                }
                else if (auto* sd = dynamic_cast<StructDefinition*>(child.get())) {
                    declare_name(sd->name, sd->name);
                }
                else if (auto* fd = dynamic_cast<FunctionDefinition*>(child.get())) {
                    declare_name(fd->name, fd->return_type.to_string() + "(...)");
                }
            }
            for (auto& child : block->statements) rewrite_statement(child.get());
            // access namespace 语句只影响名字可见性，降低后不再是可执行语句
            // An access-namespace statement only affects name visibility, so it is
            // dropped once lowered
            std::vector<std::unique_ptr<Statement>> kept;
            kept.reserve(block->statements.size());
            for (auto& child : block->statements) {
                if (dynamic_cast<AccessNamespaceStatement*>(child.get()) != nullptr) {
                    continue;
                }
                kept.push_back(std::move(child));
            }
            block->statements = std::move(kept);
            pop_scope();
            return;
        }
        if (auto* vd = dynamic_cast<VariableDeclaration*>(stmt)) {
            rewrite_type(vd->type);
            if (vd->array_size_expr) rewrite_expression(vd->array_size_expr);
            if (vd->function_pointer_type) rewrite_type(*vd->function_pointer_type);
            rewrite_initializer(vd->initializer.get());
            return;
        }
        if (auto* sd = dynamic_cast<StructDefinition*>(stmt)) {
            for (auto& member : sd->members) {
                rewrite_type(member.type);
                if (member.array_size_expr) rewrite_expression(member.array_size_expr);
                if (member.function_pointer_type) rewrite_type(*member.function_pointer_type);
                rewrite_initializer(member.initializer.get());
            }
            for (auto& smf : sd->special_members) {
                for (Type& p : smf->parameters) rewrite_type(p);
                rewrite_type(smf->parameter_type);
                for (auto& d : smf->parameter_defaults) rewrite_expression(d);
                push_scope();
                if (!smf->parameter_name.empty()) {
                    declare_name(smf->parameter_name, "object*");
                }
                for (const std::string& pname : smf->parameter_names) {
                    if (!pname.empty()) declare_name(pname, "object");
                }
                rewrite_statement(smf->body.get());
                pop_scope();
            }
            return;
        }
        if (auto* fd = dynamic_cast<FunctionDefinition*>(stmt)) {
            rewrite_type(fd->return_type);
            for (Type& p : fd->parameters) rewrite_type(p);
            for (auto& d : fd->param_defaults) rewrite_expression(d);
            push_scope();
            for (std::size_t i = 0; i < fd->param_names.size(); ++i) {
                if (fd->param_names[i].empty()) continue;
                std::string type_text = i < fd->parameters.size()
                    ? fd->parameters[i].to_string() : std::string("object");
                declare_name(fd->param_names[i], type_text);
            }
            rewrite_statement(fd->body.get());
            pop_scope();
            return;
        }
        if (auto* is = dynamic_cast<IfStatement*>(stmt)) {
            rewrite_expression(is->condition);
            rewrite_statement(is->then_block.get());
            rewrite_statement(is->else_block.get());
            return;
        }
        if (auto* fs = dynamic_cast<ForStatement*>(stmt)) {
            push_scope();
            if (fs->init) {
                if (auto* vd = dynamic_cast<VariableDeclaration*>(fs->init.get())) {
                    declare_name(vd->name, vd->type.to_string());
                }
                rewrite_statement(fs->init.get());
            }
            rewrite_expression(fs->condition);
            rewrite_expression(fs->step);
            rewrite_statement(fs->body.get());
            pop_scope();
            return;
        }
        if (auto* ws = dynamic_cast<WhileStatement*>(stmt)) {
            rewrite_expression(ws->condition);
            rewrite_statement(ws->body.get());
            return;
        }
        if (auto* rs = dynamic_cast<ReturnStatement*>(stmt)) {
            rewrite_expression(rs->value);
            return;
        }
        if (auto* es = dynamic_cast<ExpressionStatement*>(stmt)) {
            rewrite_expression(es->expr);
            return;
        }
        if (auto* ds = dynamic_cast<DestructStatement*>(stmt)) {
            rewrite_expression(ds->target);
            return;
        }
        if (auto* ac = dynamic_cast<AccessNamespaceStatement*>(stmt)) {
            handle_access_namespace(ac);
            return;
        }
        if (auto* inst = dynamic_cast<InstantiationStatement*>(stmt)) {
            rewrite_generic_ref(inst->reference);
            return;
        }
        if (auto* emit = dynamic_cast<EmitStatement*>(stmt)) {
            for (auto& piece : emit->pieces) rewrite_expression(piece);
            for (auto& item : emit->block_items) {
                if (auto* tl = dynamic_cast<TopLevel*>(item.get())) rewrite_top_level(tl);
                else if (auto* inner = dynamic_cast<Statement*>(item.get())) {
                    rewrite_statement(inner);
                }
            }
            return;
        }
    }

    void NamespaceLowering::rewrite_top_level(TopLevel* node) {
        if (node == nullptr) return;
        type_location_hint_ = node->location;
        if (auto* sd = dynamic_cast<StructDefinition*>(node)) {
            // 结构体定义同时是语句：复用语句级重写（成员、特殊成员函数）
            rewrite_statement(sd);
            return;
        }
        if (auto* fd = dynamic_cast<FunctionDefinition*>(node)) {
            // 函数定义只在顶层出现（函数体内不允许嵌套函数定义）
            rewrite_type(fd->return_type);
            for (Type& p : fd->parameters) rewrite_type(p);
            for (auto& d : fd->param_defaults) rewrite_expression(d);
            push_scope();
            for (std::size_t i = 0; i < fd->param_names.size(); ++i) {
                if (fd->param_names[i].empty()) continue;
                std::string type_text = i < fd->parameters.size()
                    ? fd->parameters[i].to_string() : std::string("object");
                declare_name(fd->param_names[i], type_text);
            }
            rewrite_statement(fd->body.get());
            pop_scope();
            return;
        }
        if (auto* vd = dynamic_cast<VariableDeclaration*>(node)) {
            rewrite_type(vd->type);
            if (vd->array_size_expr) rewrite_expression(vd->array_size_expr);
            if (vd->function_pointer_type) rewrite_type(*vd->function_pointer_type);
            rewrite_initializer(vd->initializer.get());
            return;
        }
        if (auto* gd = dynamic_cast<GenericDefinition*>(node)) {
            push_scope();
            // 泛型参数与编译期常量参数构成最内层作用域（§19）
            // Generic and constant parameters form the innermost scope (§19)
            for (const GenericParameter& param : gd->parameters) {
                declare_name(param.name, "generic parameter");
            }
            for (auto& member : gd->members) rewrite_top_level(member.get());
            for (auto& item : gd->compile_time_items) rewrite_statement(item.get());
            pop_scope();
            return;
        }
        if (auto* ed = dynamic_cast<ExternDeclaration*>(node)) {
            rewrite_type(ed->return_type);
            for (Type& p : ed->parameters) rewrite_type(p);
            return;
        }
        if (auto* inst = dynamic_cast<InstantiationStatement*>(node)) {
            rewrite_generic_ref(inst->reference);
            return;
        }
    }

    // ============================================================================
    // access namespace
    // ============================================================================

    void NamespaceLowering::handle_access_namespace(AccessNamespaceStatement* node) {
        const std::vector<std::string>& path = node->path;
        if (path.empty()) return;
        if (scopes_.empty()) push_scope();

        auto try_namespace = [&](const std::string& name) -> const NamespaceInfo* {
            auto found = namespaces_.find(name);
            return found == namespaces_.end() ? nullptr : &found->second;
        };

        if (path.size() == 1) {
            // access namespace ns：引入该命名空间的所有直接对象（不递归嵌套命名空间）
            // access namespace ns: import every direct object (nested namespaces excluded)
            std::string with_prefix = join_path(namespace_prefix_, path[0]);
            const NamespaceInfo* info = try_namespace(with_prefix);
            if (info == nullptr) info = try_namespace(path[0]);
            if (info == nullptr) {
                // ER 0103：access namespace 目标不存在或不可引入
                // ER 0103: the access-namespace target does not exist / is not importable
                report(node->location, ErrorCode::AccessNamespaceTargetInvalid, std::vector<std::string>{ path[0] });
                return;
            }
            for (const auto& kv : info->members) {
                if (declared_in_current_scope(kv.first)) {
                    // ER 0113：引入的名字与当前作用域已有声明冲突
                    // ER 0113: the imported name conflicts with an existing declaration
                    report(node->location, ErrorCode::AccessNamespaceNameConflict, std::vector<std::string>{ kv.first });
                    continue;
                }
                scopes_.back().aliases[kv.first] = kv.second;
            }
            return;
        }

        // access namespace ns::member：只引入单个对象
        // access namespace ns::member: import a single object
        std::vector<std::string> ns_parts(path.begin(), path.end() - 1);
        std::string ns_name;
        for (std::size_t i = 0; i < ns_parts.size(); ++i) {
            if (i != 0) ns_name += "::";
            ns_name += ns_parts[i];
        }
        std::string target_text;
        for (const std::string& candidate : { join_path(namespace_prefix_, ns_name), ns_name }) {
            const NamespaceInfo* info = try_namespace(candidate);
            if (info == nullptr) continue;
            auto member = info->members.find(path.back());
            if (member == info->members.end()) {
                target_text.clear();
                break;
            }
            if (declared_in_current_scope(path.back())) {
                report(node->location, ErrorCode::AccessNamespaceNameConflict, std::vector<std::string>{ path.back() });
                return;
            }
            scopes_.back().aliases[path.back()] = member->second;
            return;
        }
        // ER 0103
        std::string full;
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (i != 0) full += "::";
            full += path[i];
        }
        report(node->location, ErrorCode::AccessNamespaceTargetInvalid, std::vector<std::string>{ full });
    }

    // ============================================================================
    // 阶段二入口
    // Phase 2 entry
    // ============================================================================

    void NamespaceLowering::rename_namespace_member(TopLevel* node) {
        // 命名空间成员改用内部名字：`ns` 中的 `i` 变成内部名 `ns$i`
        // A namespace member switches to its internal name: `i` in `ns` becomes `ns$i`
        if (node == nullptr || namespace_prefix_.empty()) return;
        std::string name = declaration_name(node);
        if (name.empty()) return;
        auto info = namespaces_.find(namespace_prefix_);
        if (info == namespaces_.end()) return;
        auto found = info->second.members.find(name);
        if (found == info->second.members.end()) return;
        const std::string internal = found->second;
        if (auto* sd = dynamic_cast<StructDefinition*>(node)) { sd->name = internal; return; }
        if (auto* fd = dynamic_cast<FunctionDefinition*>(node)) { fd->name = internal; return; }
        if (auto* vd = dynamic_cast<VariableDeclaration*>(node)) { vd->name = internal; return; }
        if (auto* gd = dynamic_cast<GenericDefinition*>(node)) { gd->name = internal; return; }
        if (auto* ed = dynamic_cast<ExternDeclaration*>(node)) { ed->name = internal; return; }
    }

    void NamespaceLowering::lower_top_levels(
        std::vector<std::unique_ptr<TopLevel>>& nodes,
        std::vector<std::unique_ptr<TopLevel>>& out, bool in_namespace_body) {
        (void)in_namespace_body;
        for (auto& node : nodes) {
            if (auto* nd = dynamic_cast<NamespaceDefinition*>(node.get())) {
                lower_namespace_body(nd, out);
                continue;
            }
            if (auto* ad = dynamic_cast<AdditionNamespaceStatement*>(node.get())) {
                lower_addition_body(ad, out);
                continue;
            }
            if (auto* ac = dynamic_cast<AccessNamespaceStatement*>(node.get())) {
                handle_access_namespace(ac);
                continue;
            }
            // 命名空间成员改用内部（命名修饰）名字（Gallt 0.4.txt §21）
            // Namespace members switch to their internal (mangled) name
            if (in_namespace_body && !namespace_prefix_.empty()) {
                rename_namespace_member(node.get());
            }
            rewrite_top_level(node.get());
            out.push_back(std::move(node));
        }
    }

    void NamespaceLowering::lower_namespace_body(NamespaceDefinition* def,
        std::vector<std::unique_ptr<TopLevel>>& out) {
        std::string full = join_path(namespace_prefix_, def->name);
        std::string saved = namespace_prefix_;
        namespace_prefix_ = full;
        push_scope();
        enter_namespace_scope(full);
        lower_top_levels(def->members, out, true);
        pop_scope();
        namespace_prefix_ = saved;
    }

    void NamespaceLowering::lower_addition_body(AdditionNamespaceStatement* def,
        std::vector<std::unique_ptr<TopLevel>>& out) {
        std::string full = join_path(namespace_prefix_, def->name);
        if (namespaces_.count(full) == 0) return;   // 阶段一已报 ER 0104
        std::string saved = namespace_prefix_;
        namespace_prefix_ = full;
        push_scope();
        enter_namespace_scope(full);
        lower_top_levels(def->members, out, true);
        pop_scope();
        namespace_prefix_ = saved;
    }

    bool NamespaceLowering::run(AST::Program* program) {
        if (program == nullptr) return false;
        program_ = program;
        had_error_ = false;
        namespaces_.clear();
        scopes_.clear();
        namespace_prefix_.clear();

        // ---- 阶段一：收集命名空间并分配内部名 ----
        // ---- Phase 1: collect namespaces and assign internal names ----
        collect_members(program->top_levels, "", false, SourceLocation{});

        // ---- 全局作用域：预登记顶层声明（遮蔽与 ER 0113 冲突检查用） ----
        // ---- Global scope: pre-register top-level declarations ----
        push_scope();
        for (auto& node : program->top_levels) {
            std::string name = declaration_name(node.get());
            if (name.empty()) continue;
            declare_name(name, declaration_type_text(node.get()));
        }

        // ---- 阶段二：作用域遍历 + 降低 ----
        // ---- Phase 2: scoped walk and lowering ----
        std::vector<std::unique_ptr<TopLevel>> out;
        out.reserve(program->top_levels.size());
        lower_top_levels(program->top_levels, out, false);
        pop_scope();
        program->top_levels = std::move(out);
        return !had_error_;
    }

} // namespace gallt
