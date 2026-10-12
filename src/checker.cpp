#include "checker.hpp"
#include "parser.hpp"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <unordered_set>

namespace Checker
{
    class CheckerRecovery final
    {
    };

    class SymbolTable;
    void check_node(SymbolTable &table, Parser::ASTNode &node);
    static unsigned unsafeDepth = 0;

    static bool is_trusted_stdlib_implementation(const Parser::ASTNode &node)
    {
        if (!node.mod_path)
            return false;
        const std::string_view path = *node.mod_path;
        return path.rfind("std/", 0) == 0 || path.find("/std/") != std::string_view::npos;
    }

    void error(std::string message, Parser::ASTNode node, std::string help = {})
    {
        ErrorPos error_pos = get_error_pos(node.start, node.source);
        Error err = {message,        node.mod_path ? *node.mod_path : std::string{},
                     error_pos.line, error_pos.column,
                     false,          std::move(help)};
        if (global_stop_on_error)
            panic(std::move(err));
        record_diagnostic(std::move(err), node.source);
        throw CheckerRecovery{};
    }

    static bool type_contains_raw_pointer(const Type &type)
    {
        if (type.kind == TypeKind::Pointer)
            return true;
        if (type.innerType && type_contains_raw_pointer(*type.innerType))
            return true;
        return std::any_of(type.genericArgs.begin(), type.genericArgs.end(), type_contains_raw_pointer);
    }

    static void validate_unsafe_constructs(Parser::ASTNode &node, bool inUnsafe = false)
    {
        if (node.type == Parser::NodeType::UnsafeBlock)
        {
            for (auto &child : node.children)
                validate_unsafe_constructs(child, true);
            return;
        }
        if (node.type == Parser::NodeType::NoCheckBlock || node.type == Parser::NodeType::NoCleanupBlock)
        {
            for (auto &child : node.children)
                validate_unsafe_constructs(child, inUnsafe);
            return;
        }
        const bool cAbi = node.type == Parser::NodeType::CFunctionDecl || node.type == Parser::NodeType::CFunctionDef;
        const bool rawPointer = node.type == Parser::NodeType::PointerType;
        const bool rawUnary = node.type == Parser::NodeType::UnaryExpr &&
                              std::holds_alternative<Lexer::Operator>(node.value) &&
                              (std::get<Lexer::Operator>(node.value) == Lexer::Operator::MULTIPLY ||
                               std::get<Lexer::Operator>(node.value) == Lexer::Operator::AMPERSAND);
        if (!inUnsafe && !is_trusted_stdlib_implementation(node) &&
            (cAbi || rawPointer || rawUnary || node.type == Parser::NodeType::InlineAsmStmt))
        {
            error("raw pointers, dereferencing, C-ABI declarations, and inline assembly require an enclosing "
                  "@unsafe ... @end block.",
                  node);
        }
        for (auto &child : node.children)
            validate_unsafe_constructs(child, inUnsafe);
    }

    static bool is_hexadecimal_digit(char character)
    {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
               (character >= 'A' && character <= 'F');
    }

    static void validate_brace_format_call(const Parser::ASTNode &callee, const Parser::ASTNode &templateNode,
                                           const std::vector<Type> &arguments)
    {
        const std::string_view text = std::get<std::string_view>(templateNode.value);
        size_t placeholders = 0;
        for (size_t index = 0; index < text.size(); ++index)
        {
            if (text[index] == '{')
            {
                if (index + 1 < text.size() && text[index + 1] == '{')
                {
                    ++index;
                    continue;
                }
                if (index + 1 < text.size() && text[index + 1] == '}')
                {
                    ++placeholders;
                    ++index;
                    continue;
                }
                error("format! placeholders must be '{}' and literal '{' must be written '{{'.", templateNode);
            }
            if (text[index] == '}')
            {
                if (index + 1 < text.size() && text[index + 1] == '}')
                {
                    ++index;
                    continue;
                }
                error("format! literal '}' must be written '}}'.", templateNode);
            }
        }
        if (placeholders != arguments.size())
            error("format! placeholder count does not match its argument count.", callee);
        for (const Type &argument : arguments)
        {
            switch (argument.kind)
            {
            case TypeKind::U8:
            case TypeKind::U16:
            case TypeKind::U32:
            case TypeKind::U64:
            case TypeKind::USIZE:
            case TypeKind::I8:
            case TypeKind::I16:
            case TypeKind::I32:
            case TypeKind::I64:
            case TypeKind::F32:
            case TypeKind::F64:
            case TypeKind::Bool:
            case TypeKind::Char:
            case TypeKind::String:
                break;
            case TypeKind::Struct:
                if (argument.name == "std::string::view::View" || argument.name == "std::string::String")
                    break;
                [[fallthrough]];
            case TypeKind::Pointer:
                if (argument.innerType && argument.innerType->kind == TypeKind::U8)
                    break;
                [[fallthrough]];
            default:
                error(
                    "format! supports only primitive values, std::string::view::View, std::string::String, and cstr!.",
                    callee);
            }
        }
    }

    static void validate_string_literal_escapes(const Parser::ASTNode &node)
    {
        const std::string_view literal = std::get<std::string_view>(node.value);
        for (size_t index = 0; index < literal.size(); ++index)
        {
            if (literal[index] != '\\' || ++index == literal.size())
                continue;

            const size_t digits = literal[index] == 'x' ? 2 : literal[index] == 'u' ? 4 : 0;
            if (digits == 0)
                continue;
            if (index + digits >= literal.size())
                error(literal[index] == 'x' ? "Incomplete hexadecimal string escape."
                                            : "Incomplete Unicode string escape.",
                      node);

            unsigned codepoint = 0;
            for (size_t digit = 1; digit <= digits; ++digit)
            {
                if (!is_hexadecimal_digit(literal[index + digit]))
                    error("Invalid hexadecimal string escape.", node);
                codepoint =
                    (codepoint << 4) | static_cast<unsigned>(std::isdigit(literal[index + digit])
                                                                 ? literal[index + digit] - '0'
                                                                 : std::tolower(literal[index + digit]) - 'a' + 10);
            }
            if (literal[index] == 'u' && codepoint >= 0xd800 && codepoint <= 0xdfff)
                error("Unicode string escape cannot encode a surrogate.", node);
            index += digits;
        }
    }

    static void validate_string_literal_escapes_in_tree(const Parser::ASTNode &node)
    {
        if (node.type == Parser::NodeType::StringLiteral)
            validate_string_literal_escapes(node);
        for (const Parser::ASTNode &child : node.children)
            validate_string_literal_escapes_in_tree(child);
    }
    class SymbolTable
    {
      private:
        std::vector<std::unordered_map<std::string, Symbol>> scopes;
        std::vector<std::string> importedNamespaces;
        struct BorrowRecord
        {
            size_t scopeIndex;
            std::string name;
            bool isMutable;
        };
        std::vector<std::vector<BorrowRecord>> scopeBorrows;

      public:
        void push_scope()
        {
            scopes.push_back({});
            scopeBorrows.push_back({});
        }

        void pop_scope()
        {
            for (const BorrowRecord &record : scopeBorrows.back())
            {
                auto source = scopes[record.scopeIndex].find(record.name);
                if (source == scopes[record.scopeIndex].end())
                    continue;
                if (record.isMutable)
                    source->second.has_writer = false;
                else if (source->second.borrowCount > 0)
                    --source->second.borrowCount;
                if (source->second.borrowCount == 0 && !source->second.has_writer)
                    source->second.state = VarOwnershipState::Valid;
            }
            scopeBorrows.pop_back();
            scopes.pop_back();
        }

        bool declare(const std::string &name, Symbol sym)
        {
            if (scopes.back().find(name) != scopes.back().end())
                return false; // Redeclaration error
            scopes.back()[name] = sym;
            return true;
        }

        Symbol *lookup(const std::string &name)
        {
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            {
                auto found = it->find(name);
                if (found != it->end())
                    return &found->second;
            }
            if (name.find("::") == std::string::npos)
            {
                for (auto imported = importedNamespaces.rbegin(); imported != importedNamespaces.rend(); ++imported)
                {
                    const std::string qualified = *imported + "::" + name;
                    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
                    {
                        auto found = it->find(qualified);
                        if (found != it->end())
                            return &found->second;
                    }
                }
            }
            return nullptr;
        }

        void import_namespace(std::string name)
        {
            if (std::find(importedNamespaces.begin(), importedNamespaces.end(), name) == importedNamespaces.end())
                importedNamespaces.push_back(std::move(name));
        }

        bool record_borrow(const std::string &name, bool isMutable)
        {
            for (size_t index = scopes.size(); index-- > 0;)
            {
                auto found = scopes[index].find(name);
                if (found == scopes[index].end())
                    continue;
                Symbol &source = found->second;
                if (isMutable)
                {
                    if (source.borrowCount != 0 || source.has_writer)
                        return false;
                    source.has_writer = true;
                    source.state = VarOwnershipState::BorrowedMut;
                }
                else
                {
                    if (source.has_writer)
                        return false;
                    ++source.borrowCount;
                    source.state = VarOwnershipState::BorrowedRead;
                }
                scopeBorrows.back().push_back({index, name, isMutable});
                return true;
            }
            return false;
        }
    };

    namespace
    {
        std::unordered_map<std::string, TypeDefinition> registeredTypes;
        std::vector<std::string> activeNamespaces;
        bool predeclaringTypeShells = false;

        std::string current_namespace_name()
        {
            std::string name;
            for (const std::string &part : activeNamespaces)
            {
                if (!name.empty())
                    name += "::";
                name += part;
            }
            return name;
        }

        std::string declared_type_name(const Parser::ASTNode &node)
        {
            return node.inferredTypeName.empty() ? std::string(std::get<std::string_view>(node.children.front().value))
                                                 : node.inferredTypeName;
        }
        std::vector<std::string> activeGenericNames;
        std::vector<std::string> structNameStack;

        bool inCFunctionBody = false;
        bool stdlibEnabled = false;
        size_t functionBodyDepth = 0;
        Type activeCFunctionReturnType;
        bool activeCFunctionReturnsValue = false;
        size_t loopDepth = 0;
        std::unordered_map<std::string, TunnelSlotState> activeTunnelSlots;

        bool types_structurally_equal(const Type &left, const Type &right)
        {
            if (left.kind != right.kind || left.name != right.name || left.isOptional != right.isOptional ||
                left.isMutable != right.isMutable || left.genericArgs.size() != right.genericArgs.size() ||
                static_cast<bool>(left.innerType) != static_cast<bool>(right.innerType))
                return false;
            if (left.innerType && !types_structurally_equal(*left.innerType, *right.innerType))
                return false;
            for (size_t index = 0; index < left.genericArgs.size(); ++index)
                if (!types_structurally_equal(left.genericArgs[index], right.genericArgs[index]))
                    return false;
            return true;
        }

        bool is_active_generic_name(const std::string &name)
        {
            for (const auto &n : activeGenericNames)
                if (n == name)
                    return true;
            return false;
        }

        size_t push_generic_params(const std::vector<Parser::ASTNode> &children)
        {
            size_t pushed = 0;
            for (const auto &child : children)
            {
                if (child.type == Parser::NodeType::GenericParam)
                {
                    activeGenericNames.push_back(std::string(std::get<std::string_view>(child.value)));
                    pushed++;
                }
            }
            return pushed;
        }

        void pop_generic_params(size_t count)
        {
            for (size_t i = 0; i < count; i++)
                activeGenericNames.pop_back();
        }

        std::string type_to_string(const Type &type)
        {
            switch (type.kind)
            {
            case TypeKind::U8:
                return "u8";
            case TypeKind::U16:
                return "u16";
            case TypeKind::U32:
                return "u32";
            case TypeKind::U64:
                return "u64";
            case TypeKind::USIZE:
                return "usize";
            case TypeKind::I8:
                return "i8";
            case TypeKind::I16:
                return "i16";
            case TypeKind::I32:
                return "i32";
            case TypeKind::I64:
                return "i64";
            case TypeKind::F32:
                return "f32";
            case TypeKind::F64:
                return "f64";
            case TypeKind::Bool:
                return "bool";
            case TypeKind::Char:
                return "char";
            case TypeKind::State:
                return "State";
            case TypeKind::Thread:
                return "Thread";
            case TypeKind::Struct:
            case TypeKind::Class:
            case TypeKind::Enum:
            case TypeKind::Union:
                return type.name.empty() ? "user-defined" : type.name;
            case TypeKind::Generic:
                return type.name.empty() ? "<generic>" : type.name;
            case TypeKind::Pointer:
                return "pointer";
            case TypeKind::Reference:
                return "reference";
            case TypeKind::Optional:
                return "optional";
            case TypeKind::Array:
                return "array";
            case TypeKind::Void:
                return "void";
            default:
                return "error";
            }
        }

        std::string explicit_cast_help(const Type &destination)
        {
            return "Consider an explicit `as " + type_to_string(destination) + "` conversion.";
        }

        TypeKind keyword_to_type_kind(Lexer::Keyword keyword)
        {
            switch (keyword)
            {
            case Lexer::Keyword::U8:
                return TypeKind::U8;
            case Lexer::Keyword::U16:
                return TypeKind::U16;
            case Lexer::Keyword::U32:
                return TypeKind::U32;
            case Lexer::Keyword::U64:
                return TypeKind::U64;
            case Lexer::Keyword::USIZE:
                return TypeKind::USIZE;
            case Lexer::Keyword::I8:
                return TypeKind::I8;
            case Lexer::Keyword::I16:
                return TypeKind::I16;
            case Lexer::Keyword::I32:
                return TypeKind::I32;
            case Lexer::Keyword::I64:
                return TypeKind::I64;
            case Lexer::Keyword::F32:
                return TypeKind::F32;
            case Lexer::Keyword::F64:
                return TypeKind::F64;
            case Lexer::Keyword::BOOL:
                return TypeKind::Bool;
            case Lexer::Keyword::CHAR:
                return TypeKind::Char;
            case Lexer::Keyword::STATE:
                return TypeKind::State;
            case Lexer::Keyword::THREAD:
                return TypeKind::Thread;
            default:
                return TypeKind::Error;
            }
        }

        bool is_numeric(TypeKind kind)
        {
            switch (kind)
            {
            case TypeKind::U8:
            case TypeKind::U16:
            case TypeKind::U32:
            case TypeKind::U64:
            case TypeKind::USIZE:
            case TypeKind::I8:
            case TypeKind::I16:
            case TypeKind::I32:
            case TypeKind::I64:
            case TypeKind::F32:
            case TypeKind::F64:
            case TypeKind::Char:
            case TypeKind::Pointer:
                return true;
            default:
                return false;
            }
        }

        bool is_integer(TypeKind kind)
        {
            switch (kind)
            {
            case TypeKind::U8:
            case TypeKind::U16:
            case TypeKind::U32:
            case TypeKind::U64:
            case TypeKind::USIZE:
            case TypeKind::I8:
            case TypeKind::I16:
            case TypeKind::I32:
            case TypeKind::I64:
            case TypeKind::Char:
                return true;
            default:
                return false;
            }
        }

        bool is_signed_integer(TypeKind kind)
        {
            return kind == TypeKind::I8 || kind == TypeKind::I16 || kind == TypeKind::I32 || kind == TypeKind::I64;
        }

        unsigned integer_width(TypeKind kind)
        {
            switch (kind)
            {
            case TypeKind::U8:
            case TypeKind::I8:
                return 8;
            case TypeKind::U16:
            case TypeKind::I16:
                return 16;
            case TypeKind::U32:
            case TypeKind::I32:
            case TypeKind::Char:
                return 32;
            case TypeKind::U64:
            case TypeKind::I64:
            case TypeKind::USIZE:
                return 64;
            default:
                return 0;
            }
        }

        bool can_widen_integer(TypeKind destination, TypeKind source)
        {
            if (!is_integer(destination) || !is_integer(source))
                return false;
            const unsigned destinationWidth = integer_width(destination);
            const unsigned sourceWidth = integer_width(source);
            if (is_signed_integer(source) && !is_signed_integer(destination))
                return false;
            return is_signed_integer(source) == is_signed_integer(destination) ? destinationWidth >= sourceWidth
                                                                               : destinationWidth > sourceWidth;
        }

        std::string custom_type_name(const Parser::ASTNode &node)
        {
            return node.inferredTypeName.empty() ? std::string(std::get<std::string_view>(node.value))
                                                 : node.inferredTypeName;
        }

        Type infer_type_from_node(const Parser::ASTNode &node)
        {
            switch (node.type)
            {
            case Parser::NodeType::PrimitiveType:
            {
                Type type(keyword_to_type_kind(std::get<Lexer::Keyword>(node.value)));
                type.isMutable = node.isMutable;
                return type;
            }
            case Parser::NodeType::PointerType:
            {
                Type type{TypeKind::Pointer, "*"};
                type.isMutable = node.isMutable;
                if (!node.children.empty())
                    type.innerType = std::make_unique<Type>(infer_type_from_node(node.children.front()));
                return type;
            }
            case Parser::NodeType::ReferenceType:
            {
                Type type{TypeKind::Reference, "&"};
                type.isMutable = node.isMutable || (node.value.index() != 0 && std::get<bool>(node.value));
                if (!node.children.empty())
                    type.innerType = std::make_unique<Type>(infer_type_from_node(node.children.front()));
                return type;
            }
            case Parser::NodeType::OptionalType:
            {
                Type type{TypeKind::Optional, "?"};
                type.isOptional = true;
                type.isMutable = node.isMutable;
                if (!node.children.empty())
                    type.innerType = std::make_unique<Type>(infer_type_from_node(node.children.front()));
                return type;
            }
            case Parser::NodeType::CustomType:
            {
                std::string typeName = custom_type_name(node);

                if (registeredTypes.find(typeName) == registeredTypes.end())
                {
                    for (const std::string &usingName : Parser::using_namespace_declarations)
                    {
                        const size_t separator = usingName.rfind("::");
                        if (separator == std::string::npos)
                        {
                            const std::string qualified = usingName + "::" + typeName;
                            if (registeredTypes.find(qualified) != registeredTypes.end())
                            {
                                typeName = qualified;
                                break;
                            }
                            continue;
                        }

                        const std::string importedName = usingName.substr(separator + 2);

                        if (importedName == typeName && registeredTypes.find(usingName) != registeredTypes.end())
                        {
                            typeName = usingName;
                            break;
                        }
                    }
                }
                auto mutable_type = [&node](Type type)
                {
                    type.isMutable = node.isMutable;
                    for (const Parser::ASTNode &argument : node.children)
                        type.genericArgs.push_back(infer_type_from_node(argument));
                    return type;
                };

                const std::string unqualifiedTypeName =
                    typeName.compare(0, 5, "std::") == 0 ? typeName.substr(5) : typeName;
                if (typeName == "int")
                    return mutable_type(Type(TypeKind::I32));
                if ((unqualifiedTypeName == "String" || unqualifiedTypeName == "str") &&
                    registeredTypes.find(typeName) != registeredTypes.end())
                    return mutable_type(Type(TypeKind::String, typeName));

                if (typeName == "Self" && !structNameStack.empty())
                {
                    const std::string &structName = structNameStack.back();
                    auto structIt = registeredTypes.find(structName);
                    if (structIt != registeredTypes.end() && structIt->second.isEnum)
                        return mutable_type(Type(TypeKind::Enum, structName));
                    if (structIt != registeredTypes.end() && structIt->second.isUnion)
                        return mutable_type(Type(TypeKind::Union, structName));
                    return mutable_type(Type(TypeKind::Struct, structName));
                }

                if (is_active_generic_name(typeName))
                    return mutable_type(Type(TypeKind::Generic, typeName));

                auto it = registeredTypes.find(typeName);
                if (it != registeredTypes.end() && it->second.isEnum)
                    return mutable_type(Type(TypeKind::Enum, typeName));
                if (it != registeredTypes.end() && it->second.isUnion)
                    return mutable_type(Type(TypeKind::Union, typeName));
                if (it != registeredTypes.end() && it->second.isClass)
                    return mutable_type(Type(TypeKind::Class, typeName));
                if (it != registeredTypes.end())
                    return mutable_type(Type(TypeKind::Struct, typeName));
                if (typeName.size() == 1 && typeName.front() >= 'A' && typeName.front() <= 'Z')
                    return mutable_type(Type(TypeKind::Generic, typeName));
                error("Unknown type '" + typeName + "'.\n", node);
                exit(1);
            }
            case Parser::NodeType::ArrayType:
            {
                Type type{TypeKind::Array, "[]"};
                type.isMutable = node.isMutable;
                if (!node.children.empty())
                    type.innerType = std::make_unique<Type>(infer_type_from_node(node.children.front()));
                return type;
            }
            default:
                error("Cannot infer type from this node.", node);
                return {TypeKind::Error, {}};
            }
        }

        struct OwnershipPath
        {
            std::string root;
            std::string relative;
        };

        OwnershipPath ownership_path(const Parser::ASTNode &node)
        {
            if (node.type == Parser::NodeType::Identifier)
                return {std::string(std::get<std::string_view>(node.value)), {}};
            if (node.type == Parser::NodeType::MemberAccessExpr && !node.children.empty())
            {
                OwnershipPath path = ownership_path(node.children.front());
                if (!path.root.empty())
                {
                    if (!path.relative.empty())
                        path.relative += ".";
                    path.relative += std::string(std::get<std::string_view>(node.value));
                }
                return path;
            }
            if (node.type == Parser::NodeType::IndexExpr && node.children.size() == 2)
            {
                OwnershipPath path = ownership_path(node.children.front());
                if (path.root.empty())
                    return {};
                if (node.children[1].type == Parser::NodeType::IntegerLiteral)
                    path.relative += "[" + std::to_string(std::get<uint64_t>(node.children[1].value)) + "]";
                return path;
            }
            return {};
        }

        bool path_is_at_or_below(const std::string &path, const std::string &ancestor)
        {
            return path == ancestor ||
                   (path.size() > ancestor.size() && path.compare(0, ancestor.size(), ancestor) == 0 &&
                    (path[ancestor.size()] == '.' || path[ancestor.size()] == '['));
        }

        bool subobject_is_moved(const Symbol &symbol, const std::string &path)
        {
            for (const auto &[movedPath, state] : symbol.subobjectStates)
            {
                if (state == VarOwnershipState::Moved && path_is_at_or_below(path, movedPath))
                    return true;
            }
            return false;
        }

        bool subobject_contains_moved_value(const Symbol &symbol, const std::string &path)
        {
            for (const auto &[movedPath, state] : symbol.subobjectStates)
            {
                if (state == VarOwnershipState::Moved && path_is_at_or_below(movedPath, path))
                    return true;
            }
            return false;
        }

        void restore_subobject(Symbol &symbol, const std::string &path)
        {
            for (auto entry = symbol.subobjectStates.begin(); entry != symbol.subobjectStates.end();)
            {
                if (path_is_at_or_below(entry->first, path))
                    entry = symbol.subobjectStates.erase(entry);
                else
                    ++entry;
            }
        }

        std::string owning_root_name(const Parser::ASTNode &node)
        {
            if (node.type == Parser::NodeType::Identifier)
                return std::string(std::get<std::string_view>(node.value));
            if ((node.type == Parser::NodeType::MemberAccessExpr || node.type == Parser::NodeType::IndexExpr) &&
                !node.children.empty())
                return owning_root_name(node.children.front());
            return {};
        }

        std::string scope_access_name(const Parser::ASTNode &node)
        {
            if (node.type == Parser::NodeType::Identifier)
                return std::string(std::get<std::string_view>(node.value));
            if (node.type == Parser::NodeType::ScopeAccessExpr && !node.children.empty())
                return scope_access_name(node.children.front()) +
                       "::" + std::string(std::get<std::string_view>(node.value));
            return {};
        }

        bool type_requires_cleanup(const Type &type, std::unordered_set<std::string> &visiting)
        {
            if (type.kind == TypeKind::Pointer || type.kind == TypeKind::Reference || type.kind == TypeKind::Optional)
                return false;
            if (type.kind == TypeKind::Array)
                return type.innerType && type_requires_cleanup(*type.innerType, visiting);
            if (type.kind != TypeKind::Struct && type.kind != TypeKind::Class)
                return false;
            if (!visiting.insert(type.name).second)
                return false;
            const auto definition = registeredTypes.find(type.name);
            if (definition == registeredTypes.end())
                return false;
            for (const auto &[_, field] : definition->second.members)
            {
                if (type_requires_cleanup(infer_type_from_node(field.typeNode), visiting))
                    return true;
            }
            return false;
        }

        bool type_requires_cleanup(const Type &type)
        {
            std::unordered_set<std::string> visiting;
            return type_requires_cleanup(type, visiting);
        }

        bool type_can_contain_owned_storage(const Type &type, std::unordered_set<std::string> &visiting)
        {
            if (type.kind == TypeKind::Pointer)
                return true;
            if (type.kind == TypeKind::Reference)
                return false;
            if (type.kind == TypeKind::Optional || type.kind == TypeKind::Array)
                return type.innerType && type_can_contain_owned_storage(*type.innerType, visiting);
            if (type.kind != TypeKind::Struct && type.kind != TypeKind::Class || !visiting.insert(type.name).second)
                return false;
            const auto definition = registeredTypes.find(type.name);
            if (definition == registeredTypes.end())
                return false;
            for (const auto &[_, field] : definition->second.members)
                if (type_can_contain_owned_storage(infer_type_from_node(field.typeNode), visiting))
                    return true;
            return false;
        }

        bool type_can_contain_owned_storage(const Type &type)
        {
            std::unordered_set<std::string> visiting;
            return type_can_contain_owned_storage(type, visiting);
        }

        bool is_subobject_path(const Parser::ASTNode &node)
        {
            return node.type == Parser::NodeType::MemberAccessExpr || node.type == Parser::NodeType::IndexExpr;
        }

        static bool normalize_typeof_type(SymbolTable &table, Parser::ASTNode &typeNode);
        Type infer_expression_type(SymbolTable &table, Parser::ASTNode &expr);
        bool is_assignable(const Type &declaredType, const Type &valueType);

        Type union_payload_type(const TypeDefinition &definition, const Type &unionType,
                                const Parser::ASTNode &payloadNode)
        {
            Type payload = infer_type_from_node(payloadNode);
            if (payload.kind != TypeKind::Generic)
                return payload;
            const auto parameter =
                std::find(definition.genericParameters.begin(), definition.genericParameters.end(), payload.name);
            if (parameter == definition.genericParameters.end())
                return payload;
            const size_t index = static_cast<size_t>(parameter - definition.genericParameters.begin());
            return index < unionType.genericArgs.size() ? unionType.genericArgs[index] : payload;
        }

        Type substitute_aggregate_generics(const Type &type, const TypeDefinition &definition,
                                           const Type &aggregateType)
        {
            if (type.kind == TypeKind::Generic)
            {
                const auto parameter =
                    std::find(definition.genericParameters.begin(), definition.genericParameters.end(), type.name);
                if (parameter != definition.genericParameters.end())
                {
                    const size_t index = static_cast<size_t>(parameter - definition.genericParameters.begin());
                    if (index < aggregateType.genericArgs.size())
                        return aggregateType.genericArgs[index];
                }
                return type;
            }

            Type result = type;
            if (type.innerType)
                result.innerType =
                    std::make_shared<Type>(substitute_aggregate_generics(*type.innerType, definition, aggregateType));
            for (Type &argument : result.genericArgs)
                argument = substitute_aggregate_generics(argument, definition, aggregateType);
            return result;
        }

        Parser::ASTNode substitute_union_payload_node(const TypeDefinition &definition,
                                                      const Parser::ASTNode &subjectTypeNode,
                                                      const Parser::ASTNode &payloadNode)
        {
            if (payloadNode.type == Parser::NodeType::CustomType)
            {
                const std::string name = custom_type_name(payloadNode);
                const auto parameter =
                    std::find(definition.genericParameters.begin(), definition.genericParameters.end(), name);
                if (parameter != definition.genericParameters.end())
                {
                    const size_t index = static_cast<size_t>(parameter - definition.genericParameters.begin());
                    if (index < subjectTypeNode.children.size())
                        return subjectTypeNode.children[index];
                }
            }
            Parser::ASTNode result = payloadNode;
            for (Parser::ASTNode &child : result.children)
                child = substitute_union_payload_node(definition, subjectTypeNode, child);
            return result;
        }

        bool infer_union_constructor_type(SymbolTable &table, Parser::ASTNode &expr, Type &result)
        {
            if (expr.children.empty() || expr.children.front().type != Parser::NodeType::ScopeAccessExpr)
                return false;
            const Parser::ASTNode &callee = expr.children.front();
            const std::string unionName = scope_access_name(callee.children.front());
            const std::string variantName = std::string(std::get<std::string_view>(callee.value));
            const auto definition = registeredTypes.find(unionName);
            if (definition == registeredTypes.end() || !definition->second.isUnion)
                return false;
            const auto variant = definition->second.unionVariants.find(variantName);
            if (variant == definition->second.unionVariants.end())
                error("Unknown union variant '" + unionName + "::" + variantName + "'.", callee);
            if (expr.children.size() - 1 != variant->second.payloadTypes.size())
                error("Union constructor argument count mismatch.", callee);

            result = Type(TypeKind::Union, unionName);
            for (const std::string &parameter : definition->second.genericParameters)
            {
                bool inferred = false;
                for (size_t index = 0; index < variant->second.payloadTypes.size(); ++index)
                {
                    const Type payload = infer_type_from_node(variant->second.payloadTypes[index]);
                    if (payload.kind != TypeKind::Generic || payload.name != parameter)
                        continue;
                    const Parser::ASTNode &argument = expr.children[index + 1];
                    result.genericArgs.push_back(
                        argument.type == Parser::NodeType::IntegerLiteral
                            ? Type(TypeKind::I32)
                            : infer_expression_type(table, const_cast<Parser::ASTNode &>(argument)));
                    inferred = true;
                    break;
                }
                if (!inferred)
                    error("Cannot infer generic union argument '" + parameter + "' from this constructor.", callee);
            }

            for (size_t index = 0; index < variant->second.payloadTypes.size(); ++index)
            {
                const Type expected =
                    union_payload_type(definition->second, result, variant->second.payloadTypes[index]);
                const Parser::ASTNode &argument = expr.children[index + 1];
                const Type actual = infer_expression_type(table, const_cast<Parser::ASTNode &>(argument));
                if (!is_assignable(expected, actual) &&
                    !(argument.type == Parser::NodeType::IntegerLiteral && is_numeric(expected.kind)))
                    error("Union constructor argument type mismatch.", argument);
            }
            return true;
        }

        bool check_union_constructor_for_target(SymbolTable &table, Parser::ASTNode &expr, const Type &target)
        {
            if (expr.children.empty() || expr.children.front().type != Parser::NodeType::ScopeAccessExpr)
                return false;
            const Parser::ASTNode &callee = expr.children.front();
            const std::string unionName = scope_access_name(callee.children.front());
            const auto definition = registeredTypes.find(unionName);
            if (definition == registeredTypes.end() || !definition->second.isUnion)
                return false;
            if (target.kind != TypeKind::Union || target.name != unionName ||
                target.genericArgs.size() != definition->second.genericParameters.size())
                error("Union constructor does not match its declared target type.", callee);
            const std::string variantName = std::string(std::get<std::string_view>(callee.value));
            const auto variant = definition->second.unionVariants.find(variantName);
            if (variant == definition->second.unionVariants.end())
                error("Unknown union variant '" + unionName + "::" + variantName + "'.", callee);
            if (expr.children.size() - 1 != variant->second.payloadTypes.size())
                error("Union constructor argument count mismatch.", callee);
            for (size_t index = 0; index < variant->second.payloadTypes.size(); ++index)
            {
                const Type expected =
                    union_payload_type(definition->second, target, variant->second.payloadTypes[index]);
                const Parser::ASTNode &argument = expr.children[index + 1];
                const Type actual = infer_expression_type(table, const_cast<Parser::ASTNode &>(argument));
                if (!is_assignable(expected, actual) &&
                    !(argument.type == Parser::NodeType::IntegerLiteral && is_numeric(expected.kind)))
                    error("Union constructor argument type mismatch.", argument);
            }
            return true;
        }

        Type infer_expression_type(SymbolTable &table, Parser::ASTNode &expr)
        {
            switch (expr.type)
            {
            case Parser::NodeType::Identifier:
            {
                const auto *symbol = table.lookup(std::string(std::get<std::string_view>(expr.value)));
                if (!symbol)
                {
                    const std::string name = std::string(std::get<std::string_view>(expr.value));
                    for (const auto &[enum_name, definition] : registeredTypes)
                    {
                        if (definition.isEnum && definition.members.find(name) != definition.members.end())
                            return Type(TypeKind::Enum, enum_name);
                    }
                    error("Use of undeclared identifier.", expr);
                }
                if (symbol->state == VarOwnershipState::Moved)
                {
                    error("Use of moved variable '" + symbol->name + "'.", expr);
                }
                return infer_type_from_node(symbol->typeNode);
            }
            case Parser::NodeType::IntegerLiteral:
                return Type(TypeKind::U64);
            case Parser::NodeType::FloatLiteral:
                return Type(TypeKind::F64);
            case Parser::NodeType::BoolLiteral:
                return Type(TypeKind::Bool);
            case Parser::NodeType::CharLiteral:
                return Type(TypeKind::Char);
            case Parser::NodeType::StringLiteral:
                validate_string_literal_escapes(expr);
                return Type(TypeKind::String);
            case Parser::NodeType::StructInitExpr:
            {
                if (!expr.hasInferredType || expr.inferredTypeName.empty())
                {
                    error("Brace initializer requires an assignment or declaration target, or an explicit type name.",
                          expr);
                    return Type(TypeKind::Error);
                }
                Parser::ASTNode typeNode{Parser::NodeType::CustomType, expr.start, expr.mod_path, expr.source,
                                         std::string_view{},           {}};
                typeNode.inferredTypeName = expr.inferredTypeName;
                Type type = infer_type_from_node(typeNode);
                if (type.kind != TypeKind::Struct && type.kind != TypeKind::Class)
                    error("Brace initializer requires a struct type.", expr);
                const auto definition = registeredTypes.find(type.name);
                if (definition == registeredTypes.end())
                    error("Brace initializer names an unknown struct type.", expr);
                if (expr.children.size() != definition->second.members.size())
                    error("Brace initializer field count does not match its type.", expr);
                for (Parser::ASTNode &field : expr.children)
                    (void)infer_expression_type(table, field);
                expr.inferredTypeName = type.name;
                return type;
            }
            case Parser::NodeType::PointerType:
                return Type(TypeKind::Pointer, "*");
            case Parser::NodeType::ReferenceType:
                return Type(TypeKind::Reference, "&");
            case Parser::NodeType::MoveExpr:
            {
                if (expr.children.empty())
                {
                    error("Cannot infer type from move expression with no operand.", expr);
                    return {TypeKind::Error, {}};
                }
                return infer_expression_type(table, expr.children.front());
            }
            case Parser::NodeType::RefExpr:
            {
                if (expr.children.empty())
                {
                    error("Cannot infer type from reference expression with no operand.", expr);
                    return {TypeKind::Error, {}};
                }
                Type pointee = infer_expression_type(table, expr.children.front());
                Type reference(TypeKind::Reference, "&");
                reference.isMutable = pointee.isMutable;
                reference.innerType = std::make_shared<Type>(pointee);
                return reference;
            }
            case Parser::NodeType::SizeofExpr:
                return Type(TypeKind::U64);
            case Parser::NodeType::TypeIfExpr:
            {
                if (expr.children.size() != 4)
                {
                    error("@comptime iftype requires a subject, target type, selected expression, and fallback "
                          "expression.",
                          expr);
                    return Type(TypeKind::Error);
                }
                const Type subject = infer_expression_type(table, expr.children[0]);
                normalize_typeof_type(table, expr.children[1]);
                const Type target = infer_type_from_node(expr.children[1]);
                const bool matches = is_assignable(target, subject) && is_assignable(subject, target);
                expr.value = matches;
                expr.hasInferredType = true;
                const Type result = infer_expression_type(table, expr.children[matches ? 2 : 3]);
                expr.inferredTypeName = type_to_string(result);
                return result;
            }
            case Parser::NodeType::DoExpr:
            {
                if (expr.children.empty())
                {
                    error("A do expression requires a final yield value.", expr);
                    return Type(TypeKind::Error);
                }
                if (!expr.hasInferredType)
                {
                    table.push_scope();
                    for (size_t index = 0; index + 1 < expr.children.size(); ++index)
                        check_node(table, expr.children[index]);
                    const Type result = infer_expression_type(table, expr.children.back());
                    table.pop_scope();
                    expr.hasInferredType = true;
                    expr.inferredTypeName = type_to_string(result);
                    return result;
                }
                const std::string &name = expr.inferredTypeName;
                if (name == "bool")
                    return Type(TypeKind::Bool);
                if (name == "char")
                    return Type(TypeKind::Char);
                if (name == "i8")
                    return Type(TypeKind::I8);
                if (name == "i16")
                    return Type(TypeKind::I16);
                if (name == "i32")
                    return Type(TypeKind::I32);
                if (name == "i64")
                    return Type(TypeKind::I64);
                if (name == "u8")
                    return Type(TypeKind::U8);
                if (name == "u16")
                    return Type(TypeKind::U16);
                if (name == "u32")
                    return Type(TypeKind::U32);
                if (name == "u64")
                    return Type(TypeKind::U64);
                if (name == "usize")
                    return Type(TypeKind::USIZE);
                if (name == "f32")
                    return Type(TypeKind::F32);
                if (name == "f64")
                    return Type(TypeKind::F64);
                return Type(TypeKind::Struct, name);
            }
            case Parser::NodeType::CompileExpr:
                if (expr.children.size() != 1)
                {
                    error("@comptime expression requires exactly one expression.", expr);
                    return Type(TypeKind::Error);
                }
                return infer_expression_type(table, expr.children.front());
            case Parser::NodeType::CastExpr:
            {
                if (expr.children.size() != 2)
                {
                    error("A cast requires a value and target type.", expr);
                }
                const Type source = infer_expression_type(table, expr.children.front());
                Parser::ASTNode &targetNode = expr.children[1];
                normalize_typeof_type(table, targetNode);
                const Type target = infer_type_from_node(targetNode);
                const bool sourceNumeric = is_numeric(source.kind) || source.kind == TypeKind::Enum;
                const bool targetNumeric = is_numeric(target.kind);
                const bool booleanCast =
                    target.kind == TypeKind::Bool && (sourceNumeric || source.kind == TypeKind::Bool);
                if ((!sourceNumeric || !targetNumeric) && !booleanCast)
                {
                    error("Casts require numeric values or a numeric value cast to bool.", expr);
                }
                expr.hasInferredType = true;
                expr.inferredTypeName = type_to_string(target);
                return target;
            }
            case Parser::NodeType::CallExpr:
            {
                if (expr.children.empty())
                {
                    error("Cannot infer type from call expression with no callee.", expr);
                    return {TypeKind::Error, {}};
                }
                Type unionConstructor;
                if (infer_union_constructor_type(table, expr, unionConstructor))
                    return unionConstructor;
                const Parser::ASTNode &callee = expr.children.front();
                std::string name;
                if (callee.type == Parser::NodeType::Identifier)
                    name = std::string(std::get<std::string_view>(callee.value));
                else if (callee.type == Parser::NodeType::ScopeAccessExpr)
                    name = scope_access_name(callee);
                if ((name == "__shaft_format" || name == "__shaft_println") && stdlibEnabled)
                    return Type(TypeKind::Void);
                if (const Symbol *function = table.lookup(name);
                    function && function->isFunction && function->hasResultType)
                {
                    return function->resultType;
                }

                for (const std::string &usingName : Parser::using_namespace_declarations)
                {
                    const size_t separator = usingName.rfind("::");

                    if (separator == std::string::npos)
                        continue;

                    const std::string importedName = usingName.substr(separator + 2);

                    if (importedName != name)
                        continue;

                    if (const Symbol *function = table.lookup(usingName);
                        function && function->isFunction && function->hasResultType)
                    {
                        return function->resultType;
                    }
                }
                return {TypeKind::Error, {}};
            }
            case Parser::NodeType::UnaryExpr:
            {
                if (expr.children.empty())
                {
                    error("Cannot infer type from unary expression with no operand.", expr);
                    return {TypeKind::Error, {}};
                }
                const Type operand = infer_expression_type(table, expr.children.front());
                if (!std::holds_alternative<Lexer::Operator>(expr.value))
                    return operand;
                const Lexer::Operator op = std::get<Lexer::Operator>(expr.value);
                if (op == Lexer::Operator::EXLAMATION_MARK)
                    return Type(TypeKind::Bool);
                if (op == Lexer::Operator::AMPERSAND)
                {
                    Type result(TypeKind::Pointer, "*");
                    result.innerType = std::make_shared<Type>(operand);
                    return result;
                }
                if (op == Lexer::Operator::MULTIPLY && operand.innerType)
                    return *operand.innerType;
                return operand;
            }
            case Parser::NodeType::IndexExpr:
            {
                if (expr.children.size() != 2)
                {
                    error("Cannot infer type from index expression with invalid number of children.", expr);
                    return {TypeKind::Error, {}};
                }
                const Type receiver = infer_expression_type(table, expr.children.front());
                if (receiver.kind == TypeKind::Array && receiver.innerType)
                    return *receiver.innerType;
                if (receiver.kind == TypeKind::Pointer && receiver.innerType)
                    return *receiver.innerType;
                const auto definition = registeredTypes.find(receiver.name);
                if (definition == registeredTypes.end() || definition->second.indexedField.empty())
                {
                    error("Cannot infer type from index expression on non-array type '" + receiver.name + "'.", expr);
                    return {TypeKind::Error, {}};
                }
                const auto field = definition->second.members.find(definition->second.indexedField);
                if (field == definition->second.members.end())
                {
                    error("Cannot infer type from index expression on type '" + receiver.name +
                              "' with missing indexed field '" + definition->second.indexedField + "'.",
                          expr);
                    return {TypeKind::Error, {}};
                }
                const Parser::ASTNode &backingNode = field->second.typeNode;
                const Type backing = infer_type_from_node(backingNode);
                if (!backing.innerType)
                    return {TypeKind::Error, {}};
                if (!backingNode.children.empty() && backingNode.children.front().type == Parser::NodeType::CustomType)
                {
                    const std::string elementName =
                        std::string(std::get<std::string_view>(backingNode.children.front().value));
                    if (registeredTypes.find(elementName) == registeredTypes.end())
                        return substitute_aggregate_generics(*backing.innerType, definition->second, receiver);
                }
                return *backing.innerType;
            }
            case Parser::NodeType::MemberAccessExpr:
            {
                if (expr.children.empty())
                {
                    error("Member access on an empty expression.", expr);
                    return {TypeKind::Error, {}};
                }
                const Type receiver = infer_expression_type(table, expr.children.front());
                const Type &aggregateReceiver =
                    (receiver.kind == TypeKind::Pointer || receiver.kind == TypeKind::Reference) && receiver.innerType
                        ? *receiver.innerType
                        : receiver;
                const std::string memberName = std::string(std::get<std::string_view>(expr.value));
                if (receiver.kind == TypeKind::Array && memberName == "length")
                    return Type(TypeKind::U64);
                const auto definition = registeredTypes.find(aggregateReceiver.name);
                if (definition == registeredTypes.end())
                {
                    error("Member access on unknown type '" + aggregateReceiver.name + "'.", expr);
                    return {TypeKind::Error, {}};
                }
                const auto field = definition->second.members.find(memberName);
                if (field == definition->second.members.end())
                {
                    error("Member '" + memberName + "' not found in type '" + aggregateReceiver.name + "'.", expr);
                    return {TypeKind::Error, {}};
                }
                return substitute_aggregate_generics(infer_type_from_node(field->second.typeNode), definition->second,
                                                     aggregateReceiver);
            }
            case Parser::NodeType::ScopeAccessExpr:
            {
                if (expr.children.empty())
                {
                    error("Scope access on an empty expression.", expr);
                    return {TypeKind::Error, {}};
                }
                const std::string enumName = scope_access_name(expr.children.front());
                const std::string memberName = std::string(std::get<std::string_view>(expr.value));
                const auto definition = registeredTypes.find(enumName);
                if (definition == registeredTypes.end() || !definition->second.isEnum ||
                    definition->second.members.find(memberName) == definition->second.members.end())
                {
                    error("Unknown enum member '" + enumName + "::" + memberName + "'.", expr);
                    return {TypeKind::Error, {}};
                }
                return Type(TypeKind::Enum, enumName);
            }
            case Parser::NodeType::BinaryExpr:
            {
                if (expr.children.size() < 2)
                {
                    error("Cannot infer type from binary expression with less than two operands.", expr);
                    return {TypeKind::Error, {}};
                }

                const Type leftType = infer_expression_type(table, expr.children[0]);
                const Type rightType = infer_expression_type(table, expr.children[1]);
                Lexer::Operator op = Lexer::Operator::ASSIGN;
                if (expr.value.index() == 5)
                    op = std::get<Lexer::Operator>(expr.value);

                Type resultType(TypeKind::Error);
                if (op == Lexer::Operator::AND || op == Lexer::Operator::OR)
                    resultType = Type(TypeKind::Bool);
                else if (op == Lexer::Operator::LESS_THAN || op == Lexer::Operator::GREATER_THAN ||
                         op == Lexer::Operator::LESS_EQUAL || op == Lexer::Operator::GREATER_EQUAL ||
                         op == Lexer::Operator::EQUAL || op == Lexer::Operator::NOT_EQUAL)
                    resultType = Type(TypeKind::Bool);
                else if ((leftType.kind == TypeKind::Pointer || leftType.kind == TypeKind::Reference) &&
                         is_numeric(rightType.kind) && (op == Lexer::Operator::PLUS || op == Lexer::Operator::MINUS))
                    resultType = leftType;
                else if (is_numeric(leftType.kind) && is_numeric(rightType.kind))
                    resultType = (leftType.kind == TypeKind::F32 || leftType.kind == TypeKind::F64 ||
                                  rightType.kind == TypeKind::F32 || rightType.kind == TypeKind::F64)
                                     ? Type{TypeKind::F64, {}}
                                     : (leftType.kind == rightType.kind ? leftType : Type{TypeKind::U64, {}});
                else if (leftType.kind == rightType.kind)
                    resultType = leftType;

                expr.hasInferredType = true;
                expr.inferredTypeName = type_to_string(resultType);
                return resultType;
            }
            case Parser::NodeType::AssignmentExpr:
            {
                if (expr.children.size() < 2)
                {
                    error("Cannot infer type from assignment expression with less than two operands.", expr);
                    return {TypeKind::Error, {}};
                }
                return infer_expression_type(table, expr.children[1]);
            }
            default:
            {
                error("Cannot infer type for this expression.", expr);
                return {TypeKind::Error, {}};
            }
            }
        }

        static Lexer::Keyword primitive_keyword(TypeKind kind)
        {
            switch (kind)
            {
            case TypeKind::U8:
                return Lexer::Keyword::U8;
            case TypeKind::U16:
                return Lexer::Keyword::U16;
            case TypeKind::U32:
                return Lexer::Keyword::U32;
            case TypeKind::U64:
                return Lexer::Keyword::U64;
            case TypeKind::USIZE:
                return Lexer::Keyword::USIZE;
            case TypeKind::I8:
                return Lexer::Keyword::I8;
            case TypeKind::I16:
                return Lexer::Keyword::I16;
            case TypeKind::I32:
                return Lexer::Keyword::I32;
            case TypeKind::I64:
                return Lexer::Keyword::I64;
            case TypeKind::F32:
                return Lexer::Keyword::F32;
            case TypeKind::F64:
                return Lexer::Keyword::F64;
            case TypeKind::Bool:
                return Lexer::Keyword::BOOL;
            case TypeKind::Char:
                return Lexer::Keyword::CHAR;
            case TypeKind::State:
                return Lexer::Keyword::STATE;
            case TypeKind::Thread:
                return Lexer::Keyword::THREAD;
            default:
                return Lexer::Keyword::AS;
            }
        }

        static bool normalize_typeof_type(SymbolTable &table, Parser::ASTNode &typeNode)
        {
            if (typeNode.type != Parser::NodeType::TypeofType)
                return true;
            if (typeNode.children.size() != 1)
            {
                error("typeof requires exactly one expression.", typeNode);
            }

            Parser::ASTNode &expression = typeNode.children.front();
            if (expression.type == Parser::NodeType::Identifier)
            {
                if (const Symbol *symbol = table.lookup(std::string(std::get<std::string_view>(expression.value))))
                {
                    typeNode = symbol->typeNode;
                    return normalize_typeof_type(table, typeNode);
                }
            }

            const Type inferred = infer_expression_type(table, expression);
            const Lexer::Keyword keyword = primitive_keyword(inferred.kind);
            if (keyword == Lexer::Keyword::AS)
            {
                error("typeof can infer only primitive expression types or declared bindings.", expression);
            }
            typeNode = Parser::ASTNode{
                Parser::NodeType::PrimitiveType, expression.start, expression.mod_path, expression.source, keyword, {}};
            return true;
        }

        bool is_pointer_to_byte(const Type &type)
        {
            if (type.kind != TypeKind::Pointer || !type.innerType)
                return false;
            const TypeKind pointee = type.innerType->kind;
            return pointee == TypeKind::U8 || pointee == TypeKind::I8 || pointee == TypeKind::Char;
        }

        bool string_literal_decays_to_pointer(const Type &declaredType, const Parser::ASTNode &argNode)
        {
            if (argNode.type != Parser::NodeType::StringLiteral)
                return false;
            if (is_pointer_to_byte(declaredType))
                return true;
            if (declaredType.kind == TypeKind::String &&
                (declaredType.name == "str" || declaredType.name == "std::str"))
                return true;
            if ((declaredType.kind == TypeKind::Struct || declaredType.kind == TypeKind::Class) &&
                declaredType.name == "std::string::view::View")
                return true;
            return false;
        }

        bool is_assignable(const Type &declaredType, const Type &valueType)
        {
            if (declaredType.kind == TypeKind::Error || valueType.kind == TypeKind::Error)
                return true;
            if (declaredType.kind == TypeKind::Generic || valueType.kind == TypeKind::Generic)
                return true;
            if (declaredType.isOptional || valueType.isOptional)
            {
                if (!declaredType.isOptional)
                    return is_assignable(declaredType, valueType.innerType ? *valueType.innerType : Type{});
                if (!valueType.isOptional)
                    return is_assignable(declaredType.innerType ? *declaredType.innerType : Type{}, valueType);
                return is_assignable(declaredType.innerType ? *declaredType.innerType : Type{},
                                     valueType.innerType ? *valueType.innerType : Type{});
            }
            if ((declaredType.kind == TypeKind::Array && valueType.kind == TypeKind::Pointer) ||
                (declaredType.kind == TypeKind::Pointer && valueType.kind == TypeKind::Array))
                return true;
            if (declaredType.kind == TypeKind::Pointer || declaredType.kind == TypeKind::Reference)
            {
                if (valueType.kind != declaredType.kind)
                    return false;
                if (declaredType.kind == TypeKind::Reference)
                {
                    if (declaredType.isMutable != valueType.isMutable && declaredType.isMutable)
                        return false;
                }
                if (!declaredType.innerType || !valueType.innerType)
                    return declaredType.innerType == valueType.innerType;
                if (declaredType.kind == TypeKind::Pointer &&
                    (declaredType.innerType->kind == TypeKind::U8 || valueType.innerType->kind == TypeKind::U8))
                    return true;
                return is_assignable(*declaredType.innerType, *valueType.innerType);
            }
            if (declaredType.kind == TypeKind::String || valueType.kind == TypeKind::String)
            {
                return declaredType.kind == TypeKind::String && valueType.kind == TypeKind::String &&
                       declaredType.name == valueType.name;
            }
            if (declaredType.kind == TypeKind::Array || valueType.kind == TypeKind::Array)
            {
                return declaredType.kind == TypeKind::Array && valueType.kind == TypeKind::Array &&
                       declaredType.innerType && valueType.innerType &&
                       is_assignable(*declaredType.innerType, *valueType.innerType);
            }
            if ((valueType.kind == TypeKind::Enum && is_numeric(declaredType.kind)) ||
                (declaredType.kind == TypeKind::Enum && is_numeric(valueType.kind)))
                return true;
            if (declaredType.kind == TypeKind::Struct || declaredType.kind == TypeKind::Class ||
                declaredType.kind == TypeKind::Enum || declaredType.kind == TypeKind::Union)
            {
                return valueType.kind == declaredType.kind && declaredType.name == valueType.name &&
                       declaredType.genericArgs.size() == valueType.genericArgs.size() &&
                       std::equal(declaredType.genericArgs.begin(), declaredType.genericArgs.end(),
                                  valueType.genericArgs.begin(), [](const Type &left, const Type &right)
                                  { return types_structurally_equal(left, right); });
            }
            if (declaredType.kind == valueType.kind)
                return true;
            if (declaredType.kind == TypeKind::Struct || declaredType.kind == TypeKind::Class ||
                declaredType.kind == TypeKind::Enum || declaredType.kind == TypeKind::Union)
                return valueType.kind == declaredType.kind && declaredType.name == valueType.name;
            if (is_integer(declaredType.kind) && is_integer(valueType.kind))
                return can_widen_integer(declaredType.kind, valueType.kind);
            if ((declaredType.kind == TypeKind::F32 || declaredType.kind == TypeKind::F64) &&
                (valueType.kind == TypeKind::F32 || valueType.kind == TypeKind::F64))
                return declaredType.kind == TypeKind::F64 || valueType.kind == TypeKind::F32;

            return false;
        }

        Type substitute_generic_type(const Type &type, const std::unordered_map<std::string, Type> &bindings)
        {
            if (type.kind == TypeKind::Generic)
            {
                const auto binding = bindings.find(type.name);
                if (binding != bindings.end())
                    return binding->second;
            }

            Type result = type;
            if (type.innerType)
                result.innerType = std::make_shared<Type>(substitute_generic_type(*type.innerType, bindings));
            for (Type &argument : result.genericArgs)
                argument = substitute_generic_type(argument, bindings);
            return result;
        }

        bool implicitly_borrows_for_reference_parameter(const Type &parameterType, const Type &argumentType,
                                                        const Parser::ASTNode &argument)
        {
            if (parameterType.kind != TypeKind::Reference || argumentType.kind == TypeKind::Reference ||
                !parameterType.innerType)
                return false;

            const bool isDereference = argument.type == Parser::NodeType::UnaryExpr &&
                                       std::holds_alternative<Lexer::Operator>(argument.value) &&
                                       std::get<Lexer::Operator>(argument.value) == Lexer::Operator::MULTIPLY;
            const bool isAddressable = argument.type == Parser::NodeType::Identifier ||
                                       argument.type == Parser::NodeType::MemberAccessExpr ||
                                       argument.type == Parser::NodeType::IndexExpr || isDereference;
            return isAddressable && (!parameterType.isMutable || argumentType.isMutable) &&
                   is_assignable(*parameterType.innerType, argumentType);
        }

        bool implicitly_passes_reference_as_pointer(const Type &parameterType, const Type &argumentType)
        {
            return parameterType.kind == TypeKind::Pointer && argumentType.kind == TypeKind::Reference &&
                   parameterType.innerType && argumentType.innerType &&
                   is_assignable(*parameterType.innerType, *argumentType.innerType);
        }

        bool implicitly_dereferences_for_value_parameter(const Type &parameterType, const Type &argumentType)
        {
            return parameterType.kind != TypeKind::Reference && argumentType.kind == TypeKind::Reference &&
                   argumentType.innerType && is_assignable(parameterType, *argumentType.innerType);
        }

        bool is_valid_binary_op(const Type &left, const Type &right, Lexer::Operator op)
        {
            if (left.kind == TypeKind::Error || right.kind == TypeKind::Error)
                return true;
            // A generic body is checked before specialization
            if (left.kind == TypeKind::Generic || right.kind == TypeKind::Generic)
                return op != Lexer::Operator::MODULO && op != Lexer::Operator::AMPERSAND &&
                       op != Lexer::Operator::PIPE && op != Lexer::Operator::CARET &&
                       op != Lexer::Operator::LEFT_SHIFT && op != Lexer::Operator::RIGHT_SHIFT;
            if (op == Lexer::Operator::AND || op == Lexer::Operator::OR)
                return left.kind == TypeKind::Bool && right.kind == TypeKind::Bool;
            if (op == Lexer::Operator::ASSIGN)
                return (left.kind == TypeKind::String && right.kind == TypeKind::String) ||
                       (left.kind == right.kind && left.name == right.name && left.kind != TypeKind::Pointer);
            if (left.kind == TypeKind::Enum || right.kind == TypeKind::Enum)
                return left.kind == TypeKind::Enum && right.kind == TypeKind::Enum && left.name == right.name &&
                       (op == Lexer::Operator::EQUAL || op == Lexer::Operator::NOT_EQUAL);
            if (left.kind == TypeKind::Struct || right.kind == TypeKind::Struct)
                return false;
            if (left.kind == TypeKind::Class || left.kind == TypeKind::Union || left.kind == TypeKind::Array ||
                right.kind == TypeKind::Class || right.kind == TypeKind::Union || right.kind == TypeKind::Array)
                return false;
            if (op == Lexer::Operator::LESS_THAN || op == Lexer::Operator::GREATER_THAN ||
                op == Lexer::Operator::LESS_EQUAL || op == Lexer::Operator::GREATER_EQUAL)
                return (is_numeric(left.kind) && is_numeric(right.kind)) ||
                       (left.kind == right.kind && left.kind != TypeKind::Pointer) ||
                       (left.kind == TypeKind::Pointer && right.kind == TypeKind::Pointer) ||
                       (left.kind == TypeKind::Reference && right.kind == TypeKind::Reference);
            if ((left.kind == TypeKind::Pointer || left.kind == TypeKind::Reference) && is_numeric(right.kind) &&
                (op == Lexer::Operator::PLUS || op == Lexer::Operator::MINUS))
                return true;
            if ((left.kind == TypeKind::Pointer || right.kind == TypeKind::Pointer) &&
                (op == Lexer::Operator::PLUS || op == Lexer::Operator::MINUS))
                return false;
            if ((left.kind == TypeKind::Pointer || left.kind == TypeKind::Reference) &&
                (right.kind == TypeKind::Pointer || right.kind == TypeKind::Reference) &&
                (op == Lexer::Operator::EQUAL || op == Lexer::Operator::NOT_EQUAL))
                return true;
            if (left.kind == TypeKind::Pointer || right.kind == TypeKind::Pointer || left.kind == TypeKind::Reference ||
                right.kind == TypeKind::Reference)
                return false;

            if (left.kind == TypeKind::Bool || right.kind == TypeKind::Bool)
                return left.kind == TypeKind::Bool && right.kind == TypeKind::Bool;
            if (left.kind == TypeKind::String || right.kind == TypeKind::String)
                return (op == Lexer::Operator::EQUAL || op == Lexer::Operator::NOT_EQUAL) &&
                       left.kind == TypeKind::String && right.kind == TypeKind::String;
            const bool hasFloat = left.kind == TypeKind::F32 || left.kind == TypeKind::F64 ||
                                  right.kind == TypeKind::F32 || right.kind == TypeKind::F64;
            if (hasFloat)
                return op == Lexer::Operator::PLUS || op == Lexer::Operator::MINUS || op == Lexer::Operator::MULTIPLY ||
                       op == Lexer::Operator::DIVIDE || op == Lexer::Operator::LESS_THAN ||
                       op == Lexer::Operator::GREATER_THAN || op == Lexer::Operator::LESS_EQUAL ||
                       op == Lexer::Operator::GREATER_EQUAL || op == Lexer::Operator::EQUAL ||
                       op == Lexer::Operator::NOT_EQUAL;
            if (left.kind == right.kind)
                return true;
            return is_numeric(left.kind) && is_numeric(right.kind);
        }
    } // namespace

    void register_type_definition(const Parser::ASTNode &node)
    {
        if (node.type != Parser::NodeType::StructDecl && node.type != Parser::NodeType::EnumDecl &&
            node.type != Parser::NodeType::UnionDecl)
            return;

        TypeDefinition def;
        def.name = current_namespace_name();
        if (!def.name.empty())
            def.name += "::";
        def.name += std::string(std::get<std::string_view>(node.children.front().value));
        if (def.name.empty() && !node.children.empty() && node.children.front().type == Parser::NodeType::Identifier)
            def.name = std::string(std::get<std::string_view>(node.children.front().value));

        if (def.name.empty())
            return;

        def.isEnum = node.type == Parser::NodeType::EnumDecl;
        def.isUnion = node.type == Parser::NodeType::UnionDecl;
        for (const auto &child : node.children)
        {
            if (child.type == Parser::NodeType::GenericParam)
                def.genericParameters.push_back(std::string(std::get<std::string_view>(child.value)));
        }

        registeredTypes[def.name] = def;
        if (predeclaringTypeShells)
            return;

        if (def.isUnion)
        {
            size_t tag = 0;
            for (const auto &child : node.children)
            {
                if (child.type != Parser::NodeType::UnionVariant || child.children.empty())
                    continue;
                const std::string variantName = std::string(std::get<std::string_view>(child.children.front().value));
                if (def.unionVariants.find(variantName) != def.unionVariants.end())
                    error("Duplicate union variant '" + variantName + "'.", child);
                TypeDefinition::UnionVariant variant;
                variant.tag = tag++;
                variant.payloadTypes.assign(child.children.begin() + 1, child.children.end());
                def.unionVariants.emplace(variantName, std::move(variant));
            }
            if (def.unionVariants.empty())
                error("A union must declare at least one variant.", node);
            registeredTypes[def.name] = std::move(def);
            return;
        }

        uint64_t enumMaximum = UINT64_MAX;
        if (def.isEnum && node.children.size() > 1 && node.children[1].type == Parser::NodeType::PrimitiveType)
        {
            const Lexer::Keyword backing = std::get<Lexer::Keyword>(node.children[1].value);
            const bool integerBacking =
                backing == Lexer::Keyword::U8 || backing == Lexer::Keyword::U16 || backing == Lexer::Keyword::U32 ||
                backing == Lexer::Keyword::U64 || backing == Lexer::Keyword::I8 || backing == Lexer::Keyword::I16 ||
                backing == Lexer::Keyword::I32 || backing == Lexer::Keyword::I64 || backing == Lexer::Keyword::USIZE;
            if (!integerBacking)
            {
                error("enum backing type must be an integer.", node);
            }
            if (backing == Lexer::Keyword::U8)
                enumMaximum = UINT8_MAX;
            else if (backing == Lexer::Keyword::U16)
                enumMaximum = UINT16_MAX;
            else if (backing == Lexer::Keyword::U32)
                enumMaximum = UINT32_MAX;
            else if (backing == Lexer::Keyword::I8)
                enumMaximum = INT8_MAX;
            else if (backing == Lexer::Keyword::I16)
                enumMaximum = INT16_MAX;
            else if (backing == Lexer::Keyword::I32)
                enumMaximum = INT32_MAX;
            else if (backing == Lexer::Keyword::I64)
                enumMaximum = INT64_MAX;
        }
        int64_t enumMinimum = 0;
        if (def.isEnum && node.children.size() > 1 && node.children[1].type == Parser::NodeType::PrimitiveType)
        {
            const Lexer::Keyword backing = std::get<Lexer::Keyword>(node.children[1].value);
            if (backing == Lexer::Keyword::I8)
                enumMinimum = INT8_MIN;
            else if (backing == Lexer::Keyword::I16)
                enumMinimum = INT16_MIN;
            else if (backing == Lexer::Keyword::I32)
                enumMinimum = INT32_MIN;
            else if (backing == Lexer::Keyword::I64)
                enumMinimum = INT64_MIN;
        }
        uint64_t nextEnumValue = 0;
        bool enumNextValueOverflowed = false;
        for (const auto &child : node.children)
        {
            if (def.isEnum && child.type == Parser::NodeType::EnumMember)
            {
                const bool negativeLiteral =
                    child.children.size() > 1 && child.children[1].type == Parser::NodeType::UnaryExpr &&
                    std::get<Lexer::Operator>(child.children[1].value) == Lexer::Operator::MINUS &&
                    child.children[1].children.size() == 1 &&
                    child.children[1].children[0].type == Parser::NodeType::IntegerLiteral;
                if (negativeLiteral)
                {
                    const uint64_t magnitude = std::get<uint64_t>(child.children[1].children[0].value);
                    const uint64_t minimumMagnitude =
                        enumMinimum < 0 ? static_cast<uint64_t>(-(enumMinimum + 1)) + 1 : 0;
                    if (magnitude > minimumMagnitude)
                    {
                        error("enum member value does not fit backing type.", node);
                    }
                }
                uint64_t memberValue = nextEnumValue;
                if (child.children.size() > 1 && child.children[1].type == Parser::NodeType::IntegerLiteral)
                    memberValue = std::get<uint64_t>(child.children[1].value);
                if (enumNextValueOverflowed || memberValue > enumMaximum)
                {
                    error("enum member value does not fit backing type.", node);
                }
                enumNextValueOverflowed = memberValue == UINT64_MAX;
                if (!enumNextValueOverflowed)
                    nextEnumValue = memberValue + 1;
            }
            if (def.isEnum && child.type == Parser::NodeType::EnumMember && !child.children.empty())
            {
                const std::string memberName = std::string(std::get<std::string_view>(child.children.front().value));
                if (def.members.find(memberName) != def.members.end())
                {
                    error("Duplicate enum member '" + memberName + "'.", node);
                }
                Symbol member;
                member.name = memberName;
                def.members.emplace(memberName, std::move(member));
                continue;
            }
            if (child.type == Parser::NodeType::VariableDecl)
            {
                const auto has_named_runtime_array = [&](const auto &self, const Parser::ASTNode &typeNode) -> bool
                {
                    if (typeNode.type == Parser::NodeType::ArrayType &&
                        std::holds_alternative<std::string_view>(typeNode.value))
                        return true;
                    for (const Parser::ASTNode &nested : typeNode.children)
                        if (self(self, nested))
                            return true;
                    return false;
                };
                if (!child.children.empty() && has_named_runtime_array(has_named_runtime_array, child.children.front()))
                {
                    error("runtime array length bindings are local and cannot appear in struct fields.", node);
                }
                if (!child.children.empty() && child.children.front().type == Parser::NodeType::CustomType &&
                    std::get<std::string_view>(child.children.front().value) ==
                        std::get<std::string_view>(node.children.front().value))
                {
                    error("recursive-by-value field '" + std::string(std::get<std::string_view>(child.value)) +
                              "' requires a pointer or reference indirection.",
                          node);
                }
                Symbol field;
                field.name = std::string(std::get<std::string_view>(child.value));
                field.typeNode = child.children.empty() ? Parser::ASTNode{} : child.children.front();
                def.members[field.name] = field;
            }
            else if (child.type == Parser::NodeType::IndexDecl)
            {
                if (!def.indexedField.empty())
                {
                    error("A struct can declare only one indexed backing field.", node);
                }
                def.indexedField = std::string(std::get<std::string_view>(child.value));
            }
            else if (child.type == Parser::NodeType::InitDecl)
            {
                if (!def.initializedField.empty())
                {
                    error("A struct can declare only one initializer backing field.", node);
                }
                def.initializedField = std::string(std::get<std::string_view>(child.value));
            }
            else if (child.type == Parser::NodeType::FunctionDef || child.type == Parser::NodeType::FunctionDecl ||
                     child.type == Parser::NodeType::CFunctionDef || child.type == Parser::NodeType::CFunctionDecl)
            {
                Symbol method;
                method.name = child.children.empty()
                                  ? std::string{}
                                  : std::string(std::get<std::string_view>(child.children.front().value));
                method.typeNode = child.children.empty() ? Parser::ASTNode{} : child.children.front();
                method.isFunction = true;
                bool hasReceiver = false;
                for (const auto &parameter : child.children)
                {
                    if (parameter.type == Parser::NodeType::TunnelSlot && !parameter.children.empty())
                    {
                        method.tunnelSlotTypes.push_back(infer_type_from_node(parameter.children.front()));
                        method.tunnelSlotOptional.push_back(parameter.isOptional);
                        method.tunnelSlotNames.push_back(std::string(std::get<std::string_view>(parameter.value)));
                    }
                    if (parameter.type != Parser::NodeType::Param || parameter.children.empty())
                        continue;
                    if (!hasReceiver)
                    {
                        hasReceiver = true;
                        continue;
                    }
                    method.paramTypes.push_back(infer_type_from_node(parameter.children.front()));
                }
                if (method.name == "drop")
                {
                    error("Manual lifecycle methods are not supported; destruction is compiler-managed.", node);
                }
                if (!method.name.empty())
                    def.methods[method.name] = method;
            }
        }

        if (!def.indexedField.empty())
        {
            const auto backing = def.members.find(def.indexedField);
            if (backing == def.members.end())
            {
                error("Index declaration names unknown field '" + def.indexedField + "'.", node);
            }
            const Parser::NodeType type = backing->second.typeNode.type;
            if (type != Parser::NodeType::PointerType && type != Parser::NodeType::ArrayType)
            {
                error("Indexed backing field '" + def.indexedField + "' must be a pointer or runtime-sized array.",
                      node);
            }
        }
        if (!def.initializedField.empty())
        {
            const auto initialized = def.members.find(def.initializedField);
            if (initialized == def.members.end())
            {
                error("Init declaration names unknown field '" + def.initializedField + "'.", node);
            }
            if (initialized->second.typeNode.type != Parser::NodeType::PointerType)
            {
                error("Init backing field '" + def.initializedField + "' must be a pointer.", node);
            }
        }

        registeredTypes[def.name] = def;
    }

    void check_call_expr(SymbolTable &table, Parser::ASTNode &expr)
    {
        if (expr.children.empty())
            return;

        std::vector<Type> argTypes;
        std::vector<const Parser::ASTNode *> argNodes;
        std::vector<Type> explicitGenericArguments;
        for (size_t i = 1; i < expr.children.size(); ++i)
        {
            if (expr.children[i].type == Parser::NodeType::GenericArg)
            {
                if (expr.children[i].children.empty())
                    error("Malformed explicit generic argument.", expr.children[i]);
                explicitGenericArguments.push_back(infer_type_from_node(expr.children[i].children.front()));
                continue;
            }
            argTypes.push_back(infer_expression_type(table, expr.children[i]));
            argNodes.push_back(&expr.children[i]);
        }

        Type unionConstructor;
        if (infer_union_constructor_type(table, expr, unionConstructor))
            return;

        const auto &callee = expr.children.front();
        if (callee.type == Parser::NodeType::Identifier || callee.type == Parser::NodeType::ScopeAccessExpr)
        {
            const std::string calleeName = callee.type == Parser::NodeType::Identifier
                                               ? std::string(std::get<std::string_view>(callee.value))
                                               : scope_access_name(callee);
            if ((calleeName == "__shaft_format" || calleeName == "__shaft_println") && stdlibEnabled)
            {
                if (argTypes.empty() || expr.children[1].type != Parser::NodeType::StringLiteral)
                {
                    error(calleeName == "__shaft_format" ? "format! requires a string-literal format argument."
                                                         : "println! requires a string-literal format argument.",
                          callee);
                }
                if (calleeName == "__shaft_format" || calleeName == "__shaft_println")
                    validate_brace_format_call(callee, expr.children[1],
                                               std::vector<Type>(argTypes.begin() + 1, argTypes.end()));
                return;
            }
            Symbol *symbol = table.lookup(calleeName);

            for (std::string enclosing = current_namespace_name(); !symbol && !enclosing.empty();)
            {
                symbol = table.lookup(enclosing + "::" + calleeName);

                const size_t separator = enclosing.rfind("::");

                if (separator == std::string::npos)
                    enclosing.clear();
                else
                    enclosing.resize(separator);
            }

            for (const std::string &usingName : Parser::using_namespace_declarations)
            {
                if (symbol)
                    break;

                const size_t separator = usingName.rfind("::");

                if (separator == std::string::npos)
                    continue;

                const std::string importedName = usingName.substr(separator + 2);

                if (importedName != calleeName)
                    continue;

                symbol = table.lookup(usingName);
            }
            if (!symbol || !symbol->isFunction)
            {
                error("Unknown function '" + calleeName + "'.", callee);
            }
            const bool exposesRawPointer =
                std::any_of(symbol->paramTypes.begin(), symbol->paramTypes.end(), type_contains_raw_pointer) ||
                (symbol->hasResultType && type_contains_raw_pointer(symbol->resultType)) ||
                std::any_of(symbol->tunnelSlotTypes.begin(), symbol->tunnelSlotTypes.end(), type_contains_raw_pointer);
            const bool safeProcessExit = calleeName == "exit" || calleeName == "std::exit";
            if ((symbol->isCFunction || exposesRawPointer) && !safeProcessExit && unsafeDepth == 0 &&
                !is_trusted_stdlib_implementation(expr))
                error(symbol->isCFunction
                          ? "C-ABI calls require an enclosing @unsafe ... @end block."
                          : "calls that expose raw pointers require an enclosing @unsafe ... @end block.",
                      callee);
            if (symbol->paramTypes.size() != argTypes.size())
            {
                error("Function argument count mismatch.", callee);
            }
            if (!explicitGenericArguments.empty() &&
                symbol->genericParameters.size() != explicitGenericArguments.size())
            {
                error("Generic function argument count does not match its declaration.", callee);
            }
            std::unordered_map<std::string, Type> genericBindings;
            for (size_t index = 0; index < explicitGenericArguments.size(); ++index)
                genericBindings.emplace(symbol->genericParameters[index], explicitGenericArguments[index]);
            const bool validateArgumentTypes =
                callee.type != Parser::NodeType::ScopeAccessExpr || explicitGenericArguments.empty();
            for (size_t i = 0; i < argTypes.size() && validateArgumentTypes; ++i)
            {
                const Type parameterType = substitute_generic_type(symbol->paramTypes[i], genericBindings);
                const bool cAbiWordConversion =
                    symbol->isCFunction && ((is_numeric(parameterType.kind) && is_numeric(argTypes[i].kind)) ||
                                            (parameterType.kind == TypeKind::Pointer && is_numeric(argTypes[i].kind)) ||
                                            (is_numeric(parameterType.kind) && argTypes[i].kind == TypeKind::Pointer));
                if (!is_assignable(parameterType, argTypes[i]) &&
                    !implicitly_borrows_for_reference_parameter(parameterType, argTypes[i], *argNodes[i]) &&
                    !implicitly_passes_reference_as_pointer(parameterType, argTypes[i]) &&
                    !implicitly_dereferences_for_value_parameter(parameterType, argTypes[i]) &&
                    !string_literal_decays_to_pointer(parameterType, *argNodes[i]) && !cAbiWordConversion)
                {
                    error("Function argument type mismatch.", callee);
                }
            }
            if (!symbol->tunnelSlotNames.empty())
            {
                std::vector<Symbol *> reservations;
                bool allReservationsPresent = true;
                for (const std::string &slotName : symbol->tunnelSlotNames)
                {
                    Symbol *reservation = table.lookup(slotName);
                    if (!reservation || !reservation->isTunnelReservation)
                    {
                        allReservationsPresent = false;
                        reservations.push_back(nullptr);
                        continue;
                    }
                    reservations.push_back(reservation);
                }
                if (symbol->tunnelSlotNames.size() > 1 && !allReservationsPresent)
                {
                    error("Unbound multi-output call requires one compatible reserve declaration per tunnel slot.",
                          callee);
                }
                if (allReservationsPresent)
                {
                    for (size_t index = 0; index < reservations.size(); ++index)
                    {
                        Symbol *reservation = reservations[index];
                        const Type reservationType = infer_type_from_node(reservation->typeNode);
                        const Type reservationPayload = reservationType.isOptional && reservationType.innerType
                                                            ? *reservationType.innerType
                                                            : reservationType;
                        if (!types_structurally_equal(reservationPayload, symbol->tunnelSlotTypes[index]) ||
                            reservationType.isOptional != symbol->tunnelSlotOptional[index])
                        {
                            error("Standalone reserve binding type does not match tunnel output.", callee);
                        }
                        if (reservation->tunnelReservationConsumed)
                        {
                            error("tunnel reservation '" + symbol->tunnelSlotNames[index] + "' was already consumed.",
                                  callee);
                        }
                    }
                    for (Symbol *reservation : reservations)
                        reservation->tunnelReservationConsumed = true;
                }
            }
        }
        else if (callee.type == Parser::NodeType::MemberAccessExpr)
        {
            const Type receiverType =
                infer_expression_type(table, const_cast<Parser::ASTNode &>(callee.children.front()));
            const std::string methodName = std::string(std::get<std::string_view>(callee.value));
            auto it = registeredTypes.find(receiverType.name);
            if (it != registeredTypes.end())
            {
                auto methodIt = it->second.methods.find(methodName);
                if (methodIt != it->second.methods.end())
                {
                    if (methodIt->second.paramTypes.size() != argTypes.size())
                    {
                        error("Method argument count mismatch.", callee);
                    }
                    for (size_t i = 0; i < argTypes.size(); ++i)
                    {
                        if (!is_assignable(methodIt->second.paramTypes[i], argTypes[i]) &&
                            !implicitly_borrows_for_reference_parameter(methodIt->second.paramTypes[i], argTypes[i],
                                                                        *argNodes[i]) &&
                            !implicitly_dereferences_for_value_parameter(methodIt->second.paramTypes[i], argTypes[i]) &&
                            !string_literal_decays_to_pointer(methodIt->second.paramTypes[i], *argNodes[i]))
                        {
                            error("Method argument type mismatch.", callee);
                        }
                    }
                }
            }
        }
    }

    void check_member_access(SymbolTable &table, Parser::ASTNode &expr)
    {
        if (expr.children.empty())
            return;

        if (expr.type == Parser::NodeType::ScopeAccessExpr)
        {
            (void)infer_expression_type(table, expr);
            return;
        }

        const Type receiverType = infer_expression_type(table, expr.children.front());
        if (receiverType.kind == TypeKind::Error)
            return;

        std::string memberName;
        if (!expr.children.empty() && expr.children.size() > 1)
            memberName = std::string(std::get<std::string_view>(expr.children[1].value));
        else
            memberName = std::string(std::get<std::string_view>(expr.value));

        if (receiverType.kind == TypeKind::Class || receiverType.kind == TypeKind::Struct)
        {
            auto it = registeredTypes.find(receiverType.name);
            if (it != registeredTypes.end())
            {
                auto fieldIt = it->second.members.find(memberName);
                if (fieldIt != it->second.members.end())
                {
                    expr.hasInferredType = true;
                    expr.inferredTypeName = type_to_string(infer_type_from_node(fieldIt->second.typeNode));
                }
            }
        }
    }

    void check_mutable_assignment_target(SymbolTable &table, Parser::ASTNode &target)
    {
        if (target.type == Parser::NodeType::Identifier)
        {
            const std::string name = std::string(std::get<std::string_view>(target.value));
            Symbol *symbol = table.lookup(name);
            if (!symbol)
            {
                error("Cannot assign to undeclared variable.", target);
            }
            const Type rootType = infer_type_from_node(symbol->typeNode);
            if (symbol->borrowCount > 0 || symbol->has_writer)
            {
                error("Cannot mutate a borrowed variable.", target);
            }
            if (!rootType.isMutable && rootType.kind != TypeKind::Pointer)
            {
                error("Cannot mutate a field because its containing value is not mutable.", target);
            }
            return;
        }

        if (target.type == Parser::NodeType::IndexExpr && !target.children.empty())
        {
            check_mutable_assignment_target(table, target.children.front());
            return;
        }

        if (target.type != Parser::NodeType::MemberAccessExpr || target.children.empty())
            return;

        check_mutable_assignment_target(table, target.children.front());
        const Type receiver = infer_expression_type(table, target.children.front());
        const Type &aggregateReceiver =
            (receiver.kind == TypeKind::Pointer || receiver.kind == TypeKind::Reference) && receiver.innerType
                ? *receiver.innerType
                : receiver;
        const auto definition = registeredTypes.find(aggregateReceiver.name);
        const std::string fieldName = std::string(std::get<std::string_view>(target.value));
        if (definition == registeredTypes.end() ||
            definition->second.members.find(fieldName) == definition->second.members.end())
        {
            error("Unknown field in assignment target.", target);
        }
        const Symbol &field = definition->second.members.at(fieldName);
        if (!infer_type_from_node(field.typeNode).isMutable)
        {
            error("Cannot mutate a field because the field is not mutable.", target);
        }
    }

    // ownership pass
    void check_assignment_and_ownership(SymbolTable &table, Parser::ASTNode &node)
    {
        if (node.children.empty())
            return;

        Type declaredType(TypeKind::Error);
        bool hasDeclaredType = false;

        if (node.type == Parser::NodeType::VariableDecl)
        {
            declaredType = infer_type_from_node(node.children.front());
            hasDeclaredType = true;
        }
        else if (node.type == Parser::NodeType::AssignmentExpr && node.children.size() >= 2)
        {
            const auto &target = node.children.front();
            if (target.type == Parser::NodeType::Identifier)
            {
                Symbol *sym = table.lookup(std::string(std::get<std::string_view>(target.value)));
                if (!sym)
                {
                    error("Cannot assign to undeclared variable.", target);
                }
                if (sym->borrowCount > 0 || sym->has_writer)
                {
                    error("Cannot mutate a borrowed variable.", target);
                }
                if (!infer_type_from_node(sym->typeNode).isMutable)
                {
                    error("Cannot mutate because this binding is not mutable.", target);
                }
                declaredType = infer_type_from_node(sym->typeNode);
                hasDeclaredType = true;
            }
            else if (target.type == Parser::NodeType::MemberAccessExpr || target.type == Parser::NodeType::IndexExpr)
            {
                check_mutable_assignment_target(table, const_cast<Parser::ASTNode &>(target));
                declaredType = infer_expression_type(table, const_cast<Parser::ASTNode &>(target));
                hasDeclaredType = true;
            }
        }

        if (node.type == Parser::NodeType::AssignmentExpr && hasDeclaredType)
        {
            const Lexer::Operator assignment = std::get<Lexer::Operator>(node.value);
            Lexer::Operator binary = Lexer::Operator::ASSIGN;
            switch (assignment)
            {
            case Lexer::Operator::ADD_ASSIGN:
                binary = Lexer::Operator::PLUS;
                break;
            case Lexer::Operator::SUBTRACT_ASSIGN:
                binary = Lexer::Operator::MINUS;
                break;
            case Lexer::Operator::MULTIPLY_ASSIGN:
                binary = Lexer::Operator::MULTIPLY;
                break;
            case Lexer::Operator::DIVIDE_ASSIGN:
                binary = Lexer::Operator::DIVIDE;
                break;
            case Lexer::Operator::MODULO_ASSIGN:
                binary = Lexer::Operator::MODULO;
                break;
            case Lexer::Operator::LEFT_SHIFT_ASSIGN:
                binary = Lexer::Operator::LEFT_SHIFT;
                break;
            case Lexer::Operator::RIGHT_SHIFT_ASSIGN:
                binary = Lexer::Operator::RIGHT_SHIFT;
                break;
            default:
                break;
            }
            if (binary != Lexer::Operator::ASSIGN &&
                !is_valid_binary_op(declaredType, infer_expression_type(table, node.children[1]), binary))
            {
                error("Invalid compound assignment for the given operand types.", node);
            }
        }

        for (size_t i = (node.type == Parser::NodeType::VariableDecl ? 1u : 1u); i < node.children.size(); ++i)
        {
            const auto &child = node.children[i];
            if (child.type == Parser::NodeType::MoveExpr)
            {
                const OwnershipPath sourcePath =
                    child.children.empty() ? OwnershipPath{} : ownership_path(child.children.front());
                const std::string sourceVar =
                    sourcePath.root.empty()
                        ? (child.children.empty() ? std::string{} : owning_root_name(child.children.front()))
                        : sourcePath.root;
                if (sourceVar.empty())
                {
                    error("Cannot move without an addressable source.", node);
                }
                Symbol *sym = table.lookup(sourceVar);

                if (!sym)
                {
                    error("Cannot move from undeclared variable '" + sourceVar + "'.", node);
                }
                const auto is_named_runtime_array_type = [&](const auto &self, const Parser::ASTNode &typeNode) -> bool
                {
                    if (typeNode.type == Parser::NodeType::ArrayType &&
                        std::holds_alternative<std::string_view>(typeNode.value))
                        return true;
                    for (const Parser::ASTNode &nested : typeNode.children)
                        if (self(self, nested))
                            return true;
                    return false;
                };
                if (!child.children.empty() && child.children.front().type == Parser::NodeType::IndexExpr &&
                    child.children.front().children.size() == 2 &&
                    child.children.front().children[1].type != Parser::NodeType::IntegerLiteral &&
                    is_named_runtime_array_type(is_named_runtime_array_type, sym->typeNode))
                {
                    error("Cannot move from a dynamic runtime-array index.", node);
                }
                if (sym->state == VarOwnershipState::Moved ||
                    (!sourcePath.relative.empty() && (subobject_is_moved(*sym, sourcePath.relative) ||
                                                      subobject_contains_moved_value(*sym, sourcePath.relative))))
                {
                    error("Cannot move from already moved variable '" + sourceVar + "'.", node);
                }
                if (sym->borrowCount > 0 || sym->has_writer)
                {
                    error("Cannot move a borrowed variable.", node);
                }
                if (hasDeclaredType && !is_assignable(declaredType, infer_type_from_node(sym->typeNode)))
                {
                    error("Type mismatch in move assignment or initialization.", node);
                }

                if (sourcePath.relative.empty())
                    sym->state = VarOwnershipState::Moved;
                else
                    sym->subobjectStates[sourcePath.relative] = VarOwnershipState::Moved;
            }
            else if (child.type == Parser::NodeType::RefExpr)
            {
                const OwnershipPath sourcePath =
                    child.children.empty() ? OwnershipPath{} : ownership_path(child.children.front());
                const std::string sourceVar =
                    sourcePath.root.empty()
                        ? (child.children.empty() ? std::string{} : owning_root_name(child.children.front()))
                        : sourcePath.root;
                if (sourceVar.empty())
                {
                    error("Cannot borrow without an addressable source.", node);
                }
                Symbol *sym = table.lookup(sourceVar);

                if (!sym)
                {
                    error("Cannot borrow from undeclared variable '" + sourceVar + "'.", node);
                }
                if (sym->state == VarOwnershipState::Moved ||
                    (!sourcePath.relative.empty() && subobject_is_moved(*sym, sourcePath.relative)))
                {
                    error("Cannot borrow from moved " +
                              (sourcePath.relative.empty() ? "variable '" + sourceVar + "'" : "subobject") + ".",
                          node);
                }
                if (!hasDeclaredType || declaredType.kind != TypeKind::Reference)
                {
                    error("A ref initializer requires an &T or &mut T destination.", node);
                }
                const bool mutableBorrow = declaredType.isMutable;
                if (mutableBorrow && !infer_type_from_node(sym->typeNode).isMutable)
                {
                    error("Cannot create an &mut borrow because the source is not mutable.", node);
                }
                if (!table.record_borrow(sourceVar, mutableBorrow))
                {
                    error(std::string("Cannot borrow a variable already borrowed ") +
                              (mutableBorrow ? "by readers or a writer" : "mutably") + ".",
                          node);
                }
            }
            else if (hasDeclaredType && child.type != Parser::NodeType::PrimitiveType &&
                     child.type != Parser::NodeType::CustomType && child.type != Parser::NodeType::PointerType &&
                     child.type != Parser::NodeType::ReferenceType && child.type != Parser::NodeType::StructInitExpr &&
                     child.type != Parser::NodeType::MoveExpr && child.type != Parser::NodeType::RefExpr)
            {
                const bool contextualUnionConstructor =
                    declaredType.kind == TypeKind::Union && child.type == Parser::NodeType::CallExpr &&
                    check_union_constructor_for_target(table, const_cast<Parser::ASTNode &>(child), declaredType);
                Type valueType = contextualUnionConstructor
                                     ? declaredType
                                     : infer_expression_type(table, const_cast<Parser::ASTNode &>(child));
                if ((child.type == Parser::NodeType::Identifier || child.type == Parser::NodeType::MemberAccessExpr ||
                     child.type == Parser::NodeType::IndexExpr) &&
                    type_requires_cleanup(valueType))
                {
                    error("Cannot implicitly copy a cleanup-owning value; use move or ref.", node);
                }
                const bool negativeIntegerLiteral = child.type == Parser::NodeType::UnaryExpr &&
                                                    std::get<Lexer::Operator>(child.value) == Lexer::Operator::MINUS &&
                                                    child.children.size() == 1 &&
                                                    child.children.front().type == Parser::NodeType::IntegerLiteral;
                const bool contextualNumericLiteral =
                    ((child.type == Parser::NodeType::IntegerLiteral || child.type == Parser::NodeType::FloatLiteral) &&
                     is_numeric(declaredType.kind)) ||
                    (negativeIntegerLiteral && is_signed_integer(declaredType.kind));
                if (!is_assignable(declaredType, valueType) && !contextualNumericLiteral &&
                    !string_literal_decays_to_pointer(declaredType, child))
                {
                    error("Cannot assign type '" + type_to_string(valueType) + "' to '" + type_to_string(declaredType) +
                              "'.",
                          node, explicit_cast_help(declaredType));
                }
            }
        }

        if (node.type == Parser::NodeType::AssignmentExpr && node.children.size() >= 2)
        {
            const OwnershipPath targetPath = ownership_path(node.children.front());
            if (!targetPath.root.empty())
            {
                if (Symbol *target = table.lookup(targetPath.root))
                {
                    if (targetPath.relative.empty())
                    {
                        target->state = VarOwnershipState::Valid;
                        target->subobjectStates.clear();
                    }
                    else
                    {
                        restore_subobject(*target, targetPath.relative);
                    }
                }
            }
        }
    }

    void check_function_tunnels(SymbolTable &table, const Parser::ASTNode &funcNode)
    {
        activeTunnelSlots.clear();
        size_t tunnelSlotCount = 0;

        for (const auto &child : funcNode.children)
        {
            if (child.type != Parser::NodeType::TunnelSlot)
                continue;

            ++tunnelSlotCount;
            const std::string name = std::string(std::get<std::string_view>(child.value));
            const bool has_type = !child.children.empty();

            activeTunnelSlots[name] = {name, child.isOptional, false,
                                       has_type ? infer_type_from_node(child.children.front()) : Type{}, has_type};
        }

        struct Flow
        {
            bool may_fill = false;
            bool must_fill = false;
        };

        using Flows = std::unordered_map<std::string, Flow>;
        Flows initial;

        for (const auto &[name, slot] : activeTunnelSlots)
            initial[name] = {};

        std::unordered_map<std::string, std::string> boundTunnelSlots;

        auto mark_slot_filled = [&](Flows &flows, const std::string &slot, const Parser::ASTNode &node)
        {
            const auto declared = activeTunnelSlots.find(slot);
            if (declared == activeTunnelSlots.end())
            {
                error("Target tunnel slot '" + slot + "' does not exist.", node);
            }

            if (flows[slot].may_fill)
            {
                error("Tunnel slot '" + slot + "' executed more than once.", node);
            }

            flows[slot] = {true, true};
        };

        auto visit = [&](auto &self, const Parser::ASTNode &node, Flows flows) -> Flows
        {
            if (node.type == Parser::NodeType::TunnelStmt)
            {
                if (!node.children.empty() && node.children.front().type == Parser::NodeType::RefExpr)
                {
                    error("cannot tunnel a reference to function-local storage.", node);
                }

                const std::string slotName = std::string(std::get<std::string_view>(node.value));

                if (const auto slot = activeTunnelSlots.find(slotName);
                    slot != activeTunnelSlots.end() && node.children.size() > 1)
                {
                    const Type restated = infer_type_from_node(node.children[1]);
                    if (!types_structurally_equal(slot->second.expectedType, restated))
                    {
                        error("tunnel output type restatement does not match declared slot type.", node);
                    }
                }

                mark_slot_filled(flows, slotName, node);
                return flows;
            }

            if (node.type == Parser::NodeType::ReserveStmt && node.children.size() == 2 &&
                node.children[1].type == Parser::NodeType::TunnelBindingExpr)
            {
                const std::string local = std::string(std::get<std::string_view>(node.value));
                const std::string slot = std::string(std::get<std::string_view>(node.children[1].value));

                if (activeTunnelSlots.find(slot) == activeTunnelSlots.end())
                {
                    error("Target tunnel slot '" + slot + "' does not exist.", node);
                }

                boundTunnelSlots[local] = slot;
                return flows;
            }

            if (node.type == Parser::NodeType::AssignmentExpr && !node.children.empty() &&
                node.children.front().type == Parser::NodeType::Identifier)
            {
                const std::string target = std::string(std::get<std::string_view>(node.children.front().value));

                if (const auto bound = boundTunnelSlots.find(target); bound != boundTunnelSlots.end())
                {
                    mark_slot_filled(flows, bound->second, node);
                }

                return flows;
            }

            if (node.type == Parser::NodeType::MatchStmt && node.children.size() >= 2)
            {
                const Flows incoming = flows;
                std::vector<Flows> branchFlows;
                bool hasDefault = false;

                for (size_t index = 1; index < node.children.size(); ++index)
                {
                    const Parser::ASTNode &branch = node.children[index];

                    if (branch.type == Parser::NodeType::MatchCase && branch.children.size() >= 2)
                    {
                        branchFlows.push_back(self(self, branch.children[1], incoming));
                    }
                    else if (branch.type == Parser::NodeType::MatchDefault && !branch.children.empty())
                    {
                        hasDefault = true;
                        branchFlows.push_back(self(self, branch.children.front(), incoming));
                    }
                }

                if (!hasDefault)
                    branchFlows.push_back(incoming);

                for (auto &[name, flow] : flows)
                {
                    flow.may_fill = false;
                    flow.must_fill = !branchFlows.empty();

                    for (const Flows &branch : branchFlows)
                    {
                        flow.may_fill = flow.may_fill || branch.at(name).may_fill;
                        flow.must_fill = flow.must_fill && branch.at(name).must_fill;
                    }
                }

                return flows;
            }

            if (node.type == Parser::NodeType::IfStmt && node.children.size() >= 2)
            {
                Flows then_flow = self(self, node.children[1], flows);

                Flows else_flow = flows;

                if (node.children.size() >= 3)
                    else_flow = self(self, node.children[2], flows);

                for (auto &[name, flow] : flows)
                {
                    flow.may_fill = then_flow[name].may_fill || else_flow[name].may_fill;

                    flow.must_fill = then_flow[name].must_fill && else_flow[name].must_fill;
                }

                return flows;
            }

            if (node.type == Parser::NodeType::WhileLoop || node.type == Parser::NodeType::ForLoop ||
                node.type == Parser::NodeType::ForeachLoop)
            {
                Flows body_flow = flows;

                for (const auto &child : node.children)
                    body_flow = self(self, child, body_flow);

                for (auto &[name, flow] : flows)
                {
                    flow.may_fill = flow.may_fill || body_flow[name].may_fill;
                }

                return flows;
            }

            for (const auto &child : node.children)
                flows = self(self, child, flows);

            return flows;
        };

        Flows result = initial;

        for (const auto &child : funcNode.children)
        {
            if (child.type == Parser::NodeType::BlockStmt)
                result = visit(visit, child, result);
        }

        for (auto &[name, slot] : activeTunnelSlots)
        {
            slot.filled = result[name].must_fill;

            if (!slot.isOptional && !slot.filled)
            {
                error("Function exiting without populating required tunnel slot '" + name + "'.", funcNode);
            }
        }
    }

    // type checker
    void check_binary_expr(SymbolTable &table, Parser::ASTNode &exprNode)
    {
        if (exprNode.children.size() < 2)
            return;

        const Type leftType = infer_expression_type(table, exprNode.children[0]);
        const Type rightType = infer_expression_type(table, exprNode.children[1]);
        Lexer::Operator op = Lexer::Operator::ASSIGN;
        if (exprNode.value.index() == 5)
            op = std::get<Lexer::Operator>(exprNode.value);

        if (op == Lexer::Operator::TRIPPLE_DOT)
        {
            error("Range expressions are only valid as foreach iterables.", exprNode);
        }

        if (!is_valid_binary_op(leftType, rightType, op))
        {
            error("Invalid operation for the given operand types.", exprNode);
        }

        Type resultType(TypeKind::Error);
        if (op == Lexer::Operator::AND || op == Lexer::Operator::OR)
            resultType = Type(TypeKind::Bool);
        else if (op == Lexer::Operator::LESS_THAN || op == Lexer::Operator::GREATER_THAN ||
                 op == Lexer::Operator::LESS_EQUAL || op == Lexer::Operator::GREATER_EQUAL ||
                 op == Lexer::Operator::EQUAL || op == Lexer::Operator::NOT_EQUAL)
            resultType = {TypeKind::Bool, {}};
        else if ((leftType.kind == TypeKind::Pointer || leftType.kind == TypeKind::Reference) &&
                 is_numeric(rightType.kind) && (op == Lexer::Operator::PLUS || op == Lexer::Operator::MINUS))
            resultType = leftType;
        else if (is_numeric(leftType.kind) && is_numeric(rightType.kind))
            resultType = (leftType.kind == TypeKind::F32 || leftType.kind == TypeKind::F64 ||
                          rightType.kind == TypeKind::F32 || rightType.kind == TypeKind::F64)
                             ? Type(TypeKind::F64)
                             : (leftType.kind == rightType.kind ? leftType : Type(TypeKind::U64));
        else if (leftType.kind == rightType.kind)
            resultType = leftType;

        exprNode.hasInferredType = true;
        exprNode.inferredTypeName = type_to_string(resultType);
    }

    void declare_function_symbols(SymbolTable &table, Parser::ASTNode &node, const std::string &namespaceName = {})
    {
        if (node.type == Parser::NodeType::NamespaceDecl)
        {
            std::string nested = namespaceName;
            if (!nested.empty())
                nested += "::";
            nested += std::string(std::get<std::string_view>(node.value));
            for (auto &child : node.children)
                declare_function_symbols(table, child, nested);
            return;
        }
        if (node.type == Parser::NodeType::ImplDecl)
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::CustomType)
                return;
            std::string targetName = custom_type_name(node.children.front());
            if (targetName.find("::") == std::string::npos && !namespaceName.empty())
                targetName = namespaceName + "::" + targetName;
            structNameStack.push_back(targetName);
            for (auto &child : node.children)
            {
                if (child.type == Parser::NodeType::FunctionDecl || child.type == Parser::NodeType::FunctionDef ||
                    child.type == Parser::NodeType::CFunctionDecl || child.type == Parser::NodeType::CFunctionDef)
                    declare_function_symbols(table, child, targetName);
            }
            structNameStack.pop_back();
            return;
        }
        if (node.type == Parser::NodeType::TraitDecl)
            return;
        if (node.type == Parser::NodeType::StructDecl)
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::Identifier)
                return;
            const std::string structName = std::string(std::get<std::string_view>(node.children.front().value));
            structNameStack.push_back(structName);

            std::string nested = namespaceName;
            if (!nested.empty())
                nested += "::";
            nested += structName;

            for (auto &child : node.children)
            {
                const bool isStaticFunction =
                    (child.type == Parser::NodeType::FunctionDecl || child.type == Parser::NodeType::FunctionDef ||
                     child.type == Parser::NodeType::CFunctionDecl || child.type == Parser::NodeType::CFunctionDef) &&
                    (child.children.empty() || child.children[1].type != Parser::NodeType::Param /* no receiver */);
                declare_function_symbols(table, child, isStaticFunction ? nested : namespaceName);
            }
            structNameStack.pop_back();
            return;
        }
        if (node.type == Parser::NodeType::FunctionDecl || node.type == Parser::NodeType::FunctionDef ||
            node.type == Parser::NodeType::CFunctionDecl || node.type == Parser::NodeType::CFunctionDef)
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::Identifier)
                return;

            const std::string unqualifiedName = std::string(std::get<std::string_view>(node.children.front().value));
            const std::string name = namespaceName.empty() ? unqualifiedName : namespaceName + "::" + unqualifiedName;
            Symbol symbol;
            symbol.name = name;
            symbol.isFunction = true;
            symbol.isCFunction =
                node.type == Parser::NodeType::CFunctionDecl || node.type == Parser::NodeType::CFunctionDef;
            std::vector<std::string> genericParameters;
            for (const auto &child : node.children)
                if (child.type == Parser::NodeType::GenericParam)
                {
                    genericParameters.push_back(std::string(std::get<std::string_view>(child.value)));
                    activeGenericNames.push_back(genericParameters.back());
                }
            symbol.genericParameters = genericParameters;
            for (const auto &child : node.children)
            {
                if (child.type == Parser::NodeType::Param && !child.children.empty())
                    symbol.paramTypes.push_back(infer_type_from_node(child.children.front()));
                if ((node.type == Parser::NodeType::FunctionDecl || node.type == Parser::NodeType::FunctionDef) &&
                    child.type == Parser::NodeType::TunnelSlot && !child.children.empty())
                {
                    Type slotType = infer_type_from_node(child.children.front());
                    if (std::find(genericParameters.begin(), genericParameters.end(), slotType.name) !=
                        genericParameters.end())
                        slotType = Type(TypeKind::Generic, slotType.name);
                    symbol.tunnelSlotOptional.push_back(child.isOptional);
                    symbol.tunnelSlotNames.push_back(std::string(std::get<std::string_view>(child.value)));
                    symbol.tunnelSlotTypes.push_back(slotType);
                    if (!symbol.hasResultType)
                    {
                        symbol.resultType = slotType;
                        symbol.hasResultType = true;
                    }
                }
                if ((node.type == Parser::NodeType::CFunctionDecl || node.type == Parser::NodeType::CFunctionDef) &&
                    (child.type == Parser::NodeType::PrimitiveType || child.type == Parser::NodeType::PointerType ||
                     child.type == Parser::NodeType::ReferenceType || child.type == Parser::NodeType::CustomType) &&
                    !symbol.hasResultType)
                {
                    symbol.resultType = infer_type_from_node(child);
                    symbol.hasResultType = true;
                }
            }
            pop_generic_params(genericParameters.size());

            Symbol *existing = table.lookup(name);
            if (existing && namespaceName.empty() && existing->isFunction && existing->name != name)
            {
                *existing = std::move(symbol);
                return;
            }
            if (existing)
            {
                if (!existing->isFunction || existing->paramTypes.size() != symbol.paramTypes.size())
                {
                    error("Conflicting declaration of function '" + name + "'.", node);
                }
                return;
            }
            table.declare(name, std::move(symbol));
            return;
        }

        for (auto &child : node.children)
            declare_function_symbols(table, child, namespaceName);
    }

    void check_node(SymbolTable &table, Parser::ASTNode &node)
    {
        switch (node.type)
        {
        case Parser::NodeType::UnsafeBlock:
            ++unsafeDepth;
            table.push_scope();
            for (auto &child : node.children)
                check_node(table, child);
            table.pop_scope();
            --unsafeDepth;
            break;

        case Parser::NodeType::NoCheckBlock:
        case Parser::NodeType::NoCleanupBlock:
            table.push_scope();
            for (auto &child : node.children)
                check_node(table, child);
            table.pop_scope();
            break;

        case Parser::NodeType::BlockStmt:
            table.push_scope();
            for (auto &child : node.children)
                check_node(table, child);
            table.pop_scope();
            break;

        case Parser::NodeType::NamespaceDecl:
            activeNamespaces.push_back(std::string(std::get<std::string_view>(node.value)));
            table.push_scope();
            for (auto &child : node.children)
                check_node(table, child);
            table.pop_scope();
            activeNamespaces.pop_back();
            break;

        case Parser::NodeType::Module:
            for (auto &child : node.children)
                check_node(table, child);
            break;

        case Parser::NodeType::VariableDecl:
            if (!node.children.empty())
                normalize_typeof_type(table, node.children.front());
            check_assignment_and_ownership(table, node);
            {
                const auto validate_runtime_array_lengths = [&](const auto &self,
                                                                const Parser::ASTNode &typeNode) -> void
                {
                    if (typeNode.type == Parser::NodeType::ArrayType &&
                        std::holds_alternative<std::string_view>(typeNode.value))
                    {
                        const std::string lengthName = std::string(std::get<std::string_view>(typeNode.value));
                        const Symbol *length = table.lookup(lengthName);
                        if (!length)
                        {
                            error("runtime array length '" + lengthName + "' must be declared before the array.", node);
                        }
                        const Type lengthType = infer_type_from_node(length->typeNode);
                        if (lengthType.kind != TypeKind::U64 && lengthType.kind != TypeKind::USIZE)
                        {
                            error("runtime array length '" + lengthName + "' must have type u64 or usize.", node);
                        }
                    }
                    for (const Parser::ASTNode &child : typeNode.children)
                        self(self, child);
                };
                if (!node.children.empty())
                    validate_runtime_array_lengths(validate_runtime_array_lengths, node.children.front());
                const std::string name = std::string(std::get<std::string_view>(node.value));
                Symbol symbol;
                symbol.name = name;
                symbol.typeNode = node.children.empty() ? Parser::ASTNode{} : node.children.front();
                if (!table.declare(name, symbol))
                {
                    error("Redeclaration of variable '" + name + "'.", node);
                }
                break;
            }

        case Parser::NodeType::MultiReserveStmt:
        {
            if (node.children.size() < 5 || node.children.back().type != Parser::NodeType::CallExpr)
            {
                error("Multi-result reserve requires a function call initializer.", node);
            }
            Parser::ASTNode &call = node.children.back();
            if (call.children.empty())
            {
                error("Multi-result reserve requires a named tunnel function.", node);
            }
            const Parser::ASTNode &calleeExpression = call.children.front();
            const Symbol *callee = nullptr;
            if (calleeExpression.type == Parser::NodeType::Identifier)
            {
                const std::string calleeName = std::string(std::get<std::string_view>(calleeExpression.value));
                callee = table.lookup(calleeName);
            }
            else if (calleeExpression.type == Parser::NodeType::MemberAccessExpr && !calleeExpression.children.empty())
            {
                const Type receiverType =
                    infer_expression_type(table, const_cast<Parser::ASTNode &>(calleeExpression.children.front()));
                const auto definition = registeredTypes.find(receiverType.name);
                if (definition != registeredTypes.end())
                {
                    const std::string methodName = std::string(std::get<std::string_view>(calleeExpression.value));
                    const auto method = definition->second.methods.find(methodName);
                    if (method != definition->second.methods.end())
                        callee = &method->second;
                    if (!callee)
                        callee = table.lookup(receiverType.name + "::" + methodName);
                }
                check_call_expr(table, call);
            }
            const size_t bindingCount = (node.children.size() - 1) / 2;
            if (!callee || !callee->isFunction || callee->tunnelSlotTypes.size() != bindingCount)
            {
                error("Multi-result reserve binding count does not match tunnel outputs.", node);
            }
            for (size_t index = 0; index < bindingCount; ++index)
            {
                const Type bindingType = infer_type_from_node(node.children[index * 2]);
                const Type bindingPayload =
                    bindingType.isOptional && bindingType.innerType ? *bindingType.innerType : bindingType;
                const Type &slotType = callee->tunnelSlotTypes[index];
                if (bindingPayload.kind != slotType.kind || bindingPayload.name != slotType.name ||
                    bindingType.isOptional != callee->tunnelSlotOptional[index])
                {
                    error("Multi-result reserve binding type does not match tunnel output.", node);
                }
            }
            for (size_t index = 0; index + 1 < node.children.size() - 1; index += 2)
            {
                const std::string bindingName = std::string(std::get<std::string_view>(node.children[index + 1].value));
                Symbol symbol;
                symbol.name = bindingName;
                symbol.typeNode = node.children[index];
                if (!table.declare(bindingName, std::move(symbol)))
                {
                    error("Redeclaration of variable '" + bindingName + "'.", node);
                }
            }
            break;
        }

        case Parser::NodeType::ReserveStmt:
        {
            const std::string name = std::string(std::get<std::string_view>(node.value));
            if (node.children.empty())
            {
                error("Reserve declaration is missing its type.", node);
            }
            const Type declared = infer_type_from_node(node.children.front());
            if (node.children.size() > 1 && node.children[1].type == Parser::NodeType::CallExpr &&
                !node.children[1].children.empty() &&
                node.children[1].children.front().type == Parser::NodeType::Identifier)
            {
                const std::string calleeName =
                    std::string(std::get<std::string_view>(node.children[1].children.front().value));
                const Symbol *callee = table.lookup(calleeName);
                if (callee && callee->isFunction && !callee->tunnelSlotTypes.empty())
                {
                    if (callee->tunnelSlotTypes.size() != 1)
                    {
                        error("Reserve binding count does not match tunnel outputs.", node);
                    }
                    const Type &slotType = callee->tunnelSlotTypes.front();
                    const Type bindingPayload =
                        declared.isOptional && declared.innerType ? *declared.innerType : declared;
                    if (bindingPayload.kind != slotType.kind || bindingPayload.name != slotType.name ||
                        declared.isOptional != callee->tunnelSlotOptional.front())
                    {
                        error("reserve binding type does not match tunnel output.", node);
                    }
                }
            }
            if (node.children.size() > 1 && node.children[1].type != Parser::NodeType::TunnelBindingExpr &&
                !is_assignable(declared, infer_expression_type(table, node.children[1])) &&
                !string_literal_decays_to_pointer(declared, node.children[1]))
            {
                error("Type mismatch in reserve initialization.", node);
            }
            Symbol symbol;
            symbol.name = name;
            symbol.typeNode = node.children.front();
            symbol.isTunnelReservation = node.children.size() == 1;
            if (!table.declare(name, std::move(symbol)))
            {
                error("Redeclaration of variable '" + name + "'.", node);
            }
            break;
        }

        case Parser::NodeType::AssignmentExpr:
            check_assignment_and_ownership(table, node);
            break;

        case Parser::NodeType::DoExpr:
            (void)infer_expression_type(table, node);
            break;

        case Parser::NodeType::ExprStmt:
            if (node.children.size() != 1)
                error("Expression statement must contain exactly one expression.", node);
            validate_string_literal_escapes_in_tree(node.children.front());
            check_node(table, node.children.front());
            break;

        case Parser::NodeType::IfStmt:
        {
            if (node.children.empty() || infer_expression_type(table, node.children.front()).kind != TypeKind::Bool)
            {
                error("Boolean condition required for control flow.", node);
            }
            for (auto &child : node.children)
                check_node(table, child);
            break;
        }

        case Parser::NodeType::MatchStmt:
        {
            for (size_t index = 1; index < node.children.size(); ++index)
            {
                const Parser::ASTNode &arm = node.children[index];
                if (arm.type == Parser::NodeType::MatchDefault && index + 1 != node.children.size())
                    error("match default must be the final arm.", arm);
            }
            for (auto &child : node.children)
                check_node(table, child);
            break;
        }

        case Parser::NodeType::UnionMatchStmt:
        {
            const size_t armChildren = node.children.size() - 1 - (node.hasElse ? 1 : 0);
            if (node.children.size() < 3 || armChildren % 2 != 0 ||
                node.children[0].type != Parser::NodeType::Identifier)
                error("Malformed union pattern match.", node);
            const std::string subjectName = std::string(std::get<std::string_view>(node.children[0].value));
            Symbol *subject = table.lookup(subjectName);
            if (!subject)
                error("Union pattern subject is not declared.", node.children[0]);
            const Type subjectType = infer_type_from_node(subject->typeNode);
            if (subjectType.kind != TypeKind::Union)
                error("Union pattern requires a union value.", node.children[0]);
            const auto definition = registeredTypes.find(subjectType.name);
            if (definition == registeredTypes.end() || !definition->second.isUnion)
                error("Union pattern references an unknown union type.", node);
            for (size_t armIndex = 1; armIndex <= armChildren; armIndex += 2)
            {
                const Parser::ASTNode &pattern = node.children[armIndex];
                if (pattern.type != Parser::NodeType::UnionPattern)
                    error("Malformed union pattern arm.", pattern);
                if (pattern.inferredTypeName != subjectType.name)
                    error("Union pattern does not name the subject's union type.", pattern);
                const std::string variantName = std::string(std::get<std::string_view>(pattern.value));
                const auto variant = definition->second.unionVariants.find(variantName);
                if (variant == definition->second.unionVariants.end())
                    error("Unknown union pattern variant.", pattern);
                if (pattern.children.size() != variant->second.payloadTypes.size())
                    error("Union pattern binding count does not match variant payload.", pattern);

                table.push_scope();
                for (size_t index = 0; index < variant->second.payloadTypes.size(); ++index)
                {
                    Symbol binding;
                    const std::string bindingName =
                        std::string(std::get<std::string_view>(pattern.children[index].value));
                    binding.name = bindingName;
                    binding.typeNode = substitute_union_payload_node(definition->second, subject->typeNode,
                                                                     variant->second.payloadTypes[index]);
                    if (!table.declare(bindingName, std::move(binding)))
                        error("Duplicate union pattern binding.", pattern.children[index]);
                }
                check_node(table, node.children[armIndex + 1]);
                table.pop_scope();
            }
            if (node.hasElse)
                check_node(table, node.children.back());
            break;
        }

        case Parser::NodeType::StateBindingDecl:
        case Parser::NodeType::ThreadBindingDecl:
        {
            if (node.children.size() != 1 || node.children.front().type != Parser::NodeType::CallExpr)
            {
                error("State binding requires one function call.", node);
            }
            check_call_expr(table, node.children.front());
            const std::string stateName = std::string(std::get<std::string_view>(node.value));
            Symbol state;
            state.name = stateName;
            state.typeNode = Parser::ASTNode{Parser::NodeType::PrimitiveType,
                                             node.start,
                                             node.mod_path,
                                             node.source,
                                             node.type == Parser::NodeType::ThreadBindingDecl ? Lexer::Keyword::THREAD
                                                                                              : Lexer::Keyword::STATE,
                                             {}};
            if (!table.declare(stateName, std::move(state)))
            {
                error("Redeclaration of State binding.", node);
            }
            break;
        }

        case Parser::NodeType::StartStmt:
        {
            const std::string name = std::string(std::get<std::string_view>(node.value));
            Symbol *state = table.lookup(name);
            if (!state || (infer_type_from_node(state->typeNode).kind != TypeKind::State &&
                           infer_type_from_node(state->typeNode).kind != TypeKind::Thread))
            {
                error("Start requires a declared State binding.", node);
            }
            break;
        }

        case Parser::NodeType::AwaitExpr:
            if (!node.children.empty() && node.children.front().type == Parser::NodeType::Identifier)
            {
                const std::string name = std::string(std::get<std::string_view>(node.children.front().value));
                Symbol *state = table.lookup(name);
                if (!state || (infer_type_from_node(state->typeNode).kind != TypeKind::State &&
                               infer_type_from_node(state->typeNode).kind != TypeKind::Thread))
                {
                    error("Await requires a declared State binding or call expression.", node);
                }
            }
            else
            {
                for (auto &child : node.children)
                    check_node(table, child);
            }
            break;

        case Parser::NodeType::TunnelStmt:
        {
            const std::string slot = std::string(std::get<std::string_view>(node.value));
            const auto found = activeTunnelSlots.find(slot);
            if (found == activeTunnelSlots.end())
            {
                error("Target tunnel slot '" + slot + "' does not exist.", node);
            }
            if (!node.children.empty())
            {
                Type value = infer_expression_type(table, node.children.front());
                if (found->second.hasExpectedType && !is_assignable(found->second.expectedType, value) &&
                    !(is_numeric(found->second.expectedType.kind) && is_numeric(value.kind)))
                {
                    error("Tunnel value type mismatch for slot '" + slot + "'.", node);
                }
            }
            break;
        }

        case Parser::NodeType::CallExpr:
            check_call_expr(table, node);
            break;

        case Parser::NodeType::MemberAccessExpr:
        case Parser::NodeType::ScopeAccessExpr:
            check_member_access(table, node);
            break;

        case Parser::NodeType::ValidStmt:
        {
            const std::string name = std::string(std::get<std::string_view>(node.value));
            Symbol *symbol = table.lookup(name);
            if (!symbol || !infer_type_from_node(symbol->typeNode).isOptional || symbol->typeNode.children.empty())
            {
                error("Valid requires a declared optional value.", node);
            }
            const Parser::ASTNode optionalType = symbol->typeNode;
            symbol->typeNode = optionalType.children.front();
            symbol->typeNode.isMutable = optionalType.isMutable;
            check_node(table, node.children.front());
            symbol->typeNode = optionalType;
            if (node.children.size() > 1)
                check_node(table, node.children[1]);
            break;
        }

        case Parser::NodeType::StructDecl:
        {
            register_type_definition(node);

            std::string structName;
            if (!node.children.empty() && node.children.front().type == Parser::NodeType::Identifier)
                structName = std::string(std::get<std::string_view>(node.children.front().value));

            if (!activeNamespaces.empty())
            {
                std::string qualifiedName;
                for (const std::string &name : activeNamespaces)
                {
                    if (!qualifiedName.empty())
                        qualifiedName += "::";
                    qualifiedName += name;
                }
                qualifiedName += "::" + structName;
                structName = std::move(qualifiedName);
            }

            structNameStack.push_back(structName);
            size_t genericsPushed = push_generic_params(node.children);

            table.push_scope();
            for (auto &child : node.children)
            {
                if (child.type == Parser::NodeType::FunctionDef || child.type == Parser::NodeType::FunctionDecl ||
                    child.type == Parser::NodeType::CFunctionDef || child.type == Parser::NodeType::CFunctionDecl)
                    check_node(table, child);
            }
            table.pop_scope();

            pop_generic_params(genericsPushed);
            structNameStack.pop_back();
            break;
        }
        case Parser::NodeType::TraitDecl:
            break;
        case Parser::NodeType::ImplDecl:
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::CustomType)
                error("An impl must name its target type.", node);
            std::string targetName = custom_type_name(node.children.front());
            if (targetName.find("::") == std::string::npos && !current_namespace_name().empty())
                targetName = current_namespace_name() + "::" + targetName;
            if (registeredTypes.find(targetName) == registeredTypes.end())
                error("An impl target must be a declared struct, enum, or union.", node.children.front());
            structNameStack.push_back(targetName);
            const size_t genericsPushed = push_generic_params(node.children);
            table.push_scope();
            for (auto &child : node.children)
            {
                if (child.type == Parser::NodeType::FunctionDef || child.type == Parser::NodeType::FunctionDecl)
                    check_node(table, child);
            }
            table.pop_scope();
            pop_generic_params(genericsPushed);
            structNameStack.pop_back();
            break;
        }
        case Parser::NodeType::EnumDecl:
            register_type_definition(node);
            break;
        case Parser::NodeType::FunctionDecl:
            for (const auto &child : node.children)
                if (child.type == Parser::NodeType::GenericParam)
                    error("generic function declarations require a definition.", child);
            break;
        case Parser::NodeType::CFunctionDecl:
            if (unsafeDepth == 0 && !is_trusted_stdlib_implementation(node))
                error("C-ABI declarations require an enclosing @unsafe ... @end block.", node);
            for (const auto &child : node.children)
                if (child.type == Parser::NodeType::OptionalType || child.type == Parser::NodeType::ArrayType)
                {
                    error("C-ABI functions cannot return optional or array values.", node);
                }
            break;
        case Parser::NodeType::UsingMacroDecl:
            break;

        case Parser::NodeType::UsingNamespaceDecl:
            if (node.children.size() != 1)
                error("Using namespace declaration is missing its namespace.", node);
            table.import_namespace(scope_access_name(node.children.front()));
            break;

        case Parser::NodeType::InlineAsmStmt:
            if (!node.children.empty() && functionBodyDepth == 0)
                error("Inline assembly operands are only valid inside a function body.", node);
            for (const auto &operand : node.children)
            {
                const std::string name = std::string(std::get<std::string_view>(operand.value));
                Symbol *symbol = table.lookup(name);
                if (!symbol)
                    error("Unknown inline assembly operand '" + name + "'.", operand);
                const Type type = infer_type_from_node(symbol->typeNode);
                switch (type.kind)
                {
                case TypeKind::U8:
                case TypeKind::U16:
                case TypeKind::U32:
                case TypeKind::U64:
                case TypeKind::USIZE:
                case TypeKind::I8:
                case TypeKind::I16:
                case TypeKind::I32:
                case TypeKind::I64:
                case TypeKind::Bool:
                case TypeKind::Char:
                case TypeKind::Pointer:
                case TypeKind::Reference:
                case TypeKind::Enum:
                    break;
                default:
                    error("Inline assembly operands must be integer, pointer, reference, or enum values.", operand);
                }
                if (operand.isMutable && !type.isMutable)
                    error("A 'mut' inline assembly operand requires a mutable binding.", operand);
            }
            break;

        case Parser::NodeType::ExportDecl:
            for (auto &child : node.children)
                check_node(table, child);
            break;

        case Parser::NodeType::ReturnStmt:
        {
            if (!inCFunctionBody)
            {
                error("'return' is not allowed in a tunnel-based function (def/dec); use 'tunnel value -> Type slot;' "
                      "instead. 'return' is only valid in cdef/cdec (C-ABI) functions.",
                      node);
            }
            if (node.children.empty())
            {
                if (activeCFunctionReturnsValue)
                {
                    error("A value is required by this C function's return type.", node);
                }
                break;
            }
            if (!activeCFunctionReturnsValue)
            {
                error("A void C function cannot return a value.", node);
            }
            const Type returnedType = infer_expression_type(table, node.children.front());
            if (activeCFunctionReturnType.kind == TypeKind::Pointer &&
                node.children.front().type == Parser::NodeType::CallExpr && !node.children.front().children.empty() &&
                node.children.front().children.front().type == Parser::NodeType::Identifier)
            {
                const std::string_view callee =
                    std::get<std::string_view>(node.children.front().children.front().value);
            }
            if (!is_assignable(activeCFunctionReturnType, returnedType) &&
                !(is_numeric(activeCFunctionReturnType.kind) && is_numeric(returnedType.kind)))
            {
                error("Return type does not match function signature.", node);
            }
            check_node(table, node.children.front());
            break;
        }

        case Parser::NodeType::FunctionDef:
        {
            const bool isMain = !node.children.empty() && node.children.front().type == Parser::NodeType::Identifier &&
                                std::get<std::string_view>(node.children.front().value) == "main";
            if (isMain)
            {
                for (const auto &child : node.children)
                    if (child.type == Parser::NodeType::TunnelSlot)
                        error("main cannot declare tunnel outputs.", child);
            }
            size_t genericsPushed = push_generic_params(node.children);
            bool prevInC = inCFunctionBody;
            inCFunctionBody = false;

            check_function_tunnels(table, node);
            ++functionBodyDepth;
            table.push_scope();
            for (auto &child : node.children)
            {
                if (child.type == Parser::NodeType::Param)
                {
                    Symbol paramSym;
                    paramSym.name = std::string(std::get<std::string_view>(child.value));
                    paramSym.typeNode = child.children.empty() ? Parser::ASTNode{} : child.children.front();
                    table.declare(paramSym.name, paramSym);
                }
            }
            for (auto &child : node.children)
                check_node(table, child);
            table.pop_scope();
            --functionBodyDepth;

            inCFunctionBody = prevInC;
            pop_generic_params(genericsPushed);
            break;
        }

        case Parser::NodeType::CFunctionDef:
        {
            if (unsafeDepth == 0 && !is_trusted_stdlib_implementation(node))
                error("C-ABI definitions require an enclosing @unsafe ... @end block.", node);
            for (const auto &child : node.children)
                if (child.type == Parser::NodeType::OptionalType || child.type == Parser::NodeType::ArrayType)
                {
                    error("C-ABI functions cannot return optional or array values.", node);
                }
            bool prevInC = inCFunctionBody;
            Type previousReturnType = activeCFunctionReturnType;
            bool previousReturnsValue = activeCFunctionReturnsValue;
            inCFunctionBody = true;
            activeCFunctionReturnType = Type(TypeKind::Void);
            activeCFunctionReturnsValue = false;
            if (node.isNaked)
            {
                bool hasAssembly = false;
                for (const auto &child : node.children)
                    if (child.type == Parser::NodeType::BlockStmt)
                    {
                        for (const auto &statement : child.children)
                        {
                            if (statement.type != Parser::NodeType::InlineAsmStmt)
                                error("A naked function body may contain only @asm blocks.", statement);
                            hasAssembly = true;
                        }
                    }
                if (!hasAssembly)
                    error("A naked function body requires an @asm block.", node);
            }
            for (const auto &child : node.children)
            {
                if (child.type == Parser::NodeType::PrimitiveType || child.type == Parser::NodeType::PointerType ||
                    child.type == Parser::NodeType::ReferenceType || child.type == Parser::NodeType::CustomType)
                {
                    activeCFunctionReturnType = infer_type_from_node(child);
                    activeCFunctionReturnsValue = true;
                    break;
                }
            }

            table.push_scope();
            ++functionBodyDepth;
            for (auto &child : node.children)
            {
                if (child.type == Parser::NodeType::Param)
                {
                    Symbol paramSym;
                    paramSym.name = std::string(std::get<std::string_view>(child.value));
                    paramSym.typeNode = child.children.empty() ? Parser::ASTNode{} : child.children.front();
                    if (!table.declare(paramSym.name, paramSym))
                    {
                        error("Duplicate parameter '" + paramSym.name + "'.", node);
                    }
                }
            }
            for (auto &child : node.children)
                check_node(table, child);
            table.pop_scope();
            --functionBodyDepth;

            inCFunctionBody = prevInC;
            activeCFunctionReturnType = std::move(previousReturnType);
            activeCFunctionReturnsValue = previousReturnsValue;
            break;
        }

        case Parser::NodeType::WhileLoop:
            if (node.children.empty() || infer_expression_type(table, node.children.front()).kind != TypeKind::Bool)
            {
                error("Boolean condition required for control flow.", node);
            }
            loopDepth++;
            for (auto &child : node.children)
                check_node(table, child);
            loopDepth--;
            break;

        case Parser::NodeType::ForLoop:
            if (node.children.size() != 4)
            {
                error("Malformed for-loop.", node);
            }
            table.push_scope();
            check_node(table, node.children[0]);
            if (infer_expression_type(table, node.children[1]).kind != TypeKind::Bool)
            {
                error("Boolean condition required for control flow.", node);
            }
            loopDepth++;
            check_node(table, node.children[1]);
            check_node(table, node.children[2]);
            check_node(table, node.children[3]);
            loopDepth--;
            table.pop_scope();
            break;

        case Parser::NodeType::ForeachLoop:
        {
            if (node.children.size() != 3)
            {
                error("Malformed foreach-loop.", node);
            }
            Parser::ASTNode &iterable = node.children[1];
            const bool isRange = iterable.type == Parser::NodeType::BinaryExpr && iterable.children.size() == 2 &&
                                 iterable.value.index() == 5 &&
                                 std::get<Lexer::Operator>(iterable.value) == Lexer::Operator::TRIPPLE_DOT;
            const Type iterableType = isRange ? Type(TypeKind::Error) : infer_expression_type(table, iterable);
            const bool isVector = (iterableType.kind == TypeKind::Struct || iterableType.kind == TypeKind::Class) &&
                                  (iterableType.name == "Vector" ||
                                   (iterableType.name.size() > 8 &&
                                    iterableType.name.compare(iterableType.name.size() - 8, 8, "::Vector") == 0));
            if (!isRange && iterableType.kind != TypeKind::Array && !isVector)
            {
                error("foreach requires a fixed-size array, T[length] runtime array, or Vector<T>.", iterable);
            }
            table.push_scope();
            Symbol item;
            item.name = std::string(std::get<std::string_view>(node.value));
            item.typeNode = node.children[0];
            if (!table.declare(item.name, item))
            {
                error("Redeclaration of foreach-variable.", node);
            }
            if (isRange)
            {
                const Type startType = infer_expression_type(table, iterable.children[0]);
                const Type endType = infer_expression_type(table, iterable.children[1]);
                const Type itemType = infer_type_from_node(node.children[0]);
                if (!is_integer(startType.kind) || !is_integer(endType.kind) || !is_integer(itemType.kind))
                    error("foreach range endpoints and item type must be integral.", iterable);
                check_node(table, iterable.children[0]);
                check_node(table, iterable.children[1]);
            }
            else
            {
                check_node(table, iterable);
            }
            loopDepth++;
            check_node(table, node.children[2]);
            loopDepth--;
            table.pop_scope();
            break;
        }

        case Parser::NodeType::BreakStmt:
        case Parser::NodeType::ContinueStmt:
            if (loopDepth == 0)
            {
                error(std::string("'") + (node.type == Parser::NodeType::BreakStmt ? "break" : "continue") +
                          "' is only valid inside a loop.",
                      node);
            }
            break;

        case Parser::NodeType::BinaryExpr:
            check_binary_expr(table, node);
            for (auto &child : node.children)
                check_node(table, child);
            break;

        default:
            for (auto &child : node.children)
                check_node(table, child);
            break;
        }
    }

    void set_stdlib_enabled(bool enabled) { stdlibEnabled = enabled; }

    void run_checker()
    {
        SymbolTable table;
        table.push_scope();
        auto predeclare_types = [&](auto &self, Parser::ASTNode &node) -> void
        {
            if (node.type == Parser::NodeType::NamespaceDecl)
            {
                activeNamespaces.push_back(std::string(std::get<std::string_view>(node.value)));
                for (auto &child : node.children)
                    self(self, child);
                activeNamespaces.pop_back();
                return;
            }
            if (node.type == Parser::NodeType::StructDecl || node.type == Parser::NodeType::EnumDecl ||
                node.type == Parser::NodeType::UnionDecl)
                register_type_definition(node);
            if (node.type == Parser::NodeType::EnumDecl && !node.children.empty())
            {
                const auto &name_node = node.children.front();
                for (const auto &member : node.children)
                {
                    if (member.type != Parser::NodeType::EnumMember || member.children.empty())
                        continue;
                    Symbol symbol;
                    symbol.name = std::string(std::get<std::string_view>(member.children.front().value));
                    symbol.typeNode = Parser::ASTNode{Parser::NodeType::CustomType,
                                                      member.start,
                                                      member.mod_path,
                                                      member.source,
                                                      name_node.value,
                                                      {}};
                    table.declare(symbol.name, std::move(symbol));
                }
            }
            for (auto &child : node.children)
                self(self, child);
        };
        try
        {
            for (auto &root_node : Parser::ast)
            {
                predeclaringTypeShells = true;
                predeclare_types(predeclare_types, root_node);
            }
            predeclaringTypeShells = false;
            for (auto &root_node : Parser::ast)
                predeclare_types(predeclare_types, root_node);
        }
        catch (const CheckerRecovery &)
        {
            predeclaringTypeShells = false;
            flush_diagnostics();
            throw CompilationError("", true);
        }
        try
        {
            for (auto &root_node : Parser::ast)
                declare_function_symbols(table, root_node);
        }
        catch (const CheckerRecovery &)
        {
            flush_diagnostics();
            throw CompilationError("", true);
        }
        bool hadCheckerErrors = false;
        const auto check_top_level = [&](Parser::ASTNode &node)
        {
            SymbolTable tableSnapshot = table;
            auto registeredTypesSnapshot = registeredTypes;
            auto namespacesSnapshot = activeNamespaces;
            auto genericNamesSnapshot = activeGenericNames;
            auto structNamesSnapshot = structNameStack;
            const bool cFunctionSnapshot = inCFunctionBody;
            const size_t functionDepthSnapshot = functionBodyDepth;
            const Type cFunctionReturnSnapshot = activeCFunctionReturnType;
            const bool cFunctionReturnsValueSnapshot = activeCFunctionReturnsValue;
            const size_t loopDepthSnapshot = loopDepth;
            auto tunnelSlotsSnapshot = activeTunnelSlots;
            try
            {
                validate_unsafe_constructs(node);
                check_node(table, node);
            }
            catch (const CheckerRecovery &)
            {
                table = std::move(tableSnapshot);
                registeredTypes = std::move(registeredTypesSnapshot);
                activeNamespaces = std::move(namespacesSnapshot);
                activeGenericNames = std::move(genericNamesSnapshot);
                structNameStack = std::move(structNamesSnapshot);
                inCFunctionBody = cFunctionSnapshot;
                functionBodyDepth = functionDepthSnapshot;
                activeCFunctionReturnType = cFunctionReturnSnapshot;
                activeCFunctionReturnsValue = cFunctionReturnsValueSnapshot;
                loopDepth = loopDepthSnapshot;
                activeTunnelSlots = std::move(tunnelSlotsSnapshot);
                hadCheckerErrors = true;
            }
        };
        for (auto &root_node : Parser::ast)
        {
            if (root_node.type == Parser::NodeType::Module)
            {
                for (auto &child : root_node.children)
                    check_top_level(child);
            }
            else
                check_top_level(root_node);
        }
        table.pop_scope();
        if (hadCheckerErrors)
        {
            flush_diagnostics();
            throw CompilationError("", true);
        }
    }
} // namespace Checker