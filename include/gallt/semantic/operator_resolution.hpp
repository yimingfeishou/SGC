#ifndef GALLT_SEMANTIC_OPERATOR_RESOLUTION_HPP
#define GALLT_SEMANTIC_OPERATOR_RESOLUTION_HPP

#include "../parser/ast.hpp"
#include <memory>

namespace gallt {

    namespace operator_resolution {

        inline bool operand_can_be_addressed(const AST::Type& operand_type) {
            return operand_type.kind != AST::TypeKind::Pointer &&
                operand_type.kind != AST::TypeKind::Array &&
                operand_type.kind != AST::TypeKind::Void;
        }

        inline bool implicitly_takes_address(const AST::Type& operand_type,
            bool operand_is_lvalue, const AST::Type& parameter_type) {
            if (parameter_type.kind != AST::TypeKind::Pointer) { return false; }
            if (!operand_can_be_addressed(operand_type)) { return false; }
            return operand_is_lvalue;
        }

        inline AST::Type effective_operand_type(const AST::Type& operand_type,
            bool operand_is_lvalue, const AST::Type& parameter_type) {
            if (!implicitly_takes_address(operand_type, operand_is_lvalue,
                parameter_type)) {
                return operand_type;
            }

            return AST::Type::make_pointer(std::make_shared<AST::Type>(operand_type));
        }

    }

}

#endif
