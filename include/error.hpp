#pragma once
#include <cstdint>
#include <iostream>
#include <string>

struct Error 
{
    std::string message;
    std::string modulePath;
    uint64_t line;
    uint64_t column;
};

struct ErrorPos
{
    uint64_t line;
    uint64_t column;
};

void panic(Error error);
ErrorPos get_error_pos(uint64_t byte_pos, std::string* source);