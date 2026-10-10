#ifndef GALLT_SEMANTIC_LIFECYCLE_TRANSFER_HPP
#define GALLT_SEMANTIC_LIFECYCLE_TRANSFER_HPP

#include "../parser/ast.hpp"

namespace gallt {

    namespace lifecycle_transfer {

        inline bool is_move_source(const AST::Expression* expr) {
            if (expr == nullptr) {
                return false;
            }

            if (auto* prim = dynamic_cast<const AST::PrimaryExpression*>(expr)) {
                if (prim->kind == AST::PrimaryExpression::Kind::CopyMove) {
                    return prim->copy_move_kind == AST::PrimaryExpression::CopyMoveKind::Move;
                }

                if (prim->kind == AST::PrimaryExpression::Kind::Parens &&
                    prim->paren_expr != nullptr) {
                    return is_move_source(prim->paren_expr.get());
                }
            }

            if (dynamic_cast<const AST::ConditionalExpression*>(expr) != nullptr) {
                return false;
            }

            return !expr->is_lvalue();
        }

        inline bool is_explicit_move(const AST::Expression* expr) {
            if (auto* prim = dynamic_cast<const AST::PrimaryExpression*>(expr)) {
                if (prim->kind == AST::PrimaryExpression::Kind::CopyMove) {
                    return prim->copy_move_kind == AST::PrimaryExpression::CopyMoveKind::Move;
                }

                if (prim->kind == AST::PrimaryExpression::Kind::Parens &&
                    prim->paren_expr != nullptr) {
                    return is_explicit_move(prim->paren_expr.get());
                }
            }

            return false;
        }

        inline bool is_temporary_source(const AST::Expression* expr) {
            return is_move_source(expr) && !is_explicit_move(expr);
        }

    }

}

#endif
