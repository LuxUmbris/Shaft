#pragma once
#include <cstdint>
#include <iostream>
#include <string>

extern bool global_stop_on_error;

struct Error
{
    std::string message = "no_message";
    std::string modulePath = "unknown";
    uint64_t line = 0;
    uint64_t column = 0;
    bool is_macro_error = false;
};

struct ErrorPos
{
    uint64_t line;
    uint64_t column;
};

[[noreturn]] void panic(Error error);
ErrorPos get_error_pos(uint64_t byte_pos, std::string *source);
[[noreturn]] void panic_at_source(std::string message, const std::string &modulePath, uint64_t byte_pos,
                                  std::string *source);