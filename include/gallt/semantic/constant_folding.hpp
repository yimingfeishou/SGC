#ifndef GALLT_SEMANTIC_CONSTANT_FOLDING_HPP
#define GALLT_SEMANTIC_CONSTANT_FOLDING_HPP

#include "../parser/ast.hpp"
#include <cstddef>
#include <functional>
#include <string>

namespace gallt {

    struct ConstantEvaluationContext {
        std::function<bool(const std::string& type_name, std::size_t& size, std::size_t& align)>
            type_layout;
        std::function<bool(const std::string& name, long long& int_value, double& float_value,
            bool& is_float)> lookup_constant;
    };

    bool evaluate_constant_expression(const AST::Expression* expr, long long& int_out,
        double& float_out, bool& is_float_out, const ConstantEvaluationContext& context);

} 

#endif 
