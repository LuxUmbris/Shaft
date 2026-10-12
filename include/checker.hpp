#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "error.hpp"
#include "parser.hpp"

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

        State, // Async single-thread state
        Thread,

        // User defined
        Struct,
        Class,
        Enum,
        Union,

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
        std::unordered_map<std::string, VarOwnershipState> subobjectStates;
        size_t borrowCount = 0;
        bool has_writer = false;
        bool isOptional = false; // Is it ?T
        bool isFunction = false;
        bool isCFunction = false;
        std::vector<std::string> genericParameters;
        std::vector<Type> paramTypes;
        Type resultType{TypeKind::Error};
        bool hasResultType = false;
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
        bool isUnion = false;
        std::string baseClassName;
        std::vector<std::string> genericParameters;
        std::string indexedField;
        std::string initializedField;
        std::unordered_map<std::string, Symbol> members;
        std::unordered_map<std::string, Symbol> methods;
        struct UnionVariant
        {
            size_t tag = 0;
            std::vector<Parser::ASTNode> payloadTypes;
        };
        std::unordered_map<std::string, UnionVariant> unionVariants;
    };

    // type/borrow-checks the full parsed program (Parser::ast)
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
        TunnelSlotState(std::string n, bool opt, bool fill, Type expected = {}, bool hasExpected = false)
            : name(std::move(n)), isOptional(opt), filled(fill), expectedType(std::move(expected)),
              hasExpectedType(hasExpected)
        {
        }
    };

} // namespace Checker