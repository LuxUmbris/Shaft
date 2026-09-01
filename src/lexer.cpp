#include "lexer.hpp"
#include "error.hpp"
#include <algorithm>
#include <deque>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace Lexer
{
    std::string current_mod_path;
    std::string source;
    uint64_t line = 1;
    uint64_t column = 1;
    uint64_t pos = 0;

    std::string read_file(const std::string &path)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            std::cerr << "Failed to open file: " << path << std::endl;
            return "";
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    static std::deque<std::string> sourceArchive;
    static std::deque<std::string> modPathArchive;

    inline bool is_whitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
    inline bool is_alpha(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }

    static inline char peek(uint64_t offset = 0)
    {
        if (pos + offset >= source.size())
            return '\0';
        return source[pos + offset];
    }

    static inline char advance()
    {
        if (pos < source.size())
        {
            char c = source[pos++];
            return c;
        }
        return '\0';
    }

    static inline void update_line_and_column()
    {
        line = 1;
        column = 1;

        for (uint64_t i = 0; i < pos; i++)
        {
            if (source[i] == '\n')
            {
                line++;
                column = 1;
            }
            else column++;
        }
    }

    static void skip_whitespace_and_comments()
    {
        while (pos < source.size())
        {
            if (is_whitespace(peek()))
            {
                advance();
            }
            else if (peek() == '/' && peek(1) == '/')
            {
                advance();
                advance();
                while (peek() != '\n' && peek() != '\0')
                    advance();
            }
            else if (peek() == '/' && peek(1) == '*')
            {
                advance();
                advance();
                while (!(peek() == '*' && peek(1) == '/') && peek() != '\0')
                    advance();

                if (peek() == '*' && peek(1) == '/')
                {
                    advance();
                    advance();
                }
                else
                {
                    update_line_and_column();
                    Error err = {"Block comment was never closed.", current_mod_path, line, column};
                    panic(err);
                }
            }
            else
            {
                break;
            }
        }
    }

    static bool parse_full_number(uint64_t &integerValue, double &floatValue)
    {
        const uint64_t start = pos;

        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X'))
        {
            advance();
            advance();

            const uint64_t digitsStart = pos;

            while (is_digit(peek()) || (peek() >= 'a' && peek() <= 'f') ||
                   (peek() >= 'A' && peek() <= 'F'))
                advance();

            if (pos == digitsStart)
            {
                Error err = {"Hex literal requires at least one digit", current_mod_path, line, column};
                panic(err);
            }

            std::string_view num_view =
                std::string_view(source).substr(start + 2, pos - (start + 2));
            integerValue = std::stoull(std::string(num_view), nullptr, 16);
            return false;
        }

        while (is_digit(peek()))
            advance();

        bool isFloat = false;
        if (peek() == '.' && is_digit(peek(1)))
        {
            isFloat = true;
            advance();
            while (is_digit(peek()))
                advance();
        }

        if (peek() == 'e' || peek() == 'E')
        {
            isFloat = true;
            advance();
            if (peek() == '+' || peek() == '-')
                advance();
            if (!is_digit(peek()))
            {
                update_line_and_column();
                Error err = {"exponent requires at least one digit", current_mod_path, line, column};
                panic(err);
            }
            while (is_digit(peek()))
                advance();
        }

        std::string_view num_view = std::string_view(source).substr(start, pos - start);
        if (isFloat)
        {
            floatValue = std::stod(std::string(num_view));
            return true;
        }
        integerValue = std::stoull(std::string(num_view), nullptr, 10);
        return false;
    }

    static std::string_view parse_identifier()
    {
        auto start = pos;
        while (is_alpha(peek()) || is_digit(peek()))
            advance();
        return std::string_view(source).substr(start, pos - start);
    }

    static std::string_view parse_string()
    {
        advance();
        auto start = pos;

        while (peek() != '"' && peek() != '\0')
        {
            if (peek() == '\\')
            {
                advance();
                advance();
            }
            else
            {
                advance();
            }
        }

        if (peek() != '"')
        {
            update_line_and_column();
            Error err = {"String literal was never terminated.", current_mod_path, line, column};
            panic(err);
        }

        auto len = pos - start;
        advance();

        return std::string_view(source).substr(start, len);
    }

    static std::string_view parse_raw_string()
    {
        while (peek() == ' ' || peek() == '\t')
            advance();

        if (peek() != '-' || peek(1) != '>')
        {
            update_line_and_column();
            Error err = {"Expected '->' after raw", current_mod_path, line, column};
            panic(err);
        }
        advance();
        advance();

        while (peek() == ' ' || peek() == '\t')
            advance();

        if (!is_alpha(peek()))
        {
            update_line_and_column();
            Error err = {"Expected a delimiter after 'raw ->'", current_mod_path, line, column};
            panic(err);
        }
        const std::string_view delimiter = parse_identifier();

        while (peek() == ' ' || peek() == '\t')
            advance();

        if (peek() == '\r')
            advance();
        if (peek() != '\n')
        {
            update_line_and_column();
            Error err = {"Expected a newline after raw delimiter", current_mod_path, line, column};
            panic(err);
        }
        advance();

        const uint64_t start = pos;
        while (peek() != '\0')
        {
            const bool delimiter_at_line_start =
                (pos == start || source[pos - 1] == '\n') &&
                source.compare(pos, delimiter.size(), delimiter) == 0 &&
                (peek(delimiter.size()) == '\n' || peek(delimiter.size()) == '\r' ||
                 peek(delimiter.size()) == '\0');
            if (delimiter_at_line_start)
            {
                const uint64_t length = pos - start;
                for (size_t i = 0; i < delimiter.size(); ++i)
                    advance();
                return std::string_view(source).substr(start, length);
            }
            advance();
        }

        update_line_and_column();
        Error err = {"Raw string literal was never terminated.", current_mod_path, line, column};
        panic(err);
    }

    static std::string_view parse_char()
    {
        advance();
        auto start = pos;

        if (peek() == '\0' || peek() == '\n' || peek() == '\r')
        {
            update_line_and_column();
            Error err = {"Character literal was never terminated.", current_mod_path, line, column};
            panic(err);
        }
        if (peek() == '\\')
        {
            advance();
            if (peek() == '\0' || peek() == '\n' || peek() == '\r')
            {
                update_line_and_column();
                Error err = {"Character escape was never terminated.", current_mod_path, line, column};
                panic(err);
            }
            advance();
        }
        else
        {
            unsigned char c = peek();
            int len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
            for (int i = 0; i < len; i++)
                advance();
        }

        auto len = pos - start;
        if (peek() != '\'')
        {
            update_line_and_column();
            Error err = {"A character literal must only contain a single character.", current_mod_path, line, column};
            panic(err);
        }
        advance();

        return std::string_view(source).substr(start, len);
    }

    static std::pair<std::string_view, Operator> parse_operator()
    {
        for (const auto &pair : operator_lookup)
        {
            const std::string_view &op = pair.first;
            bool match = true;

            for (size_t i = 0; i < op.size(); i++)
            {
                if (peek(i) != op[i])
                {
                    match = false;
                    break;
                }
            }

            if (match)
            {
                for (size_t i = 0; i < op.size(); i++)
                    advance();
                return pair;
            }
        }

        return {"", Operator::PLUS};
    }

    static constexpr size_t maximum_macro_replacement_tokens = 128;
    static constexpr size_t maximum_expanded_tokens = 1'000'000;

    static bool is_using_declaration_token(const Token &token)
    {
        return token.type == TokenType::Keyword && std::get<Keyword>(token.value) == Keyword::USING;
    }

    static bool is_semicolon_token(const Token &token)
    {
        return token.type == TokenType::Operator &&
               std::get<Operator>(token.value) == Operator::SEMICOLON;
    }

    static bool is_right_arrow_token(const Token &token)
    {
        return token.type == TokenType::Operator &&
               std::get<Operator>(token.value) == Operator::RIGHT_ARROW;
    }

    struct FunctionMacro
    {
        std::vector<std::string> parameters;
        std::vector<Token> replacement;
    };

    struct MacroScope
    {
        std::unordered_map<std::string, std::vector<Token>> identifierRules;
        std::unordered_map<std::string, std::vector<Token>> stringRules;
        std::unordered_map<std::string, std::vector<FunctionMacro>> functionRules;
    };

    // Persists across tokenize() calls within one tokenize_modules() run so that
    // 'using global' declarations in one module are visible in later modules.
    static MacroScope globalMacroScope;

    static bool is_operator_token(const Token &token, Operator expected)
    {
        return token.type == TokenType::Operator && std::get<Operator>(token.value) == expected;
    }

    static const std::vector<Token> *find_macro(const Token &token,
                                                const std::vector<MacroScope> &scopes)
    {
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
        {
            if (token.type == TokenType::Identifier)
            {
                const auto found = scope->identifierRules.find(
                    std::string(std::get<std::string_view>(token.value)));
                if (found != scope->identifierRules.end())
                    return &found->second;
            }
            else if (token.type == TokenType::StringLiteral)
            {
                const auto found = scope->stringRules.find(
                    std::string(std::get<std::string_view>(token.value)));
                if (found != scope->stringRules.end())
                    return &found->second;
            }
        }
        return nullptr;
    }

    static const std::vector<FunctionMacro> *find_function_macros(const Token &token,
                                                                 const std::vector<MacroScope> &scopes)
    {
        if (token.type != TokenType::Identifier)
            return nullptr;
        const std::string name = std::string(std::get<std::string_view>(token.value));
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
        {
            const auto found = scope->functionRules.find(name);
            if (found != scope->functionRules.end())
                return &found->second;
        }
        return nullptr;
    }

    static bool is_unsafe_macro_replacement_token(const Token &token)
    {
        if (token.type != TokenType::Operator)
            return false;
        const Operator op = std::get<Operator>(token.value);
        return op == Operator::SEMICOLON || op == Operator::LEFT_BRACE || op == Operator::RIGHT_BRACE;
    }

    static size_t macro_parameter_index(const FunctionMacro &macro, const Token &token)
    {
        if (token.type != TokenType::Identifier)
            return macro.parameters.size();
        const std::string_view name = std::get<std::string_view>(token.value);
        for (size_t index = 0; index < macro.parameters.size(); ++index)
        {
            if (macro.parameters[index] == name)
                return index;
        }
        return macro.parameters.size();
    }

    static std::vector<Token> resolve_using_macros(const std::vector<Token> &tokens)
    {
        // scopes[0] is the persistent global scope, shared across all modules.
        // scopes[1] is private to this module and lexical blocks push further on top.
        std::vector<MacroScope> scopes;
        scopes.push_back(globalMacroScope);
        scopes.emplace_back();

        std::vector<Token> resolved;
        resolved.reserve(tokens.size());
        size_t expandedTokenCount = 0;

        for (size_t i = 0; i < tokens.size();)
        {
            if (!is_using_declaration_token(tokens[i]))
            {
                if (tokens[i].type == TokenType::ModuleBoundary)
                {
                    scopes.resize(2);
                    scopes[1] = MacroScope{};
                    ++i;
                    continue;
                }
                const Token &token = tokens[i];
                if (const std::vector<FunctionMacro> *candidates = find_function_macros(token, scopes);
                    candidates && i + 2 < tokens.size() &&
                    is_operator_token(tokens[i + 1], Operator::EXLAMATION_MARK) &&
                    is_operator_token(tokens[i + 2], Operator::LEFT_PAREN))
                {
                    size_t cursor = i + 3;
                    size_t nesting = 1;
                    std::vector<std::vector<Token>> arguments;
                    std::vector<Token> argument;
                    while (cursor < tokens.size() && nesting > 0)
                    {
                        const Token &argumentToken = tokens[cursor++];
                        if (is_operator_token(argumentToken, Operator::LEFT_PAREN))
                        {
                            ++nesting;
                            argument.push_back(argumentToken);
                        }
                        else if (is_operator_token(argumentToken, Operator::RIGHT_PAREN))
                        {
                            --nesting;
                            if (nesting > 0)
                                argument.push_back(argumentToken);
                        }
                        else if (nesting == 1 && is_operator_token(argumentToken, Operator::COMMA))
                        {
                            if (argument.empty())
                            {
                                update_line_and_column();
                                Error err = {"Function-like using macro arguments cannot be empty.", current_mod_path, line, column};
                                panic(err);
                            }
                            arguments.push_back(std::move(argument));
                            argument.clear();
                        }
                        else
                        {
                            argument.push_back(argumentToken);
                        }
                    }
                    if (nesting != 0)
                    {
                        update_line_and_column();
                        Error err = {"Function-like using macro invocation was never terminated.", current_mod_path, line, column};
                        panic(err);
                    }
                    if (!argument.empty())
                        arguments.push_back(std::move(argument));
                    const FunctionMacro *macro = nullptr;
                    for (const FunctionMacro &candidate : *candidates)
                    {
                        if (candidate.parameters.size() == arguments.size())
                        {
                            macro = &candidate;
                            break;
                        }
                    }
                    if (!macro)
                    {
                        update_line_and_column();
                        Error err = {"Function-like using macro argument count does not match any declaration.", current_mod_path, line, column};
                        panic(err);
                    }

                    std::vector<Token> expansion;
                    for (const Token &replacementToken : macro->replacement)
                    {
                        const size_t parameter = macro_parameter_index(*macro, replacementToken);
                        if (parameter < macro->parameters.size())
                            expansion.insert(expansion.end(), arguments[parameter].begin(), arguments[parameter].end());
                        else
                            expansion.push_back(replacementToken);
                    }
                    expandedTokenCount += expansion.size();
                    if (expandedTokenCount > maximum_expanded_tokens)
                    {
                        update_line_and_column();
                        Error err = {"Using macro expansion exceeds the token safety limit.", current_mod_path, line, column};
                        panic(err);
                    }
                    resolved.insert(resolved.end(), expansion.begin(), expansion.end());
                    i = cursor;
                    continue;
                }
                if (token.type == TokenType::Identifier && i + 2 < tokens.size() &&
                    is_operator_token(tokens[i + 1], Operator::EXLAMATION_MARK) &&
                    is_operator_token(tokens[i + 2], Operator::LEFT_PAREN))
                {
                    update_line_and_column();
                    Error err = {"Unknown using macro invocation '" + std::string(std::get<std::string_view>(token.value)) + "!'", current_mod_path, line, column};
                    panic(err);
                }

                ++i;
                if (const std::vector<Token> *replacement = find_macro(token, scopes))
                {
                    expandedTokenCount += replacement->size();
                    if (expandedTokenCount > maximum_expanded_tokens)
                    {
                        update_line_and_column();
                        Error err = {"Using macro expansion exceeds the token safety limit", current_mod_path, line, column};
                        panic(err);
                    }
                    resolved.insert(resolved.end(), replacement->begin(), replacement->end());
                }
                else
                {
                    resolved.push_back(token);
                }
                if (is_operator_token(token, Operator::LEFT_BRACE))
                    scopes.emplace_back();
                else if (is_operator_token(token, Operator::RIGHT_BRACE) && scopes.size() > 2)
                    scopes.pop_back();
                continue;
            }

            ++i;
            const bool isGlobal = i < tokens.size() && tokens[i].type == TokenType::Keyword &&
                                  std::get<Keyword>(tokens[i].value) == Keyword::GLOBAL;
            if (isGlobal)
                ++i;
            if (i >= tokens.size() ||
                (tokens[i].type != TokenType::Identifier && tokens[i].type != TokenType::StringLiteral))
                {
                    update_line_and_column();
                    Error err = {"Using macro requires an identifier or a string alias.", current_mod_path, line, column};
                    panic(err);
                }
            const Token alias = tokens[i++];
            const TokenType aliasType = alias.type;
            const std::string aliasKey = std::string(std::get<std::string_view>(alias.value));
            if (aliasType == TokenType::StringLiteral &&
                (aliasKey.empty() || aliasKey.back() != '!'))
                {
                    update_line_and_column();
                    Error err = {"String using macro aliases must end with '!'.", current_mod_path, line, column};
                    panic(err);
                }

            FunctionMacro functionMacro;
            const bool functionLike = aliasType == TokenType::Identifier && i < tokens.size() &&
                                      is_operator_token(tokens[i], Operator::EXLAMATION_MARK);
            if (functionLike)
            {
                ++i;
                if (i >= tokens.size() || !is_operator_token(tokens[i], Operator::LEFT_PAREN))
                {
                    update_line_and_column();
                    Error err = {"Function-like using macro requires '(' after '!'.", current_mod_path, line, column};
                    panic(err);
                }
                ++i;
                if (i < tokens.size() && !is_operator_token(tokens[i], Operator::RIGHT_PAREN))
                {
                    while (true)
                    {
                        if (i >= tokens.size() || tokens[i].type != TokenType::Identifier)
                        {
                            update_line_and_column();
                            Error err = {"Function-like using macro parameters must be identifiers.", current_mod_path, line, column};
                            panic(err);
                        }
                        const std::string parameter = std::string(std::get<std::string_view>(tokens[i++].value));
                        for (const std::string &existing : functionMacro.parameters)
                        {
                            if (existing == parameter)
                            {
                                update_line_and_column();
                                Error err = {"Duplicate function-like using macro parameter '" + parameter + "'.", current_mod_path, line, column};
                                panic(err);
                            }
                        }
                        functionMacro.parameters.push_back(parameter);
                        if (i < tokens.size() && is_operator_token(tokens[i], Operator::COMMA))
                        {
                            ++i;
                            continue;
                        }
                        break;
                    }
                }
                if (i >= tokens.size() || !is_operator_token(tokens[i], Operator::RIGHT_PAREN))
                {
                    update_line_and_column();
                    Error err = {"Function-like using macro requires ')' after its parameters", current_mod_path, line, column};
                    panic(err);
                }
                ++i;
            }
            if (i >= tokens.size() || !is_right_arrow_token(tokens[i]))
            {
                update_line_and_column();
                Error err = {"Using macro requires '->' after its alias.", current_mod_path, line, column};
                panic(err);
            }
            ++i;

            std::vector<Token> replacement;
            while (i < tokens.size() && !is_semicolon_token(tokens[i]))
            {
                if (is_using_declaration_token(tokens[i]))
                {
                    update_line_and_column();
                    Error err = {"Using macro replacements cannot declare macros.", current_mod_path, line, column};
                    panic(err);
                }
                if (is_unsafe_macro_replacement_token(tokens[i]))
                {
                    update_line_and_column();
                    Error err = {"Using macro replacements cannot contain ';', '{', or '}'.", current_mod_path, line, column};
                    panic(err);
                }
                if (const std::vector<Token> *expanded = find_macro(tokens[i], scopes))
                    replacement.insert(replacement.end(), expanded->begin(), expanded->end());
                else
                    replacement.push_back(tokens[i]);
                if (replacement.size() > maximum_macro_replacement_tokens)
                {
                    update_line_and_column();
                    Error err = {"Using macro replacement exceeds the token safety limit.", current_mod_path, line, column};
                    panic(err);
                }
                ++i;
            }
            if (i == tokens.size())
            {
                update_line_and_column();
                Error err = {"Using macro declaration was never terminated", current_mod_path, line, column};
                panic(err);
            }
            if (replacement.empty())
            {
                update_line_and_column();
                Error err = {"Using macro replacement cannot be empty.", current_mod_path, line, column};
                panic(err);
            }
            ++i;

            if (functionLike)
            {
                for (size_t replacementIndex = 0; replacementIndex + 1 < replacement.size(); ++replacementIndex)
                {
                    if (replacement[replacementIndex].type == TokenType::Identifier &&
                        std::string(std::get<std::string_view>(replacement[replacementIndex].value)) == aliasKey &&
                        is_operator_token(replacement[replacementIndex + 1], Operator::EXLAMATION_MARK))
                    {
                        update_line_and_column();
                        Error err = {"Recursive function-like using macro alias '" + aliasKey + "'", current_mod_path, line, column};
                        panic(err);
                    }
                }
                auto &overloads = (isGlobal ? scopes.front() : scopes.back()).functionRules[aliasKey];
                for (const FunctionMacro &existing : overloads)
                {
                    if (existing.parameters.size() == functionMacro.parameters.size())
                    {
                        update_line_and_column();
                        Error err = {"Duplicate function-like using macro alias '" + aliasKey + "' for this arity.", current_mod_path, line, column};
                        panic(err);
                    }
                }
                functionMacro.replacement = std::move(replacement);
                overloads.push_back(std::move(functionMacro));
                continue;
            }

            for (const Token &replacementToken : replacement)
            {
                if (replacementToken.type == aliasType &&
                    std::string(std::get<std::string_view>(replacementToken.value)) == aliasKey)
                {
                    update_line_and_column();
                    Error err = {"Recursive using macro alias '" + aliasKey + "'", current_mod_path, line, column};
                    panic(err);
                }
            }
            auto &rules = aliasType == TokenType::Identifier
                              ? (isGlobal ? scopes.front() : scopes.back()).identifierRules
                              : (isGlobal ? scopes.front() : scopes.back()).stringRules;
            if (rules.find(aliasKey) != rules.end())
            {
                update_line_and_column();
                Error err = {"Duplicate using macro alias '" + aliasKey + "'", current_mod_path, line, column};
                panic(err);
            }
            rules.emplace(aliasKey, std::move(replacement));
        }

        globalMacroScope = scopes.front();
        return resolved;
    }

    Token next_token()
    {
        skip_whitespace_and_comments();
        uint64_t start = pos;

        if (pos >= source.size())
        {
            return Token{TokenType::EndOfFile, source.length(), std::monostate{}};
        }

        char c = peek();

        if (is_alpha(c))
        {
            std::string_view word = parse_identifier();

            if (word == "true" || word == "false")
            {
                return Token{TokenType::BoolLiteral, start, word == "true", &source, &current_mod_path};
            }

            auto it = keyword_lookup.find(word);
            if (it != keyword_lookup.end())
            {
                if (it->second == Keyword::RAW)
                {
                    std::string_view literal = parse_raw_string();
                    return Token{TokenType::StringLiteral, start,
                                 literal, &source, &current_mod_path};
                }
                return Token{TokenType::Keyword, start, it->second, &source, &current_mod_path};
            }

            return Token{TokenType::Identifier, start, word, &source, &current_mod_path};
        }

        if (is_digit(c))
        {
            uint64_t integerValue = 0;
            double floatValue = 0.0;
            if (parse_full_number(integerValue, floatValue))
                return Token{TokenType::FloatLiteral, start, floatValue, &source, &current_mod_path};
            return Token{TokenType::IntegerLiteral, start, integerValue, &source, &current_mod_path};
        }

        if (c == '"')
        {
            std::string_view s = parse_string();
            return Token{TokenType::StringLiteral, start, s, &source, &current_mod_path};
        }

        if (c == '\'')
        {
            std::string_view ch = parse_char();
            return Token{TokenType::CharLiteral, start, ch, &source, &current_mod_path};
        }

        auto op_pair = parse_operator();
        if (!op_pair.first.empty())
        {
            return Token{TokenType::Operator, start, op_pair.second, &source, &current_mod_path};
        }

        update_line_and_column();
        Error err = {std::string("Unknown token '") + c + "'.", current_mod_path, line, column};
        panic(err);
    }

    static LexedModule tokenize_raw(const Module &mod)
    {
        modPathArchive.push_back(mod.path);
        const std::string &archivedModPath = modPathArchive.back();

        sourceArchive.push_back(mod.source);
        const std::string &archivedSource = sourceArchive.back();

        source = mod.source;
        current_mod_path = mod.path;
        line = 1;
        column = 1;
        pos = 0;

        constexpr size_t maximum_initial_token_capacity = 262'144;
        LexedModule module;
        module.path = mod.path;
        module.tokens.reserve(source.size() / 3 < maximum_initial_token_capacity ? source.size() / 3
                                                                           : maximum_initial_token_capacity);
        Token token = next_token();
        while (token.type != TokenType::EndOfFile)
        {
            module.tokens.push_back(token);
            token = next_token();
        }
        module.tokens.push_back(token);

        for (Token &fixup : module.tokens)
        {
            if (std::holds_alternative<std::string_view>(fixup.value))
            {
                const std::string_view &view = std::get<std::string_view>(fixup.value);
                const size_t offset = static_cast<size_t>(view.data() - source.data());
                fixup.value = std::string_view(archivedSource).substr(offset, view.size());
            }
            fixup.source = const_cast<std::string *>(&archivedSource);
            fixup.mod_path = const_cast<std::string *>(&archivedModPath);
        }

        return module;
    }

    static bool contains_using_macro(const std::vector<Token> &tokens)
    {
        for (const Token &token : tokens)
            if (is_using_declaration_token(token))
                return true;
        return false;
    }

    LexedModule tokenize(const Module &src)
    {
        LexedModule module = tokenize_raw(src);
        module.tokens = contains_using_macro(module.tokens) ? resolve_using_macros(module.tokens) : module.tokens;
        return module;
    }

    std::vector<LexedModule> tokenize_modules(const std::vector<Module> &modules)
    {
        globalMacroScope = MacroScope{};
        std::vector<LexedModule> lexed_modules;
        for (Module mod : modules)
        {
            current_mod_path = mod.path;
            lexed_modules.push_back(tokenize(mod));
        }
        return std::move(lexed_modules);
    }
} // namespace Lexer