#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "lexer.hpp"
#include "error.hpp"

namespace Parser
{
    enum class NodeType
    {
        // root and scopes
        Module,
        NamespaceDecl,
        BlockStmt,

        // statements and declarations
        VariableDecl,
        StructDecl,
        IndexDecl, // `index field;` selects a class's direct [] backing field
        InitDecl,  // `init field;` routes class brace initialization to that field
        EnumDecl,
        EnumMember,
        ImportStmt,
        ExportDecl,
        UsingMacroDecl,

        // functions and tunnels
        FunctionDecl,
        FunctionDef,
        CFunctionDecl,
        CFunctionDef,
        ParamList,
        Param,
        TunnelSlotList,
        TunnelSlot,
        TunnelStmt,
        ReserveStmt,
        MultiReserveStmt, // `reserve T first, U second = call();`
        GenericParamList,
        GenericParam,
        GenericArg, // explicit call-site generic instantiation: foo::<i32>(x)

        // control flow
        IfStmt,
        ValidStmt,
        MatchStmt,
        MatchCase,
        MatchDefault,
        WhileLoop,
        ForLoop,
        ForeachLoop,
        BreakStmt,
        ContinueStmt,
        ExprStmt,
        ReturnStmt, // used by cdef/cdec (C-ABI functions); `def`/`dec` use tunnel/TunnelStmt

        // types and memory annotations
        PrimitiveType,
        PointerType,   // unsafe C-Pointer
        ReferenceType, // for writer and reader (&mut and &)
        OptionalType,
        ArrayType,
        CustomType, // enums, structs, and user-defined types

        // expressions
        BinaryExpr,
        UnaryExpr,
        AssignmentExpr,
        CallExpr,
        MemberAccessExpr,  // object.member
        ScopeAccessExpr,   // Namespace::member or Enum::member
        IndexExpr,         // array[index]
        StructInitExpr,    // e. g. Vector2 pos = {1.0, 2.0};
        TunnelBindingExpr, // for inline reserve
        MoveExpr,
        RefExpr,
        SizeofExpr,

        // async / concurrency
        StateBindingDecl, // State foo() s;
        ThreadBindingDecl, // Thread foo() worker; cooperative task handle
        StartStmt,        // start s;
        AwaitExpr,        // await s; / await foo();
        ThreadTaskStmt,   // t { ... }

        // literals and identifiers
        Identifier,
        IntegerLiteral, // signed and unsigned integers
        FloatLiteral,
        StringLiteral,
        BoolLiteral,
        CharLiteral,
    };

    struct ASTNode
    {
        NodeType type;

        // relevant for errors
        uint64_t start;
        std::string* mod_path;
        std::string* source;

        std::variant<std::monostate, std::string_view, uint64_t, double, bool, Lexer::Operator,
                     Lexer::Keyword>
            value;
        std::vector<ASTNode> children;
        std::string inferredTypeName;
        bool hasInferredType = false;

        // extra flags
        bool isOptional = false;
        bool isAsync = false;
        bool isExported = false;
        bool isGlobal = false;
        bool isMutable = false;
        bool isClass = false;
        std::string baseClassName;
        uint64_t requestedAlignment = 0;
    };

    extern std::vector<Lexer::Token> tokens;
    extern uint64_t pos;
    extern std::vector<ASTNode> ast;

    struct ParsedModule
    {
        std::vector<ASTNode> ast;
    };

    void parse(std::vector<Lexer::LexedModule> input_tokens);
    void dump_ast(const std::vector<ASTNode>& ast);
} // namespace Parser