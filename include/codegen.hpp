#pragma once
#include "parser.hpp"
#include "error.hpp"
#include "thirdparty/shaft_llvm.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// note: main function implicitly gets a return 0

namespace Codegen
{
    struct CGType
    {
        LLVMTypeRef llvmType = nullptr;
        LLVMTypeRef pointeeType = nullptr;
        std::shared_ptr<CGType> innerType;
        bool isFloat = false;
        bool isSigned = true;
        bool isBool = false;
        bool isChar = false;
        bool isPointerLike = false; // pointer or reference
        bool isReference = false;   // borrowed reference; never releases allocation metadata
        bool isOptional = false;
        bool isArray = false;
        uint64_t arrayLength = 0; // nonzero only for source fixed-size arrays
        std::string runtimeArrayLengthName; // source length identifier for T[name]
        uint64_t requiredAlignment = 0; // nonzero for source `align N struct`
        std::string structName; // set when this type is a named struct
    };

    struct VarInfo
    {
        LLVMValueRef address; // allocated storage
        CGType type;
    };

    struct TunnelSlotInfo
    {
        unsigned argIndex;
        CGType type;
        bool isOptional;
        unsigned flagArgIndex = 0; // only meaningful when isOptional
        unsigned cleanupArgIndex = 0; // first ownership-token sidecar
        uint64_t cleanupLeafCount = 0;
    };

    struct FunctionSignature
    {
        std::string llvmName;
        bool isC = false;
        LLVMTypeRef llvmFunctionType = nullptr;
        LLVMValueRef llvmFunction = nullptr;
        std::vector<CGType> paramTypes;
        std::vector<std::string> paramNames;
        std::vector<Parser::ASTNode> tunnelSlotNodes; // in declaration order (non-C only)
        std::vector<CGType> tunnelSlotTypes;
        std::vector<bool> tunnelSlotOptional;          // presence sidecar follows optional output pointer
        std::vector<uint64_t> tunnelSlotCleanupLeafCounts; // ownership-token sidecars per slot
        CGType cReturnType;                           // only meaningful when isC
        bool cReturnsVoid = true;
    };

    struct StructInfo
    {
        LLVMTypeRef llvmType = nullptr;
        uint64_t requestedAlignment = 0;
        std::string baseClassName;
        std::vector<std::string> genericParameters;
        std::vector<Parser::ASTNode> fieldTypeNodes;
        std::vector<std::string> fieldNames;
        std::vector<CGType> fieldTypes;
        std::string indexedField;
        std::string initializedField;
    };

    struct EnumInfo
    {
        CGType backingType;
        std::unordered_map<std::string, int64_t> members;
    };

    struct DeferredState
    {
        const Parser::ASTNode *call = nullptr;
        bool started = false;
    };

    struct CleanupValue
    {
        LLVMValueRef address = nullptr;
        CGType type;
        LLVMValueRef activeFlag = nullptr;
        // Named runtime arrays own stack storage. Per-element cleanup ownership is
        // represented by this allocation-time count and bitmap, never by pointer shape.
        LLVMValueRef runtimeElementCount = nullptr;
        LLVMValueRef runtimeLeafActiveFlags = nullptr;
        uint64_t runtimeCleanupLeafCount = 0;
        std::vector<CleanupValue> children; // struct fields or fixed-array elements
    };

    struct Context
    {
        LLVMModuleRef module = nullptr;
        LLVMBuilderRef builder = nullptr;
        LLVMContextRef llvmCtx = nullptr;

        LLVMValueRef currentFunction = nullptr;
        std::string currentClassName;
        std::string currentNamespaceName;
        bool isCFunction = false;
        bool stdlibEnabled = false;
        unsigned targetPointerWidthBits = 64;
        CGType currentReturnType;

        std::vector<std::unordered_map<std::string, VarInfo>> scopes;
        std::vector<std::vector<CleanupValue>> cleanupScopes;
        std::unordered_map<std::string, TunnelSlotInfo> tunnelSlots;
        // Multi-result reserve supplies one destination per tunnel payload.
        std::vector<LLVMValueRef> pendingTunnelResultTargets;
        std::vector<LLVMValueRef> pendingTunnelPresenceTargets;
        std::vector<std::vector<LLVMValueRef>> pendingTunnelCleanupTargets;
        // Reservations follow lexical scopes; lookup walks outward, and scope pop expires them.
        std::vector<std::unordered_map<std::string, LLVMValueRef>> reservedTunnelTargetScopes;
        std::vector<std::unordered_map<std::string, LLVMValueRef>> reservedTunnelPresenceTargetScopes;
        std::vector<std::unordered_map<std::string, std::vector<LLVMValueRef>>> reservedTunnelCleanupTargetScopes;
        std::unordered_map<std::string, CGType> genericBindings;
        std::string functionSpecializationName;
        std::unordered_map<std::string, DeferredState> states;
        std::vector<std::vector<std::string>> scopedStateNames;
        std::vector<std::pair<LLVMBasicBlockRef, LLVMBasicBlockRef>> loopStack; // {continue, break}
        std::vector<size_t> loopCleanupDepths;
        // Storage whose optional wrapper is proven present in the active `valid` branch.
        std::vector<LLVMValueRef> validPayloadAddresses;
        const Parser::ASTNode *deferredEntryDefinition = nullptr;
        bool generatingDeferredEntry = false;

        void push_scope()
        {
            scopes.emplace_back();
            cleanupScopes.emplace_back();
            reservedTunnelTargetScopes.emplace_back();
            reservedTunnelPresenceTargetScopes.emplace_back();
            reservedTunnelCleanupTargetScopes.emplace_back();
            scopedStateNames.emplace_back();
        }
        void pop_scope()
        {
            for (const std::string &name : scopedStateNames.back())
                states.erase(name);
            scopedStateNames.pop_back();
            reservedTunnelTargetScopes.pop_back();
            reservedTunnelPresenceTargetScopes.pop_back();
            reservedTunnelCleanupTargetScopes.pop_back();
            cleanupScopes.pop_back();
            scopes.pop_back();
        }

        void declare_owned_var(const std::string &name, LLVMValueRef addr, const CGType &type,
                               CleanupValue cleanup)
        {
            declare_var(name, addr, type);
            cleanupScopes.back().push_back(std::move(cleanup));
        }

        void disarm_owned_var(LLVMValueRef addr)
        {
            for (auto scope = cleanupScopes.rbegin(); scope != cleanupScopes.rend(); ++scope)
            {
                for (CleanupValue &value : *scope)
                {
                    if (value.address == addr)
                    {
                        LLVMBuildStore(builder, LLVMConstInt(LLVMInt1TypeInContext(llvmCtx), 0, 0),
                                       value.activeFlag);
                        return;
                    }
                }
            }
        }

        void declare_var(const std::string &name, LLVMValueRef addr, const CGType &type)
        {
            scopes.back()[name] = VarInfo{addr, type};
        }

        VarInfo *find_var(const std::string &name)
        {
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            {
                auto found = it->find(name);
                if (found != it->end())
                    return &found->second;
            }
            return nullptr;
        }
    };

    extern std::unordered_map<std::string, StructInfo> structTypes;
    extern std::unordered_map<std::string, EnumInfo> enumTypes;
    extern std::unordered_map<std::string, FunctionSignature> functions;

    // main -> __main
    std::string mangle_function_name(const std::string &name);

    Context create_context(const char *module_name);
    LLVMModuleRef generate_module(Context &ctx, const std::vector<Parser::ASTNode> &roots);
    LLVMValueRef generate_node(Context &ctx, const Parser::ASTNode &node);
} // namespace Codegen