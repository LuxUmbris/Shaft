#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "parser.hpp"
#include "error.hpp"

namespace Checker
{
    enum class TypeKind
    {
        // Primitives
        U8,
        U16,
        U32,
        U64,
        USIZE,
        I8,
        I16,
        I32,
        I64,
        F32,
        F64,
        Bool,
        Char,
        String,

        // Special Shaft Types
        State, // Async state
        Thread,

        // User defined
        Struct,
        Class,
        Enum,

        // Unresolved generic type parameter (e.g. `T` inside a generic
        // function/struct body) — treated as compatible with anything, since
        // this bootstrap compiler does not do real constraint solving.
        Generic,

        Pointer,
        Reference,
        Optional,
        Array,

        Void,
        Error
    };

    struct Type
    {
        TypeKind kind = TypeKind::Error;
        std::string name;
        bool isOptional = false;
        bool isMutable = false;
        std::shared_ptr<Type> innerType;
        std::vector<Type> genericArgs;

        Type() = default;
        Type(TypeKind k, std::string n = {}) : kind(k), name(std::move(n)) {}
    };

    enum class VarOwnershipState
    {
        Valid,
        Moved,
        BorrowedRead,
        BorrowedMut
    };

    struct Symbol
    {
        std::string name;
        Parser::ASTNode typeNode;
        VarOwnershipState state = VarOwnershipState::Valid;
        // Paths are relative to this local (for example `items[0].name`).
        // They track independently moved aggregate subobjects without invalidating siblings.
        std::unordered_map<std::string, VarOwnershipState> subobjectStates;
        size_t borrowCount = 0;
        bool has_writer = false;
        bool isOptional = false; // Is it ?T
        bool isFunction = false;
        std::vector<Type> paramTypes;
        Type resultType;
        bool hasResultType = false;
        // def/dec tunnel ABI slots in source declaration order.
        std::vector<Type> tunnelSlotTypes;
        std::vector<bool> tunnelSlotOptional;
        std::vector<std::string> tunnelSlotNames;
        bool isTunnelReservation = false;
        bool tunnelReservationConsumed = false;
    };

    struct TypeDefinition
    {
        std::string name;
        bool isClass = false;
        bool isEnum = false;
        std::string baseClassName;
        std::vector<std::string> genericParameters;
        std::string indexedField;
        std::string initializedField;
        std::unordered_map<std::string, Symbol> members;
        std::unordered_map<std::string, Symbol> methods;
    };

    // type/borrow-checks the full parsed program (Parser::ast)
    // Prints an error and calls exit(1) on the first violation it finds
    void set_stdlib_enabled(bool enabled);
    void run_checker();

    void register_type_definition(const Parser::ASTNode &node);

    struct TunnelSlotState
    {
        std::string name;
        bool isOptional;
        bool filled = false;
        Type expectedType;
        bool hasExpectedType = false;

        TunnelSlotState() = default;
        TunnelSlotState(std::string n, bool opt, bool fill, Type expected = {},
                        bool hasExpected = false)
            : name(std::move(n)), isOptional(opt), filled(fill), expectedType(std::move(expected)),
              hasExpectedType(hasExpected)
        {
        }
    };

} // namespace Checker