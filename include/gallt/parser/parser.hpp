// parser/parser.hpp
// 语法分析器 —— 递归下降解析 Gallt 源代码，生成 AST
// Parser — recursive-descent parser for Gallt source, generates AST

#ifndef GALLT_PARSER_PARSER_HPP
#define GALLT_PARSER_PARSER_HPP

#include "ast.hpp"
#include "../lexer/lexer.hpp"
#include "../common/diagnostics.hpp"
#include <vector>
#include <memory>
#include <optional>

namespace gallt {

    // ============================================================================
    // 语法分析器 (Parser)
    // 负责将 Token 流根据 Gallt EBNF 规则解析为 AST
    // Responsible for parsing token stream according to Gallt EBNF rules into AST
    // ============================================================================

    class Parser {
    public:
        // ---- 构造与析构 ----
        Parser(Lexer& lexer, DiagnosticEngine& diag);
        ~Parser() = default;

        // 禁止拷贝
        Parser(const Parser&) = delete;
        Parser& operator=(const Parser&) = delete;

        // ---- 主接口 ----
        // 解析整个程序，返回 AST Program 节点
        // Parse the entire program, return AST Program node
        std::unique_ptr<AST::Program> parse();

    private:
        // ---- 成员变量 ----
        Lexer& lexer_;
        DiagnosticEngine& diag_;
        Token current_;          // 当前 Token (已消费)
        Token peek_;             // 预读 Token (下一个)
        bool has_peek_ = false;  // 是否已预读
        bool has_error_ = false; // 是否遇到语法错误（用于错误恢复）

        // ---- 循环嵌套深度（用于 break 检查） ----
        int loop_depth_ = 0;     // 当前所在循环嵌套层数

        // ---- 语法复杂度保护 ----
        // 病态输入（超长表达式、超深嵌套的括号/块/初始化列表）会让递归下降
        // 解析器及其后续递归遍历（类型检查、代码生成、AST 析构）耗尽线程栈。
        // 因此对嵌套深度与单个表达式的 token 数设置上限，超限时报 ER 0020。
        // Syntax complexity guards: pathological inputs (very long expressions or
        // deeply nested parens/blocks/initializers) would exhaust the thread stack in
        // the recursive-descent parser and in the later recursive passes; the guards
        // below report ER 0020 instead of crashing.
        static constexpr int kMaxExpressionNesting = 1024;
        static constexpr std::size_t kMaxExpressionTokens = 10000;
        static constexpr int kMaxBlockNesting = 1024;
        static constexpr int kMaxInitializerNesting = 1024;

        int expression_depth_ = 0;             // 当前表达式嵌套深度
        std::size_t expression_tokens_ = 0;    // 当前顶层表达式已消费的 token 数
        bool complexity_limit_hit_ = false;    // 是否已报告复杂度超限（抑制级联诊断）
        int block_depth_ = 0;                  // 当前块嵌套深度
        int initializer_depth_ = 0;            // 当前初始化列表嵌套深度

        // ---- 错误恢复状态 ----
        bool in_error_recovery_ = false; // 是否处于错误恢复模式

        // ---- 核心解析函数 ----
        // 获取下一个 Token (消费当前)
        void advance();
        // 预读下一个 Token
        void peek_token();
        // 检查当前 Token 类型是否匹配，若匹配则消费并返回 true，否则报错
        bool expect(TokenType type, const std::string& err_msg);
        // 检查当前 Token 类型是否匹配，若匹配则消费并返回 true，否则不报错
        bool match(TokenType type);
        // 报告语法错误（带位置和消息）
        void report_error(ErrorCode code, const std::string& msg);
        void report_error_at(SourceLocation loc, ErrorCode code, const std::string& msg);

        // 跳过连续换行 token（空行不影响语法）
        // Skip consecutive newline tokens (blank lines are not significant)
        void skip_newlines();

        // ---- 错误恢复：同步到合适的同步点 ----
        void synchronize();

        // ---- 解析顶层结构 (TopLevel) ----
        std::unique_ptr<AST::TopLevel> parse_top_level();
        std::unique_ptr<AST::GuideStatement> parse_guide_statement();
        std::unique_ptr<AST::ClibStatement> parse_clib_statement();
        std::unique_ptr<AST::ExternDeclaration> parse_extern_declaration();
        // 顶层以类型开始的声明：函数定义或全局变量
        // A top-level type-start declaration may be a function or a global variable
        std::unique_ptr<AST::TopLevel> parse_function_definition();
        std::unique_ptr<AST::StructDefinition> parse_struct_definition();

        // ---- 解析语句 (Statement) ----
        std::unique_ptr<AST::Statement> parse_statement();
        std::unique_ptr<AST::Statement> parse_declaration_or_statement(); // 用于 for-init
        std::unique_ptr<AST::VariableDeclaration> parse_variable_declaration();
        std::unique_ptr<AST::IfStatement> parse_if_statement();
        std::unique_ptr<AST::ForStatement> parse_for_statement();
        std::unique_ptr<AST::WhileStatement> parse_while_statement();
        std::unique_ptr<AST::BreakStatement> parse_break_statement();
        std::unique_ptr<AST::ReturnStatement> parse_return_statement();
        std::unique_ptr<AST::Block> parse_block();
        std::unique_ptr<AST::ExpressionStatement> parse_expression_statement();
        std::unique_ptr<AST::EmptyStatement> parse_empty_statement();

        // ---- 解析类型 (Type) ----
        // 解析类型说明符（含指针、函数指针后缀）
        // Parse type specifier (including pointers and function pointer suffix)
        AST::Type parse_type(bool allow_void = true,
            bool allow_function_suffix = true);
        // 解析类型说明符（不带函数指针后缀，用于声明中的基本类型）
        AST::Type parse_type_specifier(bool allow_void = true);

        // ---- 解析函数指针后缀 ----
        // 函数指针后缀: ( [parameter_list] ) *
        // 返回 std::optional<AST::Type>，表示函数指针类型（如果存在）
        std::optional<AST::Type> parse_function_pointer_suffix(AST::Type base_type);

        // 在声明符（变量/成员/形参名称）之后解析数组或函数指针后缀
        // Parse array or function-pointer suffix after a declarator name
        AST::Type finish_declarator_type(AST::Type base, bool allow_empty_array,
            std::optional<size_t>* out_array_size = nullptr);

        // ---- 解析形参列表 (Parameter List) ----
        // 形参列表: parameter { ',' parameter }
        // 返回 (类型列表, 名称列表)
        std::pair<std::vector<AST::Type>, std::vector<std::string>> parse_parameter_list();

        // ---- 解析初始化器 (Initializer) ----
        std::unique_ptr<AST::Initializer> parse_initializer();

        // ---- 解析表达式 (Expression) ----
        // 按照优先级从低到高实现
        std::unique_ptr<AST::Expression> parse_expression();                     // 顶层入口
        std::unique_ptr<AST::Expression> parse_assignment_expression();          // 赋值
        std::unique_ptr<AST::Expression> parse_logical_or_expression();          // ||
        std::unique_ptr<AST::Expression> parse_logical_and_expression();         // &&
        std::unique_ptr<AST::Expression> parse_comparison_expression();          // > < == != >= <=
        std::unique_ptr<AST::Expression> parse_additive_expression();            // + -
        std::unique_ptr<AST::Expression> parse_multiplicative_expression();      // * /
        std::unique_ptr<AST::Expression> parse_power_expression();               // **
        std::unique_ptr<AST::Expression> parse_unary_expression();               // 一元运算符
        std::unique_ptr<AST::Expression> parse_postfix_expression();             // 后缀运算
        std::unique_ptr<AST::Expression> parse_primary_expression();             // 基本表达式

        // ---- 解析后缀运算符 ----
        // 处理下标、函数调用、类型转换、自增/自减、成员访问
        std::unique_ptr<AST::Expression> parse_postfix_operator(
            std::unique_ptr<AST::Expression> base);

        // ---- 解析参数列表 (用于函数调用) ----
        std::vector<std::unique_ptr<AST::Expression>> parse_argument_list();

        // ---- 解析 for 初始化部分 ----
        // for_init = variable_declaration | expression
        std::unique_ptr<AST::Statement> parse_for_init();

        // ---- 解析可选表达式 ----
        // 用于 for 的条件和步进，以及 return 的可选值
        std::unique_ptr<AST::Expression> parse_optional_expression();

        // ---- 工具函数 ----
        // 检查当前 Token 是否为语句结束符 (; 或换行)
        bool is_stmt_end() const;
        // 消费语句结束符（如果当前是 ; 或换行则跳过，否则报错）
        bool expect_stmt_end(const std::string& context);

        // ---- 常量表达式求值（用于数组大小） ----
        // 尝试将表达式求值为常量整数，若失败则返回 nullopt
        std::optional<size_t> evaluate_constant_expression(AST::Expression* expr);
        // 简化版：只处理整数字面量
        std::optional<size_t> try_parse_integer_literal();

        // ---- 辅助 ----
        // 获取当前源码位置
        SourceLocation current_location() const;

        // 检查标识符是否已定义（用于重复声明检查，但此阶段只做语法，语义检查在后续）
        // 但为了提前报错，可以简单检查是否为关键字。
        bool is_valid_identifier(const std::string& name) const;
    };

} // namespace gallt

#endif // GALLT_PARSER_PARSER_HPP
