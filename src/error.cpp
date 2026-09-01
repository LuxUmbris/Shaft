#include "error.hpp"
#include <cstdlib>

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
    return {line, column};
}

void panic(Error error)
{
    std::cerr << "error: " << error.message << '\n'
              << "--> " << error.modulePath << ':' << error.line << ':' << error.column << '\n';
    exit(1);
}