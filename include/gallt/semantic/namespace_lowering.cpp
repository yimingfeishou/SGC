#include "namespace_lowering.hpp"
#include <algorithm>
#include <cstddef>
#include <utility>

using namespace gallt::AST;

namespace gallt {

    namespace {
        std::string declaration_name(TopLevel* node) {
            if (auto* sd = dynamic_cast<StructDefinition*>(node)) { return sd->name; }
            if (auto* fd = dynamic_cast<FunctionDefinition*>(node)) { return fd->name; }
            if (auto* vd = dynamic_cast<VariableDeclaration*>(node)) { return vd->name; }
            if (auto* gd = dynamic_cast<GenericDefinition*>(node)) { return gd->name; }
            if (auto* ed = dynamic_cast<ExternDeclaration*>(node)) { return ed->name; }
            if (auto* nd = dynamic_cast<NamespaceDefinition*>(node)) { return nd->name; }
            return std::string();
        }

        std::string declaration_type_text(TopLevel* node) {
            if (auto* sd = dynamic_cast<StructDefinition*>(node)) { return sd->name; }
            if (auto* vd = dynamic_cast<VariableDeclaration*>(node)) { return vd->type.to_string(); }
            if (auto* fd = dynamic_cast<FunctionDefinition*>(node)) {
                return fd->return_type.to_string() + "(...)";
            }
            if (dynamic_cast<GenericDefinition*>(node) != nullptr) { return "generic"; }
            if (dynamic_cast<NamespaceDefinition*>(node) != nullptr) { return "namespace"; }
            if (dynamic_cast<ExternDeclaration*>(node) != nullptr) { return "extern function"; }
            return "expression";
        }

        const char* declaration_kind(TopLevel* node) {
            if (dynamic_cast<GenericDefinition*>(node) != nullptr) { return "generic"; }
            if (dynamic_cast<StructDefinition*>(node) != nullptr) { return "struct"; }
            if (dynamic_cast<FunctionDefinition*>(node) != nullptr) { return "function"; }
            if (dynamic_cast<VariableDeclaration*>(node) != nullptr) { return "variable"; }
            if (dynamic_cast<ExternDeclaration*>(node) != nullptr) { return "extern"; }
            return "unknown";
        }

        std::string join_parts(const std::vector<std::string>& parts) {
            std::string out;

            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i != 0) { out += "::"; }
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

        bool is_plain_qualified_identifier(const std::string& name) {
            if (name.find("::") == std::string::npos) { return false; }

            for (char c : name) {
                bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '$';

                if (!ok) { return false; }
            }

            return true;
        }

        bool is_plain_identifier(const std::string& name) {
            if (name.empty()) { return false; }
            if (name[0] >= '0' && name[0] <= '9') { return false; }

            for (char c : name) {
                bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '$';

                if (!ok) { return false; }
            }

            return true;
        }
    }

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

    void NamespaceLowering::declare_namespace_member(const std::string& prefix,
        const std::string& name, const std::string& kind, bool addition,
        SourceLocation loc) {
        NamespaceInfo& info = namespaces_[prefix];
        auto found = info.members.find(name);

        if (found != info.members.end()) {
            auto kind_it = info.member_kinds.find(name);
            const bool same_generic_name = kind == "generic" &&
                kind_it != info.member_kinds.end() && kind_it->second == "generic";

            if (addition && !same_generic_name) {
                report(loc, ErrorCode::AdditionNamespaceMemberConflict, std::vector<std::string>{ name });
            }
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
                    report(ad->location, ErrorCode::AdditionNamespaceTargetMissing, std::vector<std::string>{ ad->name });
                    continue;
                }

                collect_members(ad->members, full, true, ad->location);
                continue;
            }

            if (dynamic_cast<AccessNamespaceStatement*>(node.get()) != nullptr) {
                continue;
            }

