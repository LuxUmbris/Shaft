#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace Lexer
{
    enum class Keyword
    {
        // control flow
        IF,
        ELSE,
        MATCH,
        CASE,
        DEFAULT,
        WHILE,
        FOR,
        FOREACH,
        BREAK,
        CONTINUE,
        // declarations
        DEF,
        DEC,
        CDEF,
        CDEC,
        ALIGN,
        STRUCT,
        CLASS,
        INDEX,
        INIT,
        NAMESPACE,
        USING,
        GLOBAL,
        IMPORT,
        EXPORT,
        ENUM,
        // functions
        TUNNEL,
        INLINE,
        RESERVE,
        RETURN,
        // memory and types
        MUT,
        MOVE,
        REF,
        SELF,
        VALID,
        RAW,
        SIZEOF,
        ASYNC,
        AWAIT,
        START,
        // primitive types
        STATE,
        THREAD,
        BOOL,
        CHAR,
        U8,
        U16,
        U32,
        U64,
        USIZE,
        I8,
        I16,
        I32,
        I64,
        F32,
        F64
    };

    enum class Operator
    {
        // assignment/arithmetic/bitwise
        ASSIGN,
        ADD_ASSIGN,
        SUBTRACT_ASSIGN,
        MULTIPLY_ASSIGN,
        DIVIDE_ASSIGN,
        MODULO_ASSIGN,
        AMPERSAND,
        PIPE,
        CARET,
        LEFT_SHIFT,
        RIGHT_SHIFT,
        LEFT_SHIFT_ASSIGN,
        RIGHT_SHIFT_ASSIGN,
        LESS_THAN,
        GREATER_THAN,
        LESS_EQUAL,
        GREATER_EQUAL,
        EQUAL,
        NOT_EQUAL,
        OR,
        AND,
        // access/scope
        DOUBLE_COLON,
        DOT,
        RIGHT_ARROW,
        LEFT_ARROW,
        // grouping/delimiters
        LEFT_PAREN,
        RIGHT_PAREN,
        LEFT_BRACKET,
        RIGHT_BRACKET,
        LEFT_BRACE,
        RIGHT_BRACE,
        COLON,
        SEMICOLON,
        COMMA,
        // other
        TRIPPLE_DOT,
        QUESTION_MARK,
        DOUBLE_QUOTE,
        SINGLE_QUOTE,
        PLUS,
        MINUS,
        MULTIPLY,
        DIVIDE,
        MODULO,
        EXLAMATION_MARK
    };

    enum class TokenType
    {
        Identifier,
        Keyword,
        Operator,
        IntegerLiteral,
        UnsignedIntegerLiteral,
        FloatLiteral,
        StringLiteral,
        CharLiteral,
        BoolLiteral,
        ModuleBoundary,
        EndOfFile
    };

    struct Token
    {
        TokenType type;

        uint64_t start;

        std::variant<std::string_view, Keyword, Operator, uint64_t, double, bool, std::monostate> value;
        std::string* source;
        std::string* mod_path;
    };

    inline const std::unordered_map<std::string_view, Keyword> keyword_lookup = {
        {"namespace", Keyword::NAMESPACE},
        {"default", Keyword::DEFAULT},
        {"foreach", Keyword::FOREACH},
        {"align", Keyword::ALIGN},
        {"struct", Keyword::STRUCT},
        {"class", Keyword::CLASS},
        {"index", Keyword::INDEX},
        {"init", Keyword::INIT},
        {"import", Keyword::IMPORT},
        {"export", Keyword::EXPORT},
        {"inline", Keyword::INLINE},
        {"reserve", Keyword::RESERVE},
        {"valid", Keyword::VALID},
        {"match", Keyword::MATCH},
        {"while", Keyword::WHILE},
        {"break", Keyword::BREAK},
        {"continue", Keyword::CONTINUE},
        {"else", Keyword::ELSE},
        {"using", Keyword::USING},
        {"global", Keyword::GLOBAL},
        {"case", Keyword::CASE},
        {"tunnel", Keyword::TUNNEL},
        {"move", Keyword::MOVE},
        {"mut", Keyword::MUT},
        {"ref", Keyword::REF},
        {"self", Keyword::SELF},
        {"raw", Keyword::RAW},
        {"sizeof", Keyword::SIZEOF},
        {"async", Keyword::ASYNC},
        {"await", Keyword::AWAIT},
        {"start", Keyword::START},
        {"def", Keyword::DEF},
        {"dec", Keyword::DEC},
        {"cdef", Keyword::CDEF},
        {"cdec", Keyword::CDEC},
        {"bool", Keyword::BOOL},
        {"char", Keyword::CHAR},
        {"State", Keyword::STATE},
        {"Thread", Keyword::THREAD},
        {"u8", Keyword::U8},
        {"u16", Keyword::U16},
        {"u32", Keyword::U32},
        {"u64", Keyword::U64},
        {"usize", Keyword::USIZE},
        {"i8", Keyword::I8},
        {"i16", Keyword::I16},
        {"i32", Keyword::I32},
        {"i64", Keyword::I64},
        {"f32", Keyword::F32},
        {"f64", Keyword::F64},
        {"if", Keyword::IF},
        {"for", Keyword::FOR},
        {"enum", Keyword::ENUM},
        {"return", Keyword::RETURN}};

    // longest match
    inline const std::vector<std::pair<std::string_view, Operator>> operator_lookup = {
        {"<<=", Operator::LEFT_SHIFT_ASSIGN},
        {">>=", Operator::RIGHT_SHIFT_ASSIGN},
        {"...", Operator::TRIPPLE_DOT},
        {"<<", Operator::LEFT_SHIFT},
        {">>", Operator::RIGHT_SHIFT},
        {"<=", Operator::LESS_EQUAL},
        {">=", Operator::GREATER_EQUAL},
        {"==", Operator::EQUAL},
        {"!=", Operator::NOT_EQUAL},
        {"&&", Operator::AND},
        {"||", Operator::OR},
        {"::", Operator::DOUBLE_COLON},
        {"->", Operator::RIGHT_ARROW},
        {"<-", Operator::LEFT_ARROW},
        {"+=", Operator::ADD_ASSIGN},
        {"-=", Operator::SUBTRACT_ASSIGN},
        {"*=", Operator::MULTIPLY_ASSIGN},
        {"/=", Operator::DIVIDE_ASSIGN},
        {"%=", Operator::MODULO_ASSIGN},
        {"+", Operator::PLUS},
        {"-", Operator::MINUS},
        {"*", Operator::MULTIPLY},
        {"/", Operator::DIVIDE},
        {"%", Operator::MODULO},
        {"&", Operator::AMPERSAND},
        {"|", Operator::PIPE},
        {"^", Operator::CARET},
        {"<", Operator::LESS_THAN},
        {">", Operator::GREATER_THAN},
        {"!", Operator::EXLAMATION_MARK},
        {".", Operator::DOT},
        {":", Operator::COLON},
        {";", Operator::SEMICOLON},
        {",", Operator::COMMA},
        {"=", Operator::ASSIGN},
        {"(", Operator::LEFT_PAREN},
        {")", Operator::RIGHT_PAREN},
        {"[", Operator::LEFT_BRACKET},
        {"]", Operator::RIGHT_BRACKET},
        {"{", Operator::LEFT_BRACE},
        {"}", Operator::RIGHT_BRACE},
        {"?", Operator::QUESTION_MARK}};

    struct Module
    {
        std::string path;
        std::string source;
    };

    struct LexedModule
    {
        std::string path;
        std::vector<Token> tokens;
    };

    LexedModule tokenize(const Module &src);
    std::vector<LexedModule> tokenize_modules(const std::vector<Module> &sources);

} // namespace Lexer