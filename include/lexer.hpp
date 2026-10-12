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
        DO,
        YIELD,
        // declarations
        DEF,
        DEC,
        CDEF,
        CDEC,
        ALIGN,
        STRUCT,
        CLASS,
        TRAIT,
        IMPL,
        INDEX,
        INIT,
        NAMESPACE,
        USING,
        META_USING,
        META_COMPTIME,
        META_UNSAFE,
        META_NOCHECK,
        META_NOCLEANUP,
        META_END,
        GLOBAL,
        IMPORT,
        EXPORT,
        ENUM,
        // functions
        TUNNEL,
        INLINE,
        NAKED,
        RESERVE,
        RETURN,
        TERMINATE,
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
        UNION,
        AS,
        TYPEOF,
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
        DOLLAR,
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
        InlineAsm,
        Attribute,
        ModuleBoundary,
        EndOfFile
    };

    struct Token
    {
        TokenType type;

        uint64_t start;

        std::variant<std::string_view, Keyword, Operator, uint64_t, double, bool, std::monostate> value;
        std::string *source;
        std::string *mod_path;
        std::string macroBacktrace;
    };

    inline const std::unordered_map<std::string_view, Keyword> keyword_lookup = {
        {"namespace", Keyword::NAMESPACE},
        {"default", Keyword::DEFAULT},
        {"foreach", Keyword::FOREACH},
        {"align", Keyword::ALIGN},
        {"struct", Keyword::STRUCT},
        {"class", Keyword::CLASS},
        {"trait", Keyword::TRAIT},
        {"impl", Keyword::IMPL},
        {"@index", Keyword::INDEX},
        {"@init", Keyword::INIT},
        {"import", Keyword::IMPORT},
        {"export", Keyword::EXPORT},
        {"inline", Keyword::INLINE},
        {"naked", Keyword::NAKED},
        {"reserve", Keyword::RESERVE},
        {"valid", Keyword::VALID},
        {"match", Keyword::MATCH},
        {"while", Keyword::WHILE},
        {"break", Keyword::BREAK},
        {"continue", Keyword::CONTINUE},
        {"else", Keyword::ELSE},
        {"using", Keyword::USING},
        {"@using", Keyword::META_USING},
        {"@comptime", Keyword::META_COMPTIME},
        {"@unsafe", Keyword::META_UNSAFE},
        {"@nocheck", Keyword::META_NOCHECK},
        {"@nocleanup", Keyword::META_NOCLEANUP},
        {"@end", Keyword::META_END},
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
        {"do", Keyword::DO},
        {"yield", Keyword::YIELD},
        {"enum", Keyword::ENUM},
        {"return", Keyword::RETURN},
        {"terminate", Keyword::TERMINATE},
        {"union", Keyword::UNION},
        {"as", Keyword::AS},
        {"typeof", Keyword::TYPEOF}};

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
        {"$", Operator::DOLLAR},
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

    struct ConfigCondition
    {
        enum class Kind
        {
            Equals,
            In,
            Matches
        };
        Kind kind = Kind::Equals;
        std::string field;
        std::vector<std::string> values;
    };

    struct ProceduralAttribute
    {
        std::string name;
        std::vector<std::pair<std::string, std::string>> params; // {kind, capture}
        std::string targetKind, targetCapture;
        std::string quote, prepend;
    };
    const ProceduralAttribute *find_procedural_attribute(std::string_view name);

    struct Module
    {
        std::string path;
        std::string source;
    };

    struct Configuration
    {
        std::unordered_map<std::string, std::string> values;
        std::vector<std::string> comptimeCommands;
    };

    struct LexedModule
    {
        std::string path;
        std::vector<Token> tokens;
    };

    LexedModule tokenize(const Module &src);
    std::string preprocess_config_blocks(const Module &source, const Configuration &configuration);
    std::vector<LexedModule> tokenize_modules(const std::vector<Module> &sources,
                                              const Configuration &configuration = {});

} // namespace Lexer