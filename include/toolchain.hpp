#pragma once

// In-process replacement for the external clang / llvm-ar / ld.lld toolchain.
//
// Everything here runs inside shaftc using the statically linked LLVM and LLD
// libraries: no helper executable is ever spawned.

#include "thirdparty/shaft_llvm.h"

#include <functional>
#include <string>
#include <vector>

namespace Toolchain
{
    enum class Format
    {
        ELF,   // Linux and other ELF systems (x86_64, aarch64, riscv64, ...)
        MachO, // macOS / Darwin
        COFF,  // Windows (MinGW flavour)
        Wasm,  // WebAssembly (wasm32 / wasm64)
    };

    // Object-file format implied by an LLVM target triple.
    Format object_format(const std::string &triple);

    // Architecture component of the triple, normalised to LLVM's spelling
    // (x86_64, aarch64, riscv64, wasm32, ...).
    std::string architecture(const std::string &triple);

    bool is_wasm(const std::string &triple);
    bool is_linux(const std::string &triple);
    bool is_darwin(const std::string &triple);
    bool is_windows(const std::string &triple);

    // Human readable name of the baked-in linker flavour ("ELF", "Mach-O", ...).
    const char *format_name(Format format);

    struct LinkJob
    {
        std::string triple;
        // Objects, archives and "-lNAME" requests in link order. The first
        // entry is the object produced from the Shaft program.
        std::vector<std::string> inputs;
        std::vector<std::string> libraryDirectories;
        std::string output;
        // Optional --sysroot override used to find the C runtime for hosted builds.
        std::string sysroot;
        bool shared = false; // --emit dynamiclib
        bool hosted = false; // link the host C runtime (crt1.o, libc, ...)
        bool verbose = false;
    };

    // Links an executable or shared library with the baked-in LLD driver for
    // the job's object format. Throws std::runtime_error on failure.
    void link(const LinkJob &job);

    // True for raw LLVM IR / bitcode linker inputs (.ll, .llvm, .bc).
    bool is_ir_input(const std::string &path);

    // Parses a textual-IR or bitcode file and hands the module to `emit`, which must write a native object
    // for it (shaftc passes its normal code generator). This replaces `clang -c` / `llc` for raw IR inputs.
    void lower_ir_input(const std::string &path, const std::function<void(LLVMModuleRef)> &emit);

    // Adds the C `main` that a hosted C runtime (crt1.o) calls, forwarding to the Shaft entry point
    // (`__shaft_entry`, or `__main` with noStd). Built directly as IR, so no C compiler is needed.
    void add_hosted_main(LLVMModuleRef module, bool noStd);

    // Writes a static archive (the llvm-ar replacement) holding `objects`.
    // The archive flavour (GNU, BSD, COFF) and symbol table follow the triple.
    void create_archive(const std::string &output, const std::vector<std::string> &objects,
                        const std::string &triple);
} // namespace Toolchain
