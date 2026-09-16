#ifndef GALLT_COMMON_TOKEN_HPP
#define GALLT_COMMON_TOKEN_HPP

#include "source_location.hpp"
#include <string_view>
#include <cstdint>

namespace gallt {

    enum class TokenType : std::uint16_t {
        Keyword_Int,        
        Keyword_Lint,       
        Keyword_Uint,       
        Keyword_Luint,      
        Keyword_Float,      
        Keyword_Double,     
        Keyword_Char,       
        Keyword_Uchar,      
        Keyword_Bool,       
        Keyword_String,     
        Keyword_File,       
        Keyword_Void,       
        Keyword_Struct,     
        Keyword_If,         
        Keyword_Else,       
        Keyword_For,        
        Keyword_While,      
        Keyword_Break,      
        Keyword_Return,     
        Keyword_Guide,      
        Keyword_Clib,       
        Keyword_Extern,     
        Keyword_From,       
        Keyword_Null,       
        Keyword_Heap,       
        Keyword_Free,       
        Keyword_Input,      
        Keyword_Output,     
        Keyword_Size,       
        Keyword_Align,      
        Keyword_Generics,   
        Keyword_Namespace,  
        Keyword_Access,     
        Keyword_Addition,   
        Keyword_Emit,       
        Keyword_Const,      
        Identifier,
        IntegerLiteral,     
        FloatLiteral,       
        CharLiteral,        
        StringLiteral,      
        BoolLiteral,        
        Assign,             
        PlusAssign,         
        MinusAssign,        
        Equal,              
        NotEqual,           
        Greater,            
        Less,               
        GreaterEqual,       
        LessEqual,          
        Plus,               
        Minus,              
        Star,               
        Slash,              
        Percent,            
        Pipe,               
        Power,              
        Increment,          
        Decrement,          
        AddressOf,          
        LogicalAnd,         
        LogicalOr,          
        LogicalNot,         
        Dot,                
        Arrow,              
        ColonColon,         
        LeftParen,          
        RightParen,         
        LeftBrace,          
        RightBrace,         
        LeftBracket,        
        RightBracket,       
        Comma,              
        Semicolon,          
        Colon,              
        Newline,            
        EndOfFile,          
        Unknown,            
    };

    struct Token {
        TokenType type;                     
        SourceLocation location;            
        std::string_view lexeme;            

        Token() = default;

        Token(TokenType t, SourceLocation loc, std::string_view lex)
            : type(t), location(loc), lexeme(lex) {
        }

        bool is(TokenType t) const noexcept { return type == t; }

        bool is_keyword() const noexcept {
            return type >= TokenType::Keyword_Int && type <= TokenType::Keyword_Const;
        }

        bool is_literal() const noexcept {
            return type >= TokenType::IntegerLiteral && type <= TokenType::BoolLiteral;
        }

        bool is_operator() const noexcept {
            return type >= TokenType::Assign && type <= TokenType::ColonColon;
        }

        bool is_delimiter() const noexcept {
            return type >= TokenType::LeftParen && type <= TokenType::Semicolon;
        }
    };

    const char* token_type_to_string(TokenType type) noexcept;

    bool is_bool_literal(std::string_view lexeme) noexcept;

    bool is_keyword_string(std::string_view lexeme) noexcept;

    TokenType keyword_to_token_type(std::string_view lexeme) noexcept;

} 

#endif 
