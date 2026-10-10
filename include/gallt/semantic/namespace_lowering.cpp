#include "namespace_lowering.hpp"
#include "../parser/ast_visitor.hpp"
#include "diagnosed_registry.hpp"
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

    NamespaceLowering::PoisonGuard::PoisonGuard(NamespaceLowering& pass,
        const AST::Node* node)
        : pass_(pass), node_(node), errors_(pass.diag_.error_count()) {}

    NamespaceLowering::PoisonGuard::~PoisonGuard() {
        if (node_ != nullptr && pass_.diag_.error_count() != errors_) {
            pass_.poison_node(node_);
        }
    }

    void NamespaceLowering::report(SourceLocation loc, ErrorCode code,
        const std::vector<std::string>& values) {
        if (!diagnosed_registry::reported_by_earlier_stage(diag_, loc,
            SemanticStage::Namespaces)) {
            diag_.report_error_template(loc, code, values);
        }

        diagnosed_registry::record(diag_, loc, SemanticStage::Namespaces);
        had_error_ = true;
    }

    void NamespaceLowering::report(SourceLocation loc, ErrorCode code,
        const std::string& message) {
        if (!diagnosed_registry::reported_by_earlier_stage(diag_, loc,
            SemanticStage::Namespaces)) {
            diag_.report_error(loc, code, message);
        }

        diagnosed_registry::record(diag_, loc, SemanticStage::Namespaces);
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
            const bool same_kind = kind_it != info.member_kinds.end() &&
                kind_it->second == kind;
            const bool overloadable = same_kind &&
                (kind == "function" || kind == "generic");

            if (addition && !overloadable) {
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

        ErrorCode pending_code = ErrorCode::ExpressionSyntaxError;
        std::vector<std::string> pending_values;
        bool has_pending = false;

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

                    pending_code = ErrorCode::NamespaceMemberNotFound;
                    pending_values = std::vector<std::string>{ member, ns_name };
                    has_pending = true;
                    break;
                }

                pending_code = ErrorCode::ScopeOperatorOperandInvalid;
                pending_values = std::vector<std::string>{ candidate[cut] };
                has_pending = true;
                break;
            }
        }

        if (has_pending) {
            report(loc, pending_code, pending_values);
            return std::nullopt;
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
            const std::size_t errors_before = diag_.error_count();
            auto resolved = resolve_qualified(primary->qualified_path,
                primary->location, false);

            if (!resolved.has_value()) {
                if (diag_.error_count() == errors_before) {
                    report(primary->location, ErrorCode::NamespaceUndefined,
                        join_parts(primary->qualified_path));
                }

                poison_node(primary);
            }
            return resolved;
        }

        if (primary->kind == PrimaryExpression::Kind::Identifier) {
            return rewrite_identifier(primary->identifier);
        }

        return std::nullopt;
    }

    void NamespaceLowering::rewrite_expression(std::unique_ptr<Expression>& holder) {
        if (holder == nullptr) { return; }
        if (poisoned_node(holder.get())) { return; }
        const std::size_t errors_before = diag_.error_count();
        Expression* original = holder.get();
        rewrite_expression_impl(holder);

        if (holder.get() == original && diag_.error_count() != errors_before) {
            poison_node(original);
        }
    }

    void NamespaceLowering::rewrite_expression_impl(std::unique_ptr<Expression>& holder) {
        SourceLocation loc;
        if (auto replacement = expression_replacement(holder.get(), loc)) {
            holder = std::make_unique<PrimaryExpression>(loc, *replacement);
            return;
        }

        if (poisoned_node(holder.get())) { return; }
        rewrite_expression_nested(holder.get());
    }

    void NamespaceLowering::rewrite_shared_expression(std::shared_ptr<Expression>& holder) {
        if (holder == nullptr) { return; }
        if (poisoned_node(holder.get())) { return; }
        const std::size_t errors_before = diag_.error_count();
        Expression* original = holder.get();
        rewrite_shared_expression_impl(holder);

        if (holder.get() == original && diag_.error_count() != errors_before) {
            poison_node(original);
        }
    }

    void NamespaceLowering::rewrite_shared_expression_impl(std::shared_ptr<Expression>& holder) {
        SourceLocation loc;
        if (auto replacement = expression_replacement(holder.get(), loc)) {
            holder = std::make_shared<PrimaryExpression>(loc, *replacement);
            return;
        }

        if (poisoned_node(holder.get())) { return; }
        rewrite_expression_nested(holder.get());
    }

    class NamespaceLowering::ExpressionRewriter : public AST::AstRewriter {
    public:
        explicit ExpressionRewriter(NamespaceLowering& owner) : owner_(owner) {
        }

    protected:
        bool EnterPrimaryExpression(PrimaryExpression* node) override {
            switch (node->kind) {
            case PrimaryExpression::Kind::QualifiedName:
                if (node->generic_ref) {
                    owner_.rewrite_generic_ref(*node->generic_ref);
                }
                return false;
            case PrimaryExpression::Kind::Parens:
                owner_.rewrite_expression(node->paren_expr);
                return false;
            case PrimaryExpression::Kind::Heap:
                owner_.rewrite_type(node->heap_type);
                owner_.rewrite_expression(node->heap_size);
                return false;
            case PrimaryExpression::Kind::Construct:
            case PrimaryExpression::Kind::PlacementConstruct:
                owner_.rewrite_type(node->construct_type);
                for (std::unique_ptr<Expression>& arg : node->construct_args) {
                    owner_.rewrite_expression(arg);
                }
                owner_.rewrite_expression(node->placement_target);
                return false;
            case PrimaryExpression::Kind::CopyMove:
                owner_.rewrite_expression(node->paren_expr);
                return false;
            default:
                return false;
            }
        }

        bool EnterCompileTimePropertyExpression(
            CompileTimePropertyExpression* node) override {
            owner_.rewrite_expression(node->receiver);

            for (std::unique_ptr<Expression>& arg : node->arguments) {
                owner_.rewrite_expression(arg);
            }

            return false;
        }

        bool EnterAssignmentExpression(AssignmentExpression* node) override {
            owner_.rewrite_expression(node->left);
            owner_.rewrite_expression(node->right);
            return false;
        }

        bool EnterLogicalOrExpression(LogicalOrExpression* node) override {
            owner_.rewrite_expression(node->left);
            owner_.rewrite_expression(node->right);
            return false;
        }

        bool EnterLogicalAndExpression(LogicalAndExpression* node) override {
            owner_.rewrite_expression(node->left);
            owner_.rewrite_expression(node->right);
            return false;
        }

        bool EnterComparisonExpression(ComparisonExpression* node) override {
            owner_.rewrite_expression(node->left);
            owner_.rewrite_expression(node->right);
            return false;
        }

        bool EnterAdditiveExpression(AdditiveExpression* node) override {
            owner_.rewrite_expression(node->left);
            owner_.rewrite_expression(node->right);
            return false;
        }

        bool EnterMultiplicativeExpression(MultiplicativeExpression* node) override {
            owner_.rewrite_expression(node->left);
            owner_.rewrite_expression(node->right);
            return false;
        }

        bool EnterPowerExpression(PowerExpression* node) override {
            owner_.rewrite_expression(node->left);
            owner_.rewrite_expression(node->right);
            return false;
        }

        bool EnterBitwiseExpression(BitwiseExpression* node) override {
            owner_.rewrite_expression(node->left);
            owner_.rewrite_expression(node->right);
            return false;
        }

        bool EnterShiftExpression(ShiftExpression* node) override {
            owner_.rewrite_expression(node->left);
            owner_.rewrite_expression(node->right);
            return false;
        }

        bool EnterConditionalExpression(ConditionalExpression* node) override {
            owner_.rewrite_expression(node->condition);
            owner_.rewrite_expression(node->then_expr);
            owner_.rewrite_expression(node->else_expr);
            return false;
        }

        bool EnterUnaryExpression(UnaryExpression* node) override {
            owner_.rewrite_expression(node->operand);
            return false;
        }

        bool EnterPostfixExpression(PostfixExpression* node) override {
            owner_.rewrite_expression(node->base);
            owner_.rewrite_expression(node->subscript_expr);

            for (std::unique_ptr<Expression>& arg : node->arguments) {
                owner_.rewrite_expression(arg);
            }

            owner_.rewrite_type(node->cast_type);
            return false;
        }

    private:
        NamespaceLowering& owner_;
    };

    void NamespaceLowering::rewrite_expression_nested(Expression* expr) {
        if (expr == nullptr) { return; }
        if (poisoned_node(expr)) { return; }
        type_location_hint_ = expr->location;

        std::unique_ptr<Expression> holder(expr);
        ExpressionRewriter rewriter(*this);
        rewriter.rewrite_expression(holder);
        holder.release();
    }

    class NamespaceLowering::StatementRewriter : public AST::AstRewriter {
    public:
        explicit StatementRewriter(NamespaceLowering& owner) : owner_(owner) {
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
            owner_.push_scope();

            for (std::unique_ptr<Statement>& child : node->statements) {
                if (auto* vd = dynamic_cast<VariableDeclaration*>(child.get())) {
                    owner_.declare_name(vd->name, vd->type.to_string());
                } else if (auto* sd = dynamic_cast<StructDefinition*>(child.get())) {
                    owner_.declare_name(sd->name, sd->name);
                } else if (auto* fd = dynamic_cast<FunctionDefinition*>(child.get())) {
                    owner_.declare_name(fd->name,
                        fd->return_type.to_string() + "(...)");
                }
            }

            for (std::unique_ptr<Statement>& child : node->statements) {
                owner_.rewrite_statement(child.get());
            }

            std::vector<std::unique_ptr<Statement>> kept;
            kept.reserve(node->statements.size());

            for (std::unique_ptr<Statement>& child : node->statements) {
                if (dynamic_cast<AccessNamespaceStatement*>(child.get()) != nullptr) {
                    continue;
                }

                kept.push_back(std::move(child));
            }

            node->statements = std::move(kept);
            owner_.pop_scope();
            return false;
        }

        bool EnterVariableDeclaration(VariableDeclaration* node) override {
            owner_.rewrite_type(node->type);

            if (node->array_size_expr) {
                owner_.rewrite_expression(node->array_size_expr);
            }

            if (node->function_pointer_type) {
                owner_.rewrite_type(*node->function_pointer_type);
            }

            owner_.rewrite_initializer(node->initializer.get());
            return false;
        }

        bool EnterStructDefinition(StructDefinition* node) override {
            for (StructDefinition::Member& member : node->members) {
                owner_.rewrite_type(member.type);

                if (member.array_size_expr) {
                    owner_.rewrite_expression(member.array_size_expr);
                }

                if (member.function_pointer_type) {
                    owner_.rewrite_type(*member.function_pointer_type);
                }

                owner_.rewrite_initializer(member.initializer.get());
            }

            for (std::unique_ptr<SpecialMemberFunction>& smf : node->special_members) {
                for (Type& parameter : smf->parameters) {
                    owner_.rewrite_type(parameter);
                }

                owner_.rewrite_type(smf->parameter_type);

                for (std::unique_ptr<Expression>& value : smf->parameter_defaults) {
                    owner_.rewrite_expression(value);
                }

                owner_.push_scope();

                if (!smf->parameter_name.empty()) {
                    owner_.declare_name(smf->parameter_name, "object*");
                }

                for (const std::string& name : smf->parameter_names) {
                    if (!name.empty()) {
                        owner_.declare_name(name, "object");
                    }
                }

                owner_.rewrite_statement(smf->body.get());
                owner_.pop_scope();
            }

            return false;
        }

        bool EnterFunctionDefinition(FunctionDefinition* node) override {
            owner_.rewrite_type(node->return_type);

            for (Type& parameter : node->parameters) {
                owner_.rewrite_type(parameter);
            }

            owner_.rewrite_type(node->conversion_target_type);

            for (std::unique_ptr<Expression>& value : node->param_defaults) {
                owner_.rewrite_expression(value);
            }

            owner_.push_scope();

            for (std::size_t i = 0; i < node->param_names.size(); ++i) {
                if (node->param_names[i].empty()) { continue; }

                std::string type_text = i < node->parameters.size()
                    ? node->parameters[i].to_string() : std::string("object");
                owner_.declare_name(node->param_names[i], type_text);
            }

            owner_.rewrite_statement(node->body.get());
            owner_.pop_scope();
            return false;
        }

        bool EnterIfStatement(IfStatement* node) override {
            owner_.rewrite_expression(node->condition);
            owner_.rewrite_statement(node->then_block.get());
            owner_.rewrite_statement(node->else_block.get());
            return false;
        }

        bool EnterForStatement(ForStatement* node) override {
            owner_.push_scope();

            if (node->init) {
                if (auto* vd = dynamic_cast<VariableDeclaration*>(node->init.get())) {
                    owner_.declare_name(vd->name, vd->type.to_string());
                }

                owner_.rewrite_statement(node->init.get());
            }

            owner_.rewrite_expression(node->condition);
            owner_.rewrite_expression(node->step);
            owner_.rewrite_statement(node->body.get());
            owner_.pop_scope();
            return false;
        }

        bool EnterWhileStatement(WhileStatement* node) override {
            owner_.rewrite_expression(node->condition);
            owner_.rewrite_statement(node->body.get());
            return false;
        }

        bool EnterSwitchCaseStatement(SwitchCaseStatement* node) override {
            owner_.rewrite_expression(node->condition);

            for (SwitchCaseStatement::Clause& clause : node->clauses) {
                owner_.rewrite_expression(clause.condition);
                owner_.push_scope();

                for (std::unique_ptr<Statement>& child : clause.statements) {
                    if (auto* vd = dynamic_cast<VariableDeclaration*>(child.get())) {
                        owner_.declare_name(vd->name, vd->type.to_string());
                    } else if (auto* sd = dynamic_cast<StructDefinition*>(child.get())) {
                        owner_.declare_name(sd->name, sd->name);
                    } else if (auto* fd = dynamic_cast<FunctionDefinition*>(child.get())) {
                        owner_.declare_name(fd->name,
                            fd->return_type.to_string() + "(...)");
                    }
                }

                for (std::unique_ptr<Statement>& child : clause.statements) {
                    owner_.rewrite_statement(child.get());
                }

                std::vector<std::unique_ptr<Statement>> kept;
                kept.reserve(clause.statements.size());

                for (std::unique_ptr<Statement>& child : clause.statements) {
                    if (dynamic_cast<AccessNamespaceStatement*>(child.get()) != nullptr) {
                        continue;
                    }

                    kept.push_back(std::move(child));
                }

                clause.statements = std::move(kept);
                owner_.pop_scope();
            }

            return false;
        }

        bool EnterReturnStatement(ReturnStatement* node) override {
            owner_.rewrite_expression(node->value);
            return false;
        }

        bool EnterExpressionStatement(ExpressionStatement* node) override {
            owner_.rewrite_expression(node->expr);
            return false;
        }

        bool EnterDestructStatement(DestructStatement* node) override {
            owner_.rewrite_expression(node->target);
            return false;
        }

        bool EnterAccessNamespaceStatement(AccessNamespaceStatement* node) override {
            if (top_level_entry_ == 0) {
                owner_.handle_access_namespace(node);
            }

            return false;
        }

        bool EnterInstantiationStatement(InstantiationStatement* node) override {
            owner_.rewrite_generic_ref(node->reference);
            return false;
        }

        bool EnterGenericDefinition(GenericDefinition* node) override {
            if (top_level_entry_ == 0) {
                return false;
            }

            owner_.push_scope();

            for (const GenericParameter& param : node->parameters) {
                owner_.declare_name(param.name, "generic parameter");
            }

            for (std::unique_ptr<TopLevel>& member : node->members) {
                if (member == nullptr) { continue; }

                if (std::string(declaration_kind(member.get())) == "unknown") {
                    continue;
                }

                std::string member_name = declaration_name(member.get());

                if (member_name.empty()) { continue; }

                owner_.declare_name(member_name,
                    declaration_type_text(member.get()));
            }

            for (std::unique_ptr<TopLevel>& member : node->members) {
                owner_.rewrite_top_level(member.get());
            }

            for (std::unique_ptr<Statement>& item : node->compile_time_items) {
                owner_.rewrite_statement(item.get());
            }

            owner_.pop_scope();
            return false;
        }

        bool EnterExternDeclaration(ExternDeclaration* node) override {
            owner_.rewrite_type(node->return_type);

            for (Type& parameter : node->parameters) {
                owner_.rewrite_type(parameter);
            }

            return false;
        }

        bool EnterEmitStatement(EmitStatement* node) override {
            for (std::unique_ptr<Expression>& piece : node->pieces) {
                owner_.rewrite_expression(piece);
            }

            for (std::unique_ptr<Node>& item : node->block_items) {
                if (auto* top = dynamic_cast<TopLevel*>(item.get())) {
                    owner_.rewrite_top_level(top);
                } else if (auto* inner = dynamic_cast<Statement*>(item.get())) {
                    owner_.rewrite_statement(inner);
                }
            }

            return false;
        }

        bool EnterNamespaceDefinition(NamespaceDefinition*) override {
            return false;
        }

        bool EnterAdditionNamespaceStatement(AdditionNamespaceStatement*) override {
            return false;
        }

        bool EnterCondDefinition(CondDefinition*) override { return false; }

        bool EnterUncondDefinition(UncondDefinition*) override { return false; }

        bool EnterConditionalBlock(ConditionalBlock*) override { return false; }

        bool EnterTopLevelBlock(TopLevelBlock*) override { return false; }

    private:
        NamespaceLowering& owner_;
        int top_level_entry_ = 0;
    };

    class NamespaceLowering::InitializerRewriter : public AST::AstRewriter {
    public:
        explicit InitializerRewriter(NamespaceLowering& owner) : owner_(owner) {
        }

    protected:
        bool EnterExpressionInitializer(ExpressionInitializer* node) override {
            owner_.rewrite_expression(node->expr);
            return false;
        }

        bool EnterArrayInitializer(ArrayInitializer* node) override {
            for (std::unique_ptr<Initializer>& element : node->elements) {
                owner_.rewrite_initializer(element.get());
            }

            return false;
        }

    private:
        NamespaceLowering& owner_;
    };

    void NamespaceLowering::rewrite_initializer(Initializer* init) {
        if (init == nullptr) { return; }
        if (poisoned_node(init)) { return; }
        PoisonGuard guard(*this, init);

        std::unique_ptr<Initializer> holder(init);
        InitializerRewriter rewriter(*this);
        rewriter.rewrite_initializer(holder);
        holder.release();
    }

    void NamespaceLowering::rewrite_statement(Statement* stmt) {
        if (stmt == nullptr) { return; }
        if (poisoned_node(stmt)) { return; }
        PoisonGuard guard(*this, stmt);
        type_location_hint_ = stmt->location;

        std::unique_ptr<Statement> holder(stmt);
        StatementRewriter rewriter(*this);
        rewriter.rewrite_statement(holder);
        holder.release();
    }

    void NamespaceLowering::rewrite_top_level(TopLevel* node) {
        if (node == nullptr) { return; }
        if (poisoned_node(node)) { return; }
        PoisonGuard guard(*this, node);
        type_location_hint_ = node->location;

        std::unique_ptr<TopLevel> holder(node);
        StatementRewriter rewriter(*this);
        rewriter.rewrite_top_level(holder);
        holder.release();
    }

    void NamespaceLowering::handle_access_namespace(AccessNamespaceStatement* node) {
        const std::vector<std::string>& path = node->path;
        if (path.empty()) { return; }
        if (poisoned_node(node)) { return; }
        PoisonGuard guard(*this, node);
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
        poisoned_.clear();
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
        program->top_levels = std::move(out);
        return !had_error_;
    }

    bool NamespaceLowering::lower_new_top_levels(
        std::vector<std::unique_ptr<AST::TopLevel>>& nodes) {
        if (nodes.empty()) { return true; }
        if (scopes_.empty()) { push_scope(); }

        const std::size_t errors_before = diag_.error_count();

        for (auto& node : nodes) {
            std::string name = declaration_name(node.get());

            if (name.empty()) { continue; }

            declare_name(name, declaration_type_text(node.get()));
        }

        std::vector<std::unique_ptr<TopLevel>> out;
        out.reserve(nodes.size());
        lower_top_levels(nodes, out, false);
        nodes = std::move(out);
        return diag_.error_count() == errors_before;
    }

}
