#pragma once
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "error.hpp"
#include "lexer.hpp"

namespace Parser
{
    enum class NodeType
    {
        // root and scopes
        Module,
        NamespaceDecl,
        BlockStmt,
        UnsafeBlock,
        NoCheckBlock,
        NoCleanupBlock,

        // statements and declarations
        VariableDecl,
        StructDecl,
        TraitDecl,
        ImplDecl,
        IndexDecl,
        InitDecl,
        EnumDecl,
        EnumMember,
        UnionDecl,
        UnionVariant,
        ImportStmt,
        ExportDecl,
        UsingNamespaceDecl,
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
        MultiReserveStmt,
        GenericParamList,
        GenericParam,
        GenericArg,

        // control flow
        IfStmt,
        ValidStmt,
        MatchStmt,
        MatchCase,
        MatchDefault,
        UnionMatchStmt,
        UnionPattern,
        WhileLoop,
        ForLoop,
        ForeachLoop,
        BreakStmt,
        ContinueStmt,
        ExprStmt,
        InlineAsmStmt,
        ReturnStmt,
        TerminateStmt,

        // types and memory annotations
        PrimitiveType,
        PointerType,
        ReferenceType,
        OptionalType,
        ArrayType,
        CustomType,
        TypeofType,

        // expressions
        BinaryExpr,
        UnaryExpr,
        AssignmentExpr,
        CallExpr,
        MemberAccessExpr,
        ScopeAccessExpr,
        IndexExpr,
        StructInitExpr,
        TunnelBindingExpr,
        MoveExpr,
        RefExpr,
        SizeofExpr,
        CastExpr,
        CompileExpr,
        TypeIfExpr,
        DoExpr,

        // async / concurrency
        StateBindingDecl,
        ThreadBindingDecl,
        StartStmt,
        AwaitExpr,
        ThreadTaskStmt,

        // literals and identifiers
        Identifier,
        IntegerLiteral,
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
        std::string *mod_path;
        std::string *source;

        std::variant<std::monostate, std::string_view, uint64_t, double, bool, Lexer::Operator, Lexer::Keyword> value;
        std::vector<ASTNode> children;
        std::string inferredTypeName;
        bool hasInferredType = false;

        // extra flags
        bool isOptional = false;
        bool hasElse = false;
        bool isAsync = false;
        bool isComptime = false;
        bool isNaked = false;
        bool isExported = false;
        bool isGlobal = false;
        bool isMutable = false;
        uint64_t requestedAlignment = 0;
    };

    struct AttributeUse
    {
        const Lexer::ProceduralAttribute *attribute;
        Lexer::Token token;
        std::vector<ASTNode> args;
    };

    using ItemEnv = std::unordered_map<std::string, std::string>;
    struct TemplateEnv
    {
        ItemEnv values;
        std::unordered_map<std::string, std::vector<ItemEnv>> lists;
    };

    extern std::vector<Lexer::Token> tokens;
    extern uint64_t pos;
    extern std::vector<ASTNode> ast;
    extern std::vector<std::string> using_namespace_declarations;

    struct ParsedModule
    {
        std::vector<ASTNode> ast;
    };

    void parse(std::vector<Lexer::LexedModule> input_tokens);
    void dump_ast(const std::vector<ASTNode> &ast);
} // namespace Parser