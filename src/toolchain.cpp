#include "toolchain.hpp"
#include "thirdparty/shaft_llvm.h"

#include <llvm/BinaryFormat/COFF.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/ArchiveWriter.h>
#include <llvm/Object/COFFImportFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>


#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace Toolchain
{
    namespace
    {
        // ---------------------------------------------------------------- helpers

        std::string lower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        bool contains(const std::string &haystack, const char *needle)
        {
            return haystack.find(needle) != std::string::npos;
        }

        bool file_exists(const fs::path &path)
        {
            std::error_code error;
            return fs::exists(path, error);
        }

        bool directory_exists(const fs::path &path)
        {
            std::error_code error;
            return fs::is_directory(path, error);
        }

        std::string environment(const char *name)
        {
            const char *value = std::getenv(name);
            return value ? value : "";
        }

        // Directory entries sorted so that the highest version string comes last.
        std::vector<fs::path> sorted_children(const fs::path &directory)
        {
            std::vector<fs::path> children;
            std::error_code error;
            for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
                children.push_back(it->path());
            std::sort(children.begin(), children.end());
            return children;
        }

        std::string join_command(const std::vector<std::string> &arguments)
        {
            std::string command;
            for (const std::string &argument : arguments)
            {
                if (!command.empty())
                    command += ' ';
                command += argument;
            }
            return command;
        }

        // Runs the LLD driver that matches `format`. args[0] is the program name LLD prints in diagnostics.
        void run_lld(Format format, const std::vector<std::string> &arguments, bool verbose)
        {
            if (verbose)
                std::cerr << "shaftc: baked-in LLD (" << format_name(format) << "): " << join_command(arguments)
                          << '\n';
            std::vector<const char *> argv;
            argv.reserve(arguments.size());
            for (const std::string &argument : arguments)
                argv.push_back(argument.c_str());

            bool ok = false;
            switch (format)
            {
            case Format::ELF:
                ok = lld_elf_link(argv.data(), argv.size());
                break;
            case Format::MachO:
                ok = lld_macho_link(argv.data(), argv.size());
                break;
            case Format::COFF:
                ok = lld_mingw_link(argv.data(), argv.size());
                break;
            case Format::Wasm:
                ok = lld_wasm_link(argv.data(), argv.size());
                break;
            }
            if (!ok)
                throw std::runtime_error(std::string("baked-in LLD (") + format_name(format) +
                                         ") failed while producing the requested artifact");
        }

        std::string llvm_error_text(llvm::Error error)
        {
            return llvm::toString(std::move(error));
        }

        // -------------------------------------------------------------------- ELF

        // GNU ld emulation name for `-m`; empty lets LLD infer the machine from its inputs.
        std::string elf_emulation(const std::string &arch)
        {
            if (arch == "x86_64")
                return "elf_x86_64";
            if (arch == "aarch64")
                return "aarch64linux";
            if (arch == "riscv64")
                return "elf64lriscv";
            if (arch == "riscv32")
                return "elf32lriscv";
            if (arch == "i386")
                return "elf_i386";
            return "";
        }

        std::string multiarch_name(const std::string &arch)
        {
            if (arch == "i386")
                return "i386-linux-gnu";
            return arch + "-linux-gnu";
        }

        std::string dynamic_linker(const std::string &triple, const std::string &arch)
        {
            const std::string override_path = environment("SHAFT_DYNAMIC_LINKER");
            if (!override_path.empty())
                return override_path;
            if (contains(triple, "musl"))
                return "/lib/ld-musl-" + arch + ".so.1";
            if (arch == "x86_64")
                return "/lib64/ld-linux-x86-64.so.2";
            if (arch == "aarch64")
                return "/lib/ld-linux-aarch64.so.1";
            if (arch == "riscv64")
                return "/lib/ld-linux-riscv64-lp64d.so.1";
            if (arch == "riscv32")
                return "/lib/ld-linux-riscv32-ilp32d.so.1";
            if (arch == "i386")
                return "/lib/ld-linux.so.2";
            throw std::runtime_error("hosted linking has no dynamic loader for architecture '" + arch + "'");
        }

        struct LinuxCRuntime
        {
            fs::path crt1, crti, crtn;
            fs::path libgcc;
            std::vector<fs::path> libraryDirectories;
        };

        LinuxCRuntime find_linux_c_runtime(const LinkJob &job, const std::string &arch)
        {
            const std::string multiarch = multiarch_name(arch);
            std::vector<fs::path> roots;
            if (!job.sysroot.empty())
                roots.emplace_back(job.sysroot);
            else
                roots.emplace_back("/");

            LinuxCRuntime runtime;
            for (const fs::path &root : roots)
            {
                const std::vector<fs::path> candidates = {
                    root / "usr" / "lib" / multiarch, root / "usr" / "lib64", root / "usr" / "lib",
                    root / "lib" / multiarch,         root / "lib64",         root / "lib",
                };
                for (const fs::path &directory : candidates)
                    if (directory_exists(directory))
                        runtime.libraryDirectories.push_back(directory);

                for (const fs::path &directory : runtime.libraryDirectories)
                {
                    if (file_exists(directory / "crt1.o") && file_exists(directory / "crti.o") &&
                        file_exists(directory / "crtn.o"))
                    {
                        runtime.crt1 = directory / "crt1.o";
                        runtime.crti = directory / "crti.o";
                        runtime.crtn = directory / "crtn.o";
                        break;
                    }
                }

                // libgcc.a provides compiler helpers (128-bit division, soft atomics, ...) that clang used to add implicitly.
                const fs::path gccRoot = root / "usr" / "lib" / "gcc";
                if (directory_exists(gccRoot))
                {
                    for (const fs::path &vendor : sorted_children(gccRoot))
                    {
                        if (vendor.filename().string().rfind(arch, 0) != 0)
                            continue;
                        for (const fs::path &version : sorted_children(vendor))
                            if (file_exists(version / "libgcc.a"))
                                runtime.libgcc = version / "libgcc.a";
                    }
                }
            }
            if (runtime.crt1.empty())
                throw std::runtime_error(
                    "hosted linking could not find the C runtime start files (crt1.o, crti.o, crtn.o); install "
                    "the C library development package or pass --sysroot");
            return runtime;
        }

        void link_elf(const LinkJob &job)
        {
            const std::string arch = architecture(job.triple);
            std::vector<std::string> args{"shaftc"};
            const std::string emulation = elf_emulation(arch);
            if (!emulation.empty())
            {
                args.emplace_back("-m");
                args.push_back(emulation);
            }

            if (job.shared)
            {
                args.emplace_back("-shared");
                args.emplace_back("-nostdlib");
                args.insert(args.end(), job.inputs.begin(), job.inputs.end());
            }
            else if (!job.hosted)
            {
                args.emplace_back("-nostdlib");
                args.emplace_back("-static");
                args.insert(args.end(), job.inputs.begin(), job.inputs.end());
                args.emplace_back("-e");
                args.emplace_back("_start");
            }
            else
            {
                const LinuxCRuntime runtime = find_linux_c_runtime(job, arch);
                if (!job.sysroot.empty())
                    args.push_back("--sysroot=" + job.sysroot);
                args.emplace_back("--eh-frame-hdr");
                args.emplace_back("-dynamic-linker");
                args.push_back(dynamic_linker(job.triple, arch));
                args.push_back(runtime.crt1.string());
                args.push_back(runtime.crti.string());
                args.insert(args.end(), job.inputs.begin(), job.inputs.end());
                for (const std::string &directory : job.libraryDirectories)
                {
                    args.push_back("-L" + directory);
                    args.emplace_back("-rpath");
                    args.push_back(directory);
                }
                for (const fs::path &directory : runtime.libraryDirectories)
                    args.push_back("-L" + directory.string());
                args.emplace_back("-lc");
                if (!runtime.libgcc.empty())
                    args.push_back(runtime.libgcc.string());
                args.push_back(runtime.crtn.string());
            }
            args.emplace_back("-o");
            args.push_back(job.output);
            run_lld(Format::ELF, args, job.verbose);
        }

        // ------------------------------------------------------------------ Mach-O

        std::string macho_arch(const std::string &arch)
        {
            if (arch == "aarch64")
                return "arm64";
            return arch;
        }

        // Returns the macOS SDK root, or an empty string when none can be found.
        std::string find_macos_sdk(const LinkJob &job)
        {
            if (!job.sysroot.empty())
                return job.sysroot;
            const std::string sdkroot = environment("SDKROOT");
            if (!sdkroot.empty())
                return sdkroot;
            const char *candidates[] = {
                "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
                "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk",
            };
            for (const char *candidate : candidates)
                if (directory_exists(candidate))
                    return candidate;
            return "";
        }

        void link_macho(const LinkJob &job)
        {
            const std::string arch = architecture(job.triple);
            const std::string minimum = arch == "aarch64" ? "11.0" : "10.13";
            std::vector<std::string> args{"shaftc",   "-arch",   macho_arch(arch), "-platform_version",
                                          "macos",    minimum,   minimum};
            if (job.shared)
            {
                args.emplace_back("-dylib");
                args.emplace_back("-install_name");
                args.push_back("@rpath/" + fs::path(job.output).filename().string());
            }
            else if (!job.hosted)
            {
                // LLD's Mach-O port does not implement -static, so freestanding executables are plain
                // executables with a custom entry point.
                args.emplace_back("-e");
                args.emplace_back("_start");
            }
            args.insert(args.end(), job.inputs.begin(), job.inputs.end());

            // Modern macOS refuses to start a binary that does not load libSystem, so link it whenever an SDK is
            // available. Hosted builds cannot work without it.
            const std::string sdk = find_macos_sdk(job);
            if (job.hosted && sdk.empty())
                throw std::runtime_error(
                    "hosted macOS linking needs a macOS SDK for libSystem; set SDKROOT or pass --sysroot");
            if (!sdk.empty())
            {
                args.emplace_back("-syslibroot");
                args.push_back(sdk);
                args.emplace_back("-lSystem");
            }
            if (job.hosted)
            {
                for (const std::string &directory : job.libraryDirectories)
                {
                    args.push_back("-L" + directory);
                    args.emplace_back("-rpath");
                    args.push_back(directory);
                }
            }
            args.emplace_back("-o");
            args.push_back(job.output);
            run_lld(Format::MachO, args, job.verbose);
        }

        // -------------------------------------------------------------------- COFF

        const std::set<std::string> &ws2_32_exports()
        {
            static const std::set<std::string> names = {
                "WSAStartup", "WSACleanup", "WSAGetLastError", "accept",   "bind",   "closesocket", "connect",
                "ioctlsocket", "listen",    "recv",            "recvfrom", "send",   "sendto",      "shutdown",
                "socket",      "select",    "getsockname",     "getpeername", "setsockopt", "getsockopt",
            };
            return names;
        }

        const std::set<std::string> &kernel32_exports()
        {
            static const std::set<std::string> names = {
                "ExitProcess", "GetStdHandle", "CreateFileA", "ReadFile", "WriteFile", "CloseHandle",
                "GetFileSizeEx", "SetFilePointerEx", "GetSystemTimeAsFileTime", "Sleep", "CreatePipe",
                "CreateProcessA", "SetHandleInformation", "WaitForSingleObject", "GetExitCodeProcess",
                "PeekNamedPipe", "GetConsoleMode", "SetConsoleMode", "PeekConsoleInputA", "ReadConsoleInputA",
                "GetCommandLineA", "GetLastError", "SetLastError", "GetModuleHandleA", "GetProcessHeap",
                "HeapAlloc", "HeapFree", "HeapReAlloc", "VirtualAlloc", "VirtualFree", "GetCurrentProcess",
                "GetCurrentThreadId", "CreateThread", "ExitThread", "QueryPerformanceCounter",
                "QueryPerformanceFrequency", "GetTickCount64", "GetEnvironmentVariableA",
                "SetEnvironmentVariableA", "GetCurrentDirectoryA", "DeleteFileA", "GetFileAttributesA",
                "CreateDirectoryA", "FlushFileBuffers", "GetSystemInfo", "WriteConsoleA", "ReadConsoleA",
            };
            return names;
        }

        llvm::COFF::MachineTypes coff_machine(const std::string &arch)
        {
            if (arch == "aarch64")
                return llvm::COFF::IMAGE_FILE_MACHINE_ARM64;
            if (arch == "x86_64")
                return llvm::COFF::IMAGE_FILE_MACHINE_AMD64;
            throw std::runtime_error("Windows linking supports x86_64 and aarch64, not '" + arch + "'");
        }

        // The Windows runtime calls kernel32 / ws2_32 directly. Instead of requiring an SDK or MinGW
        // sysroot, build the tiny import libraries those calls need straight from the object files.
        std::vector<std::string> generate_import_libraries(const LinkJob &job, const fs::path &scratch)
        {
            std::set<std::string> undefined;
            for (const std::string &input : job.inputs)
            {
                if (input.size() < 2 || input.compare(input.size() - 2, 2, ".o") != 0)
                    continue;
                auto binary = llvm::object::ObjectFile::createObjectFile(input);
                if (!binary)
                    throw std::runtime_error("cannot read object '" + input + "': " + llvm_error_text(binary.takeError()));
                for (const llvm::object::SymbolRef &symbol : binary->getBinary()->symbols())
                {
                    auto flags = symbol.getFlags();
                    if (!flags || !(*flags & llvm::object::SymbolRef::SF_Undefined))
                        continue;
                    auto name = symbol.getName();
                    if (!name)
                    {
                        llvm::consumeError(name.takeError());
                        continue;
                    }
                    undefined.insert(name->str());
                }
            }

            std::map<std::string, std::vector<llvm::object::COFFShortExport>> byLibrary;
            for (const std::string &name : undefined)
            {
                std::string dll;
                if (ws2_32_exports().count(name))
                    dll = "ws2_32.dll";
                else if (kernel32_exports().count(name))
                    dll = "kernel32.dll";
                else
                    continue;
                llvm::object::COFFShortExport exported;
                exported.Name = name;
                byLibrary[dll].push_back(exported);
            }

            std::vector<std::string> libraries;
            for (auto &entry : byLibrary)
            {
                const fs::path path = scratch / ("lib" + entry.first.substr(0, entry.first.size() - 4) + ".a");
                if (llvm::Error error = llvm::object::writeImportLibrary(entry.first, path.string(), entry.second,
                                                                         coff_machine(architecture(job.triple)),
                                                                         /*MinGW=*/true))
                    throw std::runtime_error("failed to create import library for " + entry.first + ": " +
                                             llvm_error_text(std::move(error)));
                libraries.push_back(path.string());
            }
            return libraries;
        }

        struct MinGWRuntime
        {
            fs::path crt2;
            std::vector<fs::path> libraryDirectories;
        };

        MinGWRuntime find_mingw_runtime(const LinkJob &job, const std::string &arch)
        {
            const std::string triple = arch + "-w64-mingw32";
            std::vector<fs::path> roots;
            if (!job.sysroot.empty())
                roots.emplace_back(job.sysroot);
            const std::string env = environment("MINGW_SYSROOT");
            if (!env.empty())
                roots.emplace_back(env);
            roots.emplace_back(fs::path("/usr") / triple);
            roots.emplace_back("/mingw64");
            roots.emplace_back("/clang64");
            roots.emplace_back("C:/msys64/mingw64");
            roots.emplace_back("C:/msys64/clang64");
            roots.emplace_back("C:/mingw64");

            MinGWRuntime runtime;
            for (const fs::path &root : roots)
            {
                for (const fs::path &directory : {root / "lib", root / triple / "lib"})
                {
                    if (!directory_exists(directory))
                        continue;
                    runtime.libraryDirectories.push_back(directory);
                    if (runtime.crt2.empty() && file_exists(directory / "crt2.o"))
                        runtime.crt2 = directory / "crt2.o";
                }
                const fs::path gccRoot = root / "lib" / "gcc" / triple;
                if (directory_exists(gccRoot))
                    for (const fs::path &version : sorted_children(gccRoot))
                        if (directory_exists(version))
                            runtime.libraryDirectories.push_back(version);
                if (!runtime.crt2.empty())
                    break;
            }
            if (runtime.crt2.empty())
                throw std::runtime_error("hosted Windows linking could not find a MinGW-w64 sysroot (crt2.o); "
                                         "pass --sysroot or set MINGW_SYSROOT");
            return runtime;
        }

        void link_coff(const LinkJob &job)
        {
            const std::string arch = architecture(job.triple);
            coff_machine(arch); // validates the architecture
            std::vector<std::string> args{"shaftc", "-m", arch == "aarch64" ? "arm64pe" : "i386pep"};
            fs::path scratch;
            try
            {
                if (job.shared)
                {
                    args.emplace_back("--shared");
                    args.insert(args.end(), job.inputs.begin(), job.inputs.end());
                }
                else if (!job.hosted)
                {
                    scratch = fs::temp_directory_path() /
                              ("shaftc-implib-" +
                               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                    fs::create_directories(scratch);
                    args.emplace_back("-e");
                    args.emplace_back("mainCRTStartup");
                    args.emplace_back("--subsystem");
                    args.emplace_back("console");
                    args.insert(args.end(), job.inputs.begin(), job.inputs.end());
                    const std::vector<std::string> imports = generate_import_libraries(job, scratch);
                    args.insert(args.end(), imports.begin(), imports.end());
                }
                else
                {
                    const MinGWRuntime runtime = find_mingw_runtime(job, arch);
                    args.emplace_back("-e");
                    args.emplace_back("mainCRTStartup");
                    args.emplace_back("--subsystem");
                    args.emplace_back("console");
                    args.push_back(runtime.crt2.string());
                    args.insert(args.end(), job.inputs.begin(), job.inputs.end());
                    for (const std::string &directory : job.libraryDirectories)
                        args.push_back("-L" + directory);
                    for (const fs::path &directory : runtime.libraryDirectories)
                        args.push_back("-L" + directory.string());
                    args.emplace_back("--start-group");
                    for (const char *library : {"-lmingw32", "-lgcc", "-lmingwex", "-lmsvcrt", "-ladvapi32",
                                                "-lshell32", "-luser32", "-lkernel32"})
                        args.emplace_back(library);
                    args.emplace_back("--end-group");
                }
                args.emplace_back("-o");
                args.push_back(job.output);
                run_lld(Format::COFF, args, job.verbose);
            }
            catch (...)
            {
                if (!scratch.empty())
                {
                    std::error_code error;
                    fs::remove_all(scratch, error);
                }
                throw;
            }
            if (!scratch.empty())
            {
                std::error_code error;
                fs::remove_all(scratch, error);
            }
        }

        // ------------------------------------------------------------- WebAssembly

        void link_wasm(const LinkJob &job)
        {
            if (job.hosted)
                throw std::runtime_error("--hosted is not supported for WebAssembly targets; the bundled runtime "
                                         "talks to the host through WASI imports");
            std::vector<std::string> args{"shaftc"};
            if (architecture(job.triple) == "wasm64")
                args.emplace_back("-mwasm64");
            args.insert(args.end(), job.inputs.begin(), job.inputs.end());
            args.emplace_back("--export-memory");
            args.emplace_back("-z");
            args.emplace_back("stack-size=1048576");
            if (job.shared)
            {
                // A library module: no start function, every defined symbol is exported.
                args.emplace_back("--no-entry");
                args.emplace_back("--export-all");
            }
            else
            {
                args.emplace_back("-e");
                args.emplace_back("_start");
            }
            args.emplace_back("-o");
            args.push_back(job.output);
            run_lld(Format::Wasm, args, job.verbose);
        }
    } // namespace

    // ---------------------------------------------------------------- public API

    const char *format_name(Format format)
    {
        switch (format)
        {
        case Format::ELF:
            return "ELF";
        case Format::MachO:
            return "Mach-O";
        case Format::COFF:
            return "COFF";
        case Format::Wasm:
            return "WebAssembly";
        }
        return "unknown";
    }

    std::string architecture(const std::string &triple)
    {
        const std::string value = lower(triple.substr(0, triple.find('-')));
        if (value == "amd64" || value == "x86_64h")
            return "x86_64";
        if (value == "arm64" || value == "aarch64_be")
            return "aarch64";
        if (value.rfind("riscv64", 0) == 0)
            return "riscv64";
        if (value.rfind("riscv32", 0) == 0)
            return "riscv32";
        if (value == "i486" || value == "i586" || value == "i686" || value == "x86")
            return "i386";
        return value;
    }

    bool is_wasm(const std::string &triple) { return architecture(triple).rfind("wasm", 0) == 0; }

    bool is_linux(const std::string &triple) { return contains(lower(triple), "linux"); }

    bool is_darwin(const std::string &triple)
    {
        const std::string value = lower(triple);
        return contains(value, "darwin") || contains(value, "apple") || contains(value, "macos");
    }

    bool is_windows(const std::string &triple)
    {
        const std::string value = lower(triple);
        return contains(value, "windows") || contains(value, "mingw") || contains(value, "w64") ||
               contains(value, "win32");
    }

    Format object_format(const std::string &triple)
    {
        if (is_wasm(triple))
            return Format::Wasm;
        if (is_darwin(triple))
            return Format::MachO;
        if (is_windows(triple))
            return Format::COFF;
        return Format::ELF;
    }

    void link(const LinkJob &job)
    {
        switch (object_format(job.triple))
        {
        case Format::ELF:
            link_elf(job);
            break;
        case Format::MachO:
            link_macho(job);
            break;
        case Format::COFF:
            link_coff(job);
            break;
        case Format::Wasm:
            link_wasm(job);
            break;
        }
    }

    bool is_ir_input(const std::string &path)
    {
        const std::string extension = lower(fs::path(path).extension().string());
        return extension == ".bc" || extension == ".ll" || extension == ".llvm";
    }

    void lower_ir_input(const std::string &path, const std::function<void(LLVMModuleRef)> &emit)
    {
        LLVMContextRef context = LLVMContextCreate();
        LLVMMemoryBufferRef buffer = nullptr;
        char *message = nullptr;
        if (LLVMCreateMemoryBufferWithContentsOfFile(path.c_str(), &buffer, &message))
        {
            const std::string text = message ? message : "unable to read file";
            LLVMDisposeMessage(message);
            LLVMContextDispose(context);
            throw std::runtime_error("failed to read linker input '" + path + "': " + text);
        }
        LLVMModuleRef ir = nullptr;
        // The parser takes ownership of the buffer and accepts both textual IR and bitcode.
        if (LLVMParseIRInContext(context, buffer, &ir, &message))
        {
            const std::string text = message ? message : "invalid LLVM IR";
            LLVMDisposeMessage(message);
            LLVMContextDispose(context);
            throw std::runtime_error("failed to parse linker input '" + path + "': " + text);
        }
        try
        {
            emit(ir);
        }
        catch (...)
        {
            LLVMDisposeModule(ir);
            LLVMContextDispose(context);
            throw;
        }
        LLVMDisposeModule(ir);
        LLVMContextDispose(context);
    }

    void add_hosted_main(LLVMModuleRef module, bool noStd)
    {
        if (LLVMValueRef existing = LLVMGetNamedFunction(module, "main"))
            if (LLVMGetFirstBasicBlock(existing))
                throw std::runtime_error("--hosted supplies the C main; remove the C 'main' definition");

        LLVMValueRef target = LLVMGetNamedFunction(module, noStd ? "__main" : "__shaft_entry");
        if (!target)
            throw std::runtime_error(noStd ? "--no-std --hosted requires cdef main() -> i32"
                                           : "hosted linking requires the Shaft entry point __shaft_entry");
        LLVMTypeRef targetType = LLVMGlobalGetValueType(target);
        const unsigned parameterCount = std::min(LLVMCountParamTypes(targetType), noStd ? 0u : 2u);

        LLVMContextRef context = LLVMGetModuleContext(module);
        LLVMTypeRef int32 = LLVMInt32TypeInContext(context);
        LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
        LLVMTypeRef mainParameters[] = {int32, pointer};
        LLVMValueRef entry =
            LLVMAddFunction(module, "main", LLVMFunctionType(int32, mainParameters, parameterCount, 0));

        LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
        LLVMPositionBuilderAtEnd(builder, LLVMAppendBasicBlockInContext(context, entry, "entry"));
        std::vector<LLVMValueRef> arguments;
        for (unsigned index = 0; index < parameterCount; ++index)
            arguments.push_back(LLVMGetParam(entry, index));
        LLVMValueRef status =
            LLVMBuildCall2(builder, targetType, target, arguments.data(), parameterCount, "status");
        LLVMBuildRet(builder, status);
        LLVMDisposeBuilder(builder);
    }

    void create_archive(const std::string &output, const std::vector<std::string> &objects, const std::string &triple)
    {
        llvm::object::Archive::Kind kind = llvm::object::Archive::K_GNU;
        switch (object_format(triple))
        {
        case Format::MachO:
            kind = llvm::object::Archive::K_DARWIN;
            break;
        case Format::COFF:
            kind = llvm::object::Archive::K_COFF;
            break;
        case Format::ELF:
        case Format::Wasm:
            break;
        }

        const std::string memberStem = fs::path(output).stem().string();
        std::vector<std::string> memberNames;
        memberNames.reserve(objects.size());
        std::vector<llvm::NewArchiveMember> members;
        for (size_t index = 0; index < objects.size(); ++index)
        {
            auto member = llvm::NewArchiveMember::getFile(objects[index], /*Deterministic=*/true);
            if (!member)
                throw std::runtime_error("cannot add '" + objects[index] + "' to the archive: " +
                                         llvm_error_text(member.takeError()));
            // Name members after the library rather than the temporary object path.
            memberNames.push_back(memberStem + (objects.size() > 1 ? "-" + std::to_string(index) : "") + ".o");
            member->MemberName = memberNames.back();
            members.push_back(std::move(*member));
        }

        if (llvm::Error error = llvm::writeArchive(output, members, llvm::SymtabWritingMode::NormalSymtab, kind,
                                                   /*Deterministic=*/true, /*Thin=*/false))
            throw std::runtime_error("failed to write static library '" + output +
                                     "': " + llvm_error_text(std::move(error)));
    }
} // namespace Toolchain
