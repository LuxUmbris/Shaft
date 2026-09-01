#include "checker.hpp"
#include "codegen.hpp"
#include "lexer.hpp"
#include "parser.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace
{
    enum class EmitKind
    {
        LLVM,
        Object,
        Assembly,
        StaticLibrary,
        DynamicLibrary,
        Binary,
    };

    enum class OptimizationLevel
    {
        O0,
        O1,
        O2,
        O3,
    };

    const char *optimization_name(OptimizationLevel level)
    {
        switch (level)
        {
        case OptimizationLevel::O0: return "O0";
        case OptimizationLevel::O1: return "O1";
        case OptimizationLevel::O2: return "O2";
        case OptimizationLevel::O3: return "O3";
        }
        return "O0";
    }

    struct Options
    {
        EmitKind emit = EmitKind::Binary;
        std::string inputPath;
        std::string outputPath;
        std::string stdlibPath;
        std::string runtimePath;
        std::string resourcePath;
        std::string targetTriple;
        OptimizationLevel optimization = OptimizationLevel::O2;
        std::vector<std::string> linkDirectories;
        std::vector<std::string> linkArguments;
        bool nativeCpu = false;
        bool hosted = false;
        bool noStd = false;
        bool checkOnly = false;
        bool verbose = false;
        bool version = false;
    };

    EmitKind parse_emit_kind(const std::string &value);
    OptimizationLevel parse_optimization_level(const std::string &argument);

    struct BuildInvocation
    {
        bool requested = false;
        std::string path;
    };

    std::string trim(std::string value)
    {
        const auto isSpace = [](unsigned char character) { return std::isspace(character) != 0; };
        size_t first = 0;
        while (first < value.size() && isSpace(value[first]))
            ++first;
        size_t last = value.size();
        while (last > first && isSpace(value[last - 1]))
            --last;
        return value.substr(first, last - first);
    }

    std::string without_toml_comment(const std::string &line)
    {
        bool quoted = false;
        bool escaped = false;
        for (size_t index = 0; index < line.size(); ++index)
        {
            const char character = line[index];
            if (quoted && escaped)
            {
                escaped = false;
                continue;
            }
            if (quoted && character == '\\')
            {
                escaped = true;
                continue;
            }
            if (character == '"')
                quoted = !quoted;
            else if (character == '#' && !quoted)
                return line.substr(0, index);
        }
        return line;
    }

    std::string parse_toml_string(const std::string &value, const std::string &location)
    {
        if (value.size() < 2 || value.front() != '"' || value.back() != '"')
            throw std::runtime_error(location + " must be a quoted string");
        std::string result;
        for (size_t index = 1; index + 1 < value.size(); ++index)
        {
            if (value[index] != '\\')
            {
                result += value[index];
                continue;
            }
            if (++index + 1 >= value.size())
                throw std::runtime_error(location + " has an incomplete string escape");
            switch (value[index])
            {
            case '\\': result += '\\'; break;
            case '"': result += '"'; break;
            case 'n': result += '\n'; break;
            case 't': result += '\t'; break;
            default: throw std::runtime_error(location + " has an unsupported string escape");
            }
        }
        return result;
    }

    std::vector<std::string> parse_toml_string_array(const std::string &value, const std::string &location)
    {
        size_t index = 0;
        const auto skip_space = [&value, &index]()
        {
            while (index < value.size() && std::isspace(static_cast<unsigned char>(value[index])))
                ++index;
        };
        skip_space();
        if (index == value.size() || value[index++] != '[')
            throw std::runtime_error(location + " must be an array of quoted strings");

        std::vector<std::string> values;
        for (;;)
        {
            skip_space();
            if (index == value.size())
                throw std::runtime_error(location + " has an unterminated array");
            if (value[index] == ']')
            {
                ++index;
                skip_space();
                if (index != value.size())
                    throw std::runtime_error(location + " has unexpected content after its array");
                return values;
            }
            if (value[index] != '"')
                throw std::runtime_error(location + " must contain only quoted strings");

            const size_t start = index++;
            bool escaped = false;
            while (index < value.size())
            {
                const char character = value[index++];
                if (escaped)
                {
                    escaped = false;
                    continue;
                }
                if (character == '\\')
                {
                    escaped = true;
                    continue;
                }
                if (character == '"')
                    break;
            }
            if (index == value.size() && value[index - 1] != '"')
                throw std::runtime_error(location + " has an unterminated quoted string");
            values.emplace_back(parse_toml_string(value.substr(start, index - start), location));

            skip_space();
            if (index == value.size() || value[index] == ']')
                continue;
            if (value[index++] != ',')
                throw std::runtime_error(location + " must separate array values with commas");
            skip_space();
            if (index < value.size() && value[index] == ']')
                throw std::runtime_error(location + " must not have a trailing comma");
        }
    }

    bool parse_toml_bool(const std::string &value, const std::string &location)
    {
        if (value == "true")
            return true;
        if (value == "false")
            return false;
        throw std::runtime_error(location + " must be true or false");
    }

    std::string resolve_build_path(const std::filesystem::path &base, const std::string &value)
    {
        const std::filesystem::path path(value);
        return (path.is_absolute() ? path : base / path).lexically_normal().string();
    }

    Options load_build_file(const std::string &path)
    {
        const std::filesystem::path buildPath = std::filesystem::absolute(path).lexically_normal();
        std::ifstream input(buildPath);
        if (!input)
            throw std::runtime_error("failed to read build file '" + buildPath.string() + "'");

        std::unordered_map<std::string, std::pair<std::string, size_t>> fields;
        std::string section;
        std::string rawLine;
        size_t lineNumber = 0;
        while (std::getline(input, rawLine))
        {
            ++lineNumber;
            const std::string line = trim(without_toml_comment(rawLine));
            if (line.empty())
                continue;
            if (line.front() == '[' && line.back() == ']')
            {
                section = trim(line.substr(1, line.size() - 2));
                if (section != "package" && section != "build")
                    throw std::runtime_error(buildPath.string() + ":" + std::to_string(lineNumber) +
                                             ": unknown table '" + section + "'");
                continue;
            }
            const size_t equals = line.find('=');
            if (section.empty() || equals == std::string::npos)
                throw std::runtime_error(buildPath.string() + ":" + std::to_string(lineNumber) +
                                         ": expected a table key assignment");
            const std::string key = trim(line.substr(0, equals));
            const std::string value = trim(line.substr(equals + 1));
            if (key.empty() || value.empty())
                throw std::runtime_error(buildPath.string() + ":" + std::to_string(lineNumber) +
                                         ": expected a non-empty key and value");
            const std::string field = section + "." + key;
            if (!fields.emplace(field, std::make_pair(value, lineNumber)).second)
                throw std::runtime_error(buildPath.string() + ":" + std::to_string(lineNumber) +
                                         ": duplicate key '" + field + "'");
        }

        const auto location = [&buildPath, &fields](const std::string &field) {
            return buildPath.string() + ":" + std::to_string(fields.at(field).second) + ": " + field;
        };
        const auto stringField = [&fields, &location](const std::string &field) {
            return parse_toml_string(fields.at(field).first, location(field));
        };
        const auto boolField = [&fields, &location](const std::string &field) {
            return parse_toml_bool(fields.at(field).first, location(field));
        };
        const auto stringArrayField = [&fields, &location](const std::string &field) {
            return parse_toml_string_array(fields.at(field).first, location(field));
        };
        const auto require_known = [&fields, &location](const std::string &field) {
            const bool known = field == "package.name" || field == "package.version" || field == "build.entry" ||
                               field == "build.output" || field == "build.emit" || field == "build.optimization" ||
                               field == "build.target" || field == "build.stdlib" || field == "build.runtime" ||
                               field == "build.resources" || field == "build.native" || field == "build.no_std" ||
                               field == "build.check_only" || field == "build.verbose" || field == "build.hosted" ||
                               field == "build.link_dirs" || field == "build.link_directories" || field == "build.links";
            if (!known)
                throw std::runtime_error(location(field) + ": unknown configuration key");
        };
        for (const auto &entry : fields)
            require_known(entry.first);
        if (fields.find("build.entry") == fields.end())
            throw std::runtime_error(buildPath.string() + ": build.entry is required");

        const std::filesystem::path base = buildPath.parent_path();
        Options options;
        options.inputPath = resolve_build_path(base, stringField("build.entry"));
        const auto setPath = [&fields, &stringField, &base](const std::string &field, std::string &destination) {
            if (fields.find(field) != fields.end())
                destination = resolve_build_path(base, stringField(field));
        };
        setPath("build.output", options.outputPath);
        setPath("build.stdlib", options.stdlibPath);
        setPath("build.runtime", options.runtimePath);
        setPath("build.resources", options.resourcePath);
        if (fields.find("build.emit") != fields.end())
            options.emit = parse_emit_kind(stringField("build.emit"));
        if (fields.find("build.optimization") != fields.end())
        {
            std::string optimization = stringField("build.optimization");
            if (optimization.size() == 2 && optimization.front() == 'O')
                optimization = "-" + optimization;
            else if (optimization.size() == 1 && optimization.front() >= '0' && optimization.front() <= '3')
                optimization = "-O" + optimization;
            options.optimization = parse_optimization_level(optimization);
        }
        if (fields.find("build.target") != fields.end())
            options.targetTriple = stringField("build.target");
        if (fields.find("build.native") != fields.end())
            options.nativeCpu = boolField("build.native");
        if (fields.find("build.no_std") != fields.end())
            options.noStd = boolField("build.no_std");
        if (fields.find("build.check_only") != fields.end())
            options.checkOnly = boolField("build.check_only");
        if (fields.find("build.verbose") != fields.end())
            options.verbose = boolField("build.verbose");
        if (fields.find("build.hosted") != fields.end())
            options.hosted = boolField("build.hosted");
        if (fields.find("build.link_dirs") != fields.end() && fields.find("build.link_directories") != fields.end())
            throw std::runtime_error(buildPath.string() + ": specify only one of build.link_dirs or build.link_directories");
        const std::string linkDirectoriesField = fields.find("build.link_directories") != fields.end()
                                                    ? "build.link_directories"
                                                    : "build.link_dirs";
        if (fields.find(linkDirectoriesField) != fields.end())
            for (const std::string &directory : stringArrayField(linkDirectoriesField))
                options.linkDirectories.emplace_back(resolve_build_path(base, directory));
        if (fields.find("build.links") != fields.end())
            for (const std::string &library : stringArrayField("build.links"))
            {
                if (library.empty())
                    throw std::runtime_error(location("build.links") + " must not contain an empty library name");
                options.linkArguments.emplace_back("-l" + library);
            }
        return options;
    }

    BuildInvocation find_build_invocation(int argc, char **argv)
    {
        BuildInvocation invocation;
        for (int index = 1; index < argc; ++index)
        {
            const std::string argument = argv[index];
            if (argument == "--build")
            {
                if (invocation.requested)
                    throw std::runtime_error("--build may be specified only once");
                invocation.requested = true;
                invocation.path = "Shaft.build";
                if (index + 1 < argc && argv[index + 1][0] != '-')
                    invocation.path = argv[++index];
                continue;
            }
            if (argument.rfind("--build=", 0) == 0)
            {
                if (invocation.requested || argument.size() == 8)
                    throw std::runtime_error("--build requires a non-empty configuration path");
                invocation.requested = true;
                invocation.path = argument.substr(8);
            }
        }
        return invocation;
    }

    void verbose(const Options &options, const std::string &message)
    {
        if (options.verbose)
            std::cerr << "shaftc: " << message << '\n';
    }

    std::string read_source(const std::string &path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("failed to read source file '" + path + "'");
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    struct ImportedSource
    {
        std::string path;
        std::string source;
        std::vector<std::string> imports;
    };

    bool source_identifier_character(char character)
    {
        return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
    }

    void blank_source_range(std::string &source, size_t first, size_t last)
    {
        for (size_t index = first; index < last; ++index)
            if (source[index] != '\n' && source[index] != '\r')
                source[index] = ' ';
    }

    ImportedSource extract_imports(const std::filesystem::path &path, std::string source)
    {
        ImportedSource result{path.string(), std::move(source), {}};
        for (size_t index = 0; index < result.source.size();)
        {
            if (result.source[index] == '/' && index + 1 < result.source.size() && result.source[index + 1] == '/')
            {
                index = result.source.find('\n', index + 2);
                if (index == std::string::npos)
                    break;
                continue;
            }
            if (result.source[index] == '/' && index + 1 < result.source.size() && result.source[index + 1] == '*')
            {
                const size_t end = result.source.find("*/", index + 2);
                if (end == std::string::npos)
                    throw std::runtime_error("unterminated block comment in '" + path.string() + "'");
                index = end + 2;
                continue;
            }
            if (result.source[index] == '"' || result.source[index] == '\'')
            {
                const char delimiter = result.source[index++];
                bool escaped = false;
                while (index < result.source.size())
                {
                    const char character = result.source[index++];
                    if (escaped)
                    {
                        escaped = false;
                        continue;
                    }
                    if (character == '\\')
                    {
                        escaped = true;
                        continue;
                    }
                    if (character == delimiter)
                        break;
                }
                continue;
            }
            if (!source_identifier_character(result.source[index]))
            {
                ++index;
                continue;
            }

            const size_t identifierStart = index;
            while (index < result.source.size() && source_identifier_character(result.source[index]))
                ++index;
            if (result.source.compare(identifierStart, index - identifierStart, "import") != 0)
                continue;

            size_t cursor = index;
            while (cursor < result.source.size() && std::isspace(static_cast<unsigned char>(result.source[cursor])))
                ++cursor;
            if (cursor == result.source.size() || result.source[cursor] != '"')
                continue;
            ++cursor;
            std::string importedPath;
            bool closed = false;
            while (cursor < result.source.size())
            {
                const char character = result.source[cursor++];
                if (character == '"')
                {
                    closed = true;
                    break;
                }
                if (character == '\\')
                {
                    if (cursor == result.source.size())
                        break;
                    const char escaped = result.source[cursor++];
                    if (escaped != '\\' && escaped != '"')
                        throw std::runtime_error("unsupported import-path escape in '" + path.string() + "'");
                    importedPath += escaped;
                    continue;
                }
                importedPath += character;
            }
            if (!closed)
                throw std::runtime_error("unterminated import path in '" + path.string() + "'");
            while (cursor < result.source.size() && std::isspace(static_cast<unsigned char>(result.source[cursor])))
                ++cursor;
            if (cursor == result.source.size() || result.source[cursor] != ';')
                throw std::runtime_error("expected ';' after import in '" + path.string() + "'");
            result.imports.push_back(std::move(importedPath));
            blank_source_range(result.source, identifierStart, cursor + 1);
            index = cursor + 1;
        }
        return result;
    }

    class ProjectModuleLoader
    {
    public:
        std::vector<ImportedSource> load(const std::filesystem::path &entry)
        {
            visit(entry);
            return std::move(modules);
        }

    private:
        std::vector<ImportedSource> modules;
        std::unordered_set<std::string> visited;
        std::unordered_set<std::string> active;

        void visit(const std::filesystem::path &requestedPath)
        {
            std::error_code error;
            const std::filesystem::path path = std::filesystem::canonical(requestedPath, error);
            if (error)
                throw std::runtime_error("failed to resolve source module '" + requestedPath.string() + "': " +
                                         error.message());
            const std::string key = path.string();
            if (visited.find(key) != visited.end())
                return;
            if (!active.emplace(key).second)
                throw std::runtime_error("cyclic import involving '" + key + "'");

            ImportedSource module = extract_imports(path, read_source(key));
            for (const std::string &import : module.imports)
                visit(path.parent_path() / import);
            active.erase(key);
            visited.emplace(key);
            modules.push_back(std::move(module));
        }
    };

    std::vector<ImportedSource> load_project_modules(const std::string &entryPath)
    {
        ProjectModuleLoader loader;
        return loader.load(entryPath);
    }

    const char *emit_name(EmitKind emit)
    {
        switch (emit)
        {
        case EmitKind::LLVM: return "llvm";
        case EmitKind::Object: return "object";
        case EmitKind::Assembly: return "asm";
        case EmitKind::StaticLibrary: return "staticlib";
        case EmitKind::DynamicLibrary: return "dynamiclib";
        case EmitKind::Binary: return "binary";
        }
        return "binary";
    }

    EmitKind parse_emit_kind(const std::string &value)
    {
        if (value == "llvm")
            return EmitKind::LLVM;
        if (value == "object")
            return EmitKind::Object;
        if (value == "asm")
            return EmitKind::Assembly;
        if (value == "staticlib")
            return EmitKind::StaticLibrary;
        if (value == "dynamiclib")
            return EmitKind::DynamicLibrary;
        if (value == "binary")
            return EmitKind::Binary;
        throw std::runtime_error("unknown emit kind '" + value + "'");
    }

    std::string default_output_path(const std::string &input, EmitKind emit)
    {
        const std::filesystem::path path(input);
        const std::filesystem::path base = path.parent_path() / path.stem();
        switch (emit)
        {
        case EmitKind::LLVM: return base.string() + ".ll";
        case EmitKind::Object: return base.string() + ".o";
        case EmitKind::Assembly: return base.string() + ".s";
        case EmitKind::StaticLibrary: return base.string() + ".a";
        case EmitKind::DynamicLibrary:
#if defined(_WIN32)
            return base.string() + ".dll";
#elif defined(__APPLE__)
            return base.string() + ".dylib";
#else
            return base.string() + ".so";
#endif
        case EmitKind::Binary:
#if defined(_WIN32)
            return base.string() + ".exe";
#else
            return base.string();
#endif
        }
        return base.string();
    }

    OptimizationLevel parse_optimization_level(const std::string &argument)
    {
        if (argument == "-O0")
            return OptimizationLevel::O0;
        if (argument == "-O1")
            return OptimizationLevel::O1;
        if (argument == "-O2")
            return OptimizationLevel::O2;
        if (argument == "-O3")
            return OptimizationLevel::O3;
        throw std::runtime_error("unsupported optimization level '" + argument + "'; use -O0, -O1, -O2, or -O3");
    }

    bool is_raw_link_input(const std::string &path)
    {
        const std::string extension = std::filesystem::path(path).extension().string();
        return extension == ".o" || extension == ".a" || extension == ".bc" ||
               extension == ".ll" || extension == ".llvm";
    }

    Options parse_options(int argc, char **argv)
    {
        const BuildInvocation buildInvocation = find_build_invocation(argc, argv);
        Options options = buildInvocation.requested ? load_build_file(buildInvocation.path) : Options{};
        for (int i = 1; i < argc; ++i)
        {
            const std::string argument = argv[i];
            if (argument == "--build")
            {
                if (i + 1 < argc && argv[i + 1][0] != '-')
                    ++i;
                continue;
            }
            if (argument.rfind("--build=", 0) == 0)
                continue;
            if (argument == "--version")
            {
                options.version = true;
                continue;
            }
            if (argument == "--help" || argument == "-h")
            {
                std::cout << "Usage: shaftc [OPTIONS] INPUT\n"
                             "       shaftc --build [Shaft.build] [OPTIONS]\n"
                             "--build reads a TOML-like Shaft.build file (or an explicit path)\n"
                             "--emit KIND: llvm, object, asm, staticlib, dynamiclib, binary\n"
                             "-O0, -O1, -O2, or -O3 select LLVM optimization level (default: -O2)\n"
                             "--native tunes native object, assembly, and binary output for this CPU\n"
                             "--target TRIPLE selects an LLVM target triple; Linux binary targets link through LLD (set SHAFT_LLD if needed)\n"
                             "--hosted links a host C runtime (required for --link C libraries)\n"
                             "--link NAME or -lNAME links libNAME; --link-dir PATH adds a C-library search directory\n"
                             "raw .o, .a, .ll, .llvm, and .bc linker inputs may follow the .shaft input\n"
                             "--check-only runs lexing, parsing, and checking without emitting an artifact\n"
                             "--verbose reports compilation stages to stderr\n"
                             "--version prints the compiler version\n"
                             "--no-std disables the automatic standard prelude\n"
                             "--std PATH, --runtime PATH, and --resources PATH override bundled resources\n"
                             "--dump-ast outputs ast as string";
                std::exit(0);
            }
            if (argument == "--emit" || argument == "-emit")
            {
                if (++i == argc)
                    throw std::runtime_error("shaftc: --emit requires a value");
                options.emit = parse_emit_kind(argv[i]);
                continue;
            }
            if (argument.rfind("--emit=", 0) == 0)
            {
                options.emit = parse_emit_kind(argument.substr(7));
                continue;
            }
            if (argument == "-o" || argument == "--output")
            {
                if (++i == argc)
                    throw std::runtime_error("shaftc: -o requires a path");
                options.outputPath = argv[i];
                continue;
            }
            if (argument == "--verbose" || argument == "-v")
            {
                options.verbose = true;
                continue;
            }
            if (argument == "--check-only")
            {
                options.checkOnly = true;
                continue;
            }
            if (!argument.empty() && argument.rfind("-O", 0) == 0)
            {
                options.optimization = parse_optimization_level(argument);
                continue;
            }
            if (argument == "--no-std")
            {
                options.noStd = true;
                continue;
            }
            if (argument == "--hosted")
            {
                options.hosted = true;
                continue;
            }
            if (argument == "--link" || argument == "--link-dir" || argument == "-l")
            {
                if (++i == argc)
                    throw std::runtime_error(argument + " requires a value");
                if (argument == "--link" || argument == "-l")
                    options.linkArguments.emplace_back("-l" + std::string(argv[i]));
                else
                    options.linkDirectories.emplace_back(argv[i]);
                continue;
            }
            if (argument.size() > 2 && argument.rfind("-l", 0) == 0)
            {
                options.linkArguments.emplace_back(argument);
                continue;
            }
            if (argument == "--native")
            {
                options.nativeCpu = true;
                continue;
            }
            if (argument == "--target")
            {
                if (++i == argc)
                    throw std::runtime_error("shaftc: --target requires an LLVM target triple");
                options.targetTriple = argv[i];
                continue;
            }
            if (argument.rfind("--target=", 0) == 0)
            {
                options.targetTriple = argument.substr(9);
                if (options.targetTriple.empty())
                    throw std::runtime_error("shaftc: --target requires an LLVM target triple");
                continue;
            }
            if (argument == "--std" || argument == "--runtime" || argument == "--resources")
            {
                if (++i == argc)
                    throw std::runtime_error(argument + " requires a path");
                if (argument == "--std")
                    options.stdlibPath = argv[i];
                else if (argument == "--runtime")
                    options.runtimePath = argv[i];
                else
                    options.resourcePath = argv[i];
                continue;
            }
            if (!argument.empty() && argument.front() == '-')
                throw std::runtime_error("unknown option '" + argument + "'");
            if (is_raw_link_input(argument))
            {
                options.linkArguments.emplace_back(argument);
                continue;
            }
            if (!options.inputPath.empty())
                throw std::runtime_error("shaftc: only one .shaft input file is supported; use imports for additional Shaft modules");
            options.inputPath = argument;
        }
        if (options.version)
            return options;
        if (options.nativeCpu && !options.targetTriple.empty())
        {
            char *hostTriple = LLVMGetDefaultTargetTriple();
            const std::string host = hostTriple ? hostTriple : "";
            LLVMDisposeMessage(hostTriple);
            if (options.targetTriple != host)
                throw std::runtime_error("shaftc --native cannot be combined with an explicit cross target");
        }
        const bool hasCLinkLibrary = std::any_of(options.linkArguments.begin(), options.linkArguments.end(),
                                                 [](const std::string &argument) { return argument.rfind("-l", 0) == 0; });
        const bool hasRawLinkInput = std::any_of(options.linkArguments.begin(), options.linkArguments.end(),
                                                 [](const std::string &argument) { return is_raw_link_input(argument); });
        if ((hasCLinkLibrary || !options.linkDirectories.empty()) && !options.hosted)
            throw std::runtime_error("--link, -l, and --link-dir require --hosted so linked C libraries receive a C runtime");
        if (hasRawLinkInput && options.emit != EmitKind::Binary && options.emit != EmitKind::DynamicLibrary)
            throw std::runtime_error("raw linker inputs require --emit binary or --emit dynamiclib");
        if (options.hosted && !options.targetTriple.empty())
        {
            char *hostTriple = LLVMGetDefaultTargetTriple();
            const std::string host = hostTriple ? hostTriple : "";
            LLVMDisposeMessage(hostTriple);
            if (options.targetTriple != host)
                throw std::runtime_error("--hosted does not support cross-target linking");
        }
        if (options.inputPath.empty())
            throw std::runtime_error("missing input file");
        if (options.outputPath.empty())
            options.outputPath = default_output_path(options.inputPath, options.emit);
        return options;
    }

    std::string shell_quote(const std::string &value)
    {
#if defined(_WIN32)
        std::string quoted = "\"";
        for (const char c : value)
        {
            if (c == '"')
                quoted += "\\\"";
            else
                quoted += c;
        }
        return quoted + "\"";
#else
        std::string quoted = "'";
        for (const char c : value)
        {
            if (c == '\'')
                quoted += "'\\\"'\\\"'";
            else
                quoted += c;
        }
        return quoted + "'";
#endif
    }

    void run_command(const std::vector<std::string> &arguments)
    {
        std::string command;
        for (const std::string &argument : arguments)
        {
            if (!command.empty())
                command += ' ';
            command += shell_quote(argument);
        }
        if (std::system(command.c_str()) != 0)
            throw std::runtime_error("tool failed while producing the requested artifact");
    }

    std::filesystem::path executable_path(const char *argv0)
    {
#if defined(__linux__)
        std::vector<char> buffer(4096);
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (length > 0)
            return std::filesystem::path(std::string(buffer.data(), static_cast<size_t>(length)));
#elif defined(__APPLE__)
        uint32_t length = 0;
        _NSGetExecutablePath(nullptr, &length);
        std::vector<char> buffer(length);
        if (_NSGetExecutablePath(buffer.data(), &length) == 0)
            return std::filesystem::path(buffer.data());
#elif defined(_WIN32)
        std::vector<char> buffer(32768);
        const DWORD length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length > 0 && length < buffer.size())
            return std::filesystem::path(std::string(buffer.data(), length));
#endif
        return std::filesystem::absolute(argv0);
    }

    std::filesystem::path find_resource(const Options &options, const char *argv0,
                                        const std::filesystem::path &relative)
    {
        std::vector<std::filesystem::path> roots;
        if (!options.resourcePath.empty())
            roots.emplace_back(options.resourcePath);
        if (const char *home = std::getenv("SHAFT_HOME"))
        {
            roots.emplace_back(std::filesystem::path(home) / "share" / "shaft");
            roots.emplace_back(home);
        }
        roots.emplace_back(executable_path(argv0).parent_path() / ".." / "share" / "shaft");
        roots.emplace_back(SHAFT_SOURCE_RESOURCE_DIR);
        for (const std::filesystem::path &root : roots)
        {
            const std::filesystem::path candidate = root / relative;
            if (std::filesystem::is_regular_file(candidate))
                return candidate;
        }
        throw std::runtime_error("unable to find bundled resource '" + relative.string() +
                                 "'; use --resources to set its directory");
    }

    std::vector<ImportedSource> source_modules_with_stdlib(const Options &options, const char *argv0,
                                                        std::vector<ImportedSource> projectModules)
    {
        if (options.noStd)
            return projectModules;
        const std::filesystem::path stdlib = options.stdlibPath.empty()
                                                 ? find_resource(options, argv0, "std/std.shaft")
                                                 : std::filesystem::path(options.stdlibPath);
        if (!std::filesystem::is_regular_file(stdlib))
            throw std::runtime_error("failed to read standard library '" + stdlib.string() + "'");
        if (std::filesystem::equivalent(stdlib, options.inputPath))
            return projectModules;
        std::vector<ImportedSource> modules;
        modules.reserve(projectModules.size() + 1);
        modules.push_back({"std/std.shaft", read_source(stdlib.string()), {}});
        modules.insert(modules.end(), std::make_move_iterator(projectModules.begin()),
                       std::make_move_iterator(projectModules.end()));
        return modules;
    }

    bool target_is_linux(const std::string &triple)
    {
        return triple.find("linux") != std::string::npos;
    }

    std::filesystem::path bundled_runtime(const Options &options, const char *argv0,
                                          const std::string &targetTriple)
    {
        if (!options.runtimePath.empty())
            return options.runtimePath;
        if (target_is_linux(targetTriple))
            return find_resource(options, argv0, "std/runtime/linux.c");
        if (targetTriple.find("darwin") != std::string::npos || targetTriple.find("apple") != std::string::npos)
            return find_resource(options, argv0, "std/runtime/darwin.c");
        if (targetTriple.find("windows") != std::string::npos || targetTriple.find("mingw") != std::string::npos)
            return find_resource(options, argv0, "std/runtime/windows.c");
        throw std::runtime_error("no bundled runtime matches target '" + targetTriple + "'; use --runtime");
    }

    std::string lld_for_linking()
    {
        if (const char *overridePath = std::getenv("SHAFT_LLD"); overridePath && *overridePath)
        {
            if (!std::filesystem::is_regular_file(overridePath))
                throw std::runtime_error("SHAFT_LLD does not name an LLD executable: '" + std::string(overridePath) + "'");
            return overridePath;
        }
        const std::string configured = SHAFT_LLD_PATH;
        if (!configured.empty() && configured.find("NOTFOUND") == std::string::npos &&
            std::filesystem::is_regular_file(configured))
            return configured;
        throw std::runtime_error("linking requires LLD; install ld.lld or set SHAFT_LLD to its absolute path");
    }

    std::string selected_target_triple(const Options &options)
    {
        if (!options.targetTriple.empty())
            return options.targetTriple;
        char *triple = LLVMGetDefaultTargetTriple();
        const std::string result = triple ? triple : "";
        LLVMDisposeMessage(triple);
        return result;
    }

    void initialize_targets()
    {
        LLVMInitializeAllTargetInfos();
        LLVMInitializeAllTargets();
        LLVMInitializeAllTargetMCs();
        LLVMInitializeAllAsmPrinters();
    }

    void validate_target_triple(const std::string &triple)
    {
        initialize_targets();
        LLVMTargetRef target = nullptr;
        char *error = nullptr;
        if (LLVMGetTargetFromTriple(triple.c_str(), &target, &error))
        {
            const std::string message = error ? error : "unable to select target '" + triple + "'";
            LLVMDisposeMessage(error);
            throw std::runtime_error(message);
        }
    }

    struct TargetMachineConfig
    {
        std::string cpu = "generic";
        std::string features;
    };

    TargetMachineConfig selected_target_machine_config(const Options &options)
    {
        TargetMachineConfig config;
        if (!options.nativeCpu)
            return config;
        char *cpu = LLVMGetHostCPUName();
        char *features = LLVMGetHostCPUFeatures();
        if (cpu && *cpu)
            config.cpu = cpu;
        if (features)
            config.features = features;
        LLVMDisposeMessage(cpu);
        LLVMDisposeMessage(features);
        return config;
    }

    LLVMTargetMachineRef create_target_machine(LLVMTargetRef target, const std::string &triple,
                                               const Options &options)
    {
        const TargetMachineConfig config = selected_target_machine_config(options);
        return LLVMCreateTargetMachine(target, triple.c_str(), config.cpu.c_str(), config.features.c_str(),
                                       LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);
    }

    unsigned target_pointer_width_bits(LLVMContextRef llvmContext, const std::string &triple,
                                       const Options &options)
    {
        initialize_targets();
        LLVMTargetRef target = nullptr;
        char *error = nullptr;
        if (LLVMGetTargetFromTriple(triple.c_str(), &target, &error))
        {
            const std::string message = error ? error : "unable to select a target";
            LLVMDisposeMessage(error);
            throw std::runtime_error(message);
        }
        LLVMTargetMachineRef machine = create_target_machine(target, triple, options);
        if (!machine)
            throw std::runtime_error("unable to create a target machine");
        LLVMTargetDataRef dataLayout = LLVMCreateTargetDataLayout(machine);
        const unsigned bits = static_cast<unsigned>(
            LLVMABISizeOfType(dataLayout, LLVMPointerType(LLVMInt8TypeInContext(llvmContext), 0)) * 8);
        LLVMDisposeTargetData(dataLayout);
        LLVMDisposeTargetMachine(machine);
        return bits;
    }

    const char *optimization_pipeline(OptimizationLevel level)
    {
        switch (level)
        {
        case OptimizationLevel::O1: return "default<O1>";
        case OptimizationLevel::O2: return "default<O2>";
        case OptimizationLevel::O3: return "default<O3>";
        case OptimizationLevel::O0: return nullptr;
        }
        return nullptr;
    }

    void optimize_module(LLVMModuleRef module, const Options &options)
    {
        const char *pipeline = optimization_pipeline(options.optimization);
        if (!pipeline)
            return;

        initialize_targets();
        const std::string triple = selected_target_triple(options);
        LLVMTargetRef target = nullptr;
        char *targetError = nullptr;
        if (LLVMGetTargetFromTriple(triple.c_str(), &target, &targetError))
        {
            const std::string message = targetError ? targetError : "unable to select a target";
            LLVMDisposeMessage(targetError);
            throw std::runtime_error(message);
        }
        LLVMTargetMachineRef machine = create_target_machine(target, triple, options);
        if (!machine)
            throw std::runtime_error("unable to create a target machine");

        LLVMSetTarget(module, triple.c_str());
        LLVMTargetDataRef dataLayout = LLVMCreateTargetDataLayout(machine);
        char *layout = LLVMCopyStringRepOfTargetData(dataLayout);
        LLVMSetDataLayout(module, layout);
        LLVMDisposeMessage(layout);
        LLVMDisposeTargetData(dataLayout);

        LLVMPassBuilderOptionsRef passOptions = LLVMCreatePassBuilderOptions();
        LLVMErrorRef passError = LLVMRunPasses(module, pipeline, machine, passOptions);
        LLVMDisposePassBuilderOptions(passOptions);
        LLVMDisposeTargetMachine(machine);
        if (passError)
        {
            char *message = LLVMGetErrorMessage(passError);
            const std::string text = message ? message : "LLVM optimization failed";
            LLVMDisposeErrorMessage(message);
            throw std::runtime_error("optimization failed: " + text);
        }
    }

    void emit_native(LLVMModuleRef module, const std::string &path, LLVMCodeGenFileType fileType,
                     const std::string &triple, const Options &options)
    {
        initialize_targets();

        char *error = nullptr;
        LLVMTargetRef target = nullptr;
        if (LLVMGetTargetFromTriple(triple.c_str(), &target, &error))
        {
            const std::string message = error ? error : "unable to select a target";
            LLVMDisposeMessage(error);
            throw std::runtime_error(message);
        }

        LLVMTargetMachineRef machine = create_target_machine(target, triple, options);
        if (!machine)
            throw std::runtime_error("unable to create a target machine");
        LLVMSetTarget(module, triple.c_str());

        LLVMTargetDataRef dataLayout = LLVMCreateTargetDataLayout(machine);
        char *layout = LLVMCopyStringRepOfTargetData(dataLayout);
        LLVMSetDataLayout(module, layout);
        LLVMDisposeMessage(layout);
        LLVMDisposeTargetData(dataLayout);

        if (LLVMTargetMachineEmitToFile(machine, module, const_cast<char *>(path.c_str()), fileType, &error))
        {
            const std::string message = error ? error : "unable to emit native code";
            LLVMDisposeMessage(error);
            LLVMDisposeTargetMachine(machine);
            throw std::runtime_error(message);
        }
        LLVMDisposeTargetMachine(machine);
    }

    std::filesystem::path temporary_object_path()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path() / ("shaftc-" + std::to_string(stamp) + ".o");
    }

    std::filesystem::path temporary_hosted_bridge_path()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path() / ("shaftc-hosted-" + std::to_string(stamp) + ".c");
    }

    void emit_artifact(LLVMModuleRef module, const Options &options, const char *argv0)
    {
        const std::string targetTriple = selected_target_triple(options);
        if (options.emit == EmitKind::LLVM)
        {
            LLVMSetTarget(module, targetTriple.c_str());
            char *error = nullptr;
            if (LLVMPrintModuleToFile(module, options.outputPath.c_str(), &error))
            {
                const std::string message = error ? error : "unable to write LLVM IR";
                LLVMDisposeMessage(error);
                throw std::runtime_error(message);
            }
            return;
        }
        if (options.emit == EmitKind::Object)
        {
            emit_native(module, options.outputPath, LLVMObjectFile, targetTriple, options);
            return;
        }
        if (options.emit == EmitKind::Assembly)
        {
            emit_native(module, options.outputPath, LLVMAssemblyFile, targetTriple, options);
            return;
        }

        const std::filesystem::path objectPath = temporary_object_path();
        std::filesystem::path hostedBridge;
        try
        {
            emit_native(module, objectPath.string(), LLVMObjectFile, targetTriple, options);
            if (options.emit == EmitKind::StaticLibrary)
                run_command({SHAFT_LLVM_AR_PATH, "rcs", options.outputPath, objectPath.string()});
            else if (options.emit == EmitKind::DynamicLibrary)
            {
                const std::string lld = lld_for_linking();
                std::vector<std::string> linker{SHAFT_CLANG_PATH, "-fuse-ld=" + lld, "-shared", "-nostdlib", objectPath.string()};
                linker.insert(linker.end(), options.linkArguments.begin(), options.linkArguments.end());
                linker.emplace_back("-o");
                linker.emplace_back(options.outputPath);
                run_command(linker);
            }
            else
            {
                const std::string lld = lld_for_linking();
                if (options.hosted)
                {
                    std::vector<std::string> linker{SHAFT_CLANG_PATH, "-fuse-ld=" + lld, objectPath.string()};
                    if (!options.noStd)
                    {
                        const std::filesystem::path runtime = bundled_runtime(options, argv0, targetTriple);
                        if (!std::filesystem::is_regular_file(runtime))
                            throw std::runtime_error("failed to read runtime '" + runtime.string() + "'");
                        linker.emplace_back("-DSHAFT_HOSTED");
                        linker.emplace_back(runtime.string());
                    }
                    else
                    {
                        LLVMValueRef entry = LLVMGetNamedFunction(module, "__main");
                        if (!entry || LLVMCountParams(entry) != 0 ||
                            LLVMGetTypeKind(LLVMGetReturnType(LLVMGlobalGetValueType(entry))) != LLVMIntegerTypeKind ||
                            LLVMGetIntTypeWidth(LLVMGetReturnType(LLVMGlobalGetValueType(entry))) != 32)
                        {
                            throw std::runtime_error("--no-std --hosted requires cdef main() -> i32");
                        }
                        hostedBridge = temporary_hosted_bridge_path();
                        std::ofstream bridge(hostedBridge);
                        if (!bridge)
                            throw std::runtime_error("failed to create hosted C entry bridge");
                        bridge << "extern int __main(void); int main(void) { return __main(); }\n";
                        linker.emplace_back(hostedBridge.string());
                    }
                    for (const std::string &directory : options.linkDirectories)
                    {
                        linker.emplace_back("-L" + directory);
                        linker.emplace_back("-Wl,-rpath," + directory);
                    }
                    linker.insert(linker.end(), options.linkArguments.begin(), options.linkArguments.end());
                    linker.emplace_back("-o");
                    linker.emplace_back(options.outputPath);
                    run_command(linker);
                }
                else
                {
                    const std::filesystem::path runtime = bundled_runtime(options, argv0, targetTriple);
                    if (!std::filesystem::is_regular_file(runtime))
                        throw std::runtime_error("failed to read runtime '" + runtime.string() + "'");
                    if (!options.targetTriple.empty())
                    {
                        if (!target_is_linux(targetTriple))
                            throw std::runtime_error("cross-target binary linking currently supports Linux targets; emit an object for other targets");
                        std::vector<std::string> linker{SHAFT_CLANG_PATH, "--target=" + targetTriple, "-fuse-ld=" + lld,
                                                        "-nostdlib", "-static", "-ffreestanding", "-fno-stack-protector",
                                                        objectPath.string(), runtime.string()};
                        linker.insert(linker.end(), options.linkArguments.begin(), options.linkArguments.end());
                        linker.emplace_back("-Wl,-e,_start");
                        linker.emplace_back("-o");
                        linker.emplace_back(options.outputPath);
                        run_command(linker);
                    }
                    else
                    {
                        std::vector<std::string> linker{SHAFT_CLANG_PATH, "-fuse-ld=" + lld, "-nostdlib", "-static",
                                                        "-ffreestanding", "-fno-stack-protector", objectPath.string(), runtime.string()};
                        linker.insert(linker.end(), options.linkArguments.begin(), options.linkArguments.end());
                        linker.emplace_back("-Wl,-e,_start");
                        linker.emplace_back("-o");
                        linker.emplace_back(options.outputPath);
                        run_command(linker);
                    }
                }
            }
            std::filesystem::remove(objectPath);
            if (!hostedBridge.empty())
                std::filesystem::remove(hostedBridge);
        }
        catch (...)
        {
            if (!hostedBridge.empty())
                std::filesystem::remove(hostedBridge);
            std::filesystem::remove(objectPath);
            throw;
        }
    }
} // namespace

int main(int argc, char **argv)
{
    Codegen::Context context;
    auto dispose_context = [&context]()
    {
        if (context.builder)
            LLVMDisposeBuilder(context.builder);
        if (context.module)
            LLVMDisposeModule(context.module);
        if (context.llvmCtx)
            LLVMContextDispose(context.llvmCtx);
    };

    const Options options = parse_options(argc, argv);
    if (options.version)
    {
        std::cout << "shaftc " << SHAFT_VERSION << '\n';
        return 0;
    }
    if (!options.targetTriple.empty())
        validate_target_triple(options.targetTriple);
    verbose(options, "reading " + options.inputPath);
    verbose(options, "target " + selected_target_triple(options));
    const std::string inputSource = read_source(options.inputPath);
    if (inputSource.empty())
        throw std::runtime_error("shaftc: error: source file is empty");
    const std::vector<ImportedSource> projectModules = load_project_modules(options.inputPath);
    const std::vector<ImportedSource> sourceModules =
        source_modules_with_stdlib(options, argv[0], projectModules);

    verbose(options, "lexing and parsing");
    std::vector<Lexer::Module> raw_modules;
    for (ImportedSource s : sourceModules)
    {
        raw_modules.push_back({s.path, s.source});
    }
    Parser::parse(Lexer::tokenize_modules(raw_modules));
    verbose(options, "checking");
    Checker::set_stdlib_enabled(!options.noStd);
    Checker::run_checker();
    if (options.checkOnly)
    {
        verbose(options, "check completed");
        return 0;
    }
    
    const std::filesystem::path outputParent = std::filesystem::path(options.outputPath).parent_path();
    if (!outputParent.empty())
    {
        std::error_code error;
        std::filesystem::create_directories(outputParent, error);
        if (error)
            throw std::runtime_error("shaftc: failed to create output directory '" + outputParent.string() + "': " +
                                      error.message());
            exit(1);
    }
    verbose(options, "generating LLVM IR...");
    context = Codegen::create_context(options.inputPath.c_str());
    context.stdlibEnabled = !options.noStd;
    context.targetPointerWidthBits =
        target_pointer_width_bits(context.llvmCtx, selected_target_triple(options), options);
    LLVMModuleRef module = Codegen::generate_module(context, Parser::ast);

    char *diagnostic = nullptr;
    if (LLVMVerifyModule(module, LLVMReturnStatusAction, &diagnostic))
    {
        const std::string message = diagnostic ? diagnostic : "invalid LLVM IR";
        LLVMDisposeMessage(diagnostic);
        throw std::runtime_error("shaftc: code generation failed: " + message);
    }
    if (diagnostic)
        LLVMDisposeMessage(diagnostic);
    if (options.optimization != OptimizationLevel::O0)
    {
        verbose(options, "optimizing LLVM IR at -" + std::string(optimization_name(options.optimization)));
        optimize_module(module, options);
        diagnostic = nullptr;
        if (LLVMVerifyModule(module, LLVMReturnStatusAction, &diagnostic))
        {
            const std::string message = diagnostic ? diagnostic : "invalid optimized LLVM IR";
            LLVMDisposeMessage(diagnostic);
            throw std::runtime_error("shaftc: optimization produced invalid LLVM IR: " + message);
        }
        if (diagnostic)
            LLVMDisposeMessage(diagnostic);
        verbose(options, "emitting " + std::string(emit_name(options.emit)) + " to " + options.outputPath);
        emit_artifact(module, options, argv[0]);
    }
    bool dump_ast = false;

    for (int i = 1; i < argc; i++) 
    {
        std::string arg = argv[i];
        if (arg == "--dump-ast") 
        {
            dump_ast = true;
        }
    }

    if (dump_ast) Parser::dump_ast(Parser::ast);

    dispose_context();
    return 0;
}