            if (prefix.empty()) { continue; }
            std::string name = declaration_name(node.get());
            if (name.empty()) { continue; }
            declare_namespace_member(prefix, name, declaration_kind(node.get()), addition,
                node->location);
        }
    }

    void NamespaceLowering::push_scope() { scopes_.emplace_back(); }

    void NamespaceLowering::pop_scope() {
        if (!scopes_.empty()) { scopes_.pop_back(); }
    }

    void NamespaceLowering::declare_name(const std::string& name,
        const std::string& type_text) {
        if (scopes_.empty()) { push_scope(); }
        scopes_.back().declared[name] = type_text;
    }

    bool NamespaceLowering::declared_in_current_scope(const std::string& name) const {
        if (scopes_.empty()) { return false; }
        return scopes_.back().declared.count(name) != 0 ||
            scopes_.back().aliases.count(name) != 0;
    }

    std::optional<std::string> NamespaceLowering::declared_type_of(
        const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->declared.find(name);

            if (found != it->declared.end()) { return found->second; }
        }

        return std::nullopt;
    }

    std::optional<std::string> NamespaceLowering::lookup_alias(
        const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (it->declared.count(name) != 0) { return std::nullopt; }
            auto found = it->aliases.find(name);

            if (found != it->aliases.end()) { return found->second; }
        }

        return std::nullopt;
    }

    std::optional<std::string> NamespaceLowering::rewrite_identifier(
        const std::string& name) const {
        return lookup_alias(name);
    }

    void NamespaceLowering::enter_namespace_scope(const std::string& full_name) {
        auto found = namespaces_.find(full_name);
        if (found == namespaces_.end()) { return; }
        if (scopes_.empty()) { push_scope(); }
        Scope& scope = scopes_.back();

        for (const auto& kv : found->second.members) {
            scope.aliases[kv.first] = kv.second;
        }

        for (const auto& child : found->second.child_namespaces) {
            scope.aliases[child] = join_path(full_name, child);
        }
    }

    std::optional<std::string> NamespaceLowering::resolve_qualified(
        const std::vector<std::string>& path, SourceLocation loc, bool type_context) {
        (void)type_context;
        if (path.size() < 2) { return std::nullopt; }

        std::vector<std::vector<std::string>> candidates;
        if (!namespace_prefix_.empty()) {
            std::vector<std::string> with_prefix = split_qualified(namespace_prefix_);
            with_prefix.insert(with_prefix.end(), path.begin(), path.end());
            candidates.push_back(std::move(with_prefix));
        }

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

        candidates.push_back(path);

        for (const std::vector<std::string>& candidate : candidates) {
            if (candidate.size() < 2) { continue; }

            for (std::size_t cut = candidate.size() - 1; cut >= 1; --cut) {
                std::vector<std::string> ns_parts(candidate.begin(),
                    candidate.begin() + static_cast<std::ptrdiff_t>(cut));
                std::string ns_name;

                for (std::size_t i = 0; i < ns_parts.size(); ++i) {
                    if (i != 0) { ns_name += "::"; }
                    ns_name += ns_parts[i];
                }

                auto ns = namespaces_.find(ns_name);
                if (ns == namespaces_.end()) { continue; }

                if (candidate.size() - cut == 1) {
                    const std::string& member = candidate[cut];
                    auto found = ns->second.members.find(member);

                    if (found != ns->second.members.end()) {
                        return found->second;
                    }

                    report(loc, ErrorCode::NamespaceMemberNotFound, std::vector<std::string>{ member, ns_name });
                    return std::nullopt;
                }

                report(loc, ErrorCode::ScopeOperatorOperandInvalid, std::vector<std::string>{ candidate[cut] });
                return std::nullopt;
            }
        }

        if (auto type_text = declared_type_of(path[0])) {
            report(loc, ErrorCode::ScopeOperatorOperandInvalid, std::vector<std::string>{ *type_text });
            return std::nullopt;
        }

        report(loc, ErrorCode::NamespaceUndefined, std::vector<std::string>{ path[0] });
        return std::nullopt;
    }

    std::optional<std::string> NamespaceLowering::resolve_namespace_path(
        const std::vector<std::string>& parts, SourceLocation loc) {
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

            if (namespaces_.count(joined) != 0) { return joined; }
        }

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
            std::string source_generic = join_parts(ref.namespace_path) + "::" +
                ref.generic_name;
            auto ns = resolve_namespace_path(ref.namespace_path, ref.location);
            if (!ns.has_value()) { return; }
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
                report(ref.location, ErrorCode::GenericUndefined,
                    std::vector<std::string>{ source_generic });
                return;
            }

            ref.display_name = source_generic;
            ref.generic_name = member->second;
            ref.namespace_path.clear();
        } else if (auto target = lookup_alias(ref.generic_name)) {
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
        } else if (type.kind == TypeKind::Struct && !type.struct_name.empty()) {
            if (is_plain_qualified_identifier(type.struct_name)) {
                std::vector<std::string> path = split_qualified(type.struct_name);
                if (auto resolved = resolve_qualified(path, type_location_hint_, false)) {
                    type.struct_name = *resolved;
                }
            } else if (is_plain_identifier(type.struct_name)) {
                if (auto target = lookup_alias(type.struct_name)) {
                    if (namespaces_.count(*target) == 0) { type.struct_name = *target; }
                }
            }
        }
        if (type.element_type) { rewrite_type(*type.element_type); }
        if (type.pointee_type) { rewrite_type(*type.pointee_type); }
        if (type.return_type) { rewrite_type(*type.return_type); }

        for (Type& param : type.parameter_types) { rewrite_type(param); }
    }

    std::optional<std::string> NamespaceLowering::expression_replacement(
        Expression* expr, SourceLocation& loc) {
        auto* primary = dynamic_cast<PrimaryExpression*>(expr);
        if (primary == nullptr) { return std::nullopt; }
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
        if (holder == nullptr) { return; }
        SourceLocation loc;
        if (auto replacement = expression_replacement(holder.get(), loc)) {
            holder = std::make_unique<PrimaryExpression>(loc, *replacement);
            return;
        }

        rewrite_expression_nested(holder.get());
    }

    void NamespaceLowering::rewrite_shared_expression(std::shared_ptr<Expression>& holder) {
        if (holder == nullptr) { return; }
        SourceLocation loc;
        if (auto replacement = expression_replacement(holder.get(), loc)) {
            holder = std::make_shared<PrimaryExpression>(loc, *replacement);
            return;
        }

        rewrite_expression_nested(holder.get());
    }

    void NamespaceLowering::rewrite_expression_nested(Expression* expr) {
        if (expr == nullptr) { return; }
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
                for (auto& arg : e->construct_args) { rewrite_expression(arg); }
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
            for (auto& arg : e->arguments) { rewrite_expression(arg); }
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

        if (auto* e = dynamic_cast<BitwiseExpression*>(expr)) {
            rewrite_expression(e->left);
            rewrite_expression(e->right);
            return;
        }

        if (auto* e = dynamic_cast<ShiftExpression*>(expr)) {
            rewrite_expression(e->left);
            rewrite_expression(e->right);
            return;
        }

        if (auto* e = dynamic_cast<ConditionalExpression*>(expr)) {
            rewrite_expression(e->condition);
            rewrite_expression(e->then_expr);
            rewrite_expression(e->else_expr);
            return;
        }

        if (auto* e = dynamic_cast<UnaryExpression*>(expr)) {
            rewrite_expression(e->operand);
            return;
        }

        if (auto* e = dynamic_cast<PostfixExpression*>(expr)) {
            rewrite_expression(e->base);
            rewrite_expression(e->subscript_expr);
            for (auto& arg : e->arguments) { rewrite_expression(arg); }
            rewrite_type(e->cast_type);
            return;
        }
    }

    void NamespaceLowering::rewrite_initializer(Initializer* init) {
        if (init == nullptr) { return; }
        if (auto* e = dynamic_cast<ExpressionInitializer*>(init)) {
            rewrite_expression(e->expr);
        } else if (auto* a = dynamic_cast<ArrayInitializer*>(init)) {
            for (auto& element : a->elements) { rewrite_initializer(element.get()); }
        }
    }

    void NamespaceLowering::rewrite_statement(Statement* stmt) {
        if (stmt == nullptr) { return; }
        type_location_hint_ = stmt->location;

        if (auto* block = dynamic_cast<Block*>(stmt)) {
            push_scope();

            for (auto& child : block->statements) {
                if (auto* vd = dynamic_cast<VariableDeclaration*>(child.get())) {
                    declare_name(vd->name, vd->type.to_string());
                } else if (auto* sd = dynamic_cast<StructDefinition*>(child.get())) {
                    declare_name(sd->name, sd->name);
                } else if (auto* fd = dynamic_cast<FunctionDefinition*>(child.get())) {
                    declare_name(fd->name, fd->return_type.to_string() + "(...)");
                }
            }

            for (auto& child : block->statements) { rewrite_statement(child.get()); }
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
            if (vd->array_size_expr) { rewrite_expression(vd->array_size_expr); }
            if (vd->function_pointer_type) { rewrite_type(*vd->function_pointer_type); }
            rewrite_initializer(vd->initializer.get());
            return;
        }

        if (auto* sd = dynamic_cast<StructDefinition*>(stmt)) {
            for (auto& member : sd->members) {
                rewrite_type(member.type);
                if (member.array_size_expr) { rewrite_expression(member.array_size_expr); }
                if (member.function_pointer_type) { rewrite_type(*member.function_pointer_type); }
                rewrite_initializer(member.initializer.get());
            }

            for (auto& smf : sd->special_members) {
                for (Type& p : smf->parameters) { rewrite_type(p); }
                rewrite_type(smf->parameter_type);
                for (auto& d : smf->parameter_defaults) { rewrite_expression(d); }
                push_scope();

                if (!smf->parameter_name.empty()) {
                    declare_name(smf->parameter_name, "object*");
                }

                for (const std::string& pname : smf->parameter_names) {
                    if (!pname.empty()) { declare_name(pname, "object"); }
                }

                rewrite_statement(smf->body.get());
                pop_scope();
            }
            return;
        }

        if (auto* fd = dynamic_cast<FunctionDefinition*>(stmt)) {
            rewrite_type(fd->return_type);
            for (Type& p : fd->parameters) { rewrite_type(p); }
            for (auto& d : fd->param_defaults) { rewrite_expression(d); }
            push_scope();

            for (std::size_t i = 0; i < fd->param_names.size(); ++i) {
                if (fd->param_names[i].empty()) { continue; }
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
            for (auto& piece : emit->pieces) { rewrite_expression(piece); }

            for (auto& item : emit->block_items) {
                if (auto* tl = dynamic_cast<TopLevel*>(item.get())) {
                    rewrite_top_level(tl);
                } else if (auto* inner = dynamic_cast<Statement*>(item.get())) {
                    rewrite_statement(inner);
                }
            }
            return;
        }
    }

    void NamespaceLowering::rewrite_top_level(TopLevel* node) {
        if (node == nullptr) { return; }
        type_location_hint_ = node->location;

        if (auto* sd = dynamic_cast<StructDefinition*>(node)) {
            rewrite_statement(sd);
            return;
        }

        if (auto* fd = dynamic_cast<FunctionDefinition*>(node)) {
            rewrite_type(fd->return_type);
            for (Type& p : fd->parameters) { rewrite_type(p); }
            for (auto& d : fd->param_defaults) { rewrite_expression(d); }
            push_scope();

            for (std::size_t i = 0; i < fd->param_names.size(); ++i) {
                if (fd->param_names[i].empty()) { continue; }
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
            if (vd->array_size_expr) { rewrite_expression(vd->array_size_expr); }
            if (vd->function_pointer_type) { rewrite_type(*vd->function_pointer_type); }
            rewrite_initializer(vd->initializer.get());
            return;
        }

        if (auto* gd = dynamic_cast<GenericDefinition*>(node)) {
            push_scope();

            for (const GenericParameter& param : gd->parameters) {
                declare_name(param.name, "generic parameter");
            }

            for (auto& member : gd->members) {
                if (member == nullptr) { continue; }
                if (std::string(declaration_kind(member.get())) == "unknown") { continue; }
                std::string member_name = declaration_name(member.get());
                if (member_name.empty()) { continue; }
                declare_name(member_name, declaration_type_text(member.get()));
            }

            for (auto& member : gd->members) { rewrite_top_level(member.get()); }
            for (auto& item : gd->compile_time_items) { rewrite_statement(item.get()); }
            pop_scope();
            return;
        }

        if (auto* ed = dynamic_cast<ExternDeclaration*>(node)) {
            rewrite_type(ed->return_type);
            for (Type& p : ed->parameters) { rewrite_type(p); }
            return;
        }

        if (auto* inst = dynamic_cast<InstantiationStatement*>(node)) {
            rewrite_generic_ref(inst->reference);
            return;
        }
    }

    void NamespaceLowering::handle_access_namespace(AccessNamespaceStatement* node) {
        const std::vector<std::string>& path = node->path;
        if (path.empty()) { return; }
        if (scopes_.empty()) { push_scope(); }

        auto try_namespace = [&](const std::string& name) -> const NamespaceInfo* {
            auto found = namespaces_.find(name);
            return found == namespaces_.end() ? nullptr : &found->second;
        };

        if (path.size() == 1) {
            std::string with_prefix = join_path(namespace_prefix_, path[0]);
            const NamespaceInfo* info = try_namespace(with_prefix);
            if (info == nullptr) { info = try_namespace(path[0]); }
            if (info == nullptr) {
                report(node->location, ErrorCode::AccessNamespaceTargetInvalid, std::vector<std::string>{ path[0] });
                return;
            }

            for (const auto& kv : info->members) {
                if (declared_in_current_scope(kv.first)) {
                    report(node->location, ErrorCode::AccessNamespaceNameConflict, std::vector<std::string>{ kv.first });
                    continue;
                }

                scopes_.back().aliases[kv.first] = kv.second;
            }
            return;
        }

        std::vector<std::string> ns_parts(path.begin(), path.end() - 1);
        std::string ns_name;

        for (std::size_t i = 0; i < ns_parts.size(); ++i) {
            if (i != 0) { ns_name += "::"; }
            ns_name += ns_parts[i];
        }

        std::string target_text;

        for (const std::string& candidate : { join_path(namespace_prefix_, ns_name), ns_name }) {
            const NamespaceInfo* info = try_namespace(candidate);
            if (info == nullptr) { continue; }
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

        std::string full;

        for (std::size_t i = 0; i < path.size(); ++i) {
            if (i != 0) { full += "::"; }
            full += path[i];
        }

        report(node->location, ErrorCode::AccessNamespaceTargetInvalid, std::vector<std::string>{ full });
    }

    void NamespaceLowering::rename_namespace_member(TopLevel* node) {
        if (node == nullptr || namespace_prefix_.empty()) { return; }
        std::string name = declaration_name(node);
        if (name.empty()) { return; }
        auto info = namespaces_.find(namespace_prefix_);
        if (info == namespaces_.end()) { return; }
        auto found = info->second.members.find(name);
        if (found == info->second.members.end()) { return; }
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
        if (namespaces_.count(full) == 0) { return; }
        std::string saved = namespace_prefix_;
        namespace_prefix_ = full;
        push_scope();
        enter_namespace_scope(full);
        lower_top_levels(def->members, out, true);
        pop_scope();
        namespace_prefix_ = saved;
    }

    bool NamespaceLowering::run(AST::Program* program) {
        if (program == nullptr) { return false; }
        program_ = program;
        had_error_ = false;
        namespaces_.clear();
        scopes_.clear();
        namespace_prefix_.clear();

        collect_members(program->top_levels, "", false, SourceLocation{});

        push_scope();

        for (auto& node : program->top_levels) {
            std::string name = declaration_name(node.get());
            if (name.empty()) { continue; }
            declare_name(name, declaration_type_text(node.get()));
        }

        std::vector<std::unique_ptr<TopLevel>> out;
        out.reserve(program->top_levels.size());
        lower_top_levels(program->top_levels, out, false);
        pop_scope();
        program->top_levels = std::move(out);
        return !had_error_;
    }

}
