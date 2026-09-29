#include "error.hpp"
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>

static std::string *error_source = nullptr;
static std::vector<std::unique_ptr<std::string>> error_source_buffer;
bool global_stop_on_error = false;
ErrorPos get_error_pos(uint64_t byte_pos, std::string *source)
{
    uint64_t line = 1;
    uint64_t column = 1;

    if (!source)
    {
        error_source = nullptr;
        return {line, column};
    }

    const uint64_t bounded_byte_pos = std::min<uint64_t>(byte_pos, source->size());
    for (uint64_t i = 0; i < bounded_byte_pos; i++)
    {
        if ((*source)[i] == '\n')
        {
            line++;
            column = 1;
        }
        else
            column++;
    }
    error_source = source;
    error_source_buffer.push_back(std::make_unique<std::string>(*source));
    return {line, column};
}

static std::vector<Error> error_buffer;
static bool declared_error_buffer_flush = false;

const char *RED = "\033[1;31m";
const char *BLUE = "\033[1;34m";
const char *RESET = "\033[0m";

void flush_error_buffer()
{
    for (size_t i = 0; i < error_buffer.size(); ++i)
    {
        std::cerr << RED << "error: " << RESET << error_buffer[i].message << '\n'
                  << BLUE << "--> " << RESET << error_buffer[i].modulePath << ':' << error_buffer[i].line << ':'
                  << error_buffer[i].column << '\n';

        if (i >= error_source_buffer.size() || !error_source_buffer[i])
        {
            continue;
        }

        std::string_view source(*(error_source_buffer[i]));

        size_t line_start = 0;
        uint64_t current_line = 1;

        while (current_line < error_buffer[i].line && line_start < source.length())
        {
            size_t next_newline = source.find('\n', line_start);
            if (next_newline == std::string_view::npos)
                break;
            line_start = next_newline + 1;
            current_line++;
        }

        size_t line_end = source.find('\n', line_start);
        if (line_end == std::string_view::npos)
        {
            line_end = source.length();
        }

        std::string_view line_content = source.substr(line_start, line_end - line_start);

        std::string line_str = std::to_string(error_buffer[i].line);
        size_t margin_width = line_str.length();
        std::string margin(margin_width, ' ');

        std::cerr << BLUE << margin << " |\n"
                  << error_buffer[i].line << " | " << RESET << line_content << '\n'
                  << BLUE << margin << " | " << RED;

        for (uint64_t j = 0; j < error_buffer[i].column - 1 && j < line_content.length(); ++j)
        {
            if (line_content[j] == '\t')
            {
                std::cerr << '\t';
            }
            else
            {
                std::cerr << ' ';
            }
        }
        std::cerr << '^' << RESET << "\n\n";
    }
}

void panic(Error error)
{
    if (!error.is_macro_error && global_stop_on_error)
    {
        std::cerr << RED << "error: " << RESET << error.message << '\n'
                  << BLUE << "--> " << RESET << error.modulePath << ':' << error.line << ':' << error.column << '\n';
    }
    else if (error.is_macro_error)
    {
        std::cerr << BLUE << "@error: " << RESET << error.message << '\n'
                  << BLUE << "--> " << RESET << error.modulePath << ':' << error.line << ':' << error.column << '\n';
    }
    else
    {
        error_buffer.push_back(error);
    }

    if (global_stop_on_error)
    {
        if (!error_source)
        {
            exit(1);
        }

        std::string_view source(*error_source);

        size_t line_start = 0;
        uint64_t current_line = 1;

        while (current_line < error.line && line_start < source.length())
        {
            size_t next_newline = source.find('\n', line_start);
            if (next_newline == std::string_view::npos)
                break;
            line_start = next_newline + 1;
            current_line++;
        }

        size_t line_end = source.find('\n', line_start);
        if (line_end == std::string_view::npos)
        {
            line_end = source.length();
        }

        std::string_view line_content = source.substr(line_start, line_end - line_start);

        std::string line_str = std::to_string(error.line);
        size_t margin_width = line_str.length();
        std::string margin(margin_width, ' ');

        std::cerr << BLUE << margin << " |\n"
                  << error.line << " | " << RESET << line_content << '\n'
                  << BLUE << margin << " | " << RED;

        for (uint64_t i = 0; i < error.column - 1 && i < line_content.length(); ++i)
        {
            if (line_content[i] == '\t')
            {
                std::cerr << '\t';
            }
            else
            {
                std::cerr << ' ';
            }
        }
        std::cerr << '^' << RESET << "\n\n";

        exit(1);
    }
    else if (!declared_error_buffer_flush)
    {
        declared_error_buffer_flush = true;
        std::atexit(flush_error_buffer);
    }
    exit(1);
}

void panic_at_source(std::string message, const std::string &modulePath, uint64_t byte_pos, std::string *source)
{
    const ErrorPos position = get_error_pos(byte_pos, source);
    panic({std::move(message), modulePath, position.line, position.column});
}