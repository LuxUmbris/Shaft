#include "lexer.hpp"
#include "error.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

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
    static Configuration activeConfiguration;

    static std::string trim_directive(std::string_view text)
    {
        size_t first = 0;
        while (first < text.size() &&
               (text[first] == ' ' || text[first] == '\t' || text[first] == '\r' || text[first] == '\n'))
            ++first;
        size_t last = text.size();
        while (last > first &&
               (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r' || text[last - 1] == '\n'))
            --last;
        return std::string(text.substr(first, last - first));
    }

    static std::string preprocess_restricted_comptime_commands(const std::string &input)
    {
        std::istringstream lines(input);
        std::ostringstream output;
        std::string lineText;
        size_t lineNumber = 0;
        while (std::getline(lines, lineText))
        {
            ++lineNumber;
            const std::string directive = trim_directive(lineText);
            constexpr std::string_view prefix = "@comptime run ";
            if (directive.rfind(prefix, 0) != 0)
            {
                output << lineText << '\n';
                continue;
            }
            const std::string body = directive.substr(prefix.size());
            if (body.size() < 7 || body.substr(body.size() - 5) != " @end" || body.front() != '"')
                compiler_error("invalid @comptime run directive at line " + std::to_string(lineNumber));
            const size_t close = body.find('"', 1);
            if (close == std::string::npos || close + 6 != body.size() || body.substr(close) != "\" @end")
                compiler_error("@comptime run accepts exactly one executable path at line " +
                               std::to_string(lineNumber));
            const std::string command = body.substr(1, close - 1);
            std::error_code commandError;
            const std::string canonicalCommand = std::filesystem::canonical(command, commandError).string();
            if (commandError ||
                std::find(activeConfiguration.comptimeCommands.begin(), activeConfiguration.comptimeCommands.end(),
                          canonicalCommand) == activeConfiguration.comptimeCommands.end())
                compiler_error("@comptime run command is not allowlisted: " + command);
#if defined(__unix__) || defined(__APPLE__)
            const pid_t child = fork();
            if (child < 0)
                compiler_error("failed to start allowlisted @comptime command");
            if (child == 0)
            {
                const int nullDevice = open("/dev/null", O_RDWR);
                if (nullDevice >= 0)
                {
                    dup2(nullDevice, STDIN_FILENO);
                    dup2(nullDevice, STDOUT_FILENO);
                    dup2(nullDevice, STDERR_FILENO);
                    ::close(nullDevice);
                }
                char *const arguments[] = {const_cast<char *>(canonicalCommand.c_str()), nullptr};
                char *const environment[] = {const_cast<char *>("PATH=/usr/bin:/bin"), const_cast<char *>("LC_ALL=C"),
                                             nullptr};
                execve(canonicalCommand.c_str(), arguments, environment);
                _exit(127);
            }
            int status = 0;
            for (size_t waited = 0; waited < 500; ++waited)
            {
                const pid_t result = waitpid(child, &status, WNOHANG);
                if (result == child)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (waited == 499)
                {
                    kill(child, SIGKILL);
                    waitpid(child, &status, 0);
                    compiler_error("allowlisted @comptime command timed out after 5 seconds");
                }
            }
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                compiler_error("allowlisted @comptime command failed: " + command);
#else
            compiler_error("@comptime run is supported only on Unix compiler hosts");
#endif
            output << '\n';
        }
        return output.str();
    }

    [[noreturn]] static void config_macro_error(const std::string &path, size_t lineNumber, const std::string &message,
                                                bool is_macro_error = false)
    {
        Error err = {message, path, lineNumber, 0, is_macro_error};
        panic(err);
    }

    static std::string decode_config_value(std::string value, const std::string &path, size_t lineNumber)
    {
        value = trim_directive(value);
        if (value.empty())
            config_macro_error(path, lineNumber, "@config requires a value after '='");
        if (value.front() != '"')
            return value;
        if (value.size() < 2 || value.back() != '"')
            config_macro_error(path, lineNumber, "@config string values must be quoted and terminated");

        std::string decoded;
        for (size_t index = 1; index + 1 < value.size(); ++index)
        {
            if (value[index] != '\\')
            {
                decoded += value[index];
                continue;
            }
            if (++index + 1 >= value.size())
                config_macro_error(path, lineNumber, "@config string value has an incomplete escape");
            switch (value[index])
            {
            case '\\':
                decoded += '\\';
                break;
            case '"':
                decoded += '"';
                break;
            case 'n':
                decoded += '\n';
                break;
            case 't':
                decoded += '\t';
                break;
            default:
                config_macro_error(path, lineNumber, "@config string value has an unsupported escape");
            }
        }
        return decoded;
    }

    static size_t skip_blanks(const std::string &text, size_t index)
    {
        while (index < text.size() && (text[index] == ' ' || text[index] == '\t'))
            ++index;
        return index;
    }

    static std::string parse_quoted_value(const std::string &text, size_t &index, const std::string &path,
                                          size_t lineNumber)
    {
        if (index >= text.size() || text[index] != '"')
            config_macro_error(path, lineNumber, "@config expected a quoted string");
        const size_t start = index++;
        while (index < text.size() && text[index] != '"')
            index += text[index] == '\\' ? 2 : 1;
        if (index >= text.size())
            config_macro_error(path, lineNumber, "@config string value is not terminated");
        ++index;
        return decode_config_value(text.substr(start, index - start), path, lineNumber);
    }

    static bool parse_config_condition(std::string_view lineText, const std::string &path, size_t lineNumber,
                                       ConfigCondition &condition, bool &negated)
    {
        const std::string directive = trim_directive(lineText);
        size_t index;
        if (directive.rfind("@config.", 0) == 0)
        {
            negated = false;
            index = 8;
        }
        else if (directive.rfind("@!config.", 0) == 0)
        {
            negated = true;
            index = 9;
        }
        else
            return false;
        const std::string name = negated ? "@!config" : "@config";

        const size_t fieldStart = index;
        while (index < directive.size() && directive[index] != '=' && directive[index] != ' ' &&
               directive[index] != '\t')
            ++index;
        condition.field = directive.substr(fieldStart, index - fieldStart);
        const size_t dot = condition.field.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 == condition.field.size() ||
            condition.field.find('.', dot + 1) != std::string::npos)
            config_macro_error(path, lineNumber, name + " requires " + name + ".<build|package|format|meta>.<field>");
        const std::string table = condition.field.substr(0, dot);
        if (table != "build" && table != "package" && table != "format" && table != "meta")
            config_macro_error(path, lineNumber, name + " table must be 'build', 'package', 'format', or 'meta'");

        index = skip_blanks(directive, index);
        if (index < directive.size() && directive[index] == '=')
        {
            condition.kind = ConfigCondition::Kind::Equals;
            condition.values = {decode_config_value(directive.substr(index + 1), path, lineNumber)};
            return true;
        }

        const auto is_word = [&](std::string_view word)
        {
            if (directive.compare(index, word.size(), word) != 0)
                return false;
            const size_t after = index + word.size();
            return after < directive.size() && (directive[after] == ' ' || directive[after] == '\t' ||
                                                directive[after] == '[' || directive[after] == '"');
        };

        if (is_word("in"))
        {
            condition.kind = ConfigCondition::Kind::In;
            index = skip_blanks(directive, index + 2);
            if (index >= directive.size() || directive[index] != '[')
                config_macro_error(path, lineNumber, name + " 'in' requires a list like [\"a\", \"b\"]");
            ++index;
            while (true)
            {
                index = skip_blanks(directive, index);
                if (index < directive.size() && directive[index] == ']')
                    break; // empty list or trailing comma
                condition.values.push_back(parse_quoted_value(directive, index, path, lineNumber));
                index = skip_blanks(directive, index);
                if (index < directive.size() && directive[index] == ',')
                {
                    ++index;
                    continue;
                }
                if (index < directive.size() && directive[index] == ']')
                    break;
                config_macro_error(path, lineNumber, name + " expected ',' or ']' in list");
            }
            ++index; // ']'
            if (condition.values.empty())
                config_macro_error(path, lineNumber, name + " 'in' list cannot be empty");
        }
        else if (is_word("matches"))
        {
            condition.kind = ConfigCondition::Kind::Matches;
            index = skip_blanks(directive, index + 7);
            condition.values.push_back(parse_quoted_value(directive, index, path, lineNumber));
        }
        else
            config_macro_error(path, lineNumber, name + " requires '=', 'in' or 'matches' after the field");

        if (skip_blanks(directive, index) != directive.size())
            config_macro_error(path, lineNumber, name + " has unexpected text after the condition");
        return true;
    }

    static bool glob_match(std::string_view pattern, std::string_view text)
    {
        size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
        while (t < text.size())
        {
            if (p < pattern.size() && pattern[p] == '*')
            {
                star = p++;
                mark = t;
            }
            else if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t]))
            {
                ++p;
                ++t;
            }
            else if (star != std::string_view::npos)
            {
                p = star + 1;
                t = ++mark;
            }
            else
                return false;
        }
        while (p < pattern.size() && pattern[p] == '*')
            ++p;
        return p == pattern.size();
    }

    static bool condition_matches(const ConfigCondition &condition, const Configuration &configuration)
    {
        const auto configured = configuration.values.find(condition.field);
        if (configured == configuration.values.end())
            return false;
        const std::string &actual = configured->second;
        switch (condition.kind)
        {
        case ConfigCondition::Kind::Equals:
            return actual == condition.values.front();
        case ConfigCondition::Kind::In:
            return std::find(condition.values.begin(), condition.values.end(), actual) != condition.values.end();
        case ConfigCondition::Kind::Matches:
            return glob_match(condition.values.front(), actual);
        }
        return false;
    }

    static bool parse_negated_config_header(std::string_view lineText, const std::string &path, size_t lineNumber,
                                            std::string &field, std::string &value)
    {
        const std::string directive = trim_directive(lineText);
        constexpr std::string_view prefix = "@!config.";
        if (directive.rfind(prefix, 0) != 0)
            return false;
        const size_t equals = directive.find('=');
        if (equals == std::string::npos)
            config_macro_error(path, lineNumber, "@!config requires '= value'");
        field = trim_directive(std::string_view(directive).substr(prefix.size(), equals - prefix.size()));
        const size_t dot = field.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 == field.size() ||
            field.find('.', dot + 1) != std::string::npos)
            config_macro_error(path, lineNumber, "@!config requires @config.<build|package>.<field> = value");
        const std::string table = field.substr(0, dot);
        if (table != "build" && table != "package")
            config_macro_error(path, lineNumber, "@!config table must be 'build' or 'package'");
        value = decode_config_value(directive.substr(equals + 1), path, lineNumber);
        return true;
    }

    static bool is_error_macro(std::string_view lineText)
    {
        const std::string directive = trim_directive(lineText);
        constexpr std::string_view name = "@error";
        if (directive.rfind(name, 0) != 0)
            return false;
        return directive.size() == name.size() || directive[name.size()] == '(' ||
               std::isspace(static_cast<unsigned char>(directive[name.size()]));
    }

    static std::string parse_error_message(std::string_view lineText, const std::string &path, size_t lineNumber)
    {
        std::string rest = trim_directive(trim_directive(lineText).substr(6));
        if (!rest.empty() && rest.front() == '(')
        {
            if (rest.back() != ')')
                config_macro_error(path, lineNumber, "@error is missing a closing ')'");
            rest = trim_directive(std::string_view(rest).substr(1, rest.size() - 2));
        }
        if (rest.empty() || rest.front() != '"')
            config_macro_error(path, lineNumber, "@error requires a quoted message string");
        return decode_config_value(rest, path, lineNumber);
    }

    static bool is_asm_header(std::string_view lineText)
    {
        const std::string directive = trim_directive(lineText);
        if (directive.rfind("@asm", 0) != 0)
            return false;
        return directive.size() == 4 || directive[4] == '(' || std::isspace(static_cast<unsigned char>(directive[4]));
    }

    static bool is_comptime_header(std::string_view lineText) { return trim_directive(lineText) == "@comptime"; }

    static bool is_unsafe_header(std::string_view lineText) { return trim_directive(lineText) == "@unsafe"; }

    static bool is_nocheck_header(std::string_view lineText) { return trim_directive(lineText) == "@nocheck"; }

    static bool is_nocleanup_header(std::string_view lineText) { return trim_directive(lineText) == "@nocleanup"; }

    static bool is_end_directive(std::string_view lineText) { return trim_directive(lineText) == "@end"; }

    static std::string blank_directive_range(std::string_view text)
    {
        std::string blank;
        blank.reserve(text.size());
        for (char character : text)
            if (character == '\n' || character == '\r')
                blank += character;
        return blank;
    }

    static size_t line_end(const std::string &text, size_t start)
    {
        const size_t newline = text.find('\n', start);
        return newline == std::string::npos ? text.size() : newline + 1;
    }

    static size_t matching_macro_end(const std::string &text, size_t bodyStart, const std::string &path,
                                     size_t openingLine)
    {
        size_t cursor = bodyStart;
        size_t depth = 1;
        while (cursor < text.size())
        {
            const size_t end = line_end(text, cursor);
            const std::string_view lineText(text.data() + cursor, end - cursor);
            const std::string directive = trim_directive(lineText);
            if (directive.rfind("@config.", 0) == 0 || directive.rfind("@!config.", 0) == 0 ||
                is_asm_header(lineText) || is_comptime_header(lineText) || is_unsafe_header(lineText) ||
                is_nocheck_header(lineText) || is_nocleanup_header(lineText))
                ++depth;
            else if (is_end_directive(lineText) && --depth == 0)
                return cursor;
            cursor = end;
        }
        config_macro_error(path, openingLine, "macro block was never terminated with @end");
    }

    static std::string filter_config_blocks(const std::string &text, size_t begin, size_t limit,
                                            const std::string &path, size_t firstLine,
                                            const Configuration &configuration)
    {
        std::string filtered;
        size_t cursor = begin;
        size_t lineNumber = firstLine;
        while (cursor < limit)
        {
            const size_t end = line_end(text, cursor);
            const std::string_view lineText(text.data() + cursor, end - cursor);
            std::string field;
            std::string value;
            ConfigCondition condition;
            bool negated = false;
            if (parse_config_condition(lineText, path, lineNumber, condition, negated))
            {
                const size_t bodyStart = end;
                const size_t closeStart = matching_macro_end(text, bodyStart, path, lineNumber);
                const size_t closeEnd = line_end(text, closeStart);
                const bool matches = condition_matches(condition, configuration);
                const bool enabled = negated ? !matches : matches;
                filtered += blank_directive_range(lineText);
                if (enabled)
                {
                    size_t bodyFirstLine = lineNumber + 1;
                    filtered += filter_config_blocks(text, bodyStart, closeStart, path, bodyFirstLine, configuration);
                }
                else
                {
                    filtered +=
                        blank_directive_range(std::string_view(text.data() + bodyStart, closeStart - bodyStart));
                }
                filtered += blank_directive_range(std::string_view(text.data() + closeStart, closeEnd - closeStart));
                lineNumber += static_cast<size_t>(std::count(text.begin() + cursor, text.begin() + closeEnd, '\n'));
                cursor = closeEnd;
                continue;
            }
            if (is_end_directive(lineText))
            {
                filtered.append(text, cursor, end - cursor);
                lineNumber += static_cast<size_t>(std::count(text.begin() + cursor, text.begin() + end, '\n'));
                cursor = end;
                continue;
            }
            if (is_error_macro(lineText))
                config_macro_error(path, lineNumber,
                                   "\x1b[34m@error:\x1b[0m " + parse_error_message(lineText, path, lineNumber), true);
            if (is_asm_header(lineText))
            {
                const size_t closeStart = matching_macro_end(text, end, path, lineNumber);
                const size_t closeEnd = line_end(text, closeStart);
                filtered.append(text, cursor, closeEnd - cursor);
                lineNumber += static_cast<size_t>(std::count(text.begin() + cursor, text.begin() + closeEnd, '\n'));
                cursor = closeEnd;
                continue;
            }
            filtered.append(text, cursor, end - cursor);
            lineNumber += static_cast<size_t>(std::count(text.begin() + cursor, text.begin() + end, '\n'));
            cursor = end;
        }
        return filtered;
    }

    inline bool is_whitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
    inline bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

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
            else
                column++;
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

            while (is_digit(peek()) || (peek() >= 'a' && peek() <= 'f') || (peek() >= 'A' && peek() <= 'F'))
                advance();

            if (pos == digitsStart)
            {
                Error err = {"Hex literal requires at least one digit", current_mod_path, line, column};
                panic(err);
            }

            std::string_view num_view = std::string_view(source).substr(start + 2, pos - (start + 2));
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
                (pos == start || source[pos - 1] == '\n') && source.compare(pos, delimiter.size(), delimiter) == 0 &&
                (peek(delimiter.size()) == '\n' || peek(delimiter.size()) == '\r' || peek(delimiter.size()) == '\0');
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

    static constexpr size_t maximum_macro_replacement_tokens = 16'384;
    static constexpr size_t maximum_expanded_tokens = 1'000'000;

    static bool is_macro_using_declaration_token(const Token &token)
    {
        return token.type == TokenType::Keyword && std::get<Keyword>(token.value) == Keyword::META_USING;
    }

    static bool is_semicolon_token(const Token &token)
    {
        return token.type == TokenType::Operator && std::get<Operator>(token.value) == Operator::SEMICOLON;
    }

    static bool is_right_arrow_token(const Token &token)
    {
        return token.type == TokenType::Operator && std::get<Operator>(token.value) == Operator::RIGHT_ARROW;
    }

    enum class MacroFragment
    {
        Expr,
        Type,
        Ident,
        Literal,
        IntegerLiteral,
        FloatLiteral,
        StringLiteral,
        CharLiteral,
        BoolLiteral,
        TokenTree
    };

    struct MacroParameter
    {
        std::string name;
        MacroFragment fragment = MacroFragment::TokenTree;
        bool variadic = false;
    };

    struct FunctionMacro
    {
        std::vector<MacroParameter> parameters;
        std::vector<Token> replacement;
    };

    struct MacroScope
    {
        std::unordered_map<std::string, std::vector<Token>> identifierRules;
        std::unordered_map<std::string, std::vector<Token>> stringRules;
        std::unordered_map<std::string, std::vector<FunctionMacro>> functionRules;
    };

    static MacroScope globalMacroScope;

    static void append_macro_backtrace(std::vector<Token> &expansion, const Token &invocation,
                                       const std::string &macroName)
    {
        std::string trace = invocation.macroBacktrace;
        trace += "\nnote: expanded from @using macro '" + macroName + "!' invoked at ";
        trace += invocation.mod_path ? *invocation.mod_path : "<unknown>";
        if (invocation.source)
        {
            const ErrorPos origin = get_error_pos(invocation.start, invocation.source);
            trace += ":" + std::to_string(origin.line) + ":" + std::to_string(origin.column);
        }
        for (Token &token : expansion)
        {
            token.start = invocation.start;
            token.source = invocation.source;
            token.mod_path = invocation.mod_path;
            token.macroBacktrace = trace;
        }
    }

    static bool is_operator_token(const Token &token, Operator expected)
    {
        return token.type == TokenType::Operator && std::get<Operator>(token.value) == expected;
    }

    static bool is_namespace_using_declaration(const std::vector<Token> &tokens, size_t index, size_t &end)
    {
        size_t cursor = index + 1;
        if (cursor < tokens.size() && tokens[cursor].type == TokenType::Keyword &&
            std::get<Keyword>(tokens[cursor].value) == Keyword::NAMESPACE)
            ++cursor;
        if (cursor >= tokens.size() || tokens[cursor].type != TokenType::Identifier)
            return false;
        ++cursor;
        while (cursor + 1 < tokens.size() && is_operator_token(tokens[cursor], Operator::DOUBLE_COLON) &&
               tokens[cursor + 1].type == TokenType::Identifier)
            cursor += 2;
        if (cursor >= tokens.size() || !is_semicolon_token(tokens[cursor]))
            return false;
        end = cursor;
        return true;
    }

    static const std::vector<Token> *find_macro(const Token &token, const std::vector<MacroScope> &scopes)
    {
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
        {
            if (token.type == TokenType::Identifier)
            {
                const auto found = scope->identifierRules.find(std::string(std::get<std::string_view>(token.value)));
                if (found != scope->identifierRules.end())
                    return &found->second;
            }
            else if (token.type == TokenType::StringLiteral)
            {
                const auto found = scope->stringRules.find(std::string(std::get<std::string_view>(token.value)));
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
            if (macro.parameters[index].name == name)
                return index;
        }
        return macro.parameters.size();
    }

    static std::optional<MacroFragment> macro_fragment_name(const Token &token)
    {
        if (token.type == TokenType::Keyword)
        {
            const Keyword keyword = std::get<Keyword>(token.value);
            if (keyword == Keyword::BOOL)
                return MacroFragment::BoolLiteral;
            if (keyword == Keyword::CHAR)
                return MacroFragment::CharLiteral;
            return std::nullopt;
        }
        if (token.type != TokenType::Identifier)
            return std::nullopt;
        const std::string_view name = std::get<std::string_view>(token.value);
        if (name == "expr")
            return MacroFragment::Expr;
        if (name == "ty")
            return MacroFragment::Type;
        if (name == "ident")
            return MacroFragment::Ident;
        if (name == "literal")
            return MacroFragment::Literal;
        if (name == "int")
            return MacroFragment::IntegerLiteral;
        if (name == "float")
            return MacroFragment::FloatLiteral;
        if (name == "str")
            return MacroFragment::StringLiteral;
        if (name == "char")
            return MacroFragment::CharLiteral;
        if (name == "bool")
            return MacroFragment::BoolLiteral;
        if (name == "tt")
            return MacroFragment::TokenTree;
        return std::nullopt;
    }

    static bool macro_argument_matches(const std::vector<Token> &argument, MacroFragment fragment)
    {
        if (argument.empty())
            return false;
        if (fragment == MacroFragment::TokenTree)
            return true;
        if (fragment == MacroFragment::Ident)
            return argument.size() == 1 && argument.front().type == TokenType::Identifier;
        if (fragment == MacroFragment::Literal)
        {
            if (argument.size() != 1)
                return false;
            const TokenType type = argument.front().type;
            return type == TokenType::IntegerLiteral || type == TokenType::UnsignedIntegerLiteral ||
                   type == TokenType::FloatLiteral || type == TokenType::StringLiteral ||
                   type == TokenType::CharLiteral || type == TokenType::BoolLiteral;
        }
        if (fragment == MacroFragment::IntegerLiteral)
            return argument.size() == 1 && (argument.front().type == TokenType::IntegerLiteral ||
                                            argument.front().type == TokenType::UnsignedIntegerLiteral);
        if (fragment == MacroFragment::FloatLiteral)
            return argument.size() == 1 && argument.front().type == TokenType::FloatLiteral;
        if (fragment == MacroFragment::StringLiteral)
            return argument.size() == 1 && argument.front().type == TokenType::StringLiteral;
        if (fragment == MacroFragment::CharLiteral)
            return argument.size() == 1 && argument.front().type == TokenType::CharLiteral;
        if (fragment == MacroFragment::BoolLiteral)
            return argument.size() == 1 && argument.front().type == TokenType::BoolLiteral;
        if (fragment == MacroFragment::Expr)
        {
            for (const Token &token : argument)
                if (is_semicolon_token(token) || is_operator_token(token, Operator::LEFT_BRACE) ||
                    is_operator_token(token, Operator::RIGHT_BRACE))
                    return false;
            return true;
        }
        for (const Token &token : argument)
        {
            if (token.type == TokenType::Identifier || token.type == TokenType::Keyword)
                continue;
            if (token.type != TokenType::Operator)
                return false;
            const Operator op = std::get<Operator>(token.value);
            if (op != Operator::MULTIPLY && op != Operator::AMPERSAND && op != Operator::QUESTION_MARK &&
                op != Operator::LEFT_BRACKET && op != Operator::RIGHT_BRACKET && op != Operator::LESS_THAN &&
                op != Operator::GREATER_THAN && op != Operator::COMMA && op != Operator::DOUBLE_COLON)
                return false;
        }
        return true;
    }

    static std::deque<std::string> hygienicIdentifierArchive;
    static std::deque<std::string> comptimeIdentifierArchive;
    static uint64_t nextHygienicIdentifier = 0;

    static bool is_keyword_token(const Token &token, Keyword keyword)
    {
        return token.type == TokenType::Keyword && std::get<Keyword>(token.value) == keyword;
    }

    static void hygienically_rename_locals(std::vector<Token> &expansion)
    {
        std::vector<std::string> localNames;
        for (size_t index = 0; index < expansion.size(); ++index)
        {
            size_t nameIndex = expansion.size();
            if (is_keyword_token(expansion[index], Keyword::MUT) && index + 2 < expansion.size() &&
                expansion[index + 2].type == TokenType::Identifier)
            {
                nameIndex = index + 2;
            }
            else if (is_keyword_token(expansion[index], Keyword::RESERVE))
            {
                size_t typeIndex = index + 1;
                if (typeIndex < expansion.size() && is_keyword_token(expansion[typeIndex], Keyword::MUT))
                    ++typeIndex;
                for (size_t cursor = typeIndex; cursor < expansion.size(); ++cursor)
                {
                    if (expansion[cursor].type == TokenType::Operator)
                    {
                        const Operator op = std::get<Operator>(expansion[cursor].value);
                        if (op == Operator::ASSIGN || op == Operator::SEMICOLON)
                            break;
                    }
                    if (expansion[cursor].type == TokenType::Identifier)
                        nameIndex = cursor;
                }
            }
            if (nameIndex >= expansion.size())
                continue;
            const std::string original = std::string(std::get<std::string_view>(expansion[nameIndex].value));
            if (std::find(localNames.begin(), localNames.end(), original) == localNames.end())
                localNames.push_back(original);
        }

        for (const std::string &original : localNames)
        {
            hygienicIdentifierArchive.push_back("__shaft_macro_" + std::to_string(nextHygienicIdentifier++) + "_" +
                                                original);
            const std::string_view replacement = hygienicIdentifierArchive.back();
            for (Token &token : expansion)
            {
                if (token.type == TokenType::Identifier && std::get<std::string_view>(token.value) == original)
                    token.value = replacement;
            }
        }
    }

    static std::optional<bool> evaluate_comptime_token_condition(const std::vector<Token> &tokens, size_t begin,
                                                                 size_t end)
    {
        if (end == begin + 1 && tokens[begin].type == TokenType::BoolLiteral)
            return std::get<bool>(tokens[begin].value);
        if (end != begin + 3 || tokens[begin + 1].type != TokenType::Operator)
            return std::nullopt;

        const Token &left = tokens[begin];
        const Token &right = tokens[begin + 2];
        const Operator op = std::get<Operator>(tokens[begin + 1].value);
        const bool equality = op == Operator::EQUAL || op == Operator::NOT_EQUAL;
        if (left.type == TokenType::BoolLiteral && right.type == TokenType::BoolLiteral && equality)
        {
            const bool equal = std::get<bool>(left.value) == std::get<bool>(right.value);
            return op == Operator::EQUAL ? equal : !equal;
        }
        const bool integerLeft = left.type == TokenType::IntegerLiteral ||
                                 left.type == TokenType::UnsignedIntegerLiteral || left.type == TokenType::CharLiteral;
        const bool integerRight = right.type == TokenType::IntegerLiteral ||
                                  right.type == TokenType::UnsignedIntegerLiteral ||
                                  right.type == TokenType::CharLiteral;
        if (!integerLeft || !integerRight)
            return std::nullopt;
        const uint64_t leftValue = std::get<uint64_t>(left.value);
        const uint64_t rightValue = std::get<uint64_t>(right.value);
        switch (op)
        {
        case Operator::EQUAL:
            return leftValue == rightValue;
        case Operator::NOT_EQUAL:
            return leftValue != rightValue;
        case Operator::LESS_THAN:
            return leftValue < rightValue;
        case Operator::LESS_EQUAL:
            return leftValue <= rightValue;
        case Operator::GREATER_THAN:
            return leftValue > rightValue;
        case Operator::GREATER_EQUAL:
            return leftValue >= rightValue;
        default:
            return std::nullopt;
        }
    }

    static bool is_comptime_keyword(const Token &token, Keyword keyword)
    {
        return token.type == TokenType::Keyword && std::get<Keyword>(token.value) == keyword;
    }

    static std::optional<size_t> comptime_balanced_block_end(const std::vector<Token> &tokens, size_t open)
    {
        if (open >= tokens.size() || !is_operator_token(tokens[open], Operator::LEFT_BRACE))
            return std::nullopt;
        size_t cursor = open + 1;
        size_t nesting = 1;
        while (cursor < tokens.size() && nesting > 0)
        {
            if (is_operator_token(tokens[cursor], Operator::LEFT_BRACE))
                ++nesting;
            else if (is_operator_token(tokens[cursor], Operator::RIGHT_BRACE))
                --nesting;
            ++cursor;
        }
        if (nesting != 0)
            return std::nullopt;
        return cursor - 1;
    }

    static std::vector<Token> resolve_using_macros(const std::vector<Token> &inputTokens)
    {
        std::vector<Token> tokens = inputTokens;
        std::vector<MacroScope> scopes;
        scopes.push_back(globalMacroScope);
        scopes.emplace_back();

        std::vector<Token> resolved;
        resolved.reserve(tokens.size());
        size_t expandedTokenCount = 0;

        for (size_t i = 0; i < tokens.size();)
        {
            if (is_comptime_keyword(tokens[i], Keyword::META_COMPTIME) && i + 5 < tokens.size() &&
                tokens[i + 1].type == TokenType::Identifier &&
                std::get<std::string_view>(tokens[i + 1].value) == "ident" &&
                is_operator_token(tokens[i + 2], Operator::LEFT_PAREN))
            {
                size_t cursor = i + 3;
                std::string combined;
                const Token *origin = nullptr;
                bool requireIdentifier = true;
                while (cursor < tokens.size() && !is_operator_token(tokens[cursor], Operator::RIGHT_PAREN))
                {
                    if (requireIdentifier)
                    {
                        if (tokens[cursor].type != TokenType::Identifier)
                        {
                            update_line_and_column();
                            Error err = {"@comptime ident requires identifier fragments separated by commas.",
                                         current_mod_path, line, column};
                            panic(err);
                        }
                        if (!origin)
                            origin = &tokens[cursor];
                        combined += std::get<std::string_view>(tokens[cursor].value);
                        if (combined.size() > 4096)
                        {
                            update_line_and_column();
                            Error err = {"@comptime ident result exceeds the 4096-byte identifier limit.",
                                         current_mod_path, line, column};
                            panic(err);
                        }
                    }
                    else if (!is_operator_token(tokens[cursor], Operator::COMMA))
                    {
                        update_line_and_column();
                        Error err = {"@comptime ident requires commas between identifier fragments.", current_mod_path,
                                     line, column};
                        panic(err);
                    }
                    requireIdentifier = !requireIdentifier;
                    ++cursor;
                }
                if (!origin || requireIdentifier || cursor + 1 >= tokens.size() ||
                    !is_operator_token(tokens[cursor], Operator::RIGHT_PAREN) ||
                    !is_comptime_keyword(tokens[cursor + 1], Keyword::META_END))
                {
                    update_line_and_column();
                    Error err = {"@comptime ident requires '@comptime ident(part, ...) @end'.", current_mod_path, line,
                                 column};
                    panic(err);
                }
                comptimeIdentifierArchive.push_back(std::move(combined));
                Token identifier = *origin;
                identifier.type = TokenType::Identifier;
                identifier.value = std::string_view(comptimeIdentifierArchive.back());
                tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(i),
                             tokens.begin() + static_cast<std::ptrdiff_t>(cursor + 2));
                tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(i), std::move(identifier));
                continue;
            }
            if (is_comptime_keyword(tokens[i], Keyword::META_COMPTIME) && i + 5 < tokens.size() &&
                is_comptime_keyword(tokens[i + 1], Keyword::FOR) &&
                is_operator_token(tokens[i + 2], Operator::LEFT_PAREN) && tokens[i + 3].type == TokenType::Identifier &&
                std::get<std::string_view>(tokens[i + 3].value) == "tt" &&
                tokens[i + 4].type == TokenType::Identifier && tokens[i + 5].type == TokenType::Identifier &&
                std::get<std::string_view>(tokens[i + 5].value) == "in")
            {
                size_t cursor = i + 6;
                size_t parentheses = 0;
                size_t brackets = 0;
                size_t braces = 0;
                std::vector<std::vector<Token>> values;
                std::vector<Token> value;
                while (cursor < tokens.size())
                {
                    const Token &candidate = tokens[cursor];
                    if (is_operator_token(candidate, Operator::RIGHT_PAREN) && parentheses == 0 && brackets == 0 &&
                        braces == 0)
                        break;
                    if (is_operator_token(candidate, Operator::COMMA) && parentheses == 0 && brackets == 0 &&
                        braces == 0)
                    {
                        if (value.empty())
                        {
                            update_line_and_column();
                            Error err = {"@comptime tt for token-tree arguments cannot be empty.", current_mod_path,
                                         line, column};
                            panic(err);
                        }
                        values.push_back(std::move(value));
                        value.clear();
                        ++cursor;
                        continue;
                    }
                    if (is_operator_token(candidate, Operator::LEFT_PAREN))
                        ++parentheses;
                    else if (is_operator_token(candidate, Operator::RIGHT_PAREN))
                    {
                        if (parentheses == 0)
                        {
                            update_line_and_column();
                            Error err = {"Unbalanced ')' in @comptime tt for.", current_mod_path, line, column};
                            panic(err);
                        }
                        --parentheses;
                    }
                    else if (is_operator_token(candidate, Operator::LEFT_BRACKET))
                        ++brackets;
                    else if (is_operator_token(candidate, Operator::RIGHT_BRACKET))
                    {
                        if (brackets == 0)
                        {
                            update_line_and_column();
                            Error err = {"Unbalanced ']' in @comptime tt for.", current_mod_path, line, column};
                            panic(err);
                        }
                        --brackets;
                    }
                    else if (is_operator_token(candidate, Operator::LEFT_BRACE))
                        ++braces;
                    else if (is_operator_token(candidate, Operator::RIGHT_BRACE))
                    {
                        if (braces == 0)
                        {
                            update_line_and_column();
                            Error err = {"Unbalanced '}' in @comptime tt for.", current_mod_path, line, column};
                            panic(err);
                        }
                        --braces;
                    }
                    value.push_back(candidate);
                    ++cursor;
                }
                if (cursor >= tokens.size() || parentheses != 0 || brackets != 0 || braces != 0)
                {
                    update_line_and_column();
                    Error err = {"@comptime tt for requires a balanced '(tt name in values)' header.", current_mod_path,
                                 line, column};
                    panic(err);
                }
                if (!value.empty())
                    values.push_back(std::move(value));
                const std::optional<size_t> bodyEnd = comptime_balanced_block_end(tokens, cursor + 1);
                if (!bodyEnd || *bodyEnd + 1 >= tokens.size() ||
                    !is_comptime_keyword(tokens[*bodyEnd + 1], Keyword::META_END))
                {
                    update_line_and_column();
                    Error err = {"@comptime tt for requires a balanced '{ ... }' body followed by @end.",
                                 current_mod_path, line, column};
                    panic(err);
                }
                const std::string_view binding = std::get<std::string_view>(tokens[i + 4].value);
                std::vector<Token> emitted;
                for (const std::vector<Token> &tree : values)
                {
                    for (size_t body = cursor + 2; body < *bodyEnd; ++body)
                    {
                        if (is_operator_token(tokens[body], Operator::DOLLAR) && body + 1 < *bodyEnd &&
                            tokens[body + 1].type == TokenType::Identifier &&
                            std::get<std::string_view>(tokens[body + 1].value) == binding)
                        {
                            emitted.insert(emitted.end(), tree.begin(), tree.end());
                            ++body;
                        }
                        else
                            emitted.push_back(tokens[body]);
                        if (expandedTokenCount + emitted.size() > maximum_expanded_tokens)
                        {
                            update_line_and_column();
                            Error err = {"@comptime tt for token emission exceeds the token safety limit.",
                                         current_mod_path, line, column};
                            panic(err);
                        }
                    }
                }
                expandedTokenCount += emitted.size();
                tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(i),
                             tokens.begin() + static_cast<std::ptrdiff_t>(*bodyEnd + 2));
                tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(i), emitted.begin(), emitted.end());
                continue;
            }
            if (is_comptime_keyword(tokens[i], Keyword::META_COMPTIME) && i + 9 < tokens.size() &&
                is_comptime_keyword(tokens[i + 1], Keyword::FOR) &&
                is_operator_token(tokens[i + 2], Operator::LEFT_PAREN) && tokens[i + 3].type == TokenType::Identifier &&
                std::get<std::string_view>(tokens[i + 3].value) == "byte" &&
                tokens[i + 4].type == TokenType::Identifier && tokens[i + 5].type == TokenType::Identifier &&
                std::get<std::string_view>(tokens[i + 5].value) == "at" &&
                tokens[i + 6].type == TokenType::Identifier && tokens[i + 7].type == TokenType::Identifier &&
                std::get<std::string_view>(tokens[i + 7].value) == "in" &&
                tokens[i + 8].type == TokenType::StringLiteral &&
                is_operator_token(tokens[i + 9], Operator::RIGHT_PAREN))
            {
                const std::optional<size_t> bodyEnd = comptime_balanced_block_end(tokens, i + 10);
                if (!bodyEnd || *bodyEnd + 1 >= tokens.size() ||
                    !is_comptime_keyword(tokens[*bodyEnd + 1], Keyword::META_END))
                {
                    update_line_and_column();
                    Error err = {
                        "@comptime indexed byte for requires '(byte value at index in string-literal) { ... } @end'.",
                        current_mod_path, line, column};
                    panic(err);
                }
                const std::string_view byteBinding = std::get<std::string_view>(tokens[i + 4].value);
                const std::string_view indexBinding = std::get<std::string_view>(tokens[i + 6].value);
                const std::string_view bytes = std::get<std::string_view>(tokens[i + 8].value);
                const size_t bodySize = *bodyEnd - (i + 11);
                if (bytes.size() > maximum_expanded_tokens ||
                    (bodySize != 0 && bytes.size() > maximum_expanded_tokens / bodySize))
                {
                    update_line_and_column();
                    Error err = {"@comptime indexed byte for token emission exceeds the token safety limit.",
                                 current_mod_path, line, column};
                    panic(err);
                }
                std::vector<Token> emitted;
                emitted.reserve(bytes.size() * bodySize);
                for (size_t index = 0; index < bytes.size(); ++index)
                {
                    for (size_t body = i + 11; body < *bodyEnd; ++body)
                    {
                        if (is_operator_token(tokens[body], Operator::DOLLAR) && body + 1 < *bodyEnd &&
                            tokens[body + 1].type == TokenType::Identifier)
                        {
                            const std::string_view name = std::get<std::string_view>(tokens[body + 1].value);
                            if (name == byteBinding || name == indexBinding)
                            {
                                const uint64_t value =
                                    name == byteBinding
                                        ? static_cast<uint64_t>(static_cast<unsigned char>(bytes[index]))
                                        : static_cast<uint64_t>(index);
                                emitted.push_back({TokenType::IntegerLiteral, tokens[body].start, value,
                                                   tokens[body].source, tokens[body].mod_path});
                                ++body;
                                continue;
                            }
                        }
                        emitted.push_back(tokens[body]);
                    }
                }
                expandedTokenCount += emitted.size();
                if (expandedTokenCount > maximum_expanded_tokens)
                {
                    update_line_and_column();
                    Error err = {"@comptime token emission exceeds the token safety limit.", current_mod_path, line,
                                 column};
                    panic(err);
                }
                tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(i),
                             tokens.begin() + static_cast<std::ptrdiff_t>(*bodyEnd + 2));
                tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(i), emitted.begin(), emitted.end());
                continue;
            }
            if (is_comptime_keyword(tokens[i], Keyword::META_COMPTIME) && i + 7 < tokens.size() &&
                is_comptime_keyword(tokens[i + 1], Keyword::FOR) &&
                is_operator_token(tokens[i + 2], Operator::LEFT_PAREN) && tokens[i + 3].type == TokenType::Identifier &&
                std::get<std::string_view>(tokens[i + 3].value) == "byte" &&
                tokens[i + 4].type == TokenType::Identifier && tokens[i + 5].type == TokenType::Identifier &&
                std::get<std::string_view>(tokens[i + 5].value) == "in" &&
                tokens[i + 6].type == TokenType::StringLiteral &&
                is_operator_token(tokens[i + 7], Operator::RIGHT_PAREN))
            {
                const std::optional<size_t> bodyEnd = comptime_balanced_block_end(tokens, i + 8);
                if (!bodyEnd || *bodyEnd + 1 >= tokens.size() ||
                    !is_comptime_keyword(tokens[*bodyEnd + 1], Keyword::META_END))
                {
                    update_line_and_column();
                    Error err = {"@comptime byte for requires '(byte name in string-literal) { ... } @end'.",
                                 current_mod_path, line, column};
                    panic(err);
                }
                const std::string_view binding = std::get<std::string_view>(tokens[i + 4].value);
                const std::string_view bytes = std::get<std::string_view>(tokens[i + 6].value);
                const size_t bodySize = *bodyEnd - (i + 9);
                if (bytes.size() > maximum_expanded_tokens ||
                    (bodySize != 0 && bytes.size() > maximum_expanded_tokens / bodySize))
                {
                    update_line_and_column();
                    Error err = {"@comptime byte for token emission exceeds the token safety limit.", current_mod_path,
                                 line, column};
                    panic(err);
                }
                std::vector<Token> emitted;
                emitted.reserve(bytes.size() * bodySize);
                for (unsigned char byte : bytes)
                {
                    for (size_t body = i + 9; body < *bodyEnd; ++body)
                    {
                        if (is_operator_token(tokens[body], Operator::DOLLAR) && body + 1 < *bodyEnd &&
                            tokens[body + 1].type == TokenType::Identifier &&
                            std::get<std::string_view>(tokens[body + 1].value) == binding)
                        {
                            emitted.push_back({TokenType::IntegerLiteral, tokens[body].start,
                                               static_cast<uint64_t>(byte), tokens[body].source,
                                               tokens[body].mod_path});
                            ++body;
                        }
                        else
                            emitted.push_back(tokens[body]);
                    }
                }
                expandedTokenCount += emitted.size();
                if (expandedTokenCount > maximum_expanded_tokens)
                {
                    update_line_and_column();
                    Error err = {"@comptime token emission exceeds the token safety limit.", current_mod_path, line,
                                 column};
                    panic(err);
                }
                tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(i),
                             tokens.begin() + static_cast<std::ptrdiff_t>(*bodyEnd + 2));
                tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(i), emitted.begin(), emitted.end());
                continue;
            }
            if (is_comptime_keyword(tokens[i], Keyword::META_COMPTIME) && i + 4 < tokens.size() &&
                is_comptime_keyword(tokens[i + 1], Keyword::FOR) &&
                is_operator_token(tokens[i + 2], Operator::LEFT_PAREN) &&
                (tokens[i + 3].type == TokenType::IntegerLiteral ||
                 tokens[i + 3].type == TokenType::UnsignedIntegerLiteral) &&
                is_operator_token(tokens[i + 4], Operator::RIGHT_PAREN))
            {
                const std::optional<size_t> bodyEnd = comptime_balanced_block_end(tokens, i + 5);
                if (!bodyEnd || *bodyEnd + 1 >= tokens.size() ||
                    !is_comptime_keyword(tokens[*bodyEnd + 1], Keyword::META_END))
                {
                    update_line_and_column();
                    Error err = {"@comptime for requires '(non-negative-integer) { ... } @end'.", current_mod_path,
                                 line, column};
                    panic(err);
                }
                const uint64_t iterations = std::get<uint64_t>(tokens[i + 3].value);
                const size_t bodySize = *bodyEnd - (i + 6);
                if (iterations > maximum_expanded_tokens ||
                    (bodySize != 0 && iterations > maximum_expanded_tokens / bodySize))
                {
                    update_line_and_column();
                    Error err = {"@comptime for token emission exceeds the token safety limit.", current_mod_path, line,
                                 column};
                    panic(err);
                }
                std::vector<Token> emitted;
                emitted.reserve(static_cast<size_t>(iterations) * bodySize);
                for (uint64_t iteration = 0; iteration < iterations; ++iteration)
                    emitted.insert(emitted.end(), tokens.begin() + static_cast<std::ptrdiff_t>(i + 6),
                                   tokens.begin() + static_cast<std::ptrdiff_t>(*bodyEnd));
                expandedTokenCount += emitted.size();
                if (expandedTokenCount > maximum_expanded_tokens)
                {
                    update_line_and_column();
                    Error err = {"@comptime token emission exceeds the token safety limit.", current_mod_path, line,
                                 column};
                    panic(err);
                }
                tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(i),
                             tokens.begin() + static_cast<std::ptrdiff_t>(*bodyEnd + 2));
                tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(i), emitted.begin(), emitted.end());
                continue;
            }
            if (is_comptime_keyword(tokens[i], Keyword::META_COMPTIME) && i + 3 < tokens.size() &&
                is_comptime_keyword(tokens[i + 1], Keyword::IF) &&
                is_operator_token(tokens[i + 2], Operator::LEFT_PAREN))
            {
                size_t conditionEnd = i + 3;
                while (conditionEnd < tokens.size() && !is_operator_token(tokens[conditionEnd], Operator::RIGHT_PAREN))
                    ++conditionEnd;
                const std::optional<size_t> thenEnd =
                    conditionEnd < tokens.size() ? comptime_balanced_block_end(tokens, conditionEnd + 1) : std::nullopt;
                if (conditionEnd >= tokens.size() || !thenEnd)
                {
                    update_line_and_column();
                    Error err = {"@comptime if requires '(literal-condition) { ... }'.", current_mod_path, line,
                                 column};
                    panic(err);
                }
                const std::optional<bool> condition = evaluate_comptime_token_condition(tokens, i + 3, conditionEnd);
                if (!condition)
                {
                    update_line_and_column();
                    Error err = {"@comptime if accepts only literal boolean or integer comparisons.", current_mod_path,
                                 line, column};
                    panic(err);
                }
                size_t cursor = *thenEnd + 1;
                std::optional<size_t> elseOpen;
                std::optional<size_t> elseEnd;
                if (cursor < tokens.size() && is_comptime_keyword(tokens[cursor], Keyword::ELSE))
                {
                    elseOpen = cursor + 1;
                    elseEnd = comptime_balanced_block_end(tokens, *elseOpen);
                    if (!elseEnd)
                    {
                        update_line_and_column();
                        Error err = {"@comptime else requires a balanced '{ ... }' body.", current_mod_path, line,
                                     column};
                        panic(err);
                    }
                    cursor = *elseEnd + 1;
                }
                if (cursor >= tokens.size() || !is_comptime_keyword(tokens[cursor], Keyword::META_END))
                {
                    update_line_and_column();
                    Error err = {"@comptime if requires a closing @end.", current_mod_path, line, column};
                    panic(err);
                }
                const size_t selectedOpen = *condition ? conditionEnd + 1 : elseOpen.value_or(cursor);
                const size_t selectedEnd = *condition ? *thenEnd : elseEnd.value_or(cursor);
                std::vector<Token> emitted;
                if (selectedOpen != cursor)
                    emitted.assign(tokens.begin() + static_cast<std::ptrdiff_t>(selectedOpen + 1),
                                   tokens.begin() + static_cast<std::ptrdiff_t>(selectedEnd));
                expandedTokenCount += emitted.size();
                if (expandedTokenCount > maximum_expanded_tokens)
                {
                    update_line_and_column();
                    Error err = {"@comptime token emission exceeds the token safety limit.", current_mod_path, line,
                                 column};
                    panic(err);
                }
                tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(i),
                             tokens.begin() + static_cast<std::ptrdiff_t>(cursor + 1));
                tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(i), emitted.begin(), emitted.end());
                continue;
            }
            if (tokens[i].type == TokenType::Keyword && std::get<Keyword>(tokens[i].value) == Keyword::META_COMPTIME &&
                i + 2 < tokens.size() && tokens[i + 1].type == TokenType::Identifier &&
                std::get<std::string_view>(tokens[i + 1].value) == "emit" &&
                is_operator_token(tokens[i + 2], Operator::LEFT_BRACE))
            {
                size_t cursor = i + 3;
                size_t nesting = 1;
                while (cursor < tokens.size() && nesting > 0)
                {
                    if (is_operator_token(tokens[cursor], Operator::LEFT_BRACE))
                        ++nesting;
                    else if (is_operator_token(tokens[cursor], Operator::RIGHT_BRACE))
                        --nesting;
                    ++cursor;
                }
                if (nesting != 0 || cursor >= tokens.size() || tokens[cursor].type != TokenType::Keyword ||
                    std::get<Keyword>(tokens[cursor].value) != Keyword::META_END)
                {
                    update_line_and_column();
                    Error err = {"@comptime emit requires a balanced '{ ... }' body followed by @end.",
                                 current_mod_path, line, column};
                    panic(err);
                }
                std::vector<Token> emitted(tokens.begin() + static_cast<std::ptrdiff_t>(i + 3),
                                           tokens.begin() + static_cast<std::ptrdiff_t>(cursor - 1));
                expandedTokenCount += emitted.size();
                if (expandedTokenCount > maximum_expanded_tokens)
                {
                    update_line_and_column();
                    Error err = {"@comptime token emission exceeds the token safety limit.", current_mod_path, line,
                                 column};
                    panic(err);
                }
                tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(i),
                             tokens.begin() + static_cast<std::ptrdiff_t>(cursor + 1));
                tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(i), emitted.begin(), emitted.end());
                continue;
            }
            size_t usingEnd = 0;
            if (tokens[i].type == TokenType::Keyword && std::get<Keyword>(tokens[i].value) == Keyword::USING &&
                is_namespace_using_declaration(tokens, i, usingEnd))
            {
                resolved.insert(resolved.end(), tokens.begin() + i, tokens.begin() + usingEnd + 1);
                i = usingEnd + 1;
                continue;
            }
            if (!is_macro_using_declaration_token(tokens[i]))
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
                                Error err = {"Function-like @using macro arguments cannot be empty.", current_mod_path,
                                             line, column};
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
                        Error err = {"Function-like @using macro invocation was never terminated.", current_mod_path,
                                     line, column};
                        panic(err);
                    }
                    if (!argument.empty())
                        arguments.push_back(std::move(argument));
                    const FunctionMacro *macro = nullptr;
                    for (const FunctionMacro &candidate : *candidates)
                    {
                        const bool hasVariadicParameter =
                            !candidate.parameters.empty() && candidate.parameters.back().variadic;
                        const size_t fixedParameters =
                            hasVariadicParameter ? candidate.parameters.size() - 1 : candidate.parameters.size();
                        if (arguments.size() < fixedParameters ||
                            (!hasVariadicParameter && arguments.size() != fixedParameters))
                            continue;
                        bool fragmentsMatch = true;
                        for (size_t parameter = 0; parameter < fixedParameters; ++parameter)
                        {
                            if (!macro_argument_matches(arguments[parameter], candidate.parameters[parameter].fragment))
                            {
                                fragmentsMatch = false;
                                break;
                            }
                        }
                        if (fragmentsMatch)
                        {
                            macro = &candidate;
                            break;
                        }
                    }
                    if (!macro)
                    {
                        update_line_and_column();
                        Error err = {"Function-like @using macro argument count does not match any declaration.",
                                     current_mod_path, line, column};
                        panic(err);
                    }

                    std::vector<Token> templateExpansion = macro->replacement;
                    hygienically_rename_locals(templateExpansion);
                    std::vector<Token> expansion;
                    auto append_parameter = [&](size_t parameter, const Token &origin)
                    {
                        if (macro->parameters[parameter].variadic)
                        {
                            for (size_t argumentIndex = parameter; argumentIndex < arguments.size(); ++argumentIndex)
                            {
                                if (argumentIndex != parameter)
                                    expansion.push_back({TokenType::Operator, origin.start, Operator::COMMA,
                                                         origin.source, origin.mod_path});
                                expansion.insert(expansion.end(), arguments[argumentIndex].begin(),
                                                 arguments[argumentIndex].end());
                            }
                        }
                        else
                            expansion.insert(expansion.end(), arguments[parameter].begin(), arguments[parameter].end());
                    };
                    for (size_t replacementIndex = 0; replacementIndex < templateExpansion.size(); ++replacementIndex)
                    {
                        const Token &replacementToken = templateExpansion[replacementIndex];
                        if (is_operator_token(replacementToken, Operator::DOLLAR) &&
                            replacementIndex + 1 < templateExpansion.size())
                        {
                            const Token &name = templateExpansion[++replacementIndex];
                            const size_t parameter = macro_parameter_index(*macro, name);
                            if (parameter == macro->parameters.size())
                            {
                                expansion.push_back(replacementToken);
                                expansion.push_back(name);
                                continue;
                            }
                            append_parameter(parameter, replacementToken);
                            continue;
                        }
                        const size_t parameter = macro_parameter_index(*macro, replacementToken);
                        if (parameter < macro->parameters.size())
                            append_parameter(parameter, replacementToken);
                        else
                            expansion.push_back(replacementToken);
                    }
                    append_macro_backtrace(expansion, token, std::string(std::get<std::string_view>(token.value)));
                    expandedTokenCount += expansion.size();
                    if (expandedTokenCount > maximum_expanded_tokens)
                    {
                        update_line_and_column();
                        Error err = {"Using macro expansion exceeds the token safety limit.", current_mod_path, line,
                                     column};
                        panic(err);
                    }
                    if (cursor < tokens.size() && !expansion.empty() && tokens[cursor].macroBacktrace.empty())
                        tokens[cursor].macroBacktrace = expansion.front().macroBacktrace;
                    tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(cursor), expansion.begin(),
                                  expansion.end());
                    i = cursor;
                    continue;
                }
                if (token.type == TokenType::Identifier && i + 2 < tokens.size() &&
                    is_operator_token(tokens[i + 1], Operator::EXLAMATION_MARK) &&
                    is_operator_token(tokens[i + 2], Operator::LEFT_PAREN))
                {
                    update_line_and_column();
                    Error err = {"Unknown @using macro invocation '" +
                                     std::string(std::get<std::string_view>(token.value)) + "!'",
                                 current_mod_path, line, column};
                    panic(err);
                }

                ++i;
                if (const std::vector<Token> *replacement = find_macro(token, scopes))
                {
                    if (i >= tokens.size() || !is_operator_token(tokens[i], Operator::EXLAMATION_MARK) ||
                        (i + 1 < tokens.size() && is_operator_token(tokens[i + 1], Operator::LEFT_PAREN)))
                    {
                        update_line_and_column();
                        Error err = {"Using macro invocation '" + std::string(std::get<std::string_view>(token.value)) +
                                         "' requires '!'.",
                                     current_mod_path, line, column};
                        panic(err);
                    }
                    ++i;
                    std::vector<Token> expansion = *replacement;
                    hygienically_rename_locals(expansion);
                    append_macro_backtrace(expansion, token, std::string(std::get<std::string_view>(token.value)));
                    expandedTokenCount += expansion.size();
                    if (expandedTokenCount > maximum_expanded_tokens)
                    {
                        update_line_and_column();
                        Error err = {"Using macro expansion exceeds the token safety limit", current_mod_path, line,
                                     column};
                        panic(err);
                    }
                    if (i < tokens.size() && !expansion.empty() && tokens[i].macroBacktrace.empty())
                        tokens[i].macroBacktrace = expansion.front().macroBacktrace;
                    tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(i), expansion.begin(), expansion.end());
                    continue;
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
            if (aliasType == TokenType::StringLiteral && (aliasKey.empty() || aliasKey.back() != '!'))
            {
                update_line_and_column();
                Error err = {"String @using macro aliases must end with '!'.", current_mod_path, line, column};
                panic(err);
            }

            FunctionMacro functionMacro;
            const bool aliasHasBang = aliasType == TokenType::Identifier && i < tokens.size() &&
                                      is_operator_token(tokens[i], Operator::EXLAMATION_MARK);
            if (aliasType == TokenType::Identifier && !aliasHasBang)
            {
                update_line_and_column();
                Error err = {"Identifier @using macro aliases must end with '!'.", current_mod_path, line, column};
                panic(err);
            }
            const bool functionLike =
                aliasHasBang && i + 1 < tokens.size() && is_operator_token(tokens[i + 1], Operator::LEFT_PAREN);
            if (aliasHasBang)
                ++i;
            if (functionLike)
            {
                if (i >= tokens.size() || !is_operator_token(tokens[i], Operator::LEFT_PAREN))
                {
                    update_line_and_column();
                    Error err = {"Function-like @using macro requires '(' after '!'.", current_mod_path, line, column};
                    panic(err);
                }
                ++i;
                if (i < tokens.size() && !is_operator_token(tokens[i], Operator::RIGHT_PAREN))
                {
                    while (true)
                    {
                        if (i >= tokens.size() ||
                            (tokens[i].type != TokenType::Identifier &&
                             !is_operator_token(tokens[i], Operator::DOLLAR) && !macro_fragment_name(tokens[i])))
                        {
                            update_line_and_column();
                            Error err = {
                                "Function-like @using macro parameters must be identifiers or typed fragments.",
                                current_mod_path, line, column};
                            panic(err);
                        }
                        MacroParameter parameter;
                        if (i + 3 < tokens.size() && is_operator_token(tokens[i], Operator::DOLLAR) &&
                            macro_fragment_name(tokens[i + 1]) && is_operator_token(tokens[i + 2], Operator::COLON) &&
                            tokens[i + 3].type == TokenType::Identifier)
                        {
                            const std::optional<MacroFragment> fragment = macro_fragment_name(tokens[i + 1]);
                            if (!fragment)
                            {
                                update_line_and_column();
                                Error err = {"Unknown @using macro fragment after '$'.", current_mod_path, line,
                                             column};
                                panic(err);
                            }
                            parameter.fragment = *fragment;
                            parameter.name = std::string(std::get<std::string_view>(tokens[i + 3].value));
                            i += 4;
                            if (i < tokens.size() && is_operator_token(tokens[i], Operator::TRIPPLE_DOT))
                            {
                                if (parameter.fragment != MacroFragment::TokenTree)
                                {
                                    update_line_and_column();
                                    Error err = {"Only $tt:name... may be variadic in an @using macro.",
                                                 current_mod_path, line, column};
                                    panic(err);
                                }
                                parameter.variadic = true;
                                ++i;
                            }
                        }
                        else if (const std::optional<MacroFragment> fragment = macro_fragment_name(tokens[i]);
                                 fragment && i + 2 < tokens.size() &&
                                 is_operator_token(tokens[i + 1], Operator::TRIPPLE_DOT) &&
                                 tokens[i + 2].type == TokenType::Identifier)
                        {
                            if (*fragment != MacroFragment::TokenTree)
                            {
                                update_line_and_column();
                                Error err = {"Only tt... may be variadic in an @using macro.", current_mod_path, line,
                                             column};
                                panic(err);
                            }
                            parameter.fragment = *fragment;
                            parameter.variadic = true;
                            parameter.name = std::string(std::get<std::string_view>(tokens[i + 2].value));
                            i += 3;
                        }
                        else if (const std::optional<MacroFragment> fragment = macro_fragment_name(tokens[i]);
                                 fragment && i + 1 < tokens.size() && tokens[i + 1].type == TokenType::Identifier)
                        {
                            parameter.fragment = *fragment;
                            parameter.name = std::string(std::get<std::string_view>(tokens[i + 1].value));
                            i += 2;
                        }
                        else
                        {
                            parameter.name = std::string(std::get<std::string_view>(tokens[i].value));
                            ++i;
                        }
                        for (const MacroParameter &existing : functionMacro.parameters)
                        {
                            if (existing.name == parameter.name)
                            {
                                update_line_and_column();
                                Error err = {"Duplicate function-like @using macro parameter '" + parameter.name + "'.",
                                             current_mod_path, line, column};
                                panic(err);
                            }
                        }
                        functionMacro.parameters.push_back(std::move(parameter));
                        if (functionMacro.parameters.back().variadic && i < tokens.size() &&
                            is_operator_token(tokens[i], Operator::COMMA))
                        {
                            update_line_and_column();
                            Error err = {"Variadic tt... parameter must be last in an @using macro.", current_mod_path,
                                         line, column};
                            panic(err);
                        }
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
                    Error err = {"Function-like @using macro requires ')' after its parameters", current_mod_path, line,
                                 column};
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
            size_t delimiterDepth = 0;
            while (i < tokens.size())
            {
                const Token &replacementToken = tokens[i];
                if (is_operator_token(replacementToken, Operator::LEFT_PAREN) ||
                    is_operator_token(replacementToken, Operator::LEFT_BRACKET) ||
                    is_operator_token(replacementToken, Operator::LEFT_BRACE))
                    ++delimiterDepth;
                else if (is_operator_token(replacementToken, Operator::RIGHT_PAREN) ||
                         is_operator_token(replacementToken, Operator::RIGHT_BRACKET) ||
                         is_operator_token(replacementToken, Operator::RIGHT_BRACE))
                {
                    if (delimiterDepth == 0)
                    {
                        update_line_and_column();
                        Error err = {"Unbalanced delimiters in @using macro replacement.", current_mod_path, line,
                                     column};
                        panic(err);
                    }
                    --delimiterDepth;
                }
                if (is_semicolon_token(replacementToken) && delimiterDepth == 0)
                    break;
                if (is_macro_using_declaration_token(replacementToken))
                {
                    update_line_and_column();
                    Error err = {"Using macro replacements cannot declare macros.", current_mod_path, line, column};
                    panic(err);
                }
                if (const std::vector<Token> *expanded = find_macro(replacementToken, scopes))
                    replacement.insert(replacement.end(), expanded->begin(), expanded->end());
                else
                    replacement.push_back(replacementToken);
                if (replacement.size() > maximum_macro_replacement_tokens)
                {
                    update_line_and_column();
                    Error err = {"Using macro replacement exceeds the token safety limit.", current_mod_path, line,
                                 column};
                    panic(err);
                }
                ++i;
            }
            if (delimiterDepth != 0)
            {
                update_line_and_column();
                Error err = {"Unbalanced delimiters in @using macro replacement.", current_mod_path, line, column};
                panic(err);
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
                const bool mayConsumeVariadicArguments =
                    std::any_of(functionMacro.parameters.begin(), functionMacro.parameters.end(),
                                [](const MacroParameter &parameter) { return parameter.variadic; });
                for (size_t replacementIndex = 0; replacementIndex + 1 < replacement.size(); ++replacementIndex)
                {
                    if (!mayConsumeVariadicArguments && replacement[replacementIndex].type == TokenType::Identifier &&
                        std::string(std::get<std::string_view>(replacement[replacementIndex].value)) == aliasKey &&
                        is_operator_token(replacement[replacementIndex + 1], Operator::EXLAMATION_MARK))
                    {
                        update_line_and_column();
                        Error err = {"Recursive function-like @using macro alias '" + aliasKey + "'", current_mod_path,
                                     line, column};
                        panic(err);
                    }
                }
                auto &overloads = (isGlobal ? scopes.front() : scopes.back()).functionRules[aliasKey];
                for (const FunctionMacro &existing : overloads)
                {
                    if (existing.parameters.size() != functionMacro.parameters.size())
                        continue;
                    bool sameSignature = true;
                    for (size_t parameter = 0; parameter < existing.parameters.size(); ++parameter)
                    {
                        if (existing.parameters[parameter].fragment != functionMacro.parameters[parameter].fragment ||
                            existing.parameters[parameter].variadic != functionMacro.parameters[parameter].variadic)
                        {
                            sameSignature = false;
                            break;
                        }
                    }
                    if (sameSignature)
                    {
                        update_line_and_column();
                        Error err = {"Duplicate function-like @using macro alias '" + aliasKey +
                                         "' for this fragment signature.",
                                     current_mod_path, line, column};
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
                    Error err = {"Recursive @using macro alias '" + aliasKey + "'", current_mod_path, line, column};
                    panic(err);
                }
            }
            auto &rules = aliasType == TokenType::Identifier
                              ? (isGlobal ? scopes.front() : scopes.back()).identifierRules
                              : (isGlobal ? scopes.front() : scopes.back()).stringRules;
            if (rules.find(aliasKey) != rules.end())
            {
                update_line_and_column();
                Error err = {"Duplicate @using macro alias '" + aliasKey + "'", current_mod_path, line, column};
                panic(err);
            }
            rules.emplace(aliasKey, std::move(replacement));
        }

        globalMacroScope = scopes.front();
        return resolved;
    }

    static Token parse_inline_asm()
    {
        const uint64_t start = pos;
        const size_t headerEnd = line_end(source, pos);
        const size_t closeStart = matching_macro_end(source, headerEnd, current_mod_path, line);
        const size_t closeEnd = line_end(source, closeStart);
        pos = closeEnd;
        return Token{TokenType::InlineAsm, start, std::string_view(source).substr(start + 4, closeStart - (start + 4)),
                     &source, &current_mod_path};
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

        if (c == '@' && is_asm_header(std::string_view(source).substr(pos, line_end(source, pos) - pos)))
            return parse_inline_asm();

        if (c == '@' && is_alpha(peek(1)))
        {
            advance();
            parse_identifier();
            const std::string_view word = std::string_view(source).substr(start, pos - start);
            const auto keyword = keyword_lookup.find(word);
            if (keyword != keyword_lookup.end())
                return Token{TokenType::Keyword, start, keyword->second, &source, &current_mod_path};
            if (find_procedural_attribute(word.substr(1)))
                return Token{TokenType::Attribute, start, word.substr(1), &source, &current_mod_path};
            pos = start;
        }

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
                    return Token{TokenType::StringLiteral, start, literal, &source, &current_mod_path};
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
            if (is_macro_using_declaration_token(token))
                return true;
        return false;
    }

    struct ProceduralDerive
    {
        std::string name;
        std::string targetCapture;
        std::string quote;
    };

    static bool is_identifier_start(char value)
    {
        return std::isalpha(static_cast<unsigned char>(value)) || value == '_';
    }

    static bool is_identifier_continue(char value)
    {
        return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
    }

    static std::optional<std::string> parse_plain_identifier(std::string_view text, size_t &cursor)
    {
        while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
            ++cursor;
        if (cursor >= text.size() || !is_identifier_start(text[cursor]))
            return std::nullopt;
        const size_t start = cursor++;
        while (cursor < text.size() && is_identifier_continue(text[cursor]))
            ++cursor;
        return std::string(text.substr(start, cursor - start));
    }

    static std::string substitute_procedural_capture(std::string text, const std::string &capture,
                                                     const std::string &replacement)
    {
        const std::string needle = "$" + capture;
        size_t offset = 0;
        while ((offset = text.find(needle, offset)) != std::string::npos)
        {
            const size_t tail = offset + needle.size();
            if (tail == text.size() || !is_identifier_continue(text[tail]))
            {
                text.replace(offset, needle.size(), replacement);
                offset += replacement.size();
            }
            else
            {
                offset = tail;
            }
        }
        return text;
    }

    static std::unordered_map<std::string, ProceduralAttribute> proceduralAttributes;

    const ProceduralAttribute *find_procedural_attribute(std::string_view name)
    {
        const auto it = proceduralAttributes.find(std::string(name));
        return it == proceduralAttributes.end() ? nullptr : &it->second;
    }

    static void read_procedural_attribute(const std::string &header, std::istringstream &lines, size_t &lineNumber,
                                          std::string &output, const std::string &path)
    {
        auto fail = [&](const std::string &m) { config_macro_error(path, lineNumber, m, true); };
        auto skip = [&](size_t &c)
        {
            while (c < header.size() && (header[c] == ' ' || header[c] == '\t'))
                ++c;
        };
        auto expect = [&](size_t &c, char ch)
        {
            skip(c);
            if (c >= header.size() || header[c] != ch)
                fail(std::string("@procedural attribute: expected '") + ch + "'.");
            ++c;
        };
        auto capture = [&](size_t &c)
        {
            expect(c, '$');
            auto kind = parse_plain_identifier(header, c);
            if (!kind)
                fail("@procedural attribute: expected a kind after '$'.");
            expect(c, ':');
            auto name = parse_plain_identifier(header, c);
            if (!name)
                fail("@procedural attribute: expected a capture name.");
            return std::make_pair(*kind, *name);
        };

        output += "\n"; // header line, keeps line numbers aligned
        ProceduralAttribute attr;
        size_t cursor = std::string_view("@procedural attribute").size();
        auto name = parse_plain_identifier(header, cursor);
        if (!name)
            fail("@procedural attribute requires a name.");
        attr.name = *name;
        for (const char *reserved :
             {"quote", "prepend", "original", "for", "if", "reject", "config", "error", "asm", "procedural", "derive"})
            if (attr.name == reserved)
                fail("'" + attr.name + "' is a reserved directive name.");
        if (attr.name.rfind("derive", 0) == 0 || keyword_lookup.count(std::string("@") + attr.name))
            fail("'@" + attr.name + "' collides with an existing directive.");

        expect(cursor, '(');
        skip(cursor);
        if (cursor < header.size() && header[cursor] != ')')
            while (true)
            {
                auto [kind, cap] = capture(cursor);
                if (kind != "string" && kind != "int" && kind != "ident" && kind != "bool")
                    fail("attribute parameter kind must be string, int, ident or bool.");
                attr.params.emplace_back(kind, cap);
                skip(cursor);
                if (cursor < header.size() && header[cursor] == ',')
                {
                    ++cursor;
                    continue;
                }
                break;
            }
        expect(cursor, ')');
        auto forWord = parse_plain_identifier(header, cursor);
        if (!forWord || *forWord != "for")
            fail("expected `for $kind:target` after the parameter list.");
        auto [targetKind, targetCap] = capture(cursor);
        if (targetKind != "function" && targetKind != "struct" && targetKind != "union" && targetKind != "enum" &&
            targetKind != "variable")
            fail("attribute target must be function, struct, union, enum or variable.");
        skip(cursor);
        if (cursor != header.size())
            fail("unexpected text after the attribute header.");
        attr.targetKind = targetKind;
        attr.targetCapture = targetCap;

        std::string line;
        std::string *section = nullptr;
        int depth = 0;
        bool closed = false;
        while (std::getline(lines, line))
        {
            ++lineNumber;
            output += "\n";
            const std::string t = trim_directive(line);
            if (!section)
            {
                if (t == "@quote")
                    section = &attr.quote;
                else if (t == "@prepend")
                    section = &attr.prepend;
                else if (t == "@end")
                {
                    closed = true;
                    break;
                }
                else if (!t.empty())
                    fail("expected @quote, @prepend or @end inside @procedural attribute.");
                continue;
            }
            if (t.rfind("@for ", 0) == 0 || t.rfind("@if ", 0) == 0)
                ++depth;
            else if (t == "@end")
            {
                if (depth == 0)
                {
                    section = nullptr;
                    continue;
                }
                --depth;
            }
            *section += line + "\n";
        }
        if (!closed || section)
            fail("@procedural attribute was never terminated with @end.");
        if (attr.quote.empty() && attr.prepend.empty())
            fail("@procedural attribute needs @quote or @prepend.");
        if (!attr.prepend.empty() && attr.targetKind != "function")
            fail("@prepend is only valid for function targets.");
        if (proceduralAttributes.count(attr.name))
            fail("duplicate @procedural attribute '" + attr.name + "'.");
        proceduralAttributes.emplace(attr.name, std::move(attr));
    }

    static std::string preprocess_procedural_derives(const std::string &input, const std::string &path)
    {
        std::vector<ProceduralDerive> derives;
        std::istringstream lines(input);
        std::string output;
        std::string lineText;
        std::vector<std::string> pendingDerives;
        std::string pendingTarget;
        int structBraces = 0;
        bool structOpened = false;
        size_t lineNumber = 0;

        auto find_derive = [&](const std::string &name) -> const ProceduralDerive *
        {
            for (const ProceduralDerive &derive : derives)
                if (derive.name == name)
                    return &derive;
            return nullptr;
        };
        auto append_line = [&](const std::string &text) { output += text + "\n"; };
        auto blank_line = [&]() { output += "\n"; };
        auto opens_end_block = [](const std::string &directive)
        {
            for (const std::string_view prefix :
                 {"@for", "@if", "@comptime", "@unsafe", "@nocheck", "@nocleanup", "@asm", "@config"})
                if (directive.rfind(prefix, 0) == 0 &&
                    (directive.size() == prefix.size() || !is_identifier_continue(directive[prefix.size()])))
                    return true;
            return false;
        };

        while (std::getline(lines, lineText))
        {
            ++lineNumber;
            const std::string trimmed = trim_directive(lineText);
            constexpr std::string_view attributePrefix = "@procedural attribute";
            if (trimmed.rfind(attributePrefix, 0) == 0 &&
                (trimmed.size() == attributePrefix.size() || !is_identifier_continue(trimmed[attributePrefix.size()])))
            {
                read_procedural_attribute(trimmed, lines, lineNumber, output, path);
                continue;
            }
            constexpr std::string_view proceduralPrefix = "@procedural derive";
            if (trimmed.rfind(proceduralPrefix, 0) == 0)
            {
                size_t cursor = proceduralPrefix.size();
                const std::optional<std::string> name = parse_plain_identifier(trimmed, cursor);
                if (!name || cursor >= trimmed.size() || trimmed[cursor++] != '(' || cursor >= trimmed.size() ||
                    trimmed[cursor++] != '$')
                    config_macro_error(path, lineNumber,
                                       "@procedural derive requires `@procedural derive name($struct:target)`.", true);
                const std::optional<std::string> targetKind = parse_plain_identifier(trimmed, cursor);
                if (!targetKind || *targetKind != "struct" || cursor >= trimmed.size() || trimmed[cursor++] != ':')
                    config_macro_error(path, lineNumber,
                                       "@procedural derive currently accepts only a `$struct:target` parameter.", true);
                const std::optional<std::string> targetCapture = parse_plain_identifier(trimmed, cursor);
                if (!targetCapture || cursor >= trimmed.size() || trimmed[cursor++] != ')' ||
                    !trim_directive(std::string_view(trimmed).substr(cursor)).empty())
                    config_macro_error(path, lineNumber,
                                       "@procedural derive requires one closed `$struct:target` parameter.", true);

                blank_line();
                if (!std::getline(lines, lineText))
                    config_macro_error(path, lineNumber, "@procedural derive requires an @quote body.", true);
                ++lineNumber;
                if (trim_directive(lineText) != "@quote")
                    config_macro_error(path, lineNumber,
                                       "@procedural derive requires @quote immediately after its header.", true);
                blank_line();

                std::string quote;
                int quoteDepth = 0;
                bool quoteClosed = false;
                while (std::getline(lines, lineText))
                {
                    ++lineNumber;
                    const std::string quoteDirective = trim_directive(lineText);
                    if (quoteDirective == "@end")
                    {
                        if (quoteDepth == 0)
                        {
                            quoteClosed = true;
                            blank_line();
                            break;
                        }
                        --quoteDepth;
                    }
                    else if (opens_end_block(quoteDirective))
                    {
                        ++quoteDepth;
                    }
                    quote += lineText + "\n";
                    blank_line();
                }
                if (!quoteClosed || !std::getline(lines, lineText))
                    config_macro_error(path, lineNumber, "@procedural derive @quote was never terminated with @end.",
                                       true);
                ++lineNumber;
                if (trim_directive(lineText) != "@end")
                    config_macro_error(path, lineNumber, "@procedural derive requires a final @end.", true);
                blank_line();
                if (find_derive(*name))
                    config_macro_error(path, lineNumber, "duplicate @procedural derive '" + *name + "'.", true);
                derives.push_back(ProceduralDerive{*name, *targetCapture, std::move(quote)});
                continue;
            }

            constexpr std::string_view derivePrefix = "@derive";
            if (trimmed.rfind(derivePrefix, 0) == 0 &&
                (trimmed.size() == derivePrefix.size() || !is_identifier_continue(trimmed[derivePrefix.size()])))
            {
                if (!pendingDerives.empty())
                    config_macro_error(path, lineNumber, "@derive must be followed by one struct declaration.", true);
                size_t cursor = derivePrefix.size();
                std::vector<std::string> names;
                while (true)
                {
                    const std::optional<std::string> name = parse_plain_identifier(trimmed, cursor);
                    if (!name)
                        config_macro_error(path, lineNumber, "@derive requires one or more procedural derive names.",
                                           true);
                    if (!find_derive(*name))
                        config_macro_error(path, lineNumber, "unknown @procedural derive '" + *name + "'.", true);
                    if (std::find(names.begin(), names.end(), *name) != names.end())
                        config_macro_error(path, lineNumber, "duplicate @derive name '" + *name + "'.", true);
                    names.push_back(*name);
                    while (cursor < trimmed.size() && (trimmed[cursor] == ' ' || trimmed[cursor] == '\t'))
                        ++cursor;
                    if (cursor == trimmed.size())
                        break;
                    if (trimmed[cursor++] != ',')
                        config_macro_error(path, lineNumber, "@derive names must be separated by commas.", true);
                }
                pendingDerives = std::move(names);
                blank_line();
                continue;
            }

            append_line(lineText);
            if (pendingDerives.empty())
                continue;

            if (pendingTarget.empty())
            {
                if (trimmed.empty())
                    continue;
                constexpr std::string_view structPrefix = "struct";
                if (trimmed.rfind(structPrefix, 0) != 0 ||
                    (trimmed.size() > structPrefix.size() && is_identifier_continue(trimmed[structPrefix.size()])))
                    config_macro_error(path, lineNumber,
                                       "@derive currently applies only to the immediately following struct.", true);
                size_t cursor = structPrefix.size();
                const std::optional<std::string> target = parse_plain_identifier(trimmed, cursor);
                if (!target)
                    config_macro_error(path, lineNumber, "derived struct declarations require a name.", true);
                pendingTarget = *target;
                structBraces = 0;
                structOpened = false;
            }
            for (char value : lineText)
            {
                if (value == '{')
                {
                    structOpened = true;
                    ++structBraces;
                }
                else if (value == '}')
                    --structBraces;
            }
            if (structBraces < 0)
                config_macro_error(path, lineNumber, "derived struct has an unmatched closing brace.", true);
            if (structOpened && structBraces == 0)
            {
                for (const std::string &name : pendingDerives)
                {
                    const ProceduralDerive *derive = find_derive(name);
                    output += substitute_procedural_capture(derive->quote, derive->targetCapture, pendingTarget);
                }
                pendingDerives.clear();
                pendingTarget.clear();
            }
        }
        if (!pendingDerives.empty())
            config_macro_error(path, lineNumber, "@derive was not followed by a complete struct declaration.", true);
        return output;
    }

    LexedModule tokenize(const Module &src)
    {
        Module filtered = src;
        filtered.source = preprocess_config_blocks(src, activeConfiguration);
        filtered.source = preprocess_restricted_comptime_commands(filtered.source);
        filtered.source = preprocess_procedural_derives(filtered.source, filtered.path);
        LexedModule module = tokenize_raw(filtered);
        module.tokens = (contains_using_macro(module.tokens) || !globalMacroScope.identifierRules.empty() ||
                         !globalMacroScope.stringRules.empty() || !globalMacroScope.functionRules.empty())
                            ? resolve_using_macros(module.tokens)
                            : module.tokens;
        return module;
    }

    std::vector<LexedModule> tokenize_modules(const std::vector<Module> &modules, const Configuration &configuration)
    {
        activeConfiguration = configuration;
        globalMacroScope = MacroScope{};
        std::vector<LexedModule> lexed_modules;
        for (Module mod : modules)
        {
            current_mod_path = mod.path;
            lexed_modules.push_back(tokenize(mod));
        }
        return std::move(lexed_modules);
    }

    std::string preprocess_config_blocks(const Module &module, const Configuration &configuration)
    {
        return filter_config_blocks(module.source, 0, module.source.size(), module.path, 1, configuration);
    }
} // namespace Lexer