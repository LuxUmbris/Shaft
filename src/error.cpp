#include "error.hpp"
#include <cstdlib>

static std::string* error_source = nullptr;
ErrorPos get_error_pos(uint64_t byte_pos, std::string* source)
{
    uint64_t line = 1;
    uint64_t column = 1;

    for (uint64_t i = 0; i < byte_pos; i++)
    {
        if ((*source)[i] == '\n')
        {
            line++;
            column = 1;
        }
        else column++;
    }
    error_source = source;
    return {line, column};
}

void panic(Error error)
{
    const char* RED   = "\033[1;31m";
    const char* BLUE  = "\033[1;34m";
    const char* RESET = "\033[0m";

    std::cerr << RED << "error: " << RESET << error.message << '\n'
              << BLUE << "--> " << RESET << error.modulePath << ':' 
              << error.line << ':' << error.column << '\n';

    if (!error_source) {
        exit(1);
    }

    std::string_view source(*error_source);
    
    size_t line_start = 0;
    uint64_t current_line = 1;

    while (current_line < error.line && line_start < source.length()) {
        size_t next_newline = source.find('\n', line_start);
        if (next_newline == std::string_view::npos) break;
        line_start = next_newline + 1;
        current_line++;
    }

    size_t line_end = source.find('\n', line_start);
    if (line_end == std::string_view::npos) {
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

void panic_at_source(std::string message, const std::string &modulePath, uint64_t byte_pos,
                     std::string *source)
{
    const ErrorPos position = get_error_pos(byte_pos, source);
    panic({std::move(message), modulePath, position.line, position.column});
}