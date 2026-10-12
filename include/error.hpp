#pragma once
#include <cstdint>
#include <exception>
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
    std::string help;
};

struct ErrorPos
{
    uint64_t line;
    uint64_t column;
};

class CompilationError final : public std::exception
{
  public:
    CompilationError(std::string message, bool reported = false);
    const char *what() const noexcept override;
    bool was_reported() const noexcept;

  private:
    std::string message;
    bool reported;
};

[[noreturn]] void panic(Error error);
[[noreturn]] void compiler_error(std::string message);
void report_compilation_error(const CompilationError &error);
ErrorPos get_error_pos(uint64_t byte_pos, std::string *source);
void record_diagnostic(Error error, const std::string *source);
void flush_diagnostics();
void clear_diagnostics();
bool has_diagnostics();
[[noreturn]] void panic_at_source(std::string message, const std::string &modulePath, uint64_t byte_pos,
                                  std::string *source);