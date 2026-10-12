#pragma once

#include "thirdparty/shaft_llvm.h"

#include <functional>
#include <string>
#include <vector>

namespace Toolchain
{
    enum class Format
    {
        ELF,
        MachO,
        COFF,
        Wasm,
    };

    Format object_format(const std::string &triple);

    std::string architecture(const std::string &triple);

    bool is_wasm(const std::string &triple);
    bool is_linux(const std::string &triple);
    bool is_darwin(const std::string &triple);
    bool is_windows(const std::string &triple);

    const char *format_name(Format format);

    struct LinkJob
    {
        std::string triple;
        std::vector<std::string> inputs;
        std::vector<std::string> libraryDirectories;
        std::string output;
        std::string sysroot;
        bool shared = false;
        bool hosted = false;
        bool verbose = false;
    };

    void link(const LinkJob &job);
    bool is_ir_input(const std::string &path);
    void lower_ir_input(const std::string &path, const std::function<void(LLVMModuleRef)> &emit);
    void add_hosted_main(LLVMModuleRef module, bool noStd);
    void create_archive(const std::string &output, const std::vector<std::string> &objects, const std::string &triple);
} // namespace Toolchain
