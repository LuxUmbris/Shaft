#include "codegen.hpp"
#include "error.hpp"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace Codegen
{
    std::unordered_map<std::string, StructInfo> structTypes;
    std::unordered_map<std::string, EnumInfo> enumTypes;
    std::unordered_map<std::string, UnionInfo> unionTypes;
    std::unordered_map<std::string, FunctionSignature> functions;
    static std::unordered_map<std::string, const Parser::ASTNode *> genericFunctionTemplates;
    static std::unordered_map<std::string, std::string> genericFunctionTemplateClasses;

    inline std::string mangle_function_name(const std::string &name)
    {
        if (name == "main")
            return "__main";
        return name;
    }

    Context create_context(const char *module_name)
    {
        Context ctx;
        ctx.llvmCtx = LLVMContextCreate();
        ctx.module = LLVMModuleCreateWithNameInContext(module_name, ctx.llvmCtx);
        ctx.builder = LLVMCreateBuilderInContext(ctx.llvmCtx);
        ctx.currentFunction = nullptr;
        ctx.isCFunction = false;
        ctx.push_scope();
        return ctx;
    }

    // forward declarations
    LLVMValueRef generate_node(Context &ctx, const Parser::ASTNode &node);

    [[noreturn]] static void codegen_error(const Parser::ASTNode &node, const std::string &message)
    {
        if (node.mod_path && node.source)
            panic_at_source(message, *node.mod_path, node.start, node.source);
        Error error;
        error.message = message;
        panic(std::move(error));
    }

    [[noreturn]] static void codegen_error(const std::string &message)
    {
        Error error;
        error.message = message;
        panic(std::move(error));
    }

    // @comptime is lowered to Bytecode and executed
    enum class ComptimeOp : uint8_t
    {
        Push,
        Load,
        Store,
        Add,
        Sub,
        Mul,
        Div,
        Mod,
        Eq,
        Ne,
        Lt,
        Le,
        Gt,
        Ge,
        JumpIfZero,
        Jump,
        Tunnel
    };
    struct ComptimeInstruction
    {
        ComptimeOp op;
        uint64_t operand = 0;
    };
    struct ComptimeProgram
    {
        std::vector<ComptimeInstruction> code;
        size_t localCount = 0;
    };

    static void compile_comptime_expression(const Parser::ASTNode &node, ComptimeProgram &program,
                                            const std::unordered_map<std::string, size_t> &locals)
    {
        if (node.type == Parser::NodeType::IntegerLiteral || node.type == Parser::NodeType::BoolLiteral)
        {
            uint64_t value = node.type == Parser::NodeType::BoolLiteral ? std::get<bool>(node.value)
                                                                        : std::get<uint64_t>(node.value);
            program.code.push_back({ComptimeOp::Push, value});
            return;
        }
        if (node.type == Parser::NodeType::Identifier)
        {
            const auto found = locals.find(std::string(std::get<std::string_view>(node.value)));
            if (found == locals.end())
                codegen_error(node, "@comptime bytecode cannot read an undeclared local.");
            program.code.push_back({ComptimeOp::Load, found->second});
            return;
        }
        if (node.type == Parser::NodeType::UnaryExpr && node.children.size() == 1 &&
            std::holds_alternative<Lexer::Operator>(node.value) &&
            std::get<Lexer::Operator>(node.value) == Lexer::Operator::MINUS)
        {
            program.code.push_back({ComptimeOp::Push, 0});
            compile_comptime_expression(node.children.front(), program, locals);
            program.code.push_back({ComptimeOp::Sub});
            return;
        }
        if (node.type != Parser::NodeType::BinaryExpr || node.children.size() != 2 ||
            !std::holds_alternative<Lexer::Operator>(node.value))
            codegen_error(node, "unsupported expression in @comptime bytecode.");
        compile_comptime_expression(node.children[0], program, locals);
        compile_comptime_expression(node.children[1], program, locals);
        switch (std::get<Lexer::Operator>(node.value))
        {
        case Lexer::Operator::PLUS:
            program.code.push_back({ComptimeOp::Add});
            break;
        case Lexer::Operator::MINUS:
            program.code.push_back({ComptimeOp::Sub});
            break;
        case Lexer::Operator::MULTIPLY:
            program.code.push_back({ComptimeOp::Mul});
            break;
        case Lexer::Operator::DIVIDE:
            program.code.push_back({ComptimeOp::Div});
            break;
        case Lexer::Operator::MODULO:
            program.code.push_back({ComptimeOp::Mod});
            break;
        case Lexer::Operator::EQUAL:
            program.code.push_back({ComptimeOp::Eq});
            break;
        case Lexer::Operator::NOT_EQUAL:
            program.code.push_back({ComptimeOp::Ne});
            break;
        case Lexer::Operator::LESS_THAN:
            program.code.push_back({ComptimeOp::Lt});
            break;
        case Lexer::Operator::LESS_EQUAL:
            program.code.push_back({ComptimeOp::Le});
            break;
        case Lexer::Operator::GREATER_THAN:
            program.code.push_back({ComptimeOp::Gt});
            break;
        case Lexer::Operator::GREATER_EQUAL:
            program.code.push_back({ComptimeOp::Ge});
            break;
        default:
            codegen_error(node, "unsupported binary operator in @comptime bytecode.");
        }
    }

    static void compile_comptime_statement(const Parser::ASTNode &node, ComptimeProgram &program,
                                           std::unordered_map<std::string, size_t> &locals)
    {
        if (node.type == Parser::NodeType::BlockStmt)
        {
            for (const auto &child : node.children)
                compile_comptime_statement(child, program, locals);
            return;
        }
        if (node.type == Parser::NodeType::VariableDecl)
        {
            if (node.children.size() < 2)
                codegen_error(node, "@comptime locals require an initializer.");
            const size_t local = program.localCount++;
            locals.emplace(std::string(std::get<std::string_view>(node.value)), local);
            compile_comptime_expression(node.children.back(), program, locals);
            program.code.push_back({ComptimeOp::Store, local});
            return;
        }
        if (node.type == Parser::NodeType::ExprStmt && !node.children.empty())
        {
            const Parser::ASTNode &expression = node.children.front();
            if (expression.type != Parser::NodeType::AssignmentExpr || expression.children.size() != 2 ||
                expression.children[0].type != Parser::NodeType::Identifier ||
                !std::holds_alternative<Lexer::Operator>(expression.value))
                codegen_error(node, "@comptime bytecode only permits assignment expression statements.");
            const auto local = locals.find(std::string(std::get<std::string_view>(expression.children[0].value)));
            if (local == locals.end())
                codegen_error(expression, "@comptime assignment targets an undeclared local.");
            const Lexer::Operator operation = std::get<Lexer::Operator>(expression.value);
            if (operation != Lexer::Operator::ASSIGN)
            {
                program.code.push_back({ComptimeOp::Load, local->second});
                compile_comptime_expression(expression.children[1], program, locals);
                switch (operation)
                {
                case Lexer::Operator::ADD_ASSIGN:
                    program.code.push_back({ComptimeOp::Add});
                    break;
                case Lexer::Operator::SUBTRACT_ASSIGN:
                    program.code.push_back({ComptimeOp::Sub});
                    break;
                case Lexer::Operator::MULTIPLY_ASSIGN:
                    program.code.push_back({ComptimeOp::Mul});
                    break;
                case Lexer::Operator::DIVIDE_ASSIGN:
                    program.code.push_back({ComptimeOp::Div});
                    break;
                case Lexer::Operator::MODULO_ASSIGN:
                    program.code.push_back({ComptimeOp::Mod});
                    break;
                default:
                    codegen_error(expression, "unsupported compound assignment in @comptime bytecode.");
                }
            }
            else
                compile_comptime_expression(expression.children[1], program, locals);
            program.code.push_back({ComptimeOp::Store, local->second});
            return;
        }
        if (node.type == Parser::NodeType::TunnelStmt && !node.children.empty())
        {
            compile_comptime_expression(node.children.front(), program, locals);
            program.code.push_back({ComptimeOp::Tunnel});
            return;
        }
        if (node.type == Parser::NodeType::IfStmt && node.children.size() >= 2)
        {
            compile_comptime_expression(node.children[0], program, locals);
            const size_t falseJump = program.code.size();
            program.code.push_back({ComptimeOp::JumpIfZero});
            compile_comptime_statement(node.children[1], program, locals);
            const size_t endJump = program.code.size();
            program.code.push_back({ComptimeOp::Jump});
            program.code[falseJump].operand = program.code.size();
            if (node.children.size() == 3)
                compile_comptime_statement(node.children[2], program, locals);
            program.code[endJump].operand = program.code.size();
            return;
        }
        codegen_error(node, "unsupported statement in @comptime bytecode.");
    }

    static uint64_t execute_comptime_program(const Parser::ASTNode &definition, const ComptimeProgram &program,
                                             const std::vector<uint64_t> &arguments)
    {
        std::vector<uint64_t> locals(program.localCount, 0), stack;
        std::copy(arguments.begin(), arguments.end(), locals.begin());
        for (size_t pc = 0, steps = 0; pc < program.code.size(); ++pc)
        {
            if (++steps > 1000000)
                codegen_error(definition, "@comptime bytecode exceeded its instruction limit.");
            const ComptimeInstruction &instruction = program.code[pc];
            auto pop = [&]()
            {
                if (stack.empty())
                    codegen_error(definition, "malformed @comptime bytecode stack.");
                uint64_t value = stack.back();
                stack.pop_back();
                return value;
            };
            switch (instruction.op)
            {
            case ComptimeOp::Push:
                stack.push_back(instruction.operand);
                break;
            case ComptimeOp::Load:
                stack.push_back(locals.at(instruction.operand));
                break;
            case ComptimeOp::Store:
                locals.at(instruction.operand) = pop();
                break;
            case ComptimeOp::JumpIfZero:
                if (pop() == 0)
                    pc = instruction.operand - 1;
                break;
            case ComptimeOp::Jump:
                pc = instruction.operand - 1;
                break;
            case ComptimeOp::Tunnel:
                return pop();
            default:
            {
                uint64_t right = pop(), left = pop();
                switch (instruction.op)
                {
                case ComptimeOp::Add:
                    stack.push_back(left + right);
                    break;
                case ComptimeOp::Sub:
                    stack.push_back(left - right);
                    break;
                case ComptimeOp::Mul:
                    stack.push_back(left * right);
                    break;
                case ComptimeOp::Div:
                    if (!right)
                        codegen_error(definition, "@comptime integer division by zero");
                    stack.push_back(left / right);
                    break;
                case ComptimeOp::Mod:
                    if (!right)
                        codegen_error(definition, "@comptime integer modulo by zero");
                    stack.push_back(left % right);
                    break;
                case ComptimeOp::Eq:
                    stack.push_back(left == right);
                    break;
                case ComptimeOp::Ne:
                    stack.push_back(left != right);
                    break;
                case ComptimeOp::Lt:
                    stack.push_back(left < right);
                    break;
                case ComptimeOp::Le:
                    stack.push_back(left <= right);
                    break;
                case ComptimeOp::Gt:
                    stack.push_back(left > right);
                    break;
                case ComptimeOp::Ge:
                    stack.push_back(left >= right);
                    break;
                default:
                    codegen_error(definition, "invalid @comptime bytecode instruction.");
                }
            }
            }
        }
        codegen_error(definition, "@comptime bytecode completed without tunneling a result.");
    }

    static uint64_t evaluate_comptime_integer_function(const FunctionSignature &signature,
                                                       const std::vector<uint64_t> &arguments)
    {
        const Parser::ASTNode &definition = *signature.comptimeDefinition;
        if (signature.tunnelSlotTypes.size() != 1 || signature.tunnelSlotOptional.front() ||
            LLVMGetTypeKind(signature.tunnelSlotTypes.front().llvmType) != LLVMIntegerTypeKind)
            codegen_error(definition, "@comptime requires one non-optional integer tunnel slot.");
        if (arguments.size() != signature.paramNames.size())
            codegen_error(definition, "@comptime argument count mismatch.");
        const Parser::ASTNode *body = nullptr;
        for (const Parser::ASTNode &child : definition.children)
            if (child.type == Parser::NodeType::BlockStmt)
                body = &child;
        if (!body)
            codegen_error(definition, "@comptime requires a function body.");
        ComptimeProgram program;
        program.localCount = signature.paramNames.size();
        std::unordered_map<std::string, size_t> locals;
        for (size_t index = 0; index < signature.paramNames.size(); ++index)
            locals.emplace(signature.paramNames[index], index);
        compile_comptime_statement(*body, program, locals);
        return execute_comptime_program(definition, program, arguments);
    }

    static bool evaluate_comptime_integer_expression(const Parser::ASTNode &node, uint64_t &value)
    {
        if (node.type == Parser::NodeType::IntegerLiteral)
        {
            value = std::get<uint64_t>(node.value);
            return true;
        }
        if (node.type != Parser::NodeType::BinaryExpr || node.children.size() != 2 ||
            !std::holds_alternative<Lexer::Operator>(node.value))
            return false;
        uint64_t left = 0, right = 0;
        if (!evaluate_comptime_integer_expression(node.children[0], left) ||
            !evaluate_comptime_integer_expression(node.children[1], right))
            return false;
        switch (std::get<Lexer::Operator>(node.value))
        {
        case Lexer::Operator::PLUS:
            value = left + right;
            return true;
        case Lexer::Operator::MINUS:
            value = left - right;
            return true;
        case Lexer::Operator::MULTIPLY:
            value = left * right;
            return true;
        case Lexer::Operator::DIVIDE:
            if (!right)
                codegen_error(node, "@comptime integer division by zero");
            value = left / right;
            return true;
        case Lexer::Operator::MODULO:
            if (!right)
                codegen_error(node, "@comptime integer modulo by zero");
            value = left % right;
            return true;
        case Lexer::Operator::EQUAL:
            value = left == right;
            return true;
        case Lexer::Operator::NOT_EQUAL:
            value = left != right;
            return true;
        case Lexer::Operator::LESS_THAN:
            value = left < right;
            return true;
        case Lexer::Operator::LESS_EQUAL:
            value = left <= right;
            return true;
        case Lexer::Operator::GREATER_THAN:
            value = left > right;
            return true;
        case Lexer::Operator::GREATER_EQUAL:
            value = left >= right;
            return true;
        default:
            return false;
        }
    }

    struct RuntimeArrayIndexCapture
    {
        const Parser::ASTNode *node = nullptr;
        LLVMValueRef value = nullptr;
    };
    LLVMValueRef get_lvalue(Context &ctx, const Parser::ASTNode &node,
                            RuntimeArrayIndexCapture *runtimeIndex = nullptr);
    static std::optional<CGType> try_lvalue_type(Context &ctx, const Parser::ASTNode &node);
    static std::optional<CGType> try_pointer_expression_type(Context &ctx, const Parser::ASTNode &node);
    static CGType lvalue_type(Context &ctx, const Parser::ASTNode &node);

    static LLVMValueRef cast_value(LLVMBuilderRef builder, LLVMValueRef value, LLVMTypeRef target, bool isSigned,
                                   const char *name)
    {
        if (LLVMTypeOf(value) == target)
            return value;

        const LLVMTypeKind sourceKind = LLVMGetTypeKind(LLVMTypeOf(value));
        const LLVMTypeKind targetKind = LLVMGetTypeKind(target);
        const bool sourceIsFloat = sourceKind == LLVMFloatTypeKind || sourceKind == LLVMDoubleTypeKind;
        const bool targetIsFloat = targetKind == LLVMFloatTypeKind || targetKind == LLVMDoubleTypeKind;

        if (sourceKind == LLVMIntegerTypeKind && targetKind == LLVMIntegerTypeKind)
            return LLVMBuildIntCast2(builder, value, target, isSigned ? 1 : 0, name);
        if (sourceKind == LLVMPointerTypeKind && targetKind == LLVMIntegerTypeKind)
            return LLVMBuildPtrToInt(builder, value, target, name);
        if (sourceKind == LLVMIntegerTypeKind && targetKind == LLVMPointerTypeKind)
            return LLVMBuildIntToPtr(builder, value, target, name);
        if (sourceKind == LLVMPointerTypeKind && targetKind == LLVMPointerTypeKind)
            return LLVMBuildPointerCast(builder, value, target, name);
        if (sourceIsFloat && targetIsFloat)
            return LLVMBuildFPCast(builder, value, target, name);
        if (sourceKind == LLVMIntegerTypeKind && targetIsFloat)
            return LLVMBuildSIToFP(builder, value, target, name);
        if (sourceIsFloat && targetKind == LLVMIntegerTypeKind)
            return LLVMBuildFPToSI(builder, value, target, name);

        char *sourceText = LLVMPrintTypeToString(LLVMTypeOf(value));
        char *targetText = LLVMPrintTypeToString(target);
        const std::string message = "cannot convert values between these LLVM types: " + std::string(sourceText) +
                                    " -> " + std::string(targetText);
        LLVMDisposeMessage(sourceText);
        LLVMDisposeMessage(targetText);
        compiler_error(message);
    }

    static LLVMValueRef cast_as_value(Context &ctx, LLVMValueRef value, const CGType &target, bool sourceIsSigned)
    {
        LLVMTypeRef sourceType = LLVMTypeOf(value);
        LLVMTypeRef targetType = target.llvmType;
        const LLVMTypeKind sourceKind = LLVMGetTypeKind(sourceType);
        const LLVMTypeKind targetKind = LLVMGetTypeKind(targetType);
        if (sourceType == targetType && sourceKind != LLVMIntegerTypeKind)
            return value;
        const bool sourceIsFloat = sourceKind == LLVMFloatTypeKind || sourceKind == LLVMDoubleTypeKind;
        const bool targetIsFloat = targetKind == LLVMFloatTypeKind || targetKind == LLVMDoubleTypeKind;
        const bool sourceIsPointer = sourceKind == LLVMPointerTypeKind;
        const bool targetIsPointer = targetKind == LLVMPointerTypeKind;

        if (target.isBool)
        {
            if (sourceIsFloat)
                return LLVMBuildFCmp(ctx.builder, LLVMRealUNE, value, LLVMConstReal(sourceType, 0.0), "asbool");
            return LLVMBuildICmp(ctx.builder, LLVMIntNE, value, LLVMConstNull(sourceType), "asbool");
        }
        if (sourceKind == LLVMIntegerTypeKind && targetKind == LLVMIntegerTypeKind)
        {
            const unsigned sourceBits = LLVMGetIntTypeWidth(sourceType);
            const unsigned targetBits = LLVMGetIntTypeWidth(targetType);
            const bool sourceSignedToUnsigned = sourceIsSigned && !target.isSigned;
            const bool unsignedToSigned = !sourceIsSigned && target.isSigned && sourceBits >= targetBits;
            if (sourceBits > targetBits || sourceSignedToUnsigned || unsignedToSigned)
            {
                if (sourceIsSigned && target.isSigned)
                {
                    if (sourceBits > targetBits)
                    {
                        const int64_t targetMinimum = -(INT64_C(1) << (targetBits - 1));
                        const uint64_t targetMaximum = (UINT64_C(1) << (targetBits - 1)) - 1;
                        LLVMValueRef minimum = LLVMConstInt(sourceType, static_cast<uint64_t>(targetMinimum), 1);
                        LLVMValueRef maximum = LLVMConstInt(sourceType, targetMaximum, 0);
                        LLVMValueRef below = LLVMBuildICmp(ctx.builder, LLVMIntSLT, value, minimum, "asbelowmin");
                        value = LLVMBuildSelect(ctx.builder, below, minimum, value, "asclampmin");
                        LLVMValueRef above = LLVMBuildICmp(ctx.builder, LLVMIntSGT, value, maximum, "asabovemax");
                        value = LLVMBuildSelect(ctx.builder, above, maximum, value, "asclampmax");
                    }
                }
                else
                {
                    if (sourceIsSigned)
                    {
                        LLVMValueRef zero = LLVMConstNull(sourceType);
                        LLVMValueRef below = LLVMBuildICmp(ctx.builder, LLVMIntSLT, value, zero, "asbelowzero");
                        value = LLVMBuildSelect(ctx.builder, below, zero, value, "asclampzero");
                    }
                    if (sourceBits > targetBits || unsignedToSigned)
                    {
                        const uint64_t targetMaximum =
                            target.isSigned ? (targetBits == 64 ? INT64_MAX : (UINT64_C(1) << (targetBits - 1)) - 1)
                                            : (targetBits == 64 ? UINT64_MAX : (UINT64_C(1) << targetBits) - 1);
                        LLVMValueRef maximum = LLVMConstInt(sourceType, targetMaximum, 0);
                        LLVMValueRef above = LLVMBuildICmp(ctx.builder, LLVMIntUGT, value, maximum, "asabovemax");
                        value = LLVMBuildSelect(ctx.builder, above, maximum, value, "asclampmax");
                    }
                }
            }
            return LLVMBuildIntCast2(ctx.builder, value, targetType, sourceIsSigned ? 1 : 0, "ascast");
        }
        if (sourceKind == LLVMIntegerTypeKind && targetIsFloat)
            return sourceIsSigned ? LLVMBuildSIToFP(ctx.builder, value, targetType, "ascast")
                                  : LLVMBuildUIToFP(ctx.builder, value, targetType, "ascast");
        if (sourceIsFloat && targetKind == LLVMIntegerTypeKind)
            return target.isSigned ? LLVMBuildFPToSI(ctx.builder, value, targetType, "ascast")
                                   : LLVMBuildFPToUI(ctx.builder, value, targetType, "ascast");
        if (sourceIsFloat && targetIsFloat)
            return LLVMBuildFPCast(ctx.builder, value, targetType, "ascast");
        if (sourceKind == LLVMIntegerTypeKind && targetIsPointer)
            return LLVMBuildIntToPtr(ctx.builder, value, targetType, "ascast");
        if (sourceIsPointer && targetKind == LLVMIntegerTypeKind)
            return LLVMBuildPtrToInt(ctx.builder, value, targetType, "ascast");
        if (sourceIsPointer && targetIsPointer)
            return LLVMBuildBitCast(ctx.builder, value, targetType, "ascast");
        codegen_error("unsupported source or target for cast");
    }

    static LLVMValueRef materialize_value(Context &ctx, LLVMValueRef value, const CGType &target, const char *name)
    {
        if (!target.isOptional || LLVMTypeOf(value) == target.llvmType)
            return cast_value(ctx.builder, value, target.llvmType, target.isSigned, name);
        if (!target.innerType)
            compiler_error("optional type has no payload type");
        LLVMValueRef aggregate = LLVMConstNull(target.llvmType);
        LLVMValueRef present = LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 1, 0);
        aggregate = LLVMBuildInsertValue(ctx.builder, aggregate, present, 0, "optionalpresent");
        LLVMValueRef payload =
            cast_value(ctx.builder, value, target.innerType->llvmType, target.innerType->isSigned, name);
        return LLVMBuildInsertValue(ctx.builder, aggregate, payload, 1, "optionalpayload");
    }

    static std::string decode_string_literal(std::string_view source);

    static std::string std_base_name(const std::string &name)
    {
        const size_t separator = name.rfind("::");
        return separator == std::string::npos ? name : name.substr(separator + 2);
    }

    static LLVMValueRef generate_value_for_target(Context &ctx, const Parser::ASTNode &node, const CGType &target);

    static std::string union_template_name(const std::string &specializationName)
    {
        if (unionTypes.count(specializationName))
            return specializationName;
        std::string candidate = specializationName;
        while (!candidate.empty())
        {
            if (unionTypes.count(candidate))
                return candidate;
            const size_t separator = candidate.rfind('.');
            if (separator == std::string::npos)
                break;
            candidate.resize(separator);
        }
        return {};
    }

    static std::string scope_access_name(const Parser::ASTNode &node);

    static void materialize_union_layout(Context &ctx, StructInfo &layout, UnionInfo &info)
    {
        if (!ctx.targetData)
            compiler_error("union layout requires target data");

        LLVMTypeRef alignmentType = LLVMInt8TypeInContext(ctx.llvmCtx);
        uint64_t payloadSize = 0;
        uint64_t payloadAlignment = 1;
        for (auto &[_, variant] : info.variants)
        {
            if (variant.payloadTypes.empty())
                continue;
            std::vector<LLVMTypeRef> fields;
            for (const CGType &payload : variant.payloadTypes)
                fields.push_back(payload.llvmType);
            variant.payloadType = LLVMStructTypeInContext(ctx.llvmCtx, fields.data(), fields.size(), false);
            const uint64_t size = LLVMABISizeOfType(ctx.targetData, variant.payloadType);
            const uint64_t alignment = LLVMABIAlignmentOfType(ctx.targetData, variant.payloadType);
            if (size > payloadSize)
                payloadSize = size;
            if (alignment > payloadAlignment)
            {
                payloadAlignment = alignment;
                alignmentType = variant.payloadType;
            }
        }

        LLVMTypeRef fields[] = {
            LLVMInt32TypeInContext(ctx.llvmCtx),
            LLVMArrayType(alignmentType, 0),
            LLVMArrayType(LLVMInt8TypeInContext(ctx.llvmCtx), payloadSize),
        };
        LLVMStructSetBody(layout.llvmType, fields, 3, false);
        layout.fieldNames = {"__union_tag", "__union_alignment", "__union_payload"};
        layout.fieldTypes = {CGType{fields[0]}, CGType{fields[1]}, CGType{fields[2]}};
        layout.fieldTypeNodes.clear();
    }

    static LLVMValueRef generate_union_constructor(Context &ctx, const Parser::ASTNode &node, const CGType &target)
    {
        if (node.children.empty() || node.children.front().type != Parser::NodeType::ScopeAccessExpr)
            return nullptr;
        const std::string unionName = union_template_name(target.structName);
        if (unionName.empty())
            return nullptr;
        const Parser::ASTNode &callee = node.children.front();
        if (callee.children.empty())
            codegen_error(callee, "Malformed union constructor.");
        const std::string constructorUnion = scope_access_name(callee.children.front());
        const auto metadata = unionTypes.find(unionName);
        if (metadata == unionTypes.end())
            codegen_error(callee, "Union constructor metadata is unavailable.");
        const std::string &declarationName = metadata->second.declarationName;
        const std::string unqualifiedName = declarationName.substr(declarationName.rfind('.') + 1);
        if (constructorUnion != declarationName && constructorUnion != unqualifiedName)
            return nullptr;
        const std::string variantName = std::string(std::get<std::string_view>(callee.value));
        const auto variant = metadata->second.variants.find(variantName);
        if (variant == metadata->second.variants.end())
            codegen_error(callee, "Unknown union constructor variant '" + variantName + "'.");
        if (node.children.size() - 1 != variant->second.payloadTypes.size())
            codegen_error(callee, "Union constructor argument count mismatch.");
        const auto layout = structTypes.find(target.structName);
        if (layout == structTypes.end())
            codegen_error(node, "Union constructor has no materialized target layout.");

        LLVMValueRef resultAddress = LLVMBuildAlloca(ctx.builder, target.llvmType, "unionvalue");
        LLVMBuildStore(ctx.builder, LLVMConstNull(target.llvmType), resultAddress);
        LLVMValueRef tagAddress = LLVMBuildStructGEP2(ctx.builder, target.llvmType, resultAddress, 0, "uniontagaddr");
        LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), variant->second.tag, 0),
                       tagAddress);
        if (!variant->second.payloadTypes.empty())
        {
            LLVMValueRef payload = LLVMConstNull(variant->second.payloadType);
            for (size_t index = 0; index < variant->second.payloadTypes.size(); ++index)
            {
                const CGType &fieldType = variant->second.payloadTypes[index];
                LLVMValueRef value = generate_value_for_target(ctx, node.children[index + 1], fieldType);
                if (!value)
                    codegen_error(node.children[index + 1], "Union constructor payload did not produce a value.");
                payload =
                    LLVMBuildInsertValue(ctx.builder, payload, materialize_value(ctx, value, fieldType, "unionpayload"),
                                         static_cast<unsigned>(index), "unionpayload");
            }
            LLVMValueRef storage =
                LLVMBuildStructGEP2(ctx.builder, target.llvmType, resultAddress, 2, "unionpayloadaddr");
            LLVMValueRef typedStorage = LLVMBuildBitCast(
                ctx.builder, storage, LLVMPointerType(variant->second.payloadType, 0), "unionpayloadcast");
            LLVMBuildStore(ctx.builder, payload, typedStorage);
        }
        return LLVMBuildLoad2(ctx.builder, target.llvmType, resultAddress, "unionvalue");
    }

    static LLVMValueRef generate_value_for_target(Context &ctx, const Parser::ASTNode &node, const CGType &target)
    {
        if (node.type == Parser::NodeType::CallExpr)
        {
            if (LLVMValueRef constructor = generate_union_constructor(ctx, node, target))
                return constructor;
        }
        if ((node.type == Parser::NodeType::IntegerLiteral || node.type == Parser::NodeType::FloatLiteral) &&
            (target.isBool || LLVMGetTypeKind(target.llvmType) == LLVMIntegerTypeKind ||
             LLVMGetTypeKind(target.llvmType) == LLVMFloatTypeKind ||
             LLVMGetTypeKind(target.llvmType) == LLVMDoubleTypeKind))
        {
            return cast_as_value(ctx, generate_node(ctx, node), target, false);
        }
        if (target.isBool || LLVMGetTypeKind(target.llvmType) == LLVMIntegerTypeKind ||
            LLVMGetTypeKind(target.llvmType) == LLVMFloatTypeKind ||
            LLVMGetTypeKind(target.llvmType) == LLVMDoubleTypeKind)
        {
            if (node.type == Parser::NodeType::Identifier || node.type == Parser::NodeType::MemberAccessExpr ||
                node.type == Parser::NodeType::IndexExpr)
            {
                const CGType source = lvalue_type(ctx, node);
                if (LLVMGetTypeKind(source.llvmType) == LLVMIntegerTypeKind ||
                    LLVMGetTypeKind(source.llvmType) == LLVMFloatTypeKind ||
                    LLVMGetTypeKind(source.llvmType) == LLVMDoubleTypeKind)
                {
                    return cast_as_value(ctx, generate_node(ctx, node), target, source.isSigned);
                }
            }
        }
        if ((std_base_name(target.structName) == "str" || std_base_name(target.structName) == "View" ||
             target.structName == "cstr") &&
            node.type == Parser::NodeType::StringLiteral)
        {
            LLVMValueRef value = LLVMConstNull(target.llvmType);
            LLVMValueRef data = generate_node(ctx, node);
            value = LLVMBuildInsertValue(ctx.builder, value, data, 0, "stringliteraldata");
            if (target.structName == "cstr")
                return value;
            LLVMValueRef length = LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx),
                                               decode_string_literal(std::get<std::string_view>(node.value)).size(), 0);
            return LLVMBuildInsertValue(ctx.builder, value, length, 1, "strliterallength");
        }
        if (node.type == Parser::NodeType::StructInitExpr)
        {
            if (node.hasInferredType && node.inferredTypeName != target.structName)
                codegen_error(node, "typed brace initializer does not match its target type");
            const auto info = structTypes.find(target.structName);
            if (info == structTypes.end() || !info->second.initializedField.empty())
                compiler_error("brace initializer requires a plain declared struct type");
            if (node.children.size() != info->second.fieldTypes.size())
                compiler_error("brace initializer field count does not match its type");
            LLVMValueRef aggregate = LLVMConstNull(target.llvmType);
            for (size_t index = 0; index < info->second.fieldTypes.size(); ++index)
            {
                const CGType &fieldType = info->second.fieldTypes[index];
                LLVMValueRef field = generate_value_for_target(ctx, node.children[index], fieldType);
                if (!field)
                    compiler_error("brace initializer field did not produce a value");
                aggregate = LLVMBuildInsertValue(ctx.builder, aggregate,
                                                 materialize_value(ctx, field, fieldType, "structinitcast"),
                                                 static_cast<unsigned>(index), "structinitvalue");
            }
            return aggregate;
        }
        if (target.isOptional && node.type == Parser::NodeType::Identifier)
        {
            const std::string name = std::string(std::get<std::string_view>(node.value));
            if (VarInfo *source = ctx.find_var(name); source && source->type.isOptional)
                return LLVMBuildLoad2(ctx.builder, source->type.llvmType, source->address, "optionalcopy");
        }
        return generate_node(ctx, node);
    }

    static LLVMValueRef generate_global_constant(Context &ctx, const Parser::ASTNode &node, const CGType &target)
    {
        if (node.type == Parser::NodeType::IntegerLiteral && LLVMGetTypeKind(target.llvmType) == LLVMIntegerTypeKind)
            return LLVMConstInt(target.llvmType, std::get<uint64_t>(node.value), 0);
        if (node.type == Parser::NodeType::BoolLiteral && LLVMGetTypeKind(target.llvmType) == LLVMIntegerTypeKind)
            return LLVMConstInt(target.llvmType, std::get<bool>(node.value) ? 1 : 0, 0);
        if (node.type == Parser::NodeType::FloatLiteral && (LLVMGetTypeKind(target.llvmType) == LLVMFloatTypeKind ||
                                                            LLVMGetTypeKind(target.llvmType) == LLVMDoubleTypeKind))
            return LLVMConstReal(target.llvmType, std::get<double>(node.value));

        if (node.type == Parser::NodeType::StructInitExpr)
        {
            const auto info = structTypes.find(target.structName);
            if (info == structTypes.end() || !info->second.initializedField.empty())
                codegen_error(node, "global brace initializer requires a plain declared struct type");
            if (node.children.size() != info->second.fieldTypes.size())
                codegen_error(node, "brace initializer field count does not match its type");
            std::vector<LLVMValueRef> fields;
            fields.reserve(info->second.fieldTypes.size());
            for (size_t index = 0; index < info->second.fieldTypes.size(); ++index)
            {
                LLVMValueRef value =
                    generate_global_constant(ctx, node.children[index], info->second.fieldTypes[index]);
                if (!value || !LLVMIsConstant(value) || LLVMTypeOf(value) != info->second.fieldTypes[index].llvmType)
                    codegen_error(node.children[index], "global struct field initializer must be a matching constant");
                fields.push_back(value);
            }
            return LLVMConstNamedStruct(target.llvmType, fields.data(), static_cast<unsigned>(fields.size()));
        }

        if (node.type == Parser::NodeType::Identifier)
        {
            const std::string name = std::string(std::get<std::string_view>(node.value));
            VarInfo *variable = ctx.find_var(name);
            if (variable && LLVMIsAGlobalVariable(variable->address))
            {
                LLVMValueRef initializer = LLVMGetInitializer(variable->address);
                if (!initializer)
                    return LLVMConstNull(target.llvmType);
                if (LLVMTypeOf(initializer) == target.llvmType)
                    return initializer;
                if (LLVMGetTypeKind(target.llvmType) == LLVMPointerTypeKind &&
                    LLVMGetTypeKind(LLVMTypeOf(initializer)) == LLVMPointerTypeKind)
                    return LLVMConstBitCast(initializer, target.llvmType);
            }
        }

        if (node.type == Parser::NodeType::UnaryExpr && !node.children.empty() &&
            std::get<Lexer::Operator>(node.value) == Lexer::Operator::AMPERSAND)
        {
            const Parser::ASTNode &operand = node.children.front();
            LLVMValueRef address = nullptr;
            if (operand.type == Parser::NodeType::Identifier)
            {
                if (VarInfo *variable = ctx.find_var(std::string(std::get<std::string_view>(operand.value)));
                    variable && LLVMIsAGlobalVariable(variable->address))
                    address = variable->address;
            }
            else if (operand.type == Parser::NodeType::IndexExpr && operand.children.size() == 2 &&
                     operand.children.front().type == Parser::NodeType::Identifier &&
                     operand.children[1].type == Parser::NodeType::IntegerLiteral)
            {
                if (VarInfo *variable =
                        ctx.find_var(std::string(std::get<std::string_view>(operand.children.front().value)));
                    variable && LLVMIsAGlobalVariable(variable->address) && variable->type.isArray &&
                    !variable->type.isPointerLike)
                {
                    LLVMValueRef indices[] = {
                        LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), 0, 0),
                        LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), std::get<uint64_t>(operand.children[1].value),
                                     0),
                    };
                    address = LLVMConstInBoundsGEP2(LLVMTypeOf(variable->address), variable->address, indices, 2);
                }
            }
            if (address)
            {
                if (LLVMTypeOf(address) == target.llvmType)
                    return address;
                if (LLVMGetTypeKind(target.llvmType) == LLVMPointerTypeKind)
                    return LLVMConstBitCast(address, target.llvmType);
            }
        }

        LLVMValueRef value = generate_node(ctx, node);
        if (value && LLVMIsConstant(value) && LLVMTypeOf(value) == target.llvmType)
            return value;
        codegen_error(node, "global initializer must be a constant with the declared type");
    }

    static LLVMValueRef generate_call_argument(Context &ctx, const Parser::ASTNode &node, const CGType &parameterType)
    {
        if (node.type == Parser::NodeType::StringLiteral &&
            (std_base_name(parameterType.structName) == "str" || std_base_name(parameterType.structName) == "View" ||
             parameterType.structName == "cstr"))
            return generate_value_for_target(ctx, node, parameterType);

        const auto dereference_argument = [&](const CGType &referenceType, LLVMValueRef reference)
        {
            if (!referenceType.pointeeType)
                compiler_error("reference argument has no pointee type");
            LLVMValueRef value = LLVMBuildLoad2(ctx.builder, referenceType.pointeeType, reference, "callargderef");
            return cast_value(ctx.builder, value, parameterType.llvmType, parameterType.isSigned, "callargcast");
        };

        if (!parameterType.isReference && !parameterType.isPointerLike)
        {
            if (node.type == Parser::NodeType::RefExpr && !node.children.empty())
            {
                CGType pointeeType = lvalue_type(ctx, node.children.front());
                CGType referenceType;
                referenceType.pointeeType = pointeeType.llvmType;
                return dereference_argument(referenceType, generate_node(ctx, node));
            }
            if (const auto argumentType = try_lvalue_type(ctx, node))
            {
                if (argumentType->isReference)
                    return dereference_argument(*argumentType, generate_node(ctx, node));
            }
        }

        if (parameterType.isReference && node.type != Parser::NodeType::RefExpr)
        {
            const auto argumentType = try_lvalue_type(ctx, node);
            if (!argumentType)
                codegen_error(node, "reference parameter requires an addressable argument");
            if (!argumentType->isReference)
            {
                LLVMValueRef address = get_lvalue(ctx, node);
                if (!address)
                    codegen_error(node, "reference parameter requires an addressable argument");
                return address;
            }
        }

        LLVMValueRef argument = generate_node(ctx, node);
        if (!argument)
            compiler_error("call argument did not produce a value");
        if (LLVMGetTypeKind(LLVMTypeOf(argument)) == LLVMStructTypeKind &&
            (LLVMGetTypeKind(parameterType.llvmType) != LLVMStructTypeKind ||
             LLVMTypeOf(argument) != parameterType.llvmType))
        {
            codegen_error(node, "Call argument does not match the parameter's aggregate type.");
        }
        if (LLVMGetTypeKind(LLVMTypeOf(argument)) == LLVMPointerTypeKind &&
            LLVMGetTypeKind(parameterType.llvmType) == LLVMStructTypeKind)
        {
            codegen_error(node, "Cannot pass a pointer where the parameter requires an aggregate value.");
        }
        return cast_value(ctx.builder, argument, parameterType.llvmType, parameterType.isSigned, "callargcast");
    }

    static std::string decode_string_literal(std::string_view source)
    {
        const auto hex_digit = [](char character) -> unsigned
        {
            if (character >= '0' && character <= '9')
                return character - '0';
            if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;
            if (character >= 'A' && character <= 'F')
                return character - 'A' + 10;
            compiler_error("invalid hexadecimal string escape");
        };
        const auto append_utf8 = [](std::string &output, unsigned codepoint)
        {
            if (codepoint <= 0x7f)
                output += static_cast<char>(codepoint);
            else if (codepoint <= 0x7ff)
            {
                output += static_cast<char>(0xc0 | (codepoint >> 6));
                output += static_cast<char>(0x80 | (codepoint & 0x3f));
            }
            else
            {
                output += static_cast<char>(0xe0 | (codepoint >> 12));
                output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
                output += static_cast<char>(0x80 | (codepoint & 0x3f));
            }
        };
        std::string result;
        for (size_t index = 0; index < source.size(); ++index)
        {
            if (source[index] != '\\' || index + 1 >= source.size())
            {
                result += source[index];
                continue;
            }
            const char escaped = source[++index];
            switch (escaped)
            {
            case 'n':
                result += '\n';
                break;
            case 'r':
                result += '\r';
                break;
            case 't':
                result += '\t';
                break;
            case '0':
                result += '\0';
                break;
            case '\\':
                result += '\\';
                break;
            case '"':
                result += '"';
                break;
            case '\'':
                result += '\'';
                break;
            case 'x':
                if (index + 2 >= source.size())
                    compiler_error("incomplete hexadecimal string escape");
                result += static_cast<char>((hex_digit(source[index + 1]) << 4) | hex_digit(source[index + 2]));
                index += 2;
                break;
            case 'u':
            {
                if (index + 4 >= source.size())
                    compiler_error("incomplete Unicode string escape");
                unsigned codepoint = 0;
                for (size_t digit = 1; digit <= 4; ++digit)
                    codepoint = (codepoint << 4) | hex_digit(source[index + digit]);
                if (codepoint >= 0xd800 && codepoint <= 0xdfff)
                    compiler_error("Unicode string escape cannot encode a surrogate");
                append_utf8(result, codepoint);
                index += 4;
                break;
            }
            default:
                result += escaped;
                break;
            }
        }
        return result;
    }

    static std::string custom_type_name(const Parser::ASTNode &node)
    {
        return node.inferredTypeName.empty() ? std::string(std::get<std::string_view>(node.value))
                                             : node.inferredTypeName;
    }

    static bool is_vector_template_name(const std::string &name)
    {
        static const std::string suffix = "::Vector";
        return name == "Vector" ||
               (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0);
    }

    static bool is_vector_specialization(const std::string &structName)
    {
        const size_t dot = structName.find('.');
        return dot != std::string::npos && is_vector_template_name(structName.substr(0, dot));
    }

    static std::string generic_specialization_component(const CGType &type)
    {
        const LLVMTypeKind kind = LLVMGetTypeKind(type.llvmType);
        if (kind == LLVMIntegerTypeKind)
            return std::string(type.isSigned ? "i" : "u") + std::to_string(LLVMGetIntTypeWidth(type.llvmType));
        if (kind == LLVMFloatTypeKind)
            return "f32";
        if (kind == LLVMDoubleTypeKind)
            return "f64";
        if (!type.structName.empty())
        {
            std::string result = type.structName;
            for (char &character : result)
                if (character == ':' || character == '.')
                    character = '_';
            return result;
        }
        if (type.isPointerLike)
            return "ptr" + (type.innerType ? "_" + generic_specialization_component(*type.innerType) : "");
        compiler_error("generic argument does not have a concrete backend representation");
    }

    static std::string qualified_declaration_name(const Context &ctx, const std::string &name)
    {
        return ctx.currentNamespaceName.empty() ? name : ctx.currentNamespaceName + "::" + name;
    }

    static std::string resolve_imported_function_name(const Context &ctx, const std::string &name)
    {
        if (functions.count(name))
            return name;
        for (auto imported = ctx.importedNamespaces.rbegin(); imported != ctx.importedNamespaces.rend(); ++imported)
        {
            const std::string importedName = *imported + "." + name;
            if (functions.count(importedName))
                return importedName;
        }
        return name;
    }

    static std::string scope_access_name(const Parser::ASTNode &node)
    {
        if (node.type == Parser::NodeType::Identifier)
            return std::string(std::get<std::string_view>(node.value));
        if (node.type == Parser::NodeType::ScopeAccessExpr && !node.children.empty())
            return scope_access_name(node.children.front()) +
                   "::" + std::string(std::get<std::string_view>(node.value));
        compiler_error("malformed scope access");
    }

    static bool is_string_array_type(const Parser::ASTNode &node)
    {
        return node.type == Parser::NodeType::ArrayType && !node.children.empty() &&
               node.children.front().type == Parser::NodeType::CustomType &&
               std_base_name(custom_type_name(node.children.front())) == "String";
    }

    static void validate_normal_main_signature(const Parser::ASTNode &node, const std::string &name)
    {
        if (name != "main")
            return;

        size_t parameters = 0;
        for (const auto &child : node.children)
        {
            if (child.type == Parser::NodeType::TunnelSlot)
                compiler_error("main cannot declare tunnel outputs");
            if (child.type == Parser::NodeType::Param)
            {
                ++parameters;
                if (parameters != 1 || child.children.empty() || !is_string_array_type(child.children.front()))
                    compiler_error("main must take no arguments or String[] args");
            }
        }
    }

    static int64_t evaluate_enum_constant(const Parser::ASTNode &node, const EnumInfo &members)
    {
        if (node.type == Parser::NodeType::IntegerLiteral)
            return static_cast<int64_t>(std::get<uint64_t>(node.value));
        if (node.type == Parser::NodeType::CharLiteral)
        {
            const std::string_view text = std::get<std::string_view>(node.value);
            if (text.empty())
                compiler_error("empty enum character constant");
            return static_cast<unsigned char>(text.front());
        }
        if (node.type == Parser::NodeType::Identifier)
        {
            const auto found = members.members.find(std::string(std::get<std::string_view>(node.value)));
            if (found == members.members.end())
                compiler_error("enum constant expression refers to an unknown member");
            return found->second;
        }
        if (node.type == Parser::NodeType::UnaryExpr && node.children.size() == 1)
        {
            const int64_t value = evaluate_enum_constant(node.children.front(), members);
            const Lexer::Operator op = std::get<Lexer::Operator>(node.value);
            if (op == Lexer::Operator::PLUS)
                return value;
            if (op == Lexer::Operator::MINUS)
                return -value;
            if (op == Lexer::Operator::EXLAMATION_MARK)
                return !value;
        }
        if (node.type == Parser::NodeType::BinaryExpr && node.children.size() == 2)
        {
            const int64_t lhs = evaluate_enum_constant(node.children[0], members);
            const int64_t rhs = evaluate_enum_constant(node.children[1], members);
            switch (std::get<Lexer::Operator>(node.value))
            {
            case Lexer::Operator::PLUS:
                return lhs + rhs;
            case Lexer::Operator::MINUS:
                return lhs - rhs;
            case Lexer::Operator::MULTIPLY:
                return lhs * rhs;
            case Lexer::Operator::DIVIDE:
                if (rhs == 0)
                    compiler_error("enum constant expression divides by zero");
                return lhs / rhs;
            case Lexer::Operator::MODULO:
                if (rhs == 0)
                    compiler_error("enum constant expression divides by zero");
                return lhs % rhs;
            case Lexer::Operator::LEFT_SHIFT:
                return lhs << rhs;
            case Lexer::Operator::RIGHT_SHIFT:
                return lhs >> rhs;
            case Lexer::Operator::PIPE:
                return lhs | rhs;
            case Lexer::Operator::AMPERSAND:
                return lhs & rhs;
            case Lexer::Operator::CARET:
                return lhs ^ rhs;
            default:
                break;
            }
        }
        compiler_error("enum member value must be an integral constant expression");
    }

    static CGType resolve_type(Context &ctx, const Parser::ASTNode &node)
    {
        CGType cgType;
        switch (node.type)
        {
        case Parser::NodeType::PrimitiveType:
        {
            auto kw = std::get<Lexer::Keyword>(node.value);
            switch (kw)
            {
            case Lexer::Keyword::U8:
            case Lexer::Keyword::I8:
                cgType.llvmType = LLVMInt8TypeInContext(ctx.llvmCtx);
                cgType.isSigned = (kw == Lexer::Keyword::I8);
                break;
            case Lexer::Keyword::U16:
            case Lexer::Keyword::I16:
                cgType.llvmType = LLVMInt16TypeInContext(ctx.llvmCtx);
                cgType.isSigned = (kw == Lexer::Keyword::I16);
                break;
            case Lexer::Keyword::U32:
            case Lexer::Keyword::I32:
            case Lexer::Keyword::CHAR:
                cgType.llvmType = LLVMInt32TypeInContext(ctx.llvmCtx);
                cgType.isSigned = (kw == Lexer::Keyword::I32);
                cgType.isChar = (kw == Lexer::Keyword::CHAR);
                break;
            case Lexer::Keyword::U64:
            case Lexer::Keyword::I64:
                cgType.llvmType = LLVMInt64TypeInContext(ctx.llvmCtx);
                cgType.isSigned = (kw == Lexer::Keyword::I64);
                break;
            case Lexer::Keyword::USIZE:
                cgType.llvmType = LLVMIntTypeInContext(ctx.llvmCtx, ctx.targetPointerWidthBits);
                cgType.isSigned = false;
                break;
            case Lexer::Keyword::F32:
                cgType.llvmType = LLVMFloatTypeInContext(ctx.llvmCtx);
                cgType.isFloat = true;
                break;
            case Lexer::Keyword::F64:
                cgType.llvmType = LLVMDoubleTypeInContext(ctx.llvmCtx);
                cgType.isFloat = true;
                break;
            case Lexer::Keyword::BOOL:
                cgType.llvmType = LLVMInt1TypeInContext(ctx.llvmCtx);
                cgType.isBool = true;
                break;
            default:
                cgType.llvmType = LLVMInt64TypeInContext(ctx.llvmCtx);
                break; // fallback
            }
            break;
        }
        case Parser::NodeType::PointerType:
        case Parser::NodeType::ReferenceType:
        {
            CGType pointee = resolve_type(ctx, node.children.front());
            cgType.llvmType = LLVMPointerType(LLVMInt8TypeInContext(ctx.llvmCtx), 0);
            cgType.pointeeType = pointee.llvmType;
            cgType.innerType = std::make_shared<CGType>(pointee);
            cgType.isPointerLike = true;
            cgType.isReference = node.type == Parser::NodeType::ReferenceType;
            cgType.isSigned = pointee.isSigned;
            cgType.structName = pointee.structName;
            break;
        }
        case Parser::NodeType::ArrayType:
        {
            CGType innerType = resolve_type(ctx, node.children[0]);
            cgType.innerType = std::make_shared<CGType>(innerType);
            if (std::holds_alternative<uint64_t>(node.value))
            {
                uint64_t size = std::get<uint64_t>(node.value);
                cgType.llvmType = LLVMArrayType(innerType.llvmType, size);
                cgType.arrayLength = size;
            }
            else
            {
                cgType.llvmType = LLVMPointerType(LLVMInt8TypeInContext(ctx.llvmCtx), 0);
                cgType.pointeeType = innerType.llvmType;
                cgType.innerType = std::make_shared<CGType>(innerType);
                cgType.isPointerLike = true;
                if (std::holds_alternative<std::string_view>(node.value))
                    cgType.runtimeArrayLengthName = std::string(std::get<std::string_view>(node.value));
            }
            cgType.isArray = true;
            cgType.structName = innerType.structName;
            break;
        }
        case Parser::NodeType::CustomType:
        {
            std::string name = custom_type_name(node);
            if (name.find("::") == std::string::npos && !structTypes.count(name) && !enumTypes.count(name) &&
                !unionTypes.count(name))
            {
                for (auto imported = ctx.importedNamespaces.rbegin(); imported != ctx.importedNamespaces.rend();
                     ++imported)
                {
                    if (std_base_name(*imported) == name &&
                        (structTypes.count(*imported) || enumTypes.count(*imported) || unionTypes.count(*imported)))
                    {
                        name = *imported;
                        break;
                    }
                    const std::string qualified = *imported + "::" + name;
                    if (structTypes.count(qualified) || enumTypes.count(qualified) || unionTypes.count(qualified))
                    {
                        name = qualified;
                        break;
                    }
                }
            }
            if (name == "Self" && !ctx.currentClassName.empty())
                name = ctx.currentClassName;
            if (const auto binding = ctx.genericBindings.find(name); binding != ctx.genericBindings.end())
                return binding->second;
            if (node.children.size() > 0 && structTypes.count(name) && !is_vector_template_name(name) &&
                !structTypes.at(name).genericParameters.empty())
            {
                const StructInfo &templateInfo = structTypes.at(name);
                if (node.children.size() != templateInfo.genericParameters.size())
                    compiler_error("generic type argument count does not match its declaration");
                std::vector<CGType> arguments;
                std::string specializationName = name;
                for (const Parser::ASTNode &argument : node.children)
                {
                    CGType resolved = resolve_type(ctx, argument);
                    arguments.push_back(resolved);
                    specializationName += "." + generic_specialization_component(resolved);
                }
                if (!structTypes.count(specializationName))
                {
                    StructInfo specialization = templateInfo;
                    specialization.llvmType = LLVMStructCreateNamed(ctx.llvmCtx, specializationName.c_str());
                    const auto savedBindings = ctx.genericBindings;
                    for (size_t index = 0; index < arguments.size(); ++index)
                        ctx.genericBindings[templateInfo.genericParameters[index]] = arguments[index];
                    if (const auto unionTemplate = unionTypes.find(name); unionTemplate != unionTypes.end())
                    {
                        UnionInfo unionSpecialization = unionTemplate->second;
                        for (auto &[_, variant] : unionSpecialization.variants)
                        {
                            variant.payloadTypes.clear();
                            for (const Parser::ASTNode &payloadNode : variant.payloadTypeNodes)
                                variant.payloadTypes.push_back(resolve_type(ctx, payloadNode));
                        }
                        materialize_union_layout(ctx, specialization, unionSpecialization);
                        unionTypes[specializationName] = std::move(unionSpecialization);
                    }
                    else
                    {
                        specialization.fieldTypes.clear();
                        std::vector<LLVMTypeRef> fields;
                        for (const Parser::ASTNode &fieldNode : templateInfo.fieldTypeNodes)
                        {
                            CGType field = resolve_type(ctx, fieldNode);
                            specialization.fieldTypes.push_back(field);
                            fields.push_back(field.llvmType);
                        }
                        LLVMStructSetBody(specialization.llvmType, fields.data(), fields.size(), false);
                    }
                    ctx.genericBindings = savedBindings;
                    structTypes[specializationName] = std::move(specialization);
                }
                cgType.llvmType = structTypes.at(specializationName).llvmType;
                cgType.requiredAlignment = structTypes.at(specializationName).requestedAlignment;
                cgType.structName = specializationName;
            }
            else if (is_vector_template_name(name) && node.children.size() == 1 && structTypes.count(name))
            {
                const CGType elementType = resolve_type(ctx, node.children.front());
                const std::string elementName = generic_specialization_component(elementType);
                const std::string specializationName = name + "." + elementName;
                if (!structTypes.count(specializationName))
                {
                    StructInfo specialization = structTypes.at(name);
                    specialization.llvmType = LLVMStructCreateNamed(ctx.llvmCtx, specializationName.c_str());
                    CGType dataType;
                    dataType.llvmType = LLVMPointerType(LLVMInt8TypeInContext(ctx.llvmCtx), 0);
                    dataType.pointeeType = elementType.llvmType;
                    dataType.innerType = std::make_shared<CGType>(elementType);
                    dataType.isPointerLike = true;
                    dataType.isSigned = elementType.isSigned;
                    specialization.fieldTypes.front() = dataType;
                    if (specialization.fieldTypes.size() > 1)
                        specialization.fieldTypes[1] = dataType;
                    std::vector<LLVMTypeRef> fields;
                    for (const CGType &field : specialization.fieldTypes)
                        fields.push_back(field.llvmType);
                    LLVMStructSetBody(specialization.llvmType, fields.data(), fields.size(), false);
                    structTypes[specializationName] = std::move(specialization);
                }
                cgType.llvmType = structTypes.at(specializationName).llvmType;
                cgType.structName = specializationName;
            }
            else if (structTypes.count(name))
            {
                cgType.llvmType = structTypes[name].llvmType;
                cgType.requiredAlignment = structTypes[name].requestedAlignment;
                cgType.structName = name;
            }
            else if (enumTypes.count(name))
            {
                cgType = enumTypes.at(name).backingType;
                cgType.structName = name;
            }
            else
            {
                cgType.llvmType = LLVMInt64TypeInContext(ctx.llvmCtx);
            }
            break;
        }
        case Parser::NodeType::OptionalType:
        {
            if (node.children.empty())
                compiler_error("optional type is missing its payload type");
            CGType payload = resolve_type(ctx, node.children.front());
            LLVMTypeRef fields[] = {LLVMInt1TypeInContext(ctx.llvmCtx), payload.llvmType};
            cgType.llvmType = LLVMStructTypeInContext(ctx.llvmCtx, fields, 2, 0);
            cgType.innerType = std::make_shared<CGType>(payload);
            cgType.isOptional = true;
            break;
        }
        default:
            cgType.llvmType = LLVMVoidTypeInContext(ctx.llvmCtx);
            break;
        }
        return cgType;
    }

    static LLVMValueRef declare_c_function(Context &ctx, const Parser::ASTNode &node)
    {
        if (node.children.empty() || node.children.front().type != Parser::NodeType::Identifier)
            compiler_error("C function declaration is missing its name");

        std::vector<LLVMTypeRef> paramTypes;
        LLVMTypeRef returnType = LLVMVoidTypeInContext(ctx.llvmCtx);
        for (const auto &child : node.children)
        {
            if (child.type == Parser::NodeType::Param)
                paramTypes.push_back(resolve_type(ctx, child.children.front()).llvmType);
            else if (child.type == Parser::NodeType::PrimitiveType || child.type == Parser::NodeType::PointerType ||
                     child.type == Parser::NodeType::ReferenceType || child.type == Parser::NodeType::CustomType)
                returnType = resolve_type(ctx, child).llvmType;
        }

        const std::string name =
            mangle_function_name(std::string(std::get<std::string_view>(node.children.front().value)));
        LLVMTypeRef functionType = LLVMFunctionType(returnType, paramTypes.data(), paramTypes.size(), 0);
        LLVMValueRef function = LLVMGetNamedFunction(ctx.module, name.c_str());
        if (!function)
            return LLVMAddFunction(ctx.module, name.c_str(), functionType);
        if (LLVMGlobalGetValueType(function) != functionType)
            compiler_error("conflicting C function declaration for '" + name + "'");
        return function;
    }

    static void predeclare_struct_types(Context &ctx, const Parser::ASTNode &node)
    {
        if (node.type == Parser::NodeType::NamespaceDecl)
        {
            const std::string previous = ctx.currentNamespaceName;
            ctx.currentNamespaceName =
                qualified_declaration_name(ctx, std::string(std::get<std::string_view>(node.value)));
            for (const auto &child : node.children)
                predeclare_struct_types(ctx, child);
            ctx.currentNamespaceName = previous;
            return;
        }
        if (node.type == Parser::NodeType::StructDecl || node.type == Parser::NodeType::UnionDecl)
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::Identifier)
                codegen_error(node, "Aggregate declaration is missing its name.");
            const std::string name =
                qualified_declaration_name(ctx, std::string(std::get<std::string_view>(node.children.front().value)));
            if (!structTypes.count(name))
            {
                StructInfo info;
                info.llvmType = LLVMStructCreateNamed(ctx.llvmCtx, name.c_str());
                structTypes[name] = std::move(info);
            }
            return;
        }
        for (const auto &child : node.children)
            predeclare_struct_types(ctx, child);
    }

    static void predeclare_c_functions(Context &ctx, const Parser::ASTNode &node)
    {
        if (node.type == Parser::NodeType::CFunctionDecl || node.type == Parser::NodeType::CFunctionDef)
            declare_c_function(ctx, node);
        for (const auto &child : node.children)
            predeclare_c_functions(ctx, child);
    }

    static void materialize_aggregate_layouts(Context &ctx, const Parser::ASTNode &node)
    {
        if (node.type == Parser::NodeType::NamespaceDecl)
        {
            const std::string previous = ctx.currentNamespaceName;
            ctx.currentNamespaceName =
                qualified_declaration_name(ctx, std::string(std::get<std::string_view>(node.value)));
            for (const auto &child : node.children)
                materialize_aggregate_layouts(ctx, child);
            ctx.currentNamespaceName = previous;
            return;
        }
        if (node.type == Parser::NodeType::StructDecl || node.type == Parser::NodeType::UnionDecl)
        {
            const bool previous = ctx.materializingAggregateLayouts;
            ctx.materializingAggregateLayouts = true;
            generate_node(ctx, node);
            ctx.materializingAggregateLayouts = previous;
            return;
        }
        for (const auto &child : node.children)
            materialize_aggregate_layouts(ctx, child);
    }

    static void predeclare_function_signatures(Context &ctx, const Parser::ASTNode &node)
    {
        if (node.type == Parser::NodeType::NamespaceDecl)
        {
            const std::string previous = ctx.currentNamespaceName;
            ctx.currentNamespaceName =
                qualified_declaration_name(ctx, std::string(std::get<std::string_view>(node.value)));
            for (const auto &child : node.children)
                predeclare_function_signatures(ctx, child);
            ctx.currentNamespaceName = previous;
            return;
        }
        if (node.type == Parser::NodeType::ImplDecl)
        {
            const bool previous = ctx.predeclaringFunctionSignatures;
            ctx.predeclaringFunctionSignatures = true;
            generate_node(ctx, node);
            ctx.predeclaringFunctionSignatures = previous;
            return;
        }
        if (node.type == Parser::NodeType::FunctionDef)
        {
            const bool previous = ctx.predeclaringFunctionSignatures;
            ctx.predeclaringFunctionSignatures = true;
            generate_node(ctx, node);
            ctx.predeclaringFunctionSignatures = previous;
        }
    }

    LLVMValueRef get_lvalue(Context &ctx, const Parser::ASTNode &node, RuntimeArrayIndexCapture *runtimeIndex)
    {
        if (node.type == Parser::NodeType::StructInitExpr && node.hasInferredType)
        {
            const CGType type = lvalue_type(ctx, node);
            LLVMValueRef temporary = LLVMBuildAlloca(ctx.builder, type.llvmType, "structliteral");
            LLVMValueRef value = generate_value_for_target(ctx, node, type);
            LLVMBuildStore(ctx.builder, value, temporary);
            return temporary;
        }
        if (node.type == Parser::NodeType::Identifier)
        {
            std::string name = std::string(std::get<std::string_view>(node.value));
            VarInfo *var = ctx.find_var(name);
            if (!var)
                compiler_error("undeclared identifier '" + name + "'");
            if (var->type.isOptional && std::find(ctx.validPayloadAddresses.begin(), ctx.validPayloadAddresses.end(),
                                                  var->address) != ctx.validPayloadAddresses.end())
            {
                return LLVMBuildStructGEP2(ctx.builder, var->type.llvmType, var->address, 1, "optionalpayloadaddr");
            }
            return var->address;
        }
        else if (node.type == Parser::NodeType::MemberAccessExpr)
        {
            if (node.children.empty() || !std::holds_alternative<std::string_view>(node.value))
                compiler_error("malformed member access");

            const CGType baseType = lvalue_type(ctx, node.children[0]);
            std::string structName = baseType.structName;
            LLVMValueRef baseAddr = get_lvalue(ctx, node.children[0], runtimeIndex);

            if (!baseAddr)
                compiler_error("member access requires an addressable struct receiver");

            if (baseType.isPointerLike)
            {
                baseAddr = LLVMBuildLoad2(ctx.builder, baseType.llvmType, baseAddr, "memberbase_load");
            }

            std::string fieldName = std::string(std::get<std::string_view>(node.value));

            if (structTypes.count(structName))
            {
                auto &info = structTypes[structName];
                for (unsigned i = 0; i < info.fieldNames.size(); ++i)
                {
                    if (info.fieldNames[i] == fieldName)
                    {
                        return LLVMBuildStructGEP2(ctx.builder, info.llvmType, baseAddr, i, "memberptr");
                    }
                }
            }
            codegen_error(node, "unknown struct member '" + fieldName + "'");
        }
        else if (node.type == Parser::NodeType::IndexExpr && node.children.size() == 2)
        {
            const auto lvalueReceiver = try_lvalue_type(ctx, node.children[0]);
            const auto pointerReceiver = try_pointer_expression_type(ctx, node.children[0]);
            if (!lvalueReceiver && !pointerReceiver)
                compiler_error("indexing requires an addressable array or a typed pointer expression");
            const CGType receiverType = lvalueReceiver ? *lvalueReceiver : *pointerReceiver;
            LLVMValueRef index = generate_node(ctx, node.children[1]);
            index = cast_value(ctx.builder, index, LLVMInt64TypeInContext(ctx.llvmCtx), false, "arrayindex");

            if (receiverType.isArray && receiverType.innerType && !receiverType.isPointerLike)
            {
                LLVMValueRef receiver = get_lvalue(ctx, node.children[0]);
                LLVMValueRef zero = LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), 0, 0);
                LLVMValueRef indices[] = {zero, index};
                return LLVMBuildGEP2(ctx.builder, receiverType.llvmType, receiver, indices, 2, "static_elementptr");
            }

            if (receiverType.isArray && receiverType.pointeeType)
            {
                LLVMValueRef receiver = get_lvalue(ctx, node.children[0], runtimeIndex);
                LLVMValueRef base = LLVMBuildLoad2(ctx.builder, receiverType.llvmType, receiver, "arraybase");
                if (runtimeIndex && !runtimeIndex->node && !receiverType.runtimeArrayLengthName.empty())
                {
                    runtimeIndex->node = &node.children[1];
                    runtimeIndex->value = index;
                }
                return LLVMBuildGEP2(ctx.builder, receiverType.pointeeType, base, &index, 1, "elementptr");
            }

            if (receiverType.isPointerLike && receiverType.pointeeType)
            {
                LLVMValueRef base = nullptr;
                if (lvalueReceiver)
                {
                    LLVMValueRef receiver = get_lvalue(ctx, node.children[0]);
                    base = LLVMBuildLoad2(ctx.builder, receiverType.llvmType, receiver, "pointerbase");
                }
                else
                    base = generate_node(ctx, node.children[0]);
                return LLVMBuildGEP2(ctx.builder, receiverType.pointeeType, base, &index, 1, "pointerelementptr");
            }

            const auto classInfo = structTypes.find(receiverType.structName);
            if (classInfo == structTypes.end() || classInfo->second.indexedField.empty())
                compiler_error("indexing requires a runtime-sized array or a class with an index field");

            const auto field = std::find(classInfo->second.fieldNames.begin(), classInfo->second.fieldNames.end(),
                                         classInfo->second.indexedField);
            if (field == classInfo->second.fieldNames.end())
                compiler_error("class index field is missing from its layout");

            const size_t fieldIndex = static_cast<size_t>(field - classInfo->second.fieldNames.begin());
            const CGType &backingType = classInfo->second.fieldTypes[fieldIndex];
            if (!backingType.isPointerLike || !backingType.pointeeType)
                compiler_error("class index field must lower to a pointer");

            LLVMValueRef receiver = get_lvalue(ctx, node.children[0]);
            LLVMValueRef fieldAddress =
                LLVMBuildStructGEP2(ctx.builder, classInfo->second.llvmType, receiver, fieldIndex, "indexfield");
            LLVMValueRef base = LLVMBuildLoad2(ctx.builder, backingType.llvmType, fieldAddress, "indexbase");
            return LLVMBuildGEP2(ctx.builder, backingType.pointeeType, base, &index, 1, "elementptr");
        }
        else if (node.type == Parser::NodeType::UnaryExpr)
        {
            Lexer::Operator op = std::get<Lexer::Operator>(node.value);
            if (op == Lexer::Operator::MULTIPLY)
            {
                return generate_node(ctx, node.children[0]);
            }
        }
        else if (node.type == Parser::NodeType::BinaryExpr && node.children.size() == 2 && node.value.index() == 5)
        {
            const Lexer::Operator op = std::get<Lexer::Operator>(node.value);
            if (op == Lexer::Operator::PLUS || op == Lexer::Operator::MINUS)
                return generate_node(ctx, node);
        }
        return nullptr;
    }

    static std::optional<CGType> try_lvalue_type(Context &ctx, const Parser::ASTNode &node)
    {
        if (node.type == Parser::NodeType::Identifier)
        {
            const std::string name = std::string(std::get<std::string_view>(node.value));
            if (VarInfo *var = ctx.find_var(name))
            {
                if (var->type.isOptional && var->type.innerType &&
                    std::find(ctx.validPayloadAddresses.begin(), ctx.validPayloadAddresses.end(), var->address) !=
                        ctx.validPayloadAddresses.end())
                {
                    return *var->type.innerType;
                }
                return var->type;
            }
        }
        if (node.type == Parser::NodeType::StructInitExpr && node.hasInferredType)
        {
            Parser::ASTNode typeNode{Parser::NodeType::CustomType, node.start, node.mod_path, node.source,
                                     std::string_view{},           {}};
            typeNode.inferredTypeName = node.inferredTypeName;
            return resolve_type(ctx, typeNode);
        }
        if (node.type == Parser::NodeType::MemberAccessExpr && !node.children.empty())
        {
            const std::string fieldName = std::string(std::get<std::string_view>(node.value));
            const auto baseType = try_lvalue_type(ctx, node.children[0]);
            if (!baseType)
                return std::nullopt;
            const auto info = structTypes.find(baseType->structName);
            if (info != structTypes.end())
            {
                for (size_t index = 0; index < info->second.fieldNames.size(); ++index)
                {
                    if (info->second.fieldNames[index] == fieldName)
                        return info->second.fieldTypes[index];
                }
            }
        }
        if (node.type == Parser::NodeType::IndexExpr && node.children.size() == 2)
        {
            const auto array = try_lvalue_type(ctx, node.children[0]);
            if (array)
            {
                const auto vector = structTypes.find(array->structName);
                if (is_vector_specialization(array->structName) && vector != structTypes.end() &&
                    !vector->second.fieldTypes.empty() && vector->second.fieldTypes.front().innerType)
                {
                    return *vector->second.fieldTypes.front().innerType;
                }
                if (array->isArray && array->innerType)
                {
                    return *array->innerType;
                }
                if (array->isArray && array->pointeeType)
                {
                    if (array->innerType)
                        return *array->innerType;
                    CGType element = *array;
                    element.llvmType = array->pointeeType;
                    element.pointeeType = nullptr;
                    element.isPointerLike = false;
                    element.isArray = false;
                    return element;
                }
            }
        }
        if (node.type == Parser::NodeType::IndexExpr && node.children.size() == 2)
        {
            const auto pointer = try_pointer_expression_type(ctx, node.children[0]);
            if (pointer && pointer->isPointerLike && pointer->pointeeType)
            {
                if (pointer->innerType)
                    return *pointer->innerType;
                CGType element = *pointer;
                element.llvmType = pointer->pointeeType;
                element.pointeeType = nullptr;
                element.innerType.reset();
                element.isPointerLike = false;
                element.isArray = false;
                return element;
            }
        }
        if (node.type == Parser::NodeType::IndexExpr && node.children.size() == 2)
        {
            const auto receiver = try_lvalue_type(ctx, node.children[0]);
            if (!receiver)
                return std::nullopt;
            const auto classInfo = structTypes.find(receiver->structName);
            if (classInfo != structTypes.end() && !classInfo->second.indexedField.empty())
            {
                const auto field = std::find(classInfo->second.fieldNames.begin(), classInfo->second.fieldNames.end(),
                                             classInfo->second.indexedField);
                if (field != classInfo->second.fieldNames.end())
                {
                    const CGType &backing =
                        classInfo->second.fieldTypes[static_cast<size_t>(field - classInfo->second.fieldNames.begin())];
                    CGType element = backing;
                    element.llvmType = backing.pointeeType;
                    element.pointeeType = nullptr;
                    element.isPointerLike = false;
                    return element;
                }
            }
        }
        if (node.type == Parser::NodeType::UnaryExpr && !node.children.empty() &&
            std::get<Lexer::Operator>(node.value) == Lexer::Operator::MULTIPLY)
        {
            const auto pointer = try_pointer_expression_type(ctx, node.children[0]);
            if (pointer && pointer->isPointerLike && pointer->pointeeType)
            {
                CGType pointee = *pointer;
                pointee.llvmType = pointer->pointeeType;
                pointee.pointeeType = nullptr;
                pointee.innerType.reset();
                pointee.isPointerLike = false;
                return pointee;
            }
        }
        return std::nullopt;
    }

    static std::optional<CGType> try_pointer_expression_type(Context &ctx, const Parser::ASTNode &node)
    {
        if (const auto lvalue = try_lvalue_type(ctx, node); lvalue && lvalue->isPointerLike && lvalue->pointeeType)
            return lvalue;
        if (node.type == Parser::NodeType::CastExpr && node.children.size() == 2)
        {
            const CGType target = resolve_type(ctx, node.children[1]);
            if (target.isPointerLike && target.pointeeType)
                return target;
        }
        if (node.type == Parser::NodeType::CallExpr && !node.children.empty() &&
            node.children.front().type == Parser::NodeType::Identifier)
        {
            const std::string name = std::string(std::get<std::string_view>(node.children.front().value));
            const auto function = functions.find(name);
            if (function != functions.end() && function->second.isC && function->second.cReturnType.isPointerLike &&
                function->second.cReturnType.pointeeType)
                return function->second.cReturnType;
        }
        if (node.type == Parser::NodeType::BinaryExpr && node.children.size() == 2 && node.value.index() == 5)
        {
            const Lexer::Operator op = std::get<Lexer::Operator>(node.value);
            if (op == Lexer::Operator::PLUS || op == Lexer::Operator::MINUS)
                return try_pointer_expression_type(ctx, node.children.front());
        }
        return std::nullopt;
    }

    static CGType lvalue_type(Context &ctx, const Parser::ASTNode &node)
    {
        if (const auto type = try_lvalue_type(ctx, node))
            return *type;
        compiler_error("assignment target has no known type");
    }

    static bool is_signed_expression(Context &ctx, const Parser::ASTNode &node)
    {
        if (node.type == Parser::NodeType::Identifier)
        {
            const std::string name = std::string(std::get<std::string_view>(node.value));
            if (VarInfo *var = ctx.find_var(name))
                return var->type.isSigned;
        }
        if (node.type == Parser::NodeType::IntegerLiteral)
            return false;
        if (node.type == Parser::NodeType::UnaryExpr && node.children.size() == 1)
        {
            const Lexer::Operator op = std::get<Lexer::Operator>(node.value);
            return op == Lexer::Operator::MINUS || is_signed_expression(ctx, node.children.front());
        }
        if (node.type == Parser::NodeType::CastExpr && node.children.size() == 2)
            return resolve_type(ctx, node.children[1]).isSigned;
        if (node.type == Parser::NodeType::BinaryExpr && !node.children.empty())
            return is_signed_expression(ctx, node.children.front());
        if (const auto lvalue = try_lvalue_type(ctx, node))
            return lvalue->isSigned;
        return true;
    }

    static LLVMValueRef generate_binary(Context &ctx, Lexer::Operator op, LLVMValueRef lhs, LLVMValueRef rhs,
                                        bool isSigned)
    {
        if (!lhs || !rhs)
            compiler_error("missing operand in binary expression");

        rhs = cast_value(ctx.builder, rhs, LLVMTypeOf(lhs), isSigned, "binarycast");
        const LLVMTypeKind kind = LLVMGetTypeKind(LLVMTypeOf(lhs));
        const bool isFloat = kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind;
        const bool isPointer = kind == LLVMPointerTypeKind;

        if (isFloat)
        {
            switch (op)
            {
            case Lexer::Operator::PLUS:
                return LLVMBuildFAdd(ctx.builder, lhs, rhs, "faddtmp");
            case Lexer::Operator::MINUS:
                return LLVMBuildFSub(ctx.builder, lhs, rhs, "fsubtmp");
            case Lexer::Operator::MULTIPLY:
                return LLVMBuildFMul(ctx.builder, lhs, rhs, "fmultmp");
            case Lexer::Operator::DIVIDE:
                return LLVMBuildFDiv(ctx.builder, lhs, rhs, "fdivtmp");
            case Lexer::Operator::EQUAL:
                return LLVMBuildFCmp(ctx.builder, LLVMRealOEQ, lhs, rhs, "feqtmp");
            case Lexer::Operator::NOT_EQUAL:
                return LLVMBuildFCmp(ctx.builder, LLVMRealONE, lhs, rhs, "fnetmp");
            case Lexer::Operator::LESS_THAN:
                return LLVMBuildFCmp(ctx.builder, LLVMRealOLT, lhs, rhs, "flttmp");
            case Lexer::Operator::LESS_EQUAL:
                return LLVMBuildFCmp(ctx.builder, LLVMRealOLE, lhs, rhs, "fletmp");
            case Lexer::Operator::GREATER_THAN:
                return LLVMBuildFCmp(ctx.builder, LLVMRealOGT, lhs, rhs, "fgttmp");
            case Lexer::Operator::GREATER_EQUAL:
                return LLVMBuildFCmp(ctx.builder, LLVMRealOGE, lhs, rhs, "fgetmp");
            default:
                compiler_error("unsupported floating-point operator");
            }
        }

        if (isPointer)
        {
            switch (op)
            {
            case Lexer::Operator::EQUAL:
                return LLVMBuildICmp(ctx.builder, LLVMIntEQ, lhs, rhs, "eqtmp");
            case Lexer::Operator::NOT_EQUAL:
                return LLVMBuildICmp(ctx.builder, LLVMIntNE, lhs, rhs, "netmp");
            case Lexer::Operator::LESS_THAN:
                return LLVMBuildICmp(ctx.builder, LLVMIntULT, lhs, rhs, "lttmp");
            case Lexer::Operator::LESS_EQUAL:
                return LLVMBuildICmp(ctx.builder, LLVMIntULE, lhs, rhs, "letmp");
            case Lexer::Operator::GREATER_THAN:
                return LLVMBuildICmp(ctx.builder, LLVMIntUGT, lhs, rhs, "gttmp");
            case Lexer::Operator::GREATER_EQUAL:
                return LLVMBuildICmp(ctx.builder, LLVMIntUGE, lhs, rhs, "getmp");
            default:
                compiler_error("unsupported pointer operator");
            }
        }

        if (kind != LLVMIntegerTypeKind)
            compiler_error("unsupported binary operand type");

        switch (op)
        {
        case Lexer::Operator::PLUS:
            return LLVMBuildAdd(ctx.builder, lhs, rhs, "addtmp");
        case Lexer::Operator::MINUS:
            return LLVMBuildSub(ctx.builder, lhs, rhs, "subtmp");
        case Lexer::Operator::MULTIPLY:
            return LLVMBuildMul(ctx.builder, lhs, rhs, "multmp");
        case Lexer::Operator::DIVIDE:
            return isSigned ? LLVMBuildSDiv(ctx.builder, lhs, rhs, "sdivtmp")
                            : LLVMBuildUDiv(ctx.builder, lhs, rhs, "udivtmp");
        case Lexer::Operator::MODULO:
            return isSigned ? LLVMBuildSRem(ctx.builder, lhs, rhs, "sremtmp")
                            : LLVMBuildURem(ctx.builder, lhs, rhs, "uremtmp");
        case Lexer::Operator::LEFT_SHIFT:
            return LLVMBuildShl(ctx.builder, lhs, rhs, "shltmp");
        case Lexer::Operator::RIGHT_SHIFT:
            return isSigned ? LLVMBuildAShr(ctx.builder, lhs, rhs, "ashrtmp")
                            : LLVMBuildLShr(ctx.builder, lhs, rhs, "lshrtmp");
        case Lexer::Operator::AMPERSAND:
        case Lexer::Operator::AND:
            return LLVMBuildAnd(ctx.builder, lhs, rhs, "andtmp");
        case Lexer::Operator::PIPE:
        case Lexer::Operator::OR:
            return LLVMBuildOr(ctx.builder, lhs, rhs, "ortmp");
        case Lexer::Operator::CARET:
            return LLVMBuildXor(ctx.builder, lhs, rhs, "xortmp");
        case Lexer::Operator::EQUAL:
            return LLVMBuildICmp(ctx.builder, LLVMIntEQ, lhs, rhs, "eqtmp");
        case Lexer::Operator::NOT_EQUAL:
            return LLVMBuildICmp(ctx.builder, LLVMIntNE, lhs, rhs, "netmp");
        case Lexer::Operator::LESS_THAN:
            return LLVMBuildICmp(ctx.builder, isSigned && !isPointer ? LLVMIntSLT : LLVMIntULT, lhs, rhs, "lttmp");
        case Lexer::Operator::LESS_EQUAL:
            return LLVMBuildICmp(ctx.builder, isSigned && !isPointer ? LLVMIntSLE : LLVMIntULE, lhs, rhs, "letmp");
        case Lexer::Operator::GREATER_THAN:
            return LLVMBuildICmp(ctx.builder, isSigned && !isPointer ? LLVMIntSGT : LLVMIntUGT, lhs, rhs, "gttmp");
        case Lexer::Operator::GREATER_EQUAL:
            return LLVMBuildICmp(ctx.builder, isSigned && !isPointer ? LLVMIntSGE : LLVMIntUGE, lhs, rhs, "getmp");
        default:
            compiler_error("unsupported integer operator");
        }
    }

    static bool is_string_aggregate(const CGType &type)
    {
        return std_base_name(type.structName) == "str" || std_base_name(type.structName) == "String";
    }

    static LLVMValueRef generate_string_equality(Context &ctx, LLVMValueRef lhsData, LLVMValueRef lhsLength,
                                                 LLVMValueRef rhsData, LLVMValueRef rhsLength, bool negate)
    {
        LLVMTypeRef i1 = LLVMInt1TypeInContext(ctx.llvmCtx);
        LLVMTypeRef i8 = LLVMInt8TypeInContext(ctx.llvmCtx);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx.llvmCtx);
        LLVMValueRef sameLength = LLVMBuildICmp(ctx.builder, LLVMIntEQ, lhsLength, rhsLength, "stringsamelength");
        LLVMBasicBlockRef entry = LLVMGetInsertBlock(ctx.builder);
        LLVMValueRef function = LLVMGetBasicBlockParent(entry);
        LLVMBasicBlockRef condition = LLVMAppendBasicBlockInContext(ctx.llvmCtx, function, "stringcomparecond");
        LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(ctx.llvmCtx, function, "stringcomparebody");
        LLVMBasicBlockRef result = LLVMAppendBasicBlockInContext(ctx.llvmCtx, function, "stringcompareresult");
        LLVMBuildCondBr(ctx.builder, sameLength, condition, result);

        LLVMPositionBuilderAtEnd(ctx.builder, condition);
        LLVMValueRef offset = LLVMBuildPhi(ctx.builder, i64, "stringcompareoffset");
        LLVMValueRef zero = LLVMConstInt(i64, 0, 0);
        LLVMAddIncoming(offset, &zero, &entry, 1);
        LLVMValueRef hasNext = LLVMBuildICmp(ctx.builder, LLVMIntULT, offset, lhsLength, "stringcomparehasnext");
        LLVMBuildCondBr(ctx.builder, hasNext, body, result);

        LLVMPositionBuilderAtEnd(ctx.builder, body);
        LLVMValueRef lhsByteAddress = LLVMBuildGEP2(ctx.builder, i8, lhsData, &offset, 1, "stringlhsbyteaddress");
        LLVMValueRef rhsByteAddress = LLVMBuildGEP2(ctx.builder, i8, rhsData, &offset, 1, "stringrhsbyteaddress");
        LLVMValueRef lhsByte = LLVMBuildLoad2(ctx.builder, i8, lhsByteAddress, "stringlhsbyte");
        LLVMValueRef rhsByte = LLVMBuildLoad2(ctx.builder, i8, rhsByteAddress, "stringrhsbyte");
        LLVMValueRef sameByte = LLVMBuildICmp(ctx.builder, LLVMIntEQ, lhsByte, rhsByte, "stringsamebyte");
        LLVMValueRef next = LLVMBuildAdd(ctx.builder, offset, LLVMConstInt(i64, 1, 0), "stringcomparenext");
        LLVMAddIncoming(offset, &next, &body, 1);
        LLVMBuildCondBr(ctx.builder, sameByte, condition, result);

        LLVMPositionBuilderAtEnd(ctx.builder, result);
        LLVMValueRef equal = LLVMBuildPhi(ctx.builder, i1, "stringequal");
        LLVMValueRef falseValue = LLVMConstInt(i1, 0, 0);
        LLVMValueRef trueValue = LLVMConstInt(i1, 1, 0);
        LLVMValueRef values[] = {falseValue, trueValue, falseValue};
        LLVMBasicBlockRef blocks[] = {entry, condition, body};
        LLVMAddIncoming(equal, values, blocks, 3);
        return negate ? LLVMBuildXor(ctx.builder, equal, trueValue, "stringnotequal") : equal;
    }

    static bool is_fresh_allocation_expr(const Parser::ASTNode &node)
    {
        if (node.type != Parser::NodeType::CallExpr || node.children.empty() ||
            node.children.front().type != Parser::NodeType::Identifier)
            return false;
        const std::string_view name = std::get<std::string_view>(node.children.front().value);
        return name == "__shaft_alloc" || name == "__shaft_alloc_or_exit";
    }

    static bool type_requires_cleanup(Context &ctx, const CGType &type)
    {
        if (type.isPointerLike)
            return !type.isReference;
        if (type.isOptional)
            return type.innerType && type_requires_cleanup(ctx, *type.innerType);
        if (type.isArray)
            return type.innerType && type_requires_cleanup(ctx, *type.innerType);
        if (type.structName.empty())
            return false;
        const auto definition = structTypes.find(type.structName);
        if (definition == structTypes.end())
            return false;
        for (const CGType &field : definition->second.fieldTypes)
        {
            if (type_requires_cleanup(ctx, field))
                return true;
        }
        return false;
    }

    static uint64_t cleanup_leaf_count(Context &ctx, const CGType &type)
    {
        if (type.isPointerLike)
            return type.isReference ? 0 : 1;
        if (type.isOptional)
            return type.innerType ? cleanup_leaf_count(ctx, *type.innerType) : 0;
        if (type.isArray)
        {
            if (!type.innerType || LLVMGetTypeKind(type.llvmType) != LLVMArrayTypeKind)
                return 0;
            return static_cast<uint64_t>(LLVMGetArrayLength(type.llvmType)) * cleanup_leaf_count(ctx, *type.innerType);
        }
        const auto definition = structTypes.find(type.structName);
        if (definition == structTypes.end())
            return 0;
        uint64_t total = 0;
        for (const CGType &field : definition->second.fieldTypes)
            total += cleanup_leaf_count(ctx, field);
        return total;
    }

    struct CleanupLeaf
    {
        LLVMValueRef address;
        CGType type;
    };

    static void collect_cleanup_leaves(Context &ctx, LLVMValueRef address, const CGType &type,
                                       std::vector<CleanupLeaf> &leaves)
    {
        if (type.isPointerLike)
        {
            if (!type.isReference)
                leaves.push_back({address, type});
            return;
        }
        if (type.isOptional && type.innerType)
        {
            LLVMValueRef payload =
                LLVMBuildStructGEP2(ctx.builder, type.llvmType, address, 1, "runtime_cleanup_optional_payload");
            collect_cleanup_leaves(ctx, payload, *type.innerType, leaves);
            return;
        }
        if (type.isArray && type.innerType && LLVMGetTypeKind(type.llvmType) == LLVMArrayTypeKind)
        {
            const unsigned length = LLVMGetArrayLength(type.llvmType);
            for (unsigned index = 0; index < length; ++index)
            {
                LLVMValueRef indices[] = {
                    LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), 0, 0),
                    LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), index, 0),
                };
                LLVMValueRef element =
                    LLVMBuildGEP2(ctx.builder, type.llvmType, address, indices, 2, "runtime_cleanup_element");
                collect_cleanup_leaves(ctx, element, *type.innerType, leaves);
            }
            return;
        }
        const auto definition = structTypes.find(type.structName);
        if (definition == structTypes.end())
            return;
        for (size_t index = 0; index < definition->second.fieldTypes.size(); ++index)
        {
            LLVMValueRef field = LLVMBuildStructGEP2(ctx.builder, definition->second.llvmType, address,
                                                     static_cast<unsigned>(index), "runtime_cleanup_field");
            collect_cleanup_leaves(ctx, field, definition->second.fieldTypes[index], leaves);
        }
    }

    static CleanupValue create_cleanup_value(Context &ctx, LLVMValueRef address, const CGType &type,
                                             const std::string &name)
    {
        CleanupValue value{address, type, nullptr, nullptr, nullptr, 0, {}};
        if (!type_requires_cleanup(ctx, type))
            return value;

        value.activeFlag =
            LLVMBuildAlloca(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx), (name + ".dropactive").c_str());
        LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), type.isPointerLike ? 0 : 1, 0),
                       value.activeFlag);
        if (type.isPointerLike)
            return value;

        if (type.isOptional && type.innerType)
        {
            LLVMValueRef payload =
                LLVMBuildStructGEP2(ctx.builder, type.llvmType, address, 1, "cleanup_optional_payload");
            value.children.push_back(create_cleanup_value(ctx, payload, *type.innerType, name + ".payload"));
            return value;
        }

        if (type.isArray && type.innerType && LLVMGetTypeKind(type.llvmType) == LLVMArrayTypeKind)
        {
            const unsigned length = LLVMGetArrayLength(type.llvmType);
            value.children.reserve(length);
            for (unsigned index = 0; index < length; ++index)
            {
                LLVMValueRef indices[] = {
                    LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), 0, 0),
                    LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), index, 0),
                };
                LLVMValueRef element =
                    LLVMBuildGEP2(ctx.builder, type.llvmType, address, indices, 2, "cleanup_element");
                value.children.push_back(
                    create_cleanup_value(ctx, element, *type.innerType, name + "." + std::to_string(index)));
            }
            return value;
        }

        const auto definition = structTypes.find(type.structName);
        if (definition == structTypes.end())
            return value;
        value.children.reserve(definition->second.fieldTypes.size());
        for (size_t index = 0; index < definition->second.fieldTypes.size(); ++index)
        {
            LLVMValueRef field = LLVMBuildStructGEP2(ctx.builder, definition->second.llvmType, address,
                                                     static_cast<unsigned>(index), "cleanup_field");
            value.children.push_back(create_cleanup_value(ctx, field, definition->second.fieldTypes[index],
                                                          name + "." + definition->second.fieldNames[index]));
        }
        return value;
    }

    static void collect_cleanup_token_addresses(const CleanupValue &value, std::vector<LLVMValueRef> &tokens)
    {
        if (value.type.isPointerLike)
        {
            if (!value.type.isReference && value.activeFlag)
                tokens.push_back(value.activeFlag);
            return;
        }
        for (const CleanupValue &child : value.children)
            collect_cleanup_token_addresses(child, tokens);
    }

    static void arm_cleanup_value(Context &ctx, CleanupValue &value)
    {
        if (value.activeFlag)
            LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 1, 0), value.activeFlag);
        for (CleanupValue &child : value.children)
            arm_cleanup_value(ctx, child);
    }

    static void initialize_runtime_cleanup_flags(Context &ctx, CleanupValue &value, LLVMValueRef elementCount,
                                                 const std::string &name)
    {
        if (!value.type.innerType)
            compiler_error("runtime array cleanup requires an element type");
        const uint64_t leafCount = cleanup_leaf_count(ctx, *value.type.innerType);
        if (leafCount == 0)
            return;

        const LLVMTypeRef i1 = LLVMInt1TypeInContext(ctx.llvmCtx);
        const LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx.llvmCtx);
        value.runtimeElementCount = elementCount;
        value.runtimeCleanupLeafCount = leafCount;
        LLVMValueRef flagCount =
            LLVMBuildMul(ctx.builder, elementCount, LLVMConstInt(i64, leafCount, 0), "runtimecleanupflagcount");
        value.runtimeLeafActiveFlags =
            LLVMBuildArrayAlloca(ctx.builder, i1, flagCount, (name + ".runtimecleanupflags").c_str());
        LLVMBuildStore(ctx.builder, LLVMConstInt(i1, 1, 0), value.activeFlag);

        LLVMValueRef cursor = LLVMBuildAlloca(ctx.builder, i64, "runtimecleanupinitindex");
        LLVMBuildStore(ctx.builder, LLVMConstInt(i64, 0, 0), cursor);
        LLVMBasicBlockRef condition =
            LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "runtimecleanupinitcond");
        LLVMBasicBlockRef body =
            LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "runtimecleanupinitbody");
        LLVMBasicBlockRef done =
            LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "runtimecleanupinitdone");
        LLVMBuildBr(ctx.builder, condition);
        LLVMPositionBuilderAtEnd(ctx.builder, condition);
        LLVMValueRef index = LLVMBuildLoad2(ctx.builder, i64, cursor, "runtimecleanupinitvalue");
        LLVMValueRef more = LLVMBuildICmp(ctx.builder, LLVMIntULT, index, flagCount, "runtimecleanupinitmore");
        LLVMBuildCondBr(ctx.builder, more, body, done);
        LLVMPositionBuilderAtEnd(ctx.builder, body);
        LLVMValueRef flag =
            LLVMBuildGEP2(ctx.builder, i1, value.runtimeLeafActiveFlags, &index, 1, "runtimecleanupinitflag");
        LLVMBuildStore(ctx.builder, LLVMConstInt(i1, 0, 0), flag);
        LLVMBuildStore(ctx.builder, LLVMBuildAdd(ctx.builder, index, LLVMConstInt(i64, 1, 0), "runtimecleanupinitnext"),
                       cursor);
        LLVMBuildBr(ctx.builder, condition);
        LLVMPositionBuilderAtEnd(ctx.builder, done);
    }

    static void emit_cleanup_for_type(Context &ctx, LLVMValueRef address, const CGType &type)
    {
        if (!address)
            return;
        if (type.isPointerLike)
        {
            if (!type.isReference)
            {
                LLVMTypeRef pointerType = LLVMInt8TypeInContext(ctx.llvmCtx);
                LLVMTypeRef releaseTypes[] = {LLVMPointerType(pointerType, 0)};
                LLVMTypeRef releaseType = LLVMFunctionType(LLVMVoidTypeInContext(ctx.llvmCtx), releaseTypes, 1, 0);
                LLVMValueRef release = LLVMGetNamedFunction(ctx.module, "__shaft_free");
                if (!release)
                    release = LLVMAddFunction(ctx.module, "__shaft_free", releaseType);
                LLVMValueRef value = LLVMBuildLoad2(ctx.builder, type.llvmType, address, "droprawpointer");
                LLVMBuildCall2(ctx.builder, releaseType, release, &value, 1, "");
            }
            return;
        }

        if (type.isArray)
        {
            if (!type.innerType || LLVMGetTypeKind(type.llvmType) != LLVMArrayTypeKind)
                return;
            const unsigned length = LLVMGetArrayLength(type.llvmType);
            for (unsigned index = length; index-- > 0;)
            {
                LLVMValueRef indices[] = {
                    LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), 0, 0),
                    LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), index, 0),
                };
                LLVMValueRef element = LLVMBuildGEP2(ctx.builder, type.llvmType, address, indices, 2, "dropelement");
                emit_cleanup_for_type(ctx, element, *type.innerType);
            }
            return;
        }

        if (type.structName.empty())
            return;
        const auto definition = structTypes.find(type.structName);
        if (definition == structTypes.end())
            return;
        for (size_t index = definition->second.fieldTypes.size(); index-- > 0;)
        {
            LLVMValueRef field = LLVMBuildStructGEP2(ctx.builder, definition->second.llvmType, address,
                                                     static_cast<unsigned>(index), "dropfield");
            emit_cleanup_for_type(ctx, field, definition->second.fieldTypes[index]);
        }
    }

    static void emit_cleanup_leaf_if_active(Context &ctx, LLVMValueRef address, const CGType &type,
                                            LLVMValueRef activeFlag)
    {
        const LLVMTypeRef i1 = LLVMInt1TypeInContext(ctx.llvmCtx);
        LLVMValueRef active = LLVMBuildLoad2(ctx.builder, i1, activeFlag, "runtimeleafactive");
        LLVMBasicBlockRef cleanup =
            LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "runtimeleafcleanup");
        LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "runtimeleafcontinue");
        LLVMBuildCondBr(ctx.builder, active, cleanup, done);
        LLVMPositionBuilderAtEnd(ctx.builder, cleanup);
        emit_cleanup_for_type(ctx, address, type);
        LLVMBuildStore(ctx.builder, LLVMConstInt(i1, 0, 0), activeFlag);
        LLVMBuildBr(ctx.builder, done);
        LLVMPositionBuilderAtEnd(ctx.builder, done);
    }

    static void emit_runtime_array_cleanup(Context &ctx, const CleanupValue &value)
    {
        const LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx.llvmCtx);
        const LLVMTypeRef i1 = LLVMInt1TypeInContext(ctx.llvmCtx);
        LLVMValueRef first =
            LLVMBuildSub(ctx.builder, value.runtimeElementCount, LLVMConstInt(i64, 1, 0), "runtimecleanupfirst");
        LLVMValueRef hasElements = LLVMBuildICmp(ctx.builder, LLVMIntNE, value.runtimeElementCount,
                                                 LLVMConstInt(i64, 0, 0), "runtimecleanuphasitems");
        LLVMBasicBlockRef preheader = LLVMGetInsertBlock(ctx.builder);
        LLVMBasicBlockRef loop = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "runtimecleanuploop");
        LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "runtimecleanupdone");
        LLVMBuildCondBr(ctx.builder, hasElements, loop, done);

        LLVMPositionBuilderAtEnd(ctx.builder, loop);
        LLVMValueRef index = LLVMBuildPhi(ctx.builder, i64, "runtimecleanupindex");
        LLVMAddIncoming(index, &first, &preheader, 1);
        LLVMValueRef base = LLVMBuildLoad2(ctx.builder, value.type.llvmType, value.address, "runtimecleanupbase");
        LLVMValueRef element =
            LLVMBuildGEP2(ctx.builder, value.type.pointeeType, base, &index, 1, "runtimecleanupelement");
        std::vector<CleanupLeaf> leaves;
        collect_cleanup_leaves(ctx, element, *value.type.innerType, leaves);
        if (leaves.size() != value.runtimeCleanupLeafCount)
            compiler_error("runtime array cleanup leaf layout changed after declaration");
        LLVMValueRef offset = LLVMBuildMul(ctx.builder, index, LLVMConstInt(i64, value.runtimeCleanupLeafCount, 0),
                                           "runtimecleanupleafoffset");
        for (size_t leaf = leaves.size(); leaf-- > 0;)
        {
            LLVMValueRef flagIndex = LLVMBuildAdd(
                ctx.builder, offset, LLVMConstInt(i64, static_cast<uint64_t>(leaf), 0), "runtimecleanupleafindex");
            LLVMValueRef flag =
                LLVMBuildGEP2(ctx.builder, i1, value.runtimeLeafActiveFlags, &flagIndex, 1, "runtimecleanupleafflag");
            emit_cleanup_leaf_if_active(ctx, leaves[leaf].address, leaves[leaf].type, flag);
        }
        LLVMValueRef next = LLVMBuildSub(ctx.builder, index, LLVMConstInt(i64, 1, 0), "runtimecleanupnext");
        LLVMValueRef more = LLVMBuildICmp(ctx.builder, LLVMIntNE, index, LLVMConstInt(i64, 0, 0), "runtimecleanupmore");
        LLVMBasicBlockRef loopEnd = LLVMGetInsertBlock(ctx.builder);
        LLVMBuildCondBr(ctx.builder, more, loop, done);
        LLVMAddIncoming(index, &next, &loopEnd, 1);
        LLVMPositionBuilderAtEnd(ctx.builder, done);
    }

    static void emit_cleanup_value(Context &ctx, const CleanupValue &value)
    {
        if (!value.address || !value.activeFlag)
            return;

        LLVMValueRef active =
            LLVMBuildLoad2(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx), value.activeFlag, "dropactive");
        LLVMBasicBlockRef cleanupBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "dropcleanup");
        LLVMBasicBlockRef continueBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "dropcontinue");
        LLVMBuildCondBr(ctx.builder, active, cleanupBB, continueBB);

        LLVMPositionBuilderAtEnd(ctx.builder, cleanupBB);
        if (value.runtimeElementCount && value.runtimeLeafActiveFlags)
            emit_runtime_array_cleanup(ctx, value);
        else if (value.children.empty())
            emit_cleanup_for_type(ctx, value.address, value.type);
        else
            for (auto child = value.children.rbegin(); child != value.children.rend(); ++child)
                emit_cleanup_value(ctx, *child);
        LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 0, 0), value.activeFlag);
        LLVMBuildBr(ctx.builder, continueBB);
        LLVMPositionBuilderAtEnd(ctx.builder, continueBB);
    }

    static void emit_thread_join(Context &ctx, DeferredState &state);

    static void emit_cleanup_scopes(Context &ctx, size_t retainedScopeCount)
    {
        for (size_t scopeIndex = ctx.scopedStateNames.size(); scopeIndex-- > retainedScopeCount;)
        {
            for (const std::string &name : ctx.scopedStateNames[scopeIndex])
            {
                auto state = ctx.states.find(name);
                if (state != ctx.states.end() && state->second.isThread && state->second.started)
                    emit_thread_join(ctx, state->second);
            }
        }
        for (size_t scopeIndex = ctx.cleanupScopes.size(); scopeIndex-- > retainedScopeCount;)
        {
            if (!ctx.cleanupScopeEnabled[scopeIndex])
                continue;
            const auto &values = ctx.cleanupScopes[scopeIndex];
            for (auto value = values.rbegin(); value != values.rend(); ++value)
                emit_cleanup_value(ctx, *value);
        }
    }

    static void emit_current_scope_cleanup(Context &ctx)
    {
        emit_cleanup_scopes(ctx, ctx.cleanupScopes.empty() ? 0 : ctx.cleanupScopes.size() - 1);
    }

    static CleanupValue *find_root_cleanup_value(Context &ctx, LLVMValueRef address)
    {
        for (auto scope = ctx.cleanupScopes.rbegin(); scope != ctx.cleanupScopes.rend(); ++scope)
        {
            for (CleanupValue &value : *scope)
            {
                if (value.address == address)
                    return &value;
            }
        }
        return nullptr;
    }

    struct RuntimeCleanupTarget
    {
        LLVMValueRef activeFlag = nullptr;
        CGType type;
    };

    static bool runtime_cleanup_path_for_lvalue(Context &ctx, const Parser::ASTNode &node,
                                                const RuntimeArrayIndexCapture &capturedIndex, CleanupValue *&root,
                                                CGType &current, uint64_t &leafOrdinal)
    {
        if (node.type == Parser::NodeType::IndexExpr && node.children.size() == 2 &&
            node.children[0].type == Parser::NodeType::Identifier && capturedIndex.node == &node.children[1] &&
            capturedIndex.value)
        {
            const std::string name = std::string(std::get<std::string_view>(node.children[0].value));
            VarInfo *variable = ctx.find_var(name);
            root = variable ? find_root_cleanup_value(ctx, variable->address) : nullptr;
            if (!root || !root->runtimeElementCount || !root->runtimeLeafActiveFlags || !root->type.innerType)
                return false;
            current = *root->type.innerType;
            leafOrdinal = 0;
            return true;
        }
        if (node.type == Parser::NodeType::MemberAccessExpr && !node.children.empty() &&
            runtime_cleanup_path_for_lvalue(ctx, node.children[0], capturedIndex, root, current, leafOrdinal))
        {
            const auto definition = structTypes.find(current.structName);
            if (definition == structTypes.end())
                return false;
            const std::string fieldName = std::string(std::get<std::string_view>(node.value));
            for (size_t index = 0; index < definition->second.fieldNames.size(); ++index)
            {
                if (definition->second.fieldNames[index] == fieldName)
                {
                    for (size_t preceding = 0; preceding < index; ++preceding)
                        leafOrdinal += cleanup_leaf_count(ctx, definition->second.fieldTypes[preceding]);
                    current = definition->second.fieldTypes[index];
                    return true;
                }
            }
            return false;
        }
        if (node.type == Parser::NodeType::IndexExpr && node.children.size() == 2 &&
            runtime_cleanup_path_for_lvalue(ctx, node.children[0], capturedIndex, root, current, leafOrdinal))
        {
            if (!current.isArray || !current.innerType || LLVMGetTypeKind(current.llvmType) != LLVMArrayTypeKind ||
                node.children[1].type != Parser::NodeType::IntegerLiteral)
                return false;
            const uint64_t index = std::get<uint64_t>(node.children[1].value);
            if (index >= LLVMGetArrayLength(current.llvmType))
                return false;
            leafOrdinal += index * cleanup_leaf_count(ctx, *current.innerType);
            current = *current.innerType;
            return true;
        }
        return false;
    }

    static RuntimeCleanupTarget runtime_cleanup_target_for_lvalue(Context &ctx, const Parser::ASTNode &node,
                                                                  const RuntimeArrayIndexCapture &capturedIndex)
    {
        CleanupValue *root = nullptr;
        CGType type;
        uint64_t leafOrdinal = 0;
        if (!runtime_cleanup_path_for_lvalue(ctx, node, capturedIndex, root, type, leafOrdinal) ||
            !type.isPointerLike || type.isReference || cleanup_leaf_count(ctx, type) != 1 ||
            leafOrdinal >= root->runtimeCleanupLeafCount)
            return {};
        const LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx.llvmCtx);
        const LLVMTypeRef i1 = LLVMInt1TypeInContext(ctx.llvmCtx);
        LLVMValueRef offset =
            LLVMBuildMul(ctx.builder, capturedIndex.value, LLVMConstInt(i64, root->runtimeCleanupLeafCount, 0),
                         "runtimeassignmentleafoffset");
        LLVMValueRef flagIndex =
            LLVMBuildAdd(ctx.builder, offset, LLVMConstInt(i64, leafOrdinal, 0), "runtimeassignmentleafindex");
        return {
            LLVMBuildGEP2(ctx.builder, i1, root->runtimeLeafActiveFlags, &flagIndex, 1, "runtimeassignmentleafflag"),
            type};
    }

    static CleanupValue *cleanup_value_for_lvalue(Context &ctx, const Parser::ASTNode &node)
    {
        if (node.type == Parser::NodeType::Identifier)
        {
            const std::string name = std::string(std::get<std::string_view>(node.value));
            VarInfo *variable = ctx.find_var(name);
            return variable ? find_root_cleanup_value(ctx, variable->address) : nullptr;
        }
        if (node.type == Parser::NodeType::MemberAccessExpr && !node.children.empty())
        {
            CleanupValue *parent = cleanup_value_for_lvalue(ctx, node.children.front());
            if (!parent || parent->type.structName.empty())
                return nullptr;
            const auto definition = structTypes.find(parent->type.structName);
            if (definition == structTypes.end())
                return nullptr;
            const std::string fieldName = std::string(std::get<std::string_view>(node.value));
            const auto field =
                std::find(definition->second.fieldNames.begin(), definition->second.fieldNames.end(), fieldName);
            if (field == definition->second.fieldNames.end())
                return nullptr;
            const size_t index = static_cast<size_t>(field - definition->second.fieldNames.begin());
            return index < parent->children.size() ? &parent->children[index] : nullptr;
        }
        if (node.type == Parser::NodeType::IndexExpr && node.children.size() == 2)
        {
            CleanupValue *parent = cleanup_value_for_lvalue(ctx, node.children.front());
            if (!parent || !parent->type.isArray || node.children[1].type != Parser::NodeType::IntegerLiteral)
                return nullptr;
            const uint64_t index = std::get<uint64_t>(node.children[1].value);
            return index < parent->children.size() ? &parent->children[static_cast<size_t>(index)] : nullptr;
        }
        return nullptr;
    }

    static void disarm_cleanup_at_address(Context &ctx, CleanupValue &value, LLVMValueRef address,
                                          const CGType &targetType)
    {
        if (value.activeFlag && value.type.llvmType == targetType.llvmType)
        {
            LLVMValueRef matches = LLVMBuildICmp(ctx.builder, LLVMIntEQ, value.address, address, "moved_cleanup_slot");
            LLVMValueRef active = LLVMBuildLoad2(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx), value.activeFlag,
                                                 "moved_cleanup_active");
            LLVMBuildStore(ctx.builder,
                           LLVMBuildSelect(ctx.builder, matches, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 0, 0),
                                           active, "moved_cleanup_state"),
                           value.activeFlag);
        }
        for (CleanupValue &child : value.children)
            disarm_cleanup_at_address(ctx, child, address, targetType);
    }

    static void disarm_cleanup_value(Context &ctx, CleanupValue &value)
    {
        if (value.activeFlag)
            LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 0, 0), value.activeFlag);
    }

    static LLVMValueRef format_output_runtime_function(Context &ctx, const char *name, LLVMTypeRef result,
                                                       const std::vector<LLVMTypeRef> &parameters)
    {
        LLVMValueRef function = LLVMGetNamedFunction(ctx.module, name);
        if (function)
            return function;
        return LLVMAddFunction(
            ctx.module, name,
            LLVMFunctionType(result, const_cast<LLVMTypeRef *>(parameters.data()), parameters.size(), 0));
    }

    static LLVMValueRef emit_format_bytes(Context &ctx, LLVMValueRef data, LLVMValueRef length)
    {
        LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx.llvmCtx);
        LLVMValueRef write = format_output_runtime_function(ctx, "__sys_write", i64,
                                                            {i64, LLVMPointerTypeInContext(ctx.llvmCtx, 0), i64});
        LLVMValueRef args[] = {LLVMConstInt(i64, 1, 0), data,
                               cast_value(ctx.builder, length, i64, false, "formatlength")};
        return LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(write), write, args, 3, "formatwrite");
    }

    static LLVMValueRef emit_format_literal(Context &ctx, const std::string &text)
    {
        if (text.empty())
            return LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), 0, 0);
        return emit_format_bytes(ctx, LLVMBuildGlobalStringPtr(ctx.builder, text.c_str(), "formattext"),
                                 LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), text.size(), 0));
    }

    static LLVMValueRef emit_format_value(Context &ctx, const Parser::ASTNode &node, const CGType &type)
    {
        LLVMTypeRef i8 = LLVMInt8TypeInContext(ctx.llvmCtx), i64 = LLVMInt64TypeInContext(ctx.llvmCtx);
        LLVMTypeRef pointer = LLVMPointerTypeInContext(ctx.llvmCtx, 0);
        LLVMValueRef value = generate_node(ctx, node);
        if (std_base_name(type.structName) == "str" || std_base_name(type.structName) == "View" ||
            std_base_name(type.structName) == "String")
            return emit_format_bytes(ctx, LLVMBuildExtractValue(ctx.builder, value, 0, "formatdata"),
                                     LLVMBuildExtractValue(ctx.builder, value, 1, "formatlength"));
        if (type.isPointerLike && type.pointeeType && LLVMGetTypeKind(type.pointeeType) == LLVMIntegerTypeKind &&
            LLVMGetIntTypeWidth(type.pointeeType) == 8)
        {
            LLVMValueRef strlen = format_output_runtime_function(ctx, "__cstr_strlen", i64, {pointer});
            LLVMValueRef length =
                LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(strlen), strlen, &value, 1, "cstrlength");
            return emit_format_bytes(ctx, value, length);
        }
        if (type.isBool)
        {
            LLVMValueRef data =
                LLVMBuildSelect(ctx.builder, value, LLVMBuildGlobalStringPtr(ctx.builder, "true", "formattrue"),
                                LLVMBuildGlobalStringPtr(ctx.builder, "false", "formatfalse"), "formatbooldata");
            LLVMValueRef length = LLVMBuildSelect(ctx.builder, value, LLVMConstInt(i64, 4, 0), LLVMConstInt(i64, 5, 0),
                                                  "formatboollength");
            return emit_format_bytes(ctx, data, length);
        }
        if (type.isFloat)
        {
            LLVMValueRef buffer = LLVMBuildArrayAlloca(ctx.builder, i8, LLVMConstInt(i64, 64, 0), "formatbuffer");
            LLVMValueRef convert = format_output_runtime_function(ctx, "__sys_float_to_string", i64,
                                                                  {LLVMDoubleTypeInContext(ctx.llvmCtx), pointer, i64});
            LLVMValueRef args[] = {
                cast_value(ctx.builder, value, LLVMDoubleTypeInContext(ctx.llvmCtx), true, "formatfloat"), buffer,
                LLVMConstInt(i64, 64, 0)};
            return emit_format_bytes(
                ctx, buffer,
                LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(convert), convert, args, 3, "formatconverted"));
        }
        if (type.isChar)
        {
            LLVMValueRef buffer = LLVMBuildAlloca(ctx.builder, i8, "formatchar");
            LLVMBuildStore(ctx.builder, cast_value(ctx.builder, value, i8, false, "formatcharcast"), buffer);
            return emit_format_bytes(ctx, buffer, LLVMConstInt(i64, 1, 0));
        }
        if (LLVMGetTypeKind(type.llvmType) == LLVMIntegerTypeKind)
        {
            LLVMValueRef buffer = LLVMBuildArrayAlloca(ctx.builder, i8, LLVMConstInt(i64, 32, 0), "formatbuffer");
            LLVMValueRef convert = format_output_runtime_function(
                ctx, type.isSigned ? "__sys_int_to_string" : "__sys_uint_to_string", i64, {i64, pointer, i64});
            LLVMValueRef args[] = {cast_value(ctx.builder, value, i64, type.isSigned, "formatinteger"), buffer,
                                   LLVMConstInt(i64, 32, 0)};
            return emit_format_bytes(
                ctx, buffer,
                LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(convert), convert, args, 3, "formatconverted"));
        }
        compiler_error("format! supports only primitives, str, View, String, and cstr!");
    }

    static CGType format_argument_type(Context &ctx, const Parser::ASTNode &node)
    {
        if (const auto type = try_lvalue_type(ctx, node))
            return *type;
        CGType type;
        if (node.type == Parser::NodeType::IntegerLiteral)
        {
            type.llvmType = LLVMInt64TypeInContext(ctx.llvmCtx);
            type.isSigned = false;
            return type;
        }
        if (node.type == Parser::NodeType::FloatLiteral)
        {
            type.llvmType = LLVMDoubleTypeInContext(ctx.llvmCtx);
            type.isFloat = true;
            return type;
        }
        if (node.type == Parser::NodeType::BoolLiteral)
        {
            type.llvmType = LLVMInt1TypeInContext(ctx.llvmCtx);
            type.isBool = true;
            return type;
        }
        if (node.type == Parser::NodeType::CharLiteral)
        {
            type.llvmType = LLVMInt8TypeInContext(ctx.llvmCtx);
            type.isChar = true;
            return type;
        }
        compiler_error("println! supports literals and values with a declared scalar or string type");
    }

    static LLVMValueRef make_format_view(Context &ctx, LLVMTypeRef viewType, LLVMValueRef data, LLVMValueRef length)
    {
        LLVMValueRef view = LLVMConstNull(viewType);
        view = LLVMBuildInsertValue(ctx.builder, view, data, 0, "formatviewdata");
        return LLVMBuildInsertValue(ctx.builder, view, length, 1, "formatviewlength");
    }

    static LLVMValueRef emit_format_view(Context &ctx, const Parser::ASTNode &node, const CGType &viewType)
    {
        const LLVMTypeRef i8 = LLVMInt8TypeInContext(ctx.llvmCtx);
        const LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx.llvmCtx);
        const LLVMTypeRef pointer = LLVMPointerType(i8, 0);
        if (node.type == Parser::NodeType::StringLiteral)
        {
            const std::string text = decode_string_literal(std::get<std::string_view>(node.value));
            return make_format_view(ctx, viewType.llvmType,
                                    LLVMBuildGlobalStringPtr(ctx.builder, text.c_str(), "formatliteral"),
                                    LLVMConstInt(i64, text.size(), 0));
        }

        const CGType type = format_argument_type(ctx, node);
        LLVMValueRef value = generate_node(ctx, node);
        if (!value)
            compiler_error("format! argument did not produce a value");
        if (std_base_name(type.structName) == "str" || std_base_name(type.structName) == "View" ||
            std_base_name(type.structName) == "String")
        {
            return make_format_view(ctx, viewType.llvmType, LLVMBuildExtractValue(ctx.builder, value, 0, "formatdata"),
                                    LLVMBuildExtractValue(ctx.builder, value, 1, "formatlength"));
        }
        if (type.isPointerLike && type.pointeeType && LLVMGetTypeKind(type.pointeeType) == LLVMIntegerTypeKind &&
            LLVMGetIntTypeWidth(type.pointeeType) == 8)
        {
            LLVMValueRef strlen = format_output_runtime_function(ctx, "__cstr_strlen", i64, {pointer});
            LLVMValueRef length =
                LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(strlen), strlen, &value, 1, "formatcstrlength");
            return make_format_view(ctx, viewType.llvmType, value, length);
        }
        if (type.isBool)
        {
            LLVMValueRef data =
                LLVMBuildSelect(ctx.builder, value, LLVMBuildGlobalStringPtr(ctx.builder, "true", "formattrue"),
                                LLVMBuildGlobalStringPtr(ctx.builder, "false", "formatfalse"), "formatbooldata");
            LLVMValueRef length = LLVMBuildSelect(ctx.builder, value, LLVMConstInt(i64, 4, 0), LLVMConstInt(i64, 5, 0),
                                                  "formatboollength");
            return make_format_view(ctx, viewType.llvmType, data, length);
        }
        if (type.isFloat)
        {
            LLVMValueRef buffer = LLVMBuildArrayAlloca(ctx.builder, i8, LLVMConstInt(i64, 64, 0), "formatbuffer");
            LLVMValueRef convert = format_output_runtime_function(ctx, "__sys_float_to_string", i64,
                                                                  {LLVMDoubleTypeInContext(ctx.llvmCtx), pointer, i64});
            LLVMValueRef args[] = {
                cast_value(ctx.builder, value, LLVMDoubleTypeInContext(ctx.llvmCtx), true, "formatfloat"), buffer,
                LLVMConstInt(i64, 64, 0)};
            LLVMValueRef length =
                LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(convert), convert, args, 3, "formatconverted");
            return make_format_view(ctx, viewType.llvmType, buffer, length);
        }
        if (type.isChar)
        {
            LLVMValueRef buffer = LLVMBuildAlloca(ctx.builder, i8, "formatchar");
            LLVMBuildStore(ctx.builder, cast_value(ctx.builder, value, i8, false, "formatcharcast"), buffer);
            return make_format_view(ctx, viewType.llvmType, buffer, LLVMConstInt(i64, 1, 0));
        }
        if (LLVMGetTypeKind(type.llvmType) == LLVMIntegerTypeKind)
        {
            LLVMValueRef buffer = LLVMBuildArrayAlloca(ctx.builder, i8, LLVMConstInt(i64, 32, 0), "formatbuffer");
            LLVMValueRef convert = format_output_runtime_function(
                ctx, type.isSigned ? "__sys_int_to_string" : "__sys_uint_to_string", i64, {i64, pointer, i64});
            LLVMValueRef args[] = {cast_value(ctx.builder, value, i64, type.isSigned, "formatinteger"), buffer,
                                   LLVMConstInt(i64, 32, 0)};
            LLVMValueRef length =
                LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(convert), convert, args, 3, "formatconverted");
            return make_format_view(ctx, viewType.llvmType, buffer, length);
        }
        compiler_error("format! supports only primitives, str, View, String, and cstr!");
    }

    static LLVMValueRef emit_brace_format_output(Context &ctx, const Parser::ASTNode &node, bool append_newline)
    {
        if (!ctx.stdlibEnabled)
            compiler_error("println! requires the standard library");
        if (node.children.size() < 2 || node.children[1].type != Parser::NodeType::StringLiteral)
            compiler_error("println! requires a string-literal format argument");

        const std::string format = decode_string_literal(std::get<std::string_view>(node.children[1].value));
        size_t argument = 2;
        LLVMValueRef total = LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), 0, 0);
        auto add = [&](LLVMValueRef written) { total = LLVMBuildAdd(ctx.builder, total, written, "printlncount"); };
        for (size_t cursor = 0; cursor < format.size();)
        {
            const char byte = format[cursor];
            if (byte == '{')
            {
                if (cursor + 1 < format.size() && format[cursor + 1] == '{')
                {
                    add(emit_format_literal(ctx, "{"));
                    cursor += 2;
                    continue;
                }
                if (cursor + 1 >= format.size() || format[cursor + 1] != '}')
                    compiler_error("println! format has an invalid opening brace");
                if (argument >= node.children.size())
                    compiler_error("println! format has more placeholders than arguments");
                const Parser::ASTNode &value = node.children[argument++];
                if (value.type == Parser::NodeType::StringLiteral)
                    add(emit_format_literal(ctx, decode_string_literal(std::get<std::string_view>(value.value))));
                else
                    add(emit_format_value(ctx, value, format_argument_type(ctx, value)));
                cursor += 2;
                continue;
            }
            if (byte == '}')
            {
                if (cursor + 1 >= format.size() || format[cursor + 1] != '}')
                    compiler_error("println! format has an invalid closing brace");
                add(emit_format_literal(ctx, "}"));
                cursor += 2;
                continue;
            }
            const size_t literalEnd = format.find_first_of("{}", cursor);
            add(emit_format_literal(ctx, format.substr(cursor, literalEnd - cursor)));
            cursor = literalEnd == std::string::npos ? format.size() : literalEnd;
        }
        if (argument != node.children.size())
            compiler_error("println! has more arguments than placeholders");
        if (append_newline)
            add(emit_format_literal(ctx, "\n"));
        return total;
    }

    static void emit_thread_start(Context &ctx, DeferredState &state)
    {
        if (!ctx.stdlibEnabled)
            compiler_error("Thread requires the standard library runtime");
        const std::string target = LLVMGetTarget(ctx.module);
        const bool supportedLinux = target.find("linux") != std::string::npos &&
                                   (target.find("x86_64") == 0 || target.find("aarch64") == 0 ||
                                    target.find("riscv64") == 0);
        const bool supportedDarwin = target.find("darwin") != std::string::npos &&
                                     target.find("unsupported") == std::string::npos &&
                                     (target.find("x86_64") == 0 || target.find("aarch64") == 0);
        const bool supportedWindows = target.find("windows") != std::string::npos &&
                                      (target.find("x86_64") == 0 || target.find("aarch64") == 0);
        if (!supportedLinux && !supportedDarwin && !supportedWindows)
            compiler_error("Thread is unavailable for this target runtime");
        const Parser::ASTNode &call = *state.call;
        if (call.children.empty() || call.children.front().type != Parser::NodeType::Identifier)
            compiler_error("Thread currently requires a non-generic free-function call");
        const std::string functionName = std::string(std::get<std::string_view>(call.children.front().value));
        const auto signature = functions.find(functionName);
        LLVMValueRef worker = LLVMGetNamedFunction(ctx.module, mangle_function_name(functionName).c_str());
        LLVMValueRef allocate = LLVMGetNamedFunction(ctx.module, "__shaft_alloc_or_exit");
        LLVMValueRef create = LLVMGetNamedFunction(ctx.module, "__shaft_thread_create");
        LLVMValueRef exitThread = LLVMGetNamedFunction(ctx.module, "__sys_exit");
        if (signature == functions.end() || !signature->second.tunnelSlotTypes.empty() || !worker || !allocate ||
            !create || !exitThread || !state.threadTid ||
            LLVMGetTypeKind(LLVMGetReturnType(LLVMGlobalGetValueType(worker))) != LLVMVoidTypeKind)
            compiler_error("Thread requires a void Shaft function without tunnel outputs");
        if (call.children.size() - 1 != signature->second.paramTypes.size())
            compiler_error("wrong argument count in Thread call");

        std::vector<LLVMTypeRef> fieldTypes;
        std::vector<LLVMValueRef> argumentValues;
        for (size_t index = 0; index < signature->second.paramTypes.size(); ++index)
        {
            const CGType &parameter = signature->second.paramTypes[index];
            fieldTypes.push_back(parameter.llvmType);
            LLVMValueRef value = generate_node(ctx, call.children[index + 1]);
            argumentValues.push_back(
                cast_value(ctx.builder, value, parameter.llvmType, parameter.isSigned, "threadargcast"));
        }
        LLVMTypeRef captureType = LLVMStructTypeInContext(ctx.llvmCtx, fieldTypes.data(), fieldTypes.size(), 0);
        const std::string suffix = std::to_string(ctx.nextThreadId++);
        LLVMValueRef active =
            LLVMAddGlobal(ctx.module, LLVMInt1TypeInContext(ctx.llvmCtx), ("__shaft_thread_active_" + suffix).c_str());
        LLVMSetInitializer(active, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 0, 0));
        LLVMSetLinkage(active, LLVMInternalLinkage);
        LLVMValueRef alreadyActive =
            LLVMBuildLoad2(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx), active, "threadactive");
        LLVMBasicBlockRef reject = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "thread.reentrant");
        LLVMBasicBlockRef captureStart =
            LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "thread.capture");
        LLVMBuildCondBr(ctx.builder, alreadyActive, reject, captureStart);
        LLVMPositionBuilderAtEnd(ctx.builder, reject);
        LLVMValueRef reentrantStatus = LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), 72, 0);
        LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(exitThread), exitThread, &reentrantStatus, 1, "");
        LLVMBuildUnreachable(ctx.builder);
        LLVMPositionBuilderAtEnd(ctx.builder, captureStart);
        LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 1, 0), active);

        LLVMValueRef capture = LLVMAddGlobal(ctx.module, captureType, ("__shaft_thread_capture_" + suffix).c_str());
        LLVMSetInitializer(capture, LLVMConstNull(captureType));
        LLVMSetLinkage(capture, LLVMInternalLinkage);
        for (unsigned index = 0; index < argumentValues.size(); ++index)
        {
            LLVMValueRef slot = LLVMBuildStructGEP2(ctx.builder, captureType, capture, index, "threadcaptureslot");
            LLVMBuildStore(ctx.builder, argumentValues[index], slot);
        }

        LLVMTypeRef pointerType = LLVMPointerType(LLVMInt8TypeInContext(ctx.llvmCtx), 0);
        LLVMTypeRef entryParameters[] = {pointerType};
        LLVMTypeRef entryType = LLVMFunctionType(pointerType, entryParameters, 1, 0);
        LLVMValueRef entry = LLVMAddFunction(ctx.module, ("__shaft_thread_entry_" + suffix).c_str(), entryType);
        LLVMSetLinkage(entry, LLVMInternalLinkage);
        LLVMBasicBlockRef entryBlock = LLVMAppendBasicBlockInContext(ctx.llvmCtx, entry, "entry");
        LLVMValueRef parentFunction = ctx.currentFunction;
        LLVMBasicBlockRef parentBlock = LLVMGetInsertBlock(ctx.builder);
        LLVMPositionBuilderAtEnd(ctx.builder, entryBlock);
        std::vector<LLVMValueRef> workerArguments;
        LLVMValueRef captureArgument = LLVMGetParam(entry, 0);
        for (unsigned index = 0; index < fieldTypes.size(); ++index)
        {
            LLVMValueRef typedCapture = LLVMBuildBitCast(ctx.builder, captureArgument,
                                                         LLVMPointerType(captureType, 0), "threadcapture");
            LLVMValueRef slot = LLVMBuildStructGEP2(ctx.builder, captureType, typedCapture, index,
                                                    "threadcaptureload");
            workerArguments.push_back(LLVMBuildLoad2(ctx.builder, fieldTypes[index], slot, "threadcapturevalue"));
        }
        LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(worker), worker, workerArguments.data(),
                       workerArguments.size(), "");
        LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 0, 0), active);
        LLVMBuildRet(ctx.builder, LLVMConstNull(pointerType));
        ctx.currentFunction = parentFunction;
        LLVMPositionBuilderAtEnd(ctx.builder, parentBlock);

        LLVMValueRef stack = LLVMConstNull(pointerType);
        if (target.find("linux") != std::string::npos)
        {
            LLVMValueRef stackBytes = LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), 65536, 0);
            stack = LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(allocate), allocate,
                                   &stackBytes, 1, "threadstack");
            LLVMValueRef stackIndex = LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), 1, 0);
            LLVMValueRef stackSlot = LLVMBuildGEP2(ctx.builder, LLVMInt64TypeInContext(ctx.llvmCtx),
                                                   state.threadTid, &stackIndex, 1, "threadstackslot");
            LLVMBuildStore(ctx.builder,
                           LLVMBuildPtrToInt(ctx.builder, stack, LLVMInt64TypeInContext(ctx.llvmCtx),
                                             "threadstackaddress"),
                           stackSlot);
        }
        LLVMValueRef entryPointer = LLVMBuildBitCast(ctx.builder, entry, pointerType, "threadentry");
        LLVMValueRef createArguments[] = {stack, entryPointer, capture, state.threadTid};
        LLVMValueRef createStatus = LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(create), create,
                                                   createArguments, 4, "threadcreate");
        LLVMValueRef creationFailed = LLVMBuildICmp(
            ctx.builder, LLVMIntNE, createStatus, LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), 0, 0),
            "threadcreatefailed");
        LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "thread.create.failed");
        LLVMBasicBlockRef created = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "thread.created");
        LLVMBuildCondBr(ctx.builder, creationFailed, failure, created);
        LLVMPositionBuilderAtEnd(ctx.builder, failure);
        LLVMValueRef failureStatus = LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), 72, 0);
        LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(exitThread), exitThread, &failureStatus, 1, "");
        LLVMBuildUnreachable(ctx.builder);
        LLVMPositionBuilderAtEnd(ctx.builder, created);
        state.started = true;
    }

    static void emit_thread_join(Context &ctx, DeferredState &state)
    {
        LLVMValueRef join = LLVMGetNamedFunction(ctx.module, "__sys_thread_join");
        if (!join || !state.threadTid)
            compiler_error("Thread runtime ABI is unavailable for this target");
        LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(join), join, &state.threadTid, 1, "");
    }

    LLVMValueRef generate_node(Context &ctx, const Parser::ASTNode &node)
    {
        switch (node.type)
        {

        case Parser::NodeType::Module:
        case Parser::NodeType::UsingMacroDecl:
            for (const auto &child : node.children)
                generate_node(ctx, child);
            return nullptr;
        case Parser::NodeType::ExportDecl:
        {
            const bool previousExport = ctx.exportingDeclaration;
            ctx.exportingDeclaration = true;
            for (const auto &child : node.children)
                generate_node(ctx, child);
            ctx.exportingDeclaration = previousExport;
            return nullptr;
        }

        case Parser::NodeType::UsingNamespaceDecl:
            if (node.children.size() != 1)
                codegen_error(node, "using namespace declaration is missing its namespace");
            {
                const std::string imported = scope_access_name(node.children.front());
                if (std::find(ctx.importedNamespaces.begin(), ctx.importedNamespaces.end(), imported) ==
                    ctx.importedNamespaces.end())
                    ctx.importedNamespaces.push_back(imported);
            }
            return nullptr;

        case Parser::NodeType::NamespaceDecl:
        {
            const std::string previous = ctx.currentNamespaceName;
            ctx.currentNamespaceName =
                qualified_declaration_name(ctx, std::string(std::get<std::string_view>(node.value)));
            for (const auto &child : node.children)
                generate_node(ctx, child);
            ctx.currentNamespaceName = previous;
            return nullptr;
        }

        case Parser::NodeType::DoExpr:
        {
            if (node.children.empty())
                compiler_error("do expression is missing its final yield value");
            ctx.push_scope();
            for (size_t index = 0; index + 1 < node.children.size(); ++index)
            {
                if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                    break;
                generate_node(ctx, node.children[index]);
            }
            LLVMValueRef result = nullptr;
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                result = generate_node(ctx, node.children.back());
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                emit_current_scope_cleanup(ctx);
            ctx.pop_scope();
            return result;
        }

        case Parser::NodeType::BlockStmt:
            ctx.push_scope();
            for (const auto &child : node.children)
            {
                if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                    break;
                generate_node(ctx, child);
            }
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
            {
                for (const std::string &name : ctx.scopedStateNames.back())
                {
                    auto state = ctx.states.find(name);
                    if (state != ctx.states.end() && state->second.isThread && state->second.started)
                        emit_thread_join(ctx, state->second);
                }
                emit_current_scope_cleanup(ctx);
            }
            ctx.pop_scope();
            return nullptr;

        case Parser::NodeType::NoCleanupBlock:
        {
            ctx.push_scope(false);
            for (const auto &child : node.children)
            {
                if (ctx.currentFunction && LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                    break;
                generate_node(ctx, child);
            }
            ctx.pop_scope();
            return nullptr;
        }

        case Parser::NodeType::UnsafeBlock:
        case Parser::NodeType::NoCheckBlock:
        {
            const bool unsafe = node.type == Parser::NodeType::UnsafeBlock;
            if (unsafe)
                ++ctx.unsafeDepth;
            else
                ++ctx.noCheckDepth;
            ctx.push_scope();
            for (const auto &child : node.children)
            {
                if (ctx.currentFunction && LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                    break;
                generate_node(ctx, child);
            }
            if (ctx.currentFunction && !LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                emit_current_scope_cleanup(ctx);
            ctx.pop_scope();
            if (unsafe)
                --ctx.unsafeDepth;
            else
                --ctx.noCheckDepth;
            return nullptr;
        }

        case Parser::NodeType::StructDecl:
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::Identifier)
            {
                std::cerr << "Codegen Error: Struct declaration is missing its name.\n";
                return nullptr;
            }
            std::string name =
                qualified_declaration_name(ctx, std::string(std::get<std::string_view>(node.children.front().value)));
            const auto existing = structTypes.find(name);
            if (existing != structTypes.end() && existing->second.layoutMaterialized)
                return nullptr;
            StructInfo info;
            info.requestedAlignment = node.requestedAlignment;
            const auto predeclared = existing;
            info.llvmType = predeclared == structTypes.end() ? LLVMStructCreateNamed(ctx.llvmCtx, name.c_str())
                                                             : predeclared->second.llvmType;

            std::vector<LLVMTypeRef> fieldTypes;

            for (const auto &child : node.children)
            {
                if (child.type == Parser::NodeType::GenericParam)
                {
                    info.genericParameters.push_back(std::string(std::get<std::string_view>(child.value)));
                }
                else if (child.type == Parser::NodeType::VariableDecl)
                {
                    std::string fName = std::string(std::get<std::string_view>(child.value));
                    const Parser::ASTNode &fieldTypeNode = child.children[0];
                    CGType fType = resolve_type(ctx, fieldTypeNode);
                    info.fieldNames.push_back(fName);
                    info.fieldTypeNodes.push_back(fieldTypeNode);
                    info.fieldTypes.push_back(fType);
                    fieldTypes.push_back(fType.llvmType);
                }
                else if (child.type == Parser::NodeType::IndexDecl)
                    info.indexedField = std::string(std::get<std::string_view>(child.value));
                else if (child.type == Parser::NodeType::InitDecl)
                    info.initializedField = std::string(std::get<std::string_view>(child.value));
            }
            LLVMStructSetBody(info.llvmType, fieldTypes.data(), fieldTypes.size(), false);
            info.layoutMaterialized = true;
            structTypes[name] = info;
            if (ctx.materializingAggregateLayouts)
                return nullptr;
            return nullptr;
        }

        case Parser::NodeType::TraitDecl:
            return nullptr;

        case Parser::NodeType::ImplDecl:
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::CustomType)
                codegen_error(node, "impl declaration is missing its target type.");
            std::string targetName = custom_type_name(node.children.front());
            if (!structTypes.count(targetName))
                targetName = qualified_declaration_name(ctx, targetName);
            const std::string enclosingType = ctx.currentClassName;
            ctx.currentClassName = targetName;
            for (const auto &child : node.children)
            {
                if (child.type == Parser::NodeType::FunctionDef || child.type == Parser::NodeType::FunctionDecl)
                    generate_node(ctx, child);
            }
            ctx.currentClassName = enclosingType;
            return nullptr;
        }

        case Parser::NodeType::UnionDecl:
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::Identifier)
                codegen_error(node, "Union declaration is missing its name.");
            const std::string name =
                qualified_declaration_name(ctx, std::string(std::get<std::string_view>(node.children.front().value)));
            const auto existing = structTypes.find(name);
            if (existing != structTypes.end() && existing->second.layoutMaterialized)
                return nullptr;
            StructInfo layout;
            const auto predeclared = existing;
            layout.llvmType = predeclared == structTypes.end() ? LLVMStructCreateNamed(ctx.llvmCtx, name.c_str())
                                                               : predeclared->second.llvmType;
            UnionInfo unionInfo;
            unionInfo.declarationName = name;
            unsigned tag = 0;
            for (const Parser::ASTNode &child : node.children)
            {
                if (child.type == Parser::NodeType::GenericParam)
                {
                    const std::string parameter = std::string(std::get<std::string_view>(child.value));
                    layout.genericParameters.push_back(parameter);
                    unionInfo.genericParameters.push_back(parameter);
                    continue;
                }
                if (child.type != Parser::NodeType::UnionVariant)
                    continue;
                if (child.children.empty() || child.children.front().type != Parser::NodeType::Identifier)
                    codegen_error(child, "Union variant is missing its name.");
                UnionVariantInfo variant;
                variant.tag = tag++;
                const std::string variantName = std::string(std::get<std::string_view>(child.children.front().value));
                for (size_t payloadIndex = 1; payloadIndex < child.children.size(); ++payloadIndex)
                {
                    const Parser::ASTNode &payloadNode = child.children[payloadIndex];
                    variant.payloadTypeNodes.push_back(payloadNode);
                    const CGType payloadType = resolve_type(ctx, payloadNode);
                    variant.payloadTypes.push_back(payloadType);
                }
                if (!unionInfo.variants.emplace(variantName, std::move(variant)).second)
                    codegen_error(child, "Duplicate union variant '" + variantName + "'.");
            }
            materialize_union_layout(ctx, layout, unionInfo);
            layout.layoutMaterialized = true;
            structTypes[name] = std::move(layout);
            unionTypes[name] = std::move(unionInfo);
            return nullptr;
        }

        case Parser::NodeType::UnionMatchStmt:
        {
            const size_t armChildren = node.children.size() > 1 ? node.children.size() - 1 - (node.hasElse ? 1 : 0) : 0;
            if (armChildren == 0 || armChildren % 2 != 0 || node.children.front().type != Parser::NodeType::Identifier)
                codegen_error(node, "Malformed union pattern match.");
            const std::string subjectName = std::string(std::get<std::string_view>(node.children.front().value));
            VarInfo *subjectVar = ctx.find_var(subjectName);
            if (!subjectVar)
                codegen_error(node.children.front(), "Union pattern subject is not declared.");
            const std::string unionName = union_template_name(subjectVar->type.structName);
            if (unionName.empty())
                codegen_error(node.children.front(), "Union pattern subject has no union layout.");
            const auto metadata = unionTypes.find(unionName);
            const auto layout = structTypes.find(subjectVar->type.structName);
            if (metadata == unionTypes.end() || layout == structTypes.end())
                codegen_error(node, "Union pattern layout is unavailable.");
            LLVMValueRef subject =
                LLVMBuildLoad2(ctx.builder, subjectVar->type.llvmType, subjectVar->address, "unionsubject");
            LLVMValueRef tag = LLVMBuildExtractValue(ctx.builder, subject, 0, "uniontag");
            LLVMBasicBlockRef afterBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "unionafter");

            for (size_t armIndex = 1; armIndex <= armChildren; armIndex += 2)
            {
                const Parser::ASTNode &pattern = node.children[armIndex];
                if (pattern.type != Parser::NodeType::UnionPattern)
                    codegen_error(pattern, "Malformed union pattern arm.");
                const std::string &declarationName = metadata->second.declarationName;
                const std::string unqualifiedName = declarationName.substr(declarationName.rfind('.') + 1);
                if (pattern.inferredTypeName != declarationName && pattern.inferredTypeName != unqualifiedName)
                    codegen_error(pattern, "Union pattern does not name the subject's union type.");
                const std::string variantName = std::string(std::get<std::string_view>(pattern.value));
                const auto variant = metadata->second.variants.find(variantName);
                if (variant == metadata->second.variants.end())
                    codegen_error(pattern, "Unknown union pattern variant '" + variantName + "'.");
                if (pattern.children.size() != variant->second.payloadTypes.size())
                    codegen_error(pattern, "Union pattern binding count does not match variant payload.");

                LLVMBasicBlockRef armBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "unionarm");
                const bool lastArm = armIndex == armChildren;
                LLVMBasicBlockRef nextBB =
                    lastArm && !node.hasElse
                        ? afterBB
                        : LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "unionnext");
                LLVMValueRef matches = LLVMBuildICmp(
                    ctx.builder, LLVMIntEQ, tag,
                    LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), variant->second.tag, 0), "unionmatches");
                LLVMBuildCondBr(ctx.builder, matches, armBB, nextBB);

                LLVMPositionBuilderAtEnd(ctx.builder, armBB);
                ctx.push_scope();
                if (!variant->second.payloadTypes.empty())
                {
                    LLVMValueRef storage = LLVMBuildStructGEP2(ctx.builder, subjectVar->type.llvmType,
                                                               subjectVar->address, 2, "unionpayloadaddr");
                    LLVMValueRef typedStorage = LLVMBuildBitCast(
                        ctx.builder, storage, LLVMPointerType(variant->second.payloadType, 0), "unionpayloadcast");
                    LLVMValueRef payload =
                        LLVMBuildLoad2(ctx.builder, variant->second.payloadType, typedStorage, "unionpayload");
                    for (size_t index = 0; index < pattern.children.size(); ++index)
                    {
                        const CGType &fieldType = variant->second.payloadTypes[index];
                        LLVMValueRef address = LLVMBuildAlloca(ctx.builder, fieldType.llvmType, "unionpayloadaddr");
                        LLVMBuildStore(
                            ctx.builder,
                            LLVMBuildExtractValue(ctx.builder, payload, static_cast<unsigned>(index), "unionpayload"),
                            address);
                        ctx.declare_var(std::string(std::get<std::string_view>(pattern.children[index].value)), address,
                                        fieldType);
                    }
                }
                generate_node(ctx, node.children[armIndex + 1]);
                if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                    LLVMBuildBr(ctx.builder, afterBB);
                ctx.pop_scope();
                LLVMPositionBuilderAtEnd(ctx.builder, nextBB);
            }
            if (node.hasElse)
            {
                generate_node(ctx, node.children.back());
                if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                    LLVMBuildBr(ctx.builder, afterBB);
            }
            LLVMPositionBuilderAtEnd(ctx.builder, afterBB);
            return nullptr;
        }

        case Parser::NodeType::EnumDecl:
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::Identifier)
            {
                std::cerr << "Codegen Error: Enum declaration is missing its name.\n";
                return nullptr;
            }
            std::string name =
                qualified_declaration_name(ctx, std::string(std::get<std::string_view>(node.children.front().value)));
            EnumInfo info;
            info.backingType.llvmType = LLVMInt32TypeInContext(ctx.llvmCtx);
            info.backingType.isSigned = true;
            size_t firstMember = 1;
            if (node.children.size() > 1 && (node.children[1].type == Parser::NodeType::PrimitiveType ||
                                             node.children[1].type == Parser::NodeType::CustomType))
            {
                info.backingType = resolve_type(ctx, node.children[1]);
                firstMember = 2;
            }
            int64_t counter = 0;
            for (size_t index = firstMember; index < node.children.size(); ++index)
            {
                const auto &child = node.children[index];
                if (child.type != Parser::NodeType::EnumMember || child.children.empty())
                    continue;
                std::string memberName = std::string(std::get<std::string_view>(child.children.front().value));
                if (child.children.size() > 1)
                    counter = evaluate_enum_constant(child.children[1], info);
                info.members[memberName] = counter++;
            }
            enumTypes[name] = info;
            return nullptr;
        }

        case Parser::NodeType::TypeIfExpr:
        {
            if (node.children.size() != 4 || !std::holds_alternative<bool>(node.value))
                codegen_error(node, "@comptime iftype was not resolved by semantic checking.");
            return generate_node(ctx, node.children[std::get<bool>(node.value) ? 2 : 3]);
        }

        case Parser::NodeType::CompileExpr:
        {
            if (node.children.size() != 1)
                codegen_error(node, "@comptime expression requires exactly one expression.");
            uint64_t value = 0;
            if (!evaluate_comptime_integer_expression(node.children.front(), value))
                codegen_error(node, "@comptime expression currently permits integer literals and +, -, *, /, or %.");
            return LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), value, 0);
        }

        case Parser::NodeType::IntegerLiteral:
            return LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), std::get<uint64_t>(node.value), 0);
        case Parser::NodeType::FloatLiteral:
            return LLVMConstReal(LLVMDoubleTypeInContext(ctx.llvmCtx), std::get<double>(node.value));
        case Parser::NodeType::BoolLiteral:
            return LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), std::get<bool>(node.value) ? 1 : 0, 0);
        case Parser::NodeType::StringLiteral:
        {
            std::string_view sv = std::get<std::string_view>(node.value);
            const std::string text = decode_string_literal(sv);
            return LLVMBuildGlobalStringPtr(ctx.builder, text.c_str(), "strtmp");
        }

        case Parser::NodeType::CharLiteral:
        {
            const std::string_view text = std::get<std::string_view>(node.value);
            if (text.empty())
                compiler_error("empty character literal");
            unsigned char value = static_cast<unsigned char>(text.front());
            if (text.front() == '\\' && text.size() == 2)
            {
                switch (text[1])
                {
                case 'n':
                    value = '\n';
                    break;
                case 'r':
                    value = '\r';
                    break;
                case 't':
                    value = '\t';
                    break;
                case '0':
                    value = '\0';
                    break;
                default:
                    value = static_cast<unsigned char>(text[1]);
                    break;
                }
            }
            return LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), value, 0);
        }

        case Parser::NodeType::CastExpr:
        {
            if (node.children.size() != 2)
                codegen_error(node, "cast requires a source value and target type");
            const LLVMValueRef value = generate_node(ctx, node.children.front());
            const CGType target = resolve_type(ctx, node.children[1]);
            return cast_as_value(ctx, value, target, is_signed_expression(ctx, node.children.front()));
        }

        case Parser::NodeType::Identifier:
        {
            std::string name = std::string(std::get<std::string_view>(node.value));
            VarInfo *var = ctx.find_var(name);
            if (!var)
                compiler_error("undeclared identifier '" + name + "'");
            if (var->type.isOptional)
            {
                if (!var->type.innerType)
                    compiler_error("optional variable has no payload type");
                LLVMValueRef payload =
                    LLVMBuildStructGEP2(ctx.builder, var->type.llvmType, var->address, 1, "optionalpayloadaddr");
                return LLVMBuildLoad2(ctx.builder, var->type.innerType->llvmType, payload, name.c_str());
            }
            return LLVMBuildLoad2(ctx.builder, var->type.llvmType, var->address, name.c_str());
        }

        case Parser::NodeType::VariableDecl:
        {
            std::string name = std::string(std::get<std::string_view>(node.value));
            CGType cgType = resolve_type(ctx, node.children[0]);

            if (!ctx.currentFunction)
            {
                LLVMValueRef global = LLVMAddGlobal(ctx.module, cgType.llvmType, name.c_str());
                LLVMValueRef initializer = LLVMConstNull(cgType.llvmType);
                if (node.children.size() > 1)
                {
                    initializer = node.children[1].type == Parser::NodeType::StructInitExpr
                                      ? generate_global_constant(ctx, node.children[1], cgType)
                                      : generate_node(ctx, node.children[1]);
                    if (!initializer || !LLVMIsConstant(initializer) || LLVMTypeOf(initializer) != cgType.llvmType)
                        compiler_error("global initializer must be a constant with the declared type");
                }
                LLVMSetInitializer(global, initializer);
                ctx.declare_var(name, global, cgType);
                return nullptr;
            }

            LLVMValueRef alloca = LLVMBuildAlloca(ctx.builder, cgType.llvmType, name.c_str());
            LLVMValueRef runtimeArrayCount = nullptr;
            if (cgType.isArray && cgType.isPointerLike && cgType.innerType && !cgType.runtimeArrayLengthName.empty())
            {
                VarInfo *length = ctx.find_var(cgType.runtimeArrayLengthName);
                if (!length)
                    compiler_error("runtime array length binding is not declared");
                LLVMValueRef count =
                    LLVMBuildLoad2(ctx.builder, length->type.llvmType, length->address, "runtimearraycount");
                count = cast_value(ctx.builder, count, LLVMInt64TypeInContext(ctx.llvmCtx), length->type.isSigned,
                                   "runtimearraycountcast");
                LLVMValueRef storage =
                    LLVMBuildArrayAlloca(ctx.builder, cgType.innerType->llvmType, count, "runtimearraystorage");
                LLVMBuildStore(ctx.builder, storage, alloca);
                runtimeArrayCount = count;
            }
            if (cgType.requiredAlignment != 0)
                LLVMSetAlignment(alloca, static_cast<unsigned>(cgType.requiredAlignment));
            const bool stackRuntimeArray =
                cgType.isArray && cgType.isPointerLike && !cgType.runtimeArrayLengthName.empty();
            const bool ownsCleanup = type_requires_cleanup(ctx, cgType);
            if (ownsCleanup)
            {
                if (!stackRuntimeArray)
                    LLVMBuildStore(ctx.builder, LLVMConstNull(cgType.llvmType), alloca);
                CleanupValue cleanup = create_cleanup_value(ctx, alloca, cgType, name);
                if (stackRuntimeArray)
                    initialize_runtime_cleanup_flags(ctx, cleanup, runtimeArrayCount, name);
                if (type_requires_cleanup(ctx, cgType) && node.children.size() > 1 &&
                    is_fresh_allocation_expr(node.children[1]))
                    arm_cleanup_value(ctx, cleanup);
                ctx.declare_owned_var(name, alloca, cgType, std::move(cleanup));
            }
            else
            {
                ctx.declare_var(name, alloca, cgType);
            }

            if (node.children.size() > 1 && node.children[1].type == Parser::NodeType::StructInitExpr)
            {
                const auto classInfo = structTypes.find(cgType.structName);
                if (classInfo == structTypes.end())
                    compiler_error("brace initialization requires a declared struct or class type");
                if (classInfo->second.initializedField.empty())
                {
                    if (node.children[1].children.size() != classInfo->second.fieldTypes.size())
                        compiler_error("brace initializer field count does not match its type");
                    LLVMBuildStore(ctx.builder, LLVMConstNull(cgType.llvmType), alloca);
                    for (size_t index = 0; index < classInfo->second.fieldTypes.size(); ++index)
                    {
                        const CGType &fieldType = classInfo->second.fieldTypes[index];
                        LLVMValueRef fieldAddress =
                            LLVMBuildStructGEP2(ctx.builder, cgType.llvmType, alloca, index, "structinitfield");
                        LLVMValueRef value =
                            generate_value_for_target(ctx, node.children[1].children[index], fieldType);
                        if (!value)
                            compiler_error("brace initializer field did not produce a value");
                        LLVMBuildStore(ctx.builder, materialize_value(ctx, value, fieldType, "structinitcast"),
                                       fieldAddress);
                    }
                    return nullptr;
                }
                const auto field = std::find(classInfo->second.fieldNames.begin(), classInfo->second.fieldNames.end(),
                                             classInfo->second.initializedField);
                if (field == classInfo->second.fieldNames.end())
                    compiler_error("class init field is missing from its layout");
                const size_t fieldIndex = static_cast<size_t>(field - classInfo->second.fieldNames.begin());
                const CGType &backingType = classInfo->second.fieldTypes[fieldIndex];
                if (!backingType.isPointerLike || !backingType.pointeeType)
                    compiler_error("class init field must lower to a pointer");

                LLVMBuildStore(ctx.builder, LLVMConstNull(cgType.llvmType), alloca);
                LLVMValueRef count =
                    LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), node.children[1].children.size(), 0);
                LLVMValueRef byteCount =
                    LLVMBuildMul(ctx.builder, LLVMSizeOf(backingType.pointeeType), count, "initbytes");
                LLVMValueRef alloc = LLVMGetNamedFunction(ctx.module, "__shaft_alloc");
                if (!alloc)
                    compiler_error("class brace initialization requires __shaft_alloc");
                LLVMTypeRef allocArg = LLVMInt64TypeInContext(ctx.llvmCtx);
                LLVMTypeRef allocType =
                    LLVMFunctionType(LLVMPointerType(LLVMInt8TypeInContext(ctx.llvmCtx), 0), &allocArg, 1, 0);
                LLVMValueRef data = LLVMBuildCall2(ctx.builder, allocType, alloc, &byteCount, 1, "initdata");
                LLVMValueRef dataField =
                    LLVMBuildStructGEP2(ctx.builder, cgType.llvmType, alloca, fieldIndex, "initfield");
                LLVMBuildStore(ctx.builder, data, dataField);
                for (size_t index = 0; index < node.children[1].children.size(); ++index)
                {
                    LLVMValueRef elementIndex = LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), index, 0);
                    LLVMValueRef elementAddress =
                        LLVMBuildGEP2(ctx.builder, backingType.pointeeType, data, &elementIndex, 1, "initelement");
                    LLVMValueRef value = generate_node(ctx, node.children[1].children[index]);
                    LLVMBuildStore(ctx.builder,
                                   cast_value(ctx.builder, value, backingType.pointeeType, backingType.isSigned,
                                              "initelementcast"),
                                   elementAddress);
                }
                for (size_t index = 0; index < classInfo->second.fieldNames.size(); ++index)
                {
                    const std::string &fieldName = classInfo->second.fieldNames[index];
                    if (fieldName != "length" && fieldName != "capacity")
                        continue;
                    const CGType &fieldType = classInfo->second.fieldTypes[index];
                    LLVMValueRef metadata =
                        LLVMBuildStructGEP2(ctx.builder, cgType.llvmType, alloca, index, "initmetadata");
                    LLVMBuildStore(
                        ctx.builder,
                        cast_value(ctx.builder, count, fieldType.llvmType, fieldType.isSigned, "initmetadatacast"),
                        metadata);
                }
            }
            else if (cgType.isOptional)
            {
                LLVMBuildStore(ctx.builder, LLVMConstNull(cgType.llvmType), alloca);
            }
            if (node.children.size() > 1 && node.children[1].type != Parser::NodeType::StructInitExpr)
            {
                LLVMValueRef initVal = generate_value_for_target(ctx, node.children[1], cgType);
                if (!initVal)
                    compiler_error("initializer did not produce a value");
                initVal = materialize_value(ctx, initVal, cgType, "initcast");
                LLVMBuildStore(ctx.builder, initVal, alloca);
            }
            return nullptr;
        }

        case Parser::NodeType::AssignmentExpr:
        {
            if (node.children.size() < 2)
                compiler_error("malformed assignment expression");
            RuntimeArrayIndexCapture runtimeIndex;
            LLVMValueRef lval = get_lvalue(ctx, node.children[0], &runtimeIndex);
            const CGType targetType = lvalue_type(ctx, node.children[0]);
            const Lexer::Operator assignment = std::get<Lexer::Operator>(node.value);
            RuntimeCleanupTarget runtimeTarget;
            if (assignment == Lexer::Operator::ASSIGN && type_requires_cleanup(ctx, targetType))
                runtimeTarget = runtime_cleanup_target_for_lvalue(ctx, node.children[0], runtimeIndex);
            const bool rhsIsFreshAllocation = is_fresh_allocation_expr(node.children[1]);
            const bool rhsIsMove = node.children[1].type == Parser::NodeType::MoveExpr;
            if (runtimeTarget.activeFlag && rhsIsMove)
                compiler_error("move into a runtime array cleanup leaf is not supported");
            LLVMValueRef rval = generate_value_for_target(ctx, node.children[1], targetType);
            if (!rval)
                compiler_error("assignment right-hand side did not produce a value");

            rval = materialize_value(ctx, rval, targetType, "assigncast");
            if (assignment != Lexer::Operator::ASSIGN)
            {
                Lexer::Operator binaryOp;
                switch (assignment)
                {
                case Lexer::Operator::ADD_ASSIGN:
                    binaryOp = Lexer::Operator::PLUS;
                    break;
                case Lexer::Operator::SUBTRACT_ASSIGN:
                    binaryOp = Lexer::Operator::MINUS;
                    break;
                case Lexer::Operator::MULTIPLY_ASSIGN:
                    binaryOp = Lexer::Operator::MULTIPLY;
                    break;
                case Lexer::Operator::DIVIDE_ASSIGN:
                    binaryOp = Lexer::Operator::DIVIDE;
                    break;
                case Lexer::Operator::MODULO_ASSIGN:
                    binaryOp = Lexer::Operator::MODULO;
                    break;
                case Lexer::Operator::LEFT_SHIFT_ASSIGN:
                    binaryOp = Lexer::Operator::LEFT_SHIFT;
                    break;
                case Lexer::Operator::RIGHT_SHIFT_ASSIGN:
                    binaryOp = Lexer::Operator::RIGHT_SHIFT;
                    break;
                default:
                    compiler_error("unsupported assignment operator");
                }
                LLVMValueRef current = LLVMBuildLoad2(ctx.builder, targetType.llvmType, lval, "assigntmp");
                rval = generate_binary(ctx, binaryOp, current, rval, targetType.isSigned);
            }
            CleanupValue *targetCleanup = nullptr;
            if (assignment == Lexer::Operator::ASSIGN && type_requires_cleanup(ctx, targetType))
            {
                if (runtimeTarget.activeFlag)
                    emit_cleanup_leaf_if_active(ctx, lval, runtimeTarget.type, runtimeTarget.activeFlag);
                else
                {
                    targetCleanup = cleanup_value_for_lvalue(ctx, node.children[0]);
                    if (targetCleanup && targetCleanup->activeFlag)
                        emit_cleanup_value(ctx, *targetCleanup);
                    else if (!runtimeIndex.value)
                        emit_cleanup_for_type(ctx, lval, targetType);
                }
            }
            LLVMBuildStore(ctx.builder, rval, lval);
            if (runtimeTarget.activeFlag)
            {
                if (rhsIsFreshAllocation)
                    LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 1, 0),
                                   runtimeTarget.activeFlag);
            }
            else if (targetCleanup && targetCleanup->activeFlag)
            {
                if (rhsIsFreshAllocation || rhsIsMove)
                    arm_cleanup_value(ctx, *targetCleanup);
                else if (!targetType.isPointerLike && !targetType.isOptional)
                    LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 1, 0),
                                   targetCleanup->activeFlag);
            }
            return rval;
        }

        case Parser::NodeType::BinaryExpr:
        {
            Lexer::Operator op = std::get<Lexer::Operator>(node.value);
            LLVMValueRef lhs = generate_node(ctx, node.children[0]);
            if (op == Lexer::Operator::AND || op == Lexer::Operator::OR)
            {
                LLVMTypeRef boolType = LLVMInt1TypeInContext(ctx.llvmCtx);
                lhs = cast_value(ctx.builder, lhs, boolType, false, "logical.lhs");
                LLVMBasicBlockRef current = LLVMGetInsertBlock(ctx.builder);
                LLVMValueRef function = LLVMGetBasicBlockParent(current);
                LLVMBasicBlockRef rhsBlock = LLVMAppendBasicBlockInContext(ctx.llvmCtx, function, "logical.rhs");
                LLVMBasicBlockRef mergeBlock = LLVMAppendBasicBlockInContext(ctx.llvmCtx, function, "logical.merge");
                if (op == Lexer::Operator::AND)
                    LLVMBuildCondBr(ctx.builder, lhs, rhsBlock, mergeBlock);
                else
                    LLVMBuildCondBr(ctx.builder, lhs, mergeBlock, rhsBlock);

                LLVMPositionBuilderAtEnd(ctx.builder, rhsBlock);
                LLVMValueRef rhs = generate_node(ctx, node.children[1]);
                rhs = cast_value(ctx.builder, rhs, boolType, false, "logical.rhs");
                LLVMBasicBlockRef rhsEnd = LLVMGetInsertBlock(ctx.builder);
                LLVMBuildBr(ctx.builder, mergeBlock);

                LLVMPositionBuilderAtEnd(ctx.builder, mergeBlock);
                LLVMValueRef phi = LLVMBuildPhi(ctx.builder, boolType, "logical.result");
                LLVMValueRef values[2] = {LLVMConstInt(boolType, op == Lexer::Operator::OR, 0), rhs};
                LLVMBasicBlockRef blocks[2] = {current, rhsEnd};
                LLVMAddIncoming(phi, values, blocks, 2);
                return phi;
            }
            LLVMValueRef rhs = generate_node(ctx, node.children[1]);
            if (op == Lexer::Operator::EQUAL || op == Lexer::Operator::NOT_EQUAL)
            {
                const bool leftLiteral = node.children[0].type == Parser::NodeType::StringLiteral;
                const bool rightLiteral = node.children[1].type == Parser::NodeType::StringLiteral;
                const auto leftType =
                    leftLiteral ? std::optional<CGType>(CGType{}) : try_lvalue_type(ctx, node.children[0]);
                const auto rightType =
                    rightLiteral ? std::optional<CGType>(CGType{}) : try_lvalue_type(ctx, node.children[1]);
                if (leftType && rightType)
                {
                    if ((leftLiteral || is_string_aggregate(*leftType)) &&
                        (rightLiteral || is_string_aggregate(*rightType)))
                    {
                        const auto string_data_and_length = [&](const Parser::ASTNode &operand, LLVMValueRef value,
                                                                const char *dataName, const char *lengthName)
                        {
                            if (operand.type == Parser::NodeType::StringLiteral)
                            {
                                const std::string text =
                                    decode_string_literal(std::get<std::string_view>(operand.value));
                                return std::pair<LLVMValueRef, LLVMValueRef>{
                                    value, LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), text.size(), 0)};
                            }
                            return std::pair<LLVMValueRef, LLVMValueRef>{
                                LLVMBuildExtractValue(ctx.builder, value, 0, dataName),
                                LLVMBuildExtractValue(ctx.builder, value, 1, lengthName)};
                        };
                        const auto left =
                            string_data_and_length(node.children[0], lhs, "stringlhsdata", "stringlhslength");
                        const auto right =
                            string_data_and_length(node.children[1], rhs, "stringrhsdata", "stringrhslength");
                        return generate_string_equality(ctx, left.first, left.second, right.first, right.second,
                                                        op == Lexer::Operator::NOT_EQUAL);
                    }
                }
            }
            if (op == Lexer::Operator::PLUS || op == Lexer::Operator::MINUS)
            {
                if (const auto pointer = try_pointer_expression_type(ctx, node.children[0]))
                {
                    if (pointer->isPointerLike && pointer->pointeeType)
                    {
                        LLVMTypeRef indexType = LLVMInt64TypeInContext(ctx.llvmCtx);
                        rhs = cast_value(ctx.builder, rhs, indexType, is_signed_expression(ctx, node.children[1]),
                                         "pointer_offset");
                        if (op == Lexer::Operator::MINUS)
                            rhs = LLVMBuildNeg(ctx.builder, rhs, "negative_pointer_offset");
                        return LLVMBuildGEP2(ctx.builder, pointer->pointeeType, lhs, &rhs, 1, "pointer_offset");
                    }
                }
                if (LLVMGetTypeKind(LLVMTypeOf(lhs)) == LLVMPointerTypeKind)
                {
                    LLVMTypeRef elementType = LLVMGetElementType(LLVMTypeOf(lhs));
                    if (elementType)
                    {
                        LLVMTypeRef indexType = LLVMInt64TypeInContext(ctx.llvmCtx);
                        rhs = cast_value(ctx.builder, rhs, indexType, is_signed_expression(ctx, node.children[1]),
                                         "pointer_offset");
                        if (op == Lexer::Operator::MINUS)
                            rhs = LLVMBuildNeg(ctx.builder, rhs, "negative_pointer_offset");
                        return LLVMBuildGEP2(ctx.builder, elementType, lhs, &rhs, 1, "pointer_offset");
                    }
                }
            }
            return generate_binary(ctx, op, lhs, rhs, is_signed_expression(ctx, node.children[0]));
        }

        case Parser::NodeType::UnaryExpr:
        {
            Lexer::Operator op = std::get<Lexer::Operator>(node.value);
            if (op == Lexer::Operator::AMPERSAND)
            {
                return get_lvalue(ctx, node.children[0]);
            }
            if (op == Lexer::Operator::MULTIPLY)
            { // Dereference
                LLVMValueRef ptr = generate_node(ctx, node.children[0]);
                const CGType pointee = lvalue_type(ctx, node);
                return LLVMBuildLoad2(ctx.builder, pointee.llvmType, ptr, "deref");
            }
            LLVMValueRef operand = generate_node(ctx, node.children[0]);
            if (op == Lexer::Operator::MINUS)
            {
                const LLVMTypeKind kind = LLVMGetTypeKind(LLVMTypeOf(operand));
                if (kind == LLVMHalfTypeKind || kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind)
                    return LLVMBuildFNeg(ctx.builder, operand, "fnegtmp");
                return LLVMBuildNeg(ctx.builder, operand, "negtmp");
            }
            if (op == Lexer::Operator::EXLAMATION_MARK)
                return LLVMBuildNot(ctx.builder, operand, "nottmp");
            return operand;
        }

        case Parser::NodeType::SizeofExpr:
        {
            if (node.children.size() != 1)
                compiler_error("sizeof requires exactly one type");
            return LLVMSizeOf(resolve_type(ctx, node.children.front()).llvmType);
        }

        case Parser::NodeType::MemberAccessExpr:
        {
            if (node.children.empty())
                compiler_error("malformed member access");
            const CGType memberBase = lvalue_type(ctx, node.children[0]);
            const std::string fieldName = std::string(std::get<std::string_view>(node.value));
            if (memberBase.isArray && fieldName == "length")
            {
                LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx.llvmCtx);
                if (!memberBase.isPointerLike)
                    return LLVMConstInt(i64, memberBase.arrayLength, 0);
                LLVMValueRef count = LLVMGetNamedGlobal(ctx.module, "__shaft_main_argc");
                if (!count)
                    codegen_error(node, "runtime array length is unavailable outside the standard entry bridge");
                return LLVMBuildLoad2(ctx.builder, i64, count, "arraylength");
            }
            LLVMValueRef lval = get_lvalue(ctx, node);
            if (!structTypes.count(memberBase.structName))
                compiler_error("member access requires a known struct type");
            const auto &info = structTypes.at(memberBase.structName);
            for (size_t i = 0; i < info.fieldNames.size(); ++i)
            {
                if (info.fieldNames[i] == fieldName)
                    return LLVMBuildLoad2(ctx.builder, info.fieldTypes[i].llvmType, lval, "memberload");
            }
            compiler_error("unknown struct member '" + fieldName + "'");
        }

        case Parser::NodeType::IndexExpr:
        {
            LLVMValueRef lval = get_lvalue(ctx, node);
            const CGType elementType = lvalue_type(ctx, node);
            return LLVMBuildLoad2(ctx.builder, elementType.llvmType, lval, "elementload");
        }

        case Parser::NodeType::ScopeAccessExpr: // Enum::Value
        {
            if (node.children.empty() || !std::holds_alternative<std::string_view>(node.value))
                compiler_error("malformed scope access");
            std::string enumName = scope_access_name(node.children[0]);
            std::string memberName = std::string(std::get<std::string_view>(node.value));
            if (enumTypes.count(enumName))
            {
                const auto member = enumTypes[enumName].members.find(memberName);
                if (member != enumTypes[enumName].members.end())
                    return LLVMConstInt(enumTypes[enumName].backingType.llvmType, member->second, 0);
            }
            compiler_error("unknown enum member '" + enumName + "::" + memberName + "'");
        }

        case Parser::NodeType::ExprStmt:
            if (!node.children.empty())
                generate_node(ctx, node.children[0]);
            return nullptr;

        case Parser::NodeType::InlineAsmStmt:
        {
            ctx.inlineAssemblyDiagnosticNode = &node;
            const std::string_view assemblySource = std::get<std::string_view>(node.value);
            if (!ctx.currentFunction)
            {
                LLVMAppendModuleInlineAsm(ctx.module, assemblySource.data(), assemblySource.size());
                return nullptr;
            }

            std::vector<LLVMValueRef> inputs;
            std::vector<LLVMTypeRef> inputTypes;
            std::vector<VarInfo *> writableOperands;
            std::unordered_map<std::string, unsigned> operandNumbers;
            std::string constraints;
            for (const auto &operand : node.children)
                if (operand.isMutable)
                {
                    const std::string name = std::string(std::get<std::string_view>(operand.value));
                    VarInfo *variable = ctx.find_var(name);
                    if (!variable)
                        compiler_error("unknown inline assembly operand '" + name + "'");
                    if (!constraints.empty())
                        constraints += ',';
                    constraints += "=r";
                    operandNumbers[name] = static_cast<unsigned>(writableOperands.size());
                    writableOperands.push_back(variable);
                }

            for (const auto &operand : node.children)
            {
                const std::string name = std::string(std::get<std::string_view>(operand.value));
                VarInfo *variable = ctx.find_var(name);
                if (!variable)
                    compiler_error("unknown inline assembly operand '" + name + "'");
                inputTypes.push_back(variable->type.llvmType);
                inputs.push_back(LLVMBuildLoad2(ctx.builder, variable->type.llvmType, variable->address, "asminput"));
                if (!constraints.empty())
                    constraints += ',';
                if (operand.isMutable)
                {
                    constraints += std::to_string(operandNumbers.at(name));
                }
                else
                {
                    operandNumbers[name] = static_cast<unsigned>(writableOperands.size() + inputs.size() - 1);
                    constraints += "r";
                }
            }

            std::string assembly;
            assembly.reserve(assemblySource.size());
            for (size_t index = 0; index < assemblySource.size();)
            {
                if (assemblySource[index] == '$' && index + 1 < assemblySource.size() &&
                    (std::isalpha(static_cast<unsigned char>(assemblySource[index + 1])) ||
                     assemblySource[index + 1] == '_'))
                {
                    size_t end = index + 2;
                    while (
                        end < assemblySource.size() &&
                        (std::isalnum(static_cast<unsigned char>(assemblySource[end])) || assemblySource[end] == '_'))
                        ++end;
                    const std::string name(assemblySource.substr(index + 1, end - index - 1));
                    const auto found = operandNumbers.find(name);
                    if (found != operandNumbers.end())
                    {
                        assembly += "${";
                        assembly += std::to_string(found->second);
                        assembly += '}';
                        index = end;
                        continue;
                    }
                }
                if (assemblySource[index] == '$')
                    assembly += '$';
                assembly += assemblySource[index++];
            }

            std::vector<LLVMTypeRef> outputTypes;
            for (VarInfo *variable : writableOperands)
                outputTypes.push_back(variable->type.llvmType);
            LLVMTypeRef resultType = LLVMVoidTypeInContext(ctx.llvmCtx);
            if (outputTypes.size() == 1)
                resultType = outputTypes.front();
            else if (outputTypes.size() > 1)
                resultType = LLVMStructTypeInContext(ctx.llvmCtx, outputTypes.data(), outputTypes.size(), 0);
            LLVMTypeRef functionType = LLVMFunctionType(resultType, inputTypes.data(), inputTypes.size(), 0);
            LLVMValueRef inlineAssembly =
                LLVMGetInlineAsm(functionType, assembly.data(), assembly.size(), constraints.data(), constraints.size(),
                                 1, 0, LLVMInlineAsmDialectATT, 0);
            LLVMValueRef call =
                LLVMBuildCall2(ctx.builder, functionType, inlineAssembly, inputs.data(), inputs.size(), "");
            for (size_t index = 0; index < writableOperands.size(); ++index)
            {
                LLVMValueRef output =
                    writableOperands.size() == 1
                        ? call
                        : LLVMBuildExtractValue(ctx.builder, call, static_cast<unsigned>(index), "asmoutput");
                LLVMBuildStore(ctx.builder, output, writableOperands[index]->address);
            }
            return call;
        }

        case Parser::NodeType::IfStmt:
        {
            LLVMValueRef cond = generate_node(ctx, node.children[0]);
            cond = LLVMBuildICmp(ctx.builder, LLVMIntNE, cond, LLVMConstNull(LLVMTypeOf(cond)), "ifcond");

            LLVMBasicBlockRef thenBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "then");
            LLVMBasicBlockRef elseBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "else");
            LLVMBasicBlockRef mergeBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "ifcont");

            LLVMBuildCondBr(ctx.builder, cond, thenBB, elseBB);

            LLVMPositionBuilderAtEnd(ctx.builder, thenBB);
            generate_node(ctx, node.children[1]);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                LLVMBuildBr(ctx.builder, mergeBB);

            LLVMPositionBuilderAtEnd(ctx.builder, elseBB);
            if (node.children.size() > 2)
                generate_node(ctx, node.children[2]);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                LLVMBuildBr(ctx.builder, mergeBB);

            LLVMPositionBuilderAtEnd(ctx.builder, mergeBB);
            return nullptr;
        }

        case Parser::NodeType::WhileLoop:
        {
            LLVMBasicBlockRef condBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "whilecond");
            LLVMBasicBlockRef loopBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "whileloop");
            LLVMBasicBlockRef afterBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "whileafter");

            LLVMBuildBr(ctx.builder, condBB);
            LLVMPositionBuilderAtEnd(ctx.builder, condBB);

            LLVMValueRef cond = generate_node(ctx, node.children[0]);
            cond = LLVMBuildICmp(ctx.builder, LLVMIntNE, cond, LLVMConstNull(LLVMTypeOf(cond)), "loopcond");
            LLVMBuildCondBr(ctx.builder, cond, loopBB, afterBB);

            ctx.loopStack.push_back({condBB, afterBB});
            ctx.loopCleanupDepths.push_back(ctx.cleanupScopes.size());

            LLVMPositionBuilderAtEnd(ctx.builder, loopBB);
            generate_node(ctx, node.children[1]);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                LLVMBuildBr(ctx.builder, condBB);

            ctx.loopStack.pop_back();
            ctx.loopCleanupDepths.pop_back();
            LLVMPositionBuilderAtEnd(ctx.builder, afterBB);
            return nullptr;
        }

        case Parser::NodeType::BreakStmt:
            if (!ctx.loopStack.empty())
            {
                emit_cleanup_scopes(ctx, ctx.loopCleanupDepths.back());
                LLVMBuildBr(ctx.builder, ctx.loopStack.back().second);
            }
            return nullptr;

        case Parser::NodeType::ContinueStmt:
            if (!ctx.loopStack.empty())
            {
                emit_cleanup_scopes(ctx, ctx.loopCleanupDepths.back());
                LLVMBuildBr(ctx.builder, ctx.loopStack.back().first);
            }
            return nullptr;

        case Parser::NodeType::ForLoop:
        {
            if (node.children.size() != 4)
                compiler_error("malformed for loop");
            ctx.push_scope();
            generate_node(ctx, node.children[0]);

            LLVMBasicBlockRef condBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "forcond");
            LLVMBasicBlockRef bodyBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "forbody");
            LLVMBasicBlockRef postBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "forpost");
            LLVMBasicBlockRef afterBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "forafter");
            LLVMBuildBr(ctx.builder, condBB);

            LLVMPositionBuilderAtEnd(ctx.builder, condBB);
            LLVMValueRef condition = generate_node(ctx, node.children[1]);
            condition =
                LLVMBuildICmp(ctx.builder, LLVMIntNE, condition, LLVMConstNull(LLVMTypeOf(condition)), "forcondvalue");
            LLVMBuildCondBr(ctx.builder, condition, bodyBB, afterBB);

            ctx.loopStack.push_back({postBB, afterBB});
            ctx.loopCleanupDepths.push_back(ctx.cleanupScopes.size());
            LLVMPositionBuilderAtEnd(ctx.builder, bodyBB);
            generate_node(ctx, node.children[3]);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                LLVMBuildBr(ctx.builder, postBB);

            LLVMPositionBuilderAtEnd(ctx.builder, postBB);
            generate_node(ctx, node.children[2]);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                LLVMBuildBr(ctx.builder, condBB);
            ctx.loopStack.pop_back();
            ctx.loopCleanupDepths.pop_back();

            LLVMPositionBuilderAtEnd(ctx.builder, afterBB);
            emit_current_scope_cleanup(ctx);
            ctx.pop_scope();
            return nullptr;
        }

        case Parser::NodeType::MatchStmt:
        {
            if (node.children.empty())
                compiler_error("malformed match statement");

            LLVMValueRef subject = generate_node(ctx, node.children[0]);
            LLVMBasicBlockRef afterBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "matchafter");
            bool sawDefault = false;

            for (size_t index = 1; index < node.children.size(); ++index)
            {
                const Parser::ASTNode &arm = node.children[index];
                if (arm.type == Parser::NodeType::MatchCase)
                {
                    if (sawDefault || arm.children.size() != 2)
                        compiler_error("malformed match case");
                    LLVMValueRef value = generate_node(ctx, arm.children[0]);
                    LLVMValueRef matches = generate_binary(ctx, Lexer::Operator::EQUAL, subject, value,
                                                           is_signed_expression(ctx, node.children[0]));
                    LLVMBasicBlockRef caseBB =
                        LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "matchcase");
                    LLVMBasicBlockRef nextBB =
                        LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "matchnext");
                    LLVMBuildCondBr(ctx.builder, matches, caseBB, nextBB);

                    LLVMPositionBuilderAtEnd(ctx.builder, caseBB);
                    generate_node(ctx, arm.children[1]);
                    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                        LLVMBuildBr(ctx.builder, afterBB);
                    LLVMPositionBuilderAtEnd(ctx.builder, nextBB);
                }
                else if (arm.type == Parser::NodeType::MatchDefault)
                {
                    if (sawDefault || arm.children.size() != 1 || index + 1 != node.children.size())
                        compiler_error("match default must be the final arm");
                    sawDefault = true;
                    generate_node(ctx, arm.children[0]);
                    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                        LLVMBuildBr(ctx.builder, afterBB);
                }
                else
                {
                    compiler_error("malformed match arm");
                }
            }

            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                LLVMBuildBr(ctx.builder, afterBB);
            LLVMPositionBuilderAtEnd(ctx.builder, afterBB);
            return nullptr;
        }

        case Parser::NodeType::ForeachLoop:
        {
            if (node.children.size() != 3)
                compiler_error("malformed foreach loop");
            const CGType itemType = resolve_type(ctx, node.children[0]);
            if (node.children[1].type == Parser::NodeType::BinaryExpr &&
                std::get<Lexer::Operator>(node.children[1].value) == Lexer::Operator::TRIPPLE_DOT)
            {
                if (node.children[1].children.size() != 2 || !itemType.llvmType ||
                    LLVMGetTypeKind(itemType.llvmType) != LLVMIntegerTypeKind)
                    compiler_error("foreach range requires an integral item type");
                LLVMValueRef start = generate_node(ctx, node.children[1].children[0]);
                LLVMValueRef end = generate_node(ctx, node.children[1].children[1]);
                LLVMTypeRef indexType = LLVMInt64TypeInContext(ctx.llvmCtx);
                start = cast_value(ctx.builder, start, indexType, itemType.isSigned, "rangestart");
                end = cast_value(ctx.builder, end, indexType, itemType.isSigned, "rangeend");
                ctx.push_scope();
                const std::string itemName = std::string(std::get<std::string_view>(node.value));
                LLVMValueRef cursor = LLVMBuildAlloca(ctx.builder, indexType, "rangeindex");
                LLVMBuildStore(ctx.builder, start, cursor);
                LLVMValueRef item = LLVMBuildAlloca(ctx.builder, itemType.llvmType, itemName.c_str());
                ctx.declare_var(itemName, item, itemType);
                LLVMBasicBlockRef condBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "rangecond");
                LLVMBasicBlockRef bodyBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "rangebody");
                LLVMBasicBlockRef nextBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "rangenext");
                LLVMBasicBlockRef afterBB =
                    LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "rangeafter");
                LLVMBuildBr(ctx.builder, condBB);
                LLVMPositionBuilderAtEnd(ctx.builder, condBB);
                LLVMValueRef index = LLVMBuildLoad2(ctx.builder, indexType, cursor, "rangeindexvalue");
                LLVMValueRef inRange =
                    LLVMBuildICmp(ctx.builder, itemType.isSigned ? LLVMIntSLE : LLVMIntULE, index, end, "rangeinrange");
                LLVMBuildCondBr(ctx.builder, inRange, bodyBB, afterBB);
                ctx.loopStack.push_back({nextBB, afterBB});
                ctx.loopCleanupDepths.push_back(ctx.cleanupScopes.size());
                LLVMPositionBuilderAtEnd(ctx.builder, bodyBB);
                LLVMBuildStore(ctx.builder,
                               cast_value(ctx.builder, index, itemType.llvmType, itemType.isSigned, "rangeitem"), item);
                generate_node(ctx, node.children[2]);
                if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                    LLVMBuildBr(ctx.builder, nextBB);
                LLVMPositionBuilderAtEnd(ctx.builder, nextBB);
                LLVMValueRef reachedEnd = LLVMBuildICmp(ctx.builder, LLVMIntEQ, index, end, "rangeatend");
                LLVMBasicBlockRef incrementBB =
                    LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "rangeincrement");
                LLVMBuildCondBr(ctx.builder, reachedEnd, afterBB, incrementBB);
                LLVMPositionBuilderAtEnd(ctx.builder, incrementBB);
                LLVMBuildStore(ctx.builder,
                               LLVMBuildAdd(ctx.builder, index, LLVMConstInt(indexType, 1, 0), "rangenextindex"),
                               cursor);
                LLVMBuildBr(ctx.builder, condBB);
                ctx.loopStack.pop_back();
                ctx.loopCleanupDepths.pop_back();
                LLVMPositionBuilderAtEnd(ctx.builder, afterBB);
                emit_current_scope_cleanup(ctx);
                ctx.pop_scope();
                return nullptr;
            }
            const CGType containerType = lvalue_type(ctx, node.children[1]);
            const bool fixedArray = !containerType.isPointerLike && containerType.arrayLength != 0;
            const bool sizedRuntimeArray = containerType.isPointerLike && !containerType.runtimeArrayLengthName.empty();
            const auto vectorInfo = structTypes.find(containerType.structName);
            const bool vector = is_vector_specialization(containerType.structName) && vectorInfo != structTypes.end() &&
                                vectorInfo->second.fieldTypes.size() > 1 &&
                                vectorInfo->second.fieldTypes.front().innerType;
            CGType elementType;
            if (vector)
                elementType = *vectorInfo->second.fieldTypes.front().innerType;
            else if (containerType.isArray && containerType.innerType)
                elementType = *containerType.innerType;
            else
                compiler_error("foreach requires a fixed-size array, T[length] runtime array, or Vector<T>");
            if ((!fixedArray && !sizedRuntimeArray && !vector) || itemType.llvmType != elementType.llvmType)
                compiler_error(vector ? "foreach item type does not match the Vector element type"
                                      : "foreach item type does not match the array element type");
            LLVMValueRef vectorStorage = vector ? get_lvalue(ctx, node.children[1]) : nullptr;
            ctx.push_scope();
            const std::string itemName = std::string(std::get<std::string_view>(node.value));
            LLVMValueRef cursor = LLVMBuildAlloca(ctx.builder, LLVMInt64TypeInContext(ctx.llvmCtx), "foreachindex");
            LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), 0, 0), cursor);

            LLVMBasicBlockRef condBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "foreachcond");
            LLVMBasicBlockRef bodyBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "foreachbody");
            LLVMBasicBlockRef nextBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "foreachnext");
            LLVMBasicBlockRef afterBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "foreachafter");
            LLVMBuildBr(ctx.builder, condBB);

            LLVMPositionBuilderAtEnd(ctx.builder, condBB);
            LLVMValueRef index =
                LLVMBuildLoad2(ctx.builder, LLVMInt64TypeInContext(ctx.llvmCtx), cursor, "foreachindexvalue");
            LLVMValueRef count = nullptr;
            if (fixedArray)
            {
                count = LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), containerType.arrayLength, 0);
            }
            else if (sizedRuntimeArray)
            {
                VarInfo *length = ctx.find_var(containerType.runtimeArrayLengthName);
                if (!length)
                    compiler_error("foreach runtime array length is not in scope");
                LLVMValueRef lengthValue =
                    LLVMBuildLoad2(ctx.builder, length->type.llvmType, length->address, "foreachlength");
                count = cast_value(ctx.builder, lengthValue, LLVMInt64TypeInContext(ctx.llvmCtx), false,
                                   "foreachlengthcast");
            }
            else
            {
                const CGType &lengthType = vectorInfo->second.fieldTypes[1];
                LLVMValueRef lengthAddress =
                    LLVMBuildStructGEP2(ctx.builder, containerType.llvmType, vectorStorage, 1, "vectorlength");
                LLVMValueRef lengthValue =
                    LLVMBuildLoad2(ctx.builder, lengthType.llvmType, lengthAddress, "vectorlengthvalue");
                count = cast_value(ctx.builder, lengthValue, LLVMInt64TypeInContext(ctx.llvmCtx), false,
                                   "vectorlengthcast");
            }
            LLVMValueRef inRange = LLVMBuildICmp(ctx.builder, LLVMIntULT, index, count, "foreachinrange");
            LLVMBuildCondBr(ctx.builder, inRange, bodyBB, afterBB);

            ctx.loopStack.push_back({nextBB, afterBB});
            ctx.loopCleanupDepths.push_back(ctx.cleanupScopes.size());
            LLVMPositionBuilderAtEnd(ctx.builder, bodyBB);
            LLVMValueRef container = get_lvalue(ctx, node.children[1]);
            LLVMValueRef element = nullptr;
            if (fixedArray)
            {
                LLVMValueRef zero = LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), 0, 0);
                LLVMValueRef indices[] = {zero, index};
                element = LLVMBuildGEP2(ctx.builder, containerType.llvmType, container, indices, 2, "foreachelement");
            }
            else if (sizedRuntimeArray)
            {
                LLVMValueRef base = LLVMBuildLoad2(ctx.builder, containerType.llvmType, container, "foreachbase");
                element = LLVMBuildGEP2(ctx.builder, containerType.pointeeType, base, &index, 1, "foreachelement");
            }
            else
            {
                const CGType &dataType = vectorInfo->second.fieldTypes.front();
                LLVMValueRef dataAddress =
                    LLVMBuildStructGEP2(ctx.builder, containerType.llvmType, vectorStorage, 0, "vectordata");
                LLVMValueRef base = LLVMBuildLoad2(ctx.builder, dataType.llvmType, dataAddress, "vectordatavalue");
                element = LLVMBuildGEP2(ctx.builder, elementType.llvmType, base, &index, 1, "foreachelement");
            }
            ctx.declare_var(itemName, element, itemType);
            generate_node(ctx, node.children[2]);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                LLVMBuildBr(ctx.builder, nextBB);

            LLVMPositionBuilderAtEnd(ctx.builder, nextBB);
            LLVMValueRef nextIndex = LLVMBuildAdd(
                ctx.builder, index, LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), 1, 0), "foreachnextindex");
            LLVMBuildStore(ctx.builder, nextIndex, cursor);
            LLVMBuildBr(ctx.builder, condBB);
            ctx.loopStack.pop_back();
            ctx.loopCleanupDepths.pop_back();

            LLVMPositionBuilderAtEnd(ctx.builder, afterBB);
            emit_current_scope_cleanup(ctx);
            ctx.pop_scope();
            return nullptr;
        }

        case Parser::NodeType::StructInitExpr:
        {
            if (!node.hasInferredType || node.inferredTypeName.empty())
                codegen_error(node, "brace initializer requires an assignment or declaration target");
            Parser::ASTNode typeNode{Parser::NodeType::CustomType, node.start, node.mod_path, node.source,
                                     std::string_view{},           {}};
            typeNode.inferredTypeName = node.inferredTypeName;
            const CGType type = resolve_type(ctx, typeNode);
            return generate_value_for_target(ctx, node, type);
        }

        case Parser::NodeType::CFunctionDecl:
        case Parser::NodeType::CFunctionDef:
        {
            std::string funcName = std::string(std::get<std::string_view>(node.children[0].value));
            if (funcName == "__shaft_entry" && node.type == Parser::NodeType::CFunctionDef &&
                !ctx.generatingDeferredEntry)
            {
                ctx.deferredEntryDefinition = &node;
                return nullptr;
            }
            std::string mangledName = mangle_function_name(funcName);
            bool isCFunc = node.type == Parser::NodeType::CFunctionDef || node.type == Parser::NodeType::CFunctionDecl;

            std::vector<LLVMTypeRef> paramTypes;
            for (const auto &child : node.children)
            {
                if (child.type == Parser::NodeType::Param)
                {
                    paramTypes.push_back(resolve_type(ctx, child.children[0]).llvmType);
                }
            }

            LLVMTypeRef retType = LLVMVoidTypeInContext(ctx.llvmCtx);
            CGType returnCGType;
            returnCGType.llvmType = retType;
            if (isCFunc)
            {
                for (const auto &child : node.children)
                {
                    if (child.type == Parser::NodeType::PrimitiveType || child.type == Parser::NodeType::PointerType ||
                        child.type == Parser::NodeType::ReferenceType || child.type == Parser::NodeType::CustomType)
                    {
                        returnCGType = resolve_type(ctx, child);
                        retType = returnCGType.llvmType;
                        break;
                    }
                }
            }
            LLVMTypeRef funcType = LLVMFunctionType(retType, paramTypes.data(), paramTypes.size(), 0);

            LLVMValueRef func = LLVMGetNamedFunction(ctx.module, mangledName.c_str());
            if (!func)
                func = LLVMAddFunction(ctx.module, mangledName.c_str(), funcType);

            if (node.isNaked)
            {
                const unsigned nakedKind = LLVMGetEnumAttributeKindForName("naked", 5);
                const unsigned noInlineKind = LLVMGetEnumAttributeKindForName("noinline", 8);
                if (!nakedKind)
                    compiler_error("LLVM does not support the naked function attribute");
                if (!noInlineKind)
                    compiler_error("LLVM does not support the noinline function attribute");
                LLVMAddAttributeAtIndex(func, LLVMAttributeFunctionIndex,
                                        LLVMCreateEnumAttribute(ctx.llvmCtx, nakedKind, 0));
                LLVMAddAttributeAtIndex(func, LLVMAttributeFunctionIndex,
                                        LLVMCreateEnumAttribute(ctx.llvmCtx, noInlineKind, 0));
            }

            // `cdec __wasi_<name>` are only understood by the WebAssembly backend
            if (node.type == Parser::NodeType::CFunctionDecl && funcName.rfind("__wasi_", 0) == 0)
            {
                const std::string importName = funcName.substr(7);
                const char *importModule = "wasi_snapshot_preview1";
                LLVMAddAttributeAtIndex(func, LLVMAttributeFunctionIndex,
                                        LLVMCreateStringAttribute(ctx.llvmCtx, "wasm-import-module", 18, importModule,
                                                                  static_cast<unsigned>(strlen(importModule))));
                LLVMAddAttributeAtIndex(func, LLVMAttributeFunctionIndex,
                                        LLVMCreateStringAttribute(ctx.llvmCtx, "wasm-import-name", 16,
                                                                  importName.c_str(),
                                                                  static_cast<unsigned>(importName.size())));
            }

            FunctionSignature signature;
            signature.llvmName = mangledName;
            signature.isC = true;
            signature.llvmFunctionType = funcType;
            signature.llvmFunction = func;
            signature.cReturnType = returnCGType;
            signature.cReturnsVoid = retType == LLVMVoidTypeInContext(ctx.llvmCtx);
            for (const auto &child : node.children)
            {
                if (child.type == Parser::NodeType::Param)
                {
                    signature.paramNames.push_back(std::string(std::get<std::string_view>(child.value)));
                    signature.paramTypes.push_back(resolve_type(ctx, child.children.front()));
                }
            }
            functions[funcName] = std::move(signature);

            if (node.type == Parser::NodeType::CFunctionDecl)
                return func;
            if (LLVMGetFirstBasicBlock(func))
                compiler_error("duplicate C function definition for '" + funcName + "'");

            ctx.currentFunction = func;
            ctx.isCFunction = isCFunc;
            ctx.currentReturnType = returnCGType;

            LLVMBasicBlockRef entryBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, func, "entry");
            LLVMPositionBuilderAtEnd(ctx.builder, entryBB);
            ctx.push_scope();

            unsigned argIdx = 0;
            for (const auto &child : node.children)
            {
                if (child.type == Parser::NodeType::Param)
                {
                    std::string argName = std::string(std::get<std::string_view>(child.value));
                    LLVMValueRef argVal = LLVMGetParam(func, argIdx++);
                    CGType pType = resolve_type(ctx, child.children[0]);

                    LLVMValueRef alloca = LLVMBuildAlloca(ctx.builder, pType.llvmType, argName.c_str());
                    LLVMBuildStore(ctx.builder, argVal, alloca);
                    ctx.declare_var(argName, alloca, pType);
                }
            }

            generate_node(ctx, node.children.back()); // Body

            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
            {
                if (LLVMGetTypeKind(retType) == LLVMVoidTypeKind)
                    LLVMBuildRetVoid(ctx.builder);
                else
                    LLVMBuildRet(ctx.builder, LLVMConstNull(retType));
            }

            ctx.pop_scope();
            ctx.currentFunction = nullptr;
            return func;
        }

        case Parser::NodeType::FunctionDecl:
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::Identifier)
                compiler_error("function declaration is missing its name");
            const std::string declaredName = std::string(std::get<std::string_view>(node.children.front().value));
            const std::string name =
                !ctx.currentClassName.empty()
                    ? ctx.currentClassName + "." + declaredName
                    : (!ctx.currentNamespaceName.empty() ? ctx.currentNamespaceName + "." + declaredName
                                                         : declaredName);
            std::vector<LLVMTypeRef> argumentTypes;
            FunctionSignature signature;
            signature.llvmName = mangle_function_name(name);
            for (const auto &child : node.children)
            {
                if (child.type == Parser::NodeType::Param)
                {
                    CGType type = resolve_type(ctx, child.children.front());
                    signature.paramNames.push_back(std::string(std::get<std::string_view>(child.value)));
                    signature.paramTypes.push_back(type);
                    argumentTypes.push_back(type.llvmType);
                }
                else if (child.type == Parser::NodeType::TunnelSlot)
                {
                    CGType type = resolve_type(ctx, child.children.front());
                    signature.tunnelSlotTypes.push_back(type);
                    signature.tunnelSlotNodes.push_back(child);
                    signature.tunnelSlotOptional.push_back(child.isOptional);
                    argumentTypes.push_back(LLVMPointerType(LLVMInt8TypeInContext(ctx.llvmCtx), 0));
                    if (child.isOptional)
                        argumentTypes.push_back(LLVMPointerType(LLVMInt8TypeInContext(ctx.llvmCtx), 0));
                    const uint64_t cleanupLeaves = cleanup_leaf_count(ctx, type);
                    signature.tunnelSlotCleanupLeafCounts.push_back(cleanupLeaves);
                    for (uint64_t leaf = 0; leaf < cleanupLeaves; ++leaf)
                        argumentTypes.push_back(LLVMPointerType(LLVMInt1TypeInContext(ctx.llvmCtx), 0));
                }
                else if (child.type == Parser::NodeType::GenericParam)
                {
                    compiler_error("generic function declarations require a definition");
                }
            }
            signature.llvmFunctionType =
                LLVMFunctionType(LLVMVoidTypeInContext(ctx.llvmCtx), argumentTypes.data(), argumentTypes.size(), 0);
            signature.llvmFunction = LLVMGetNamedFunction(ctx.module, signature.llvmName.c_str());
            if (!signature.llvmFunction)
                signature.llvmFunction =
                    LLVMAddFunction(ctx.module, signature.llvmName.c_str(), signature.llvmFunctionType);
            functions[name] = std::move(signature);
            return functions.at(name).llvmFunction;
        }

        case Parser::NodeType::FunctionDef:
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::Identifier)
                compiler_error("function definition is missing its name");

            const std::string declaredName = std::string(std::get<std::string_view>(node.children.front().value));
            const std::string baseName =
                !ctx.currentClassName.empty()
                    ? ctx.currentClassName + "." + declaredName
                    : (!ctx.currentNamespaceName.empty() ? ctx.currentNamespaceName + "." + declaredName
                                                         : declaredName);
            if (ctx.functionSpecializationName.empty())
            {
                for (const auto &child : node.children)
                {
                    if (child.type == Parser::NodeType::GenericParam)
                    {
                        genericFunctionTemplates[baseName] = &node;
                        genericFunctionTemplateClasses[baseName] = ctx.currentClassName;
                        return nullptr;
                    }
                }
            }
            const std::string name = ctx.functionSpecializationName.empty() ? baseName : ctx.functionSpecializationName;
            const std::string llvmName = mangle_function_name(name);
            validate_normal_main_signature(node, declaredName);
            std::vector<LLVMTypeRef> argumentTypes;
            std::vector<std::pair<std::string, CGType>> parameters;
            std::vector<std::pair<std::string, CGType>> slots;
            std::vector<Parser::ASTNode> slotNodes;
            std::vector<bool> slotOptionals;
            std::vector<uint64_t> slotCleanupLeafCounts;
            const Parser::ASTNode *body = nullptr;
            for (const auto &child : node.children)
            {
                if (child.type == Parser::NodeType::Param)
                {
                    CGType type = resolve_type(ctx, child.children.front());
                    parameters.emplace_back(std::string(std::get<std::string_view>(child.value)), type);
                    argumentTypes.push_back(type.llvmType);
                }
                else if (child.type == Parser::NodeType::TunnelSlot)
                {
                    CGType type = child.children.empty() ? CGType{} : resolve_type(ctx, child.children.front());
                    slots.emplace_back(std::string(std::get<std::string_view>(child.value)), type);
                    slotNodes.push_back(child);
                    slotOptionals.push_back(child.isOptional);
                    argumentTypes.push_back(LLVMPointerType(LLVMInt8TypeInContext(ctx.llvmCtx), 0));
                    if (child.isOptional)
                        argumentTypes.push_back(LLVMPointerType(LLVMInt8TypeInContext(ctx.llvmCtx), 0));
                    const uint64_t cleanupLeaves = cleanup_leaf_count(ctx, type);
                    slotCleanupLeafCounts.push_back(cleanupLeaves);
                    for (uint64_t leaf = 0; leaf < cleanupLeaves; ++leaf)
                        argumentTypes.push_back(LLVMPointerType(LLVMInt1TypeInContext(ctx.llvmCtx), 0));
                }
                else if (child.type == Parser::NodeType::BlockStmt)
                    body = &child;
            }
            if (!body)
                compiler_error("function definition is missing its body");

            LLVMTypeRef functionType =
                LLVMFunctionType(LLVMVoidTypeInContext(ctx.llvmCtx), argumentTypes.data(), argumentTypes.size(), 0);
            LLVMValueRef function = LLVMGetNamedFunction(ctx.module, llvmName.c_str());
            if (!function && !node.isComptime)
                function = LLVMAddFunction(ctx.module, llvmName.c_str(), functionType);
            if (function)
            {
                LLVMSetLinkage(function, (node.isExported || ctx.exportingDeclaration || declaredName == "main")
                                             ? LLVMExternalLinkage
                                             : LLVMPrivateLinkage);
                if (LLVMGetFirstBasicBlock(function))
                    compiler_error("duplicate function definition for '" + name + "'");
            }

            FunctionSignature signature;
            signature.llvmName = llvmName;
            signature.llvmFunctionType = functionType;
            signature.llvmFunction = function;
            signature.comptimeDefinition = node.isComptime ? &node : nullptr;
            for (const auto &[paramName, paramType] : parameters)
            {
                signature.paramNames.push_back(paramName);
                signature.paramTypes.push_back(paramType);
            }
            for (size_t index = 0; index < slots.size(); ++index)
            {
                signature.tunnelSlotTypes.push_back(slots[index].second);
                signature.tunnelSlotNodes.push_back(slotNodes[index]);
                signature.tunnelSlotOptional.push_back(slotOptionals[index]);
                signature.tunnelSlotCleanupLeafCounts.push_back(slotCleanupLeafCounts[index]);
            }
            functions[name] = std::move(signature);
            if (node.isComptime && !ctx.predeclaringFunctionSignatures)
                return function;
            if (ctx.predeclaringFunctionSignatures)
                return function;

            ctx.currentFunction = function;
            ctx.isCFunction = false;
            ctx.tunnelSlots.clear();
            LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx.llvmCtx, function, "entry");
            LLVMPositionBuilderAtEnd(ctx.builder, entry);
            ctx.push_scope();

            unsigned argument = 0;
            for (const auto &[paramName, paramType] : parameters)
            {
                LLVMValueRef value = LLVMGetParam(function, argument++);
                LLVMValueRef address = LLVMBuildAlloca(ctx.builder, paramType.llvmType, paramName.c_str());
                LLVMBuildStore(ctx.builder, value, address);
                ctx.declare_var(paramName, address, paramType);
            }
            for (size_t index = 0; index < slots.size(); ++index)
            {
                const auto &[slotName, slotType] = slots[index];
                const unsigned outputIndex = argument++;
                const unsigned flagIndex = slotOptionals[index] ? argument++ : 0;
                const unsigned cleanupArgIndex = argument;
                const uint64_t cleanupLeafCount = slotCleanupLeafCounts[index];
                argument += static_cast<unsigned>(cleanupLeafCount);
                ctx.tunnelSlots[slotName] = {outputIndex, slotType,        slotOptionals[index],
                                             flagIndex,   cleanupArgIndex, cleanupLeafCount};
                if (slotOptionals[index])
                    LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 0, 0),
                                   LLVMGetParam(function, flagIndex));
            }

            generate_node(ctx, *body);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
            {
                emit_current_scope_cleanup(ctx);
                LLVMBuildRetVoid(ctx.builder);
            }
            ctx.pop_scope();
            ctx.tunnelSlots.clear();
            ctx.currentFunction = nullptr;
            return function;
        }

        case Parser::NodeType::TerminateStmt:
        {
            LLVMTypeRef functionType = LLVMGlobalGetValueType(ctx.currentFunction);
            if (LLVMGetTypeKind(LLVMGetReturnType(functionType)) != LLVMVoidTypeKind)
                compiler_error("terminate is valid only in a void function");
            emit_cleanup_scopes(ctx, 0);
            return LLVMBuildRetVoid(ctx.builder);
        }

        case Parser::NodeType::ReturnStmt:
        {
            LLVMTypeRef functionType = LLVMGlobalGetValueType(ctx.currentFunction);
            LLVMTypeRef returnType = LLVMGetReturnType(functionType);
            if (node.children.empty())
            {
                if (LLVMGetTypeKind(returnType) != LLVMVoidTypeKind)
                    compiler_error("non-void function must return a value");
                emit_cleanup_scopes(ctx, 0);
                return LLVMBuildRetVoid(ctx.builder);
            }
            if (LLVMGetTypeKind(returnType) == LLVMVoidTypeKind)
                compiler_error("void function cannot return a value");
            LLVMValueRef value = generate_node(ctx, node.children[0]);
            if (!value)
                compiler_error("return expression did not produce a value");
            value = cast_value(ctx.builder, value, returnType, ctx.currentReturnType.isSigned, "returncast");
            if (node.children[0].type == Parser::NodeType::Identifier)
            {
                const std::string name = std::string(std::get<std::string_view>(node.children[0].value));
                if (VarInfo *variable = ctx.find_var(name))
                {
                    if (CleanupValue *cleanup = find_root_cleanup_value(ctx, variable->address);
                        cleanup && cleanup->type.isPointerLike && !cleanup->type.isReference && cleanup->activeFlag)
                    {
                        LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 0, 0),
                                       cleanup->activeFlag);
                    }
                }
            }
            emit_cleanup_scopes(ctx, 0);
            return LLVMBuildRet(ctx.builder, value);
        }

        case Parser::NodeType::CallExpr:
        {
            if (node.children.empty())
                return nullptr;
            std::string funcName;
            LLVMValueRef receiver = nullptr;
            CGType receiverType;
            if (node.children[0].type == Parser::NodeType::Identifier)
            {
                funcName = resolve_imported_function_name(
                    ctx, std::string(std::get<std::string_view>(node.children[0].value)));
            }
            else if (node.children[0].type == Parser::NodeType::ScopeAccessExpr && !node.children[0].children.empty())
            {
                auto qualified_scope_name = [&](const auto &self, const Parser::ASTNode &scope) -> std::string
                {
                    if (scope.type == Parser::NodeType::Identifier)
                        return std::string(std::get<std::string_view>(scope.value));
                    if (scope.type == Parser::NodeType::ScopeAccessExpr && !scope.children.empty())
                        return self(self, scope.children.front()) +
                               "::" + std::string(std::get<std::string_view>(scope.value));
                    compiler_error("malformed scoped function call");
                };
                const std::string className = qualified_scope_name(qualified_scope_name, node.children[0].children[0]);
                const std::string methodName = std::string(std::get<std::string_view>(node.children[0].value));
                funcName = className + "." + methodName;
                if (structTypes.count(className) &&
                    !LLVMGetNamedFunction(ctx.module, mangle_function_name(funcName).c_str()))
                {
                    const size_t enclosingNamespace = className.rfind("::");
                    if (enclosingNamespace != std::string::npos)
                    {
                        const std::string namespaceFactory = className.substr(0, enclosingNamespace) + "." + methodName;
                        if (LLVMGetNamedFunction(ctx.module, mangle_function_name(namespaceFactory).c_str()))
                            funcName = namespaceFactory;
                    }
                }
                if (!structTypes.count(className) &&
                    !LLVMGetNamedFunction(ctx.module, mangle_function_name(funcName).c_str()))
                {
                    // C functions are not tracked in `functions`.
                    if (LLVMGetNamedFunction(ctx.module, mangle_function_name(methodName).c_str()))
                        funcName = methodName;
                }
            }
            else if (node.children[0].type == Parser::NodeType::MemberAccessExpr && !node.children[0].children.empty())
            {
                receiverType = lvalue_type(ctx, node.children[0].children[0]);
                if (receiverType.structName.empty())
                    codegen_error(node, "method receiver has no declared type");
                const std::string methodName = std::string(std::get<std::string_view>(node.children[0].value));
                funcName = receiverType.structName + "." + methodName;
                receiver = get_lvalue(ctx, node.children[0].children[0]);
                std::string baseName = receiverType.structName;
                while (!functions.count(funcName))
                {
                    const auto derived = structTypes.find(baseName);
                    if (derived == structTypes.end() || derived->second.baseClassName.empty())
                        break;
                    baseName = derived->second.baseClassName;
                    funcName = baseName + "." + methodName;
                }
            }
            if (funcName == "__shaft_format" && ctx.stdlibEnabled)
                return emit_brace_format_output(ctx, node, false);
            if (funcName == "__shaft_println" && ctx.stdlibEnabled)
                return emit_brace_format_output(ctx, node, true);
            size_t genericCount = 0;
            while (1 + genericCount < node.children.size() &&
                   node.children[1 + genericCount].type == Parser::NodeType::GenericArg)
                ++genericCount;
            std::vector<CGType> inferredGenericArguments;
            std::string inferredTemplateName;
            if (genericCount == 0 && receiver && is_vector_specialization(receiverType.structName))
            {
                const size_t method = funcName.rfind('.');
                if (method != std::string::npos)
                {
                    inferredTemplateName =
                        receiverType.structName.substr(0, receiverType.structName.find('.')) + funcName.substr(method);
                    const auto templateIt = genericFunctionTemplates.find(inferredTemplateName);
                    const auto vectorType = structTypes.find(receiverType.structName);
                    if (templateIt != genericFunctionTemplates.end() && vectorType != structTypes.end() &&
                        vectorType->second.fieldTypes.size() >= 1 &&
                        vectorType->second.fieldTypes.front().isPointerLike &&
                        vectorType->second.fieldTypes.front().innerType)
                        inferredGenericArguments.push_back(*vectorType->second.fieldTypes.front().innerType);
                }
            }
            if (genericCount != 0 || !inferredGenericArguments.empty())
            {
                auto templateIt = genericFunctionTemplates.find(funcName);
                if (templateIt == genericFunctionTemplates.end() && !inferredTemplateName.empty())
                    templateIt = genericFunctionTemplates.find(inferredTemplateName);
                // generic specializations have names such as Vector.i32
                // their method template is registered under Vector.push.
                if (templateIt == genericFunctionTemplates.end() && receiver)
                {
                    const size_t specialization = funcName.find('.');
                    const size_t method = funcName.rfind('.');
                    if (specialization != std::string::npos && method != std::string::npos && method > specialization)
                        templateIt =
                            genericFunctionTemplates.find(funcName.substr(0, specialization) + funcName.substr(method));
                }
                if (templateIt == genericFunctionTemplates.end())
                    compiler_error("unknown generic function '" + funcName + "'");
                std::vector<std::string> parameters;
                for (const auto &child : templateIt->second->children)
                    if (child.type == Parser::NodeType::GenericParam)
                        parameters.push_back(std::string(std::get<std::string_view>(child.value)));
                if (parameters.size() != genericCount + inferredGenericArguments.size())
                    compiler_error("generic function argument count does not match its declaration");
                std::string specializationName = funcName;
                const auto savedBindings = ctx.genericBindings;
                for (size_t index = 0; index < parameters.size(); ++index)
                {
                    const CGType argument = index < genericCount
                                                ? resolve_type(ctx, node.children[1 + index].children.front())
                                                : inferredGenericArguments[index - genericCount];
                    ctx.genericBindings[parameters[index]] = argument;
                    specializationName += "." + generic_specialization_component(argument);
                }
                if (!functions.count(specializationName))
                {
                    LLVMValueRef caller = ctx.currentFunction;
                    LLVMBasicBlockRef callerBlock = LLVMGetInsertBlock(ctx.builder);
                    const bool callerIsCFunction = ctx.isCFunction;
                    const CGType callerReturnType = ctx.currentReturnType;
                    const auto callerTunnelSlots = ctx.tunnelSlots;
                    const std::string callerClassName = ctx.currentClassName;
                    const std::string callerFunctionSpecializationName = ctx.functionSpecializationName;
                    const auto callerScopes = ctx.scopes;
                    const auto callerCleanupScopes = ctx.cleanupScopes;
                    const auto callerPendingTunnelResultTargets = ctx.pendingTunnelResultTargets;
                    const auto callerPendingTunnelPresenceTargets = ctx.pendingTunnelPresenceTargets;
                    const auto callerPendingTunnelCleanupTargets = ctx.pendingTunnelCleanupTargets;
                    const auto callerReservedTunnelTargetScopes = ctx.reservedTunnelTargetScopes;
                    const auto callerReservedTunnelPresenceTargetScopes = ctx.reservedTunnelPresenceTargetScopes;
                    const auto callerReservedTunnelCleanupTargetScopes = ctx.reservedTunnelCleanupTargetScopes;
                    const auto callerStates = ctx.states;
                    const auto callerScopedStateNames = ctx.scopedStateNames;
                    const auto callerLoopStack = ctx.loopStack;
                    const auto callerLoopCleanupDepths = ctx.loopCleanupDepths;
                    const auto callerValidPayloadAddresses = ctx.validPayloadAddresses;
                    ctx.scopes = {callerScopes.front()};
                    ctx.cleanupScopes = {{}};
                    ctx.pendingTunnelResultTargets.clear();
                    ctx.pendingTunnelPresenceTargets.clear();
                    ctx.pendingTunnelCleanupTargets.clear();
                    ctx.reservedTunnelTargetScopes = {{}};
                    ctx.reservedTunnelPresenceTargetScopes = {{}};
                    ctx.reservedTunnelCleanupTargetScopes = {{}};
                    ctx.states.clear();
                    ctx.scopedStateNames = {{}};
                    ctx.loopStack.clear();
                    ctx.loopCleanupDepths.clear();
                    ctx.validPayloadAddresses.clear();
                    ctx.functionSpecializationName = specializationName;
                    ctx.currentClassName = genericFunctionTemplateClasses[templateIt->first];
                    generate_node(ctx, *templateIt->second);
                    ctx.functionSpecializationName = callerFunctionSpecializationName;
                    ctx.currentClassName = callerClassName;
                    ctx.currentFunction = caller;
                    ctx.isCFunction = callerIsCFunction;
                    ctx.currentReturnType = callerReturnType;
                    ctx.tunnelSlots = callerTunnelSlots;
                    ctx.scopes = callerScopes;
                    ctx.cleanupScopes = callerCleanupScopes;
                    ctx.pendingTunnelResultTargets = callerPendingTunnelResultTargets;
                    ctx.pendingTunnelPresenceTargets = callerPendingTunnelPresenceTargets;
                    ctx.pendingTunnelCleanupTargets = callerPendingTunnelCleanupTargets;
                    ctx.reservedTunnelTargetScopes = callerReservedTunnelTargetScopes;
                    ctx.reservedTunnelPresenceTargetScopes = callerReservedTunnelPresenceTargetScopes;
                    ctx.reservedTunnelCleanupTargetScopes = callerReservedTunnelCleanupTargetScopes;
                    ctx.states = callerStates;
                    ctx.scopedStateNames = callerScopedStateNames;
                    ctx.loopStack = callerLoopStack;
                    ctx.loopCleanupDepths = callerLoopCleanupDepths;
                    ctx.validPayloadAddresses = callerValidPayloadAddresses;
                    LLVMPositionBuilderAtEnd(ctx.builder, callerBlock);
                }
                ctx.genericBindings = savedBindings;
                funcName = specializationName;
            }
            std::string mangledName = mangle_function_name(funcName);

            const auto normalFunction = functions.find(funcName);
            LLVMValueRef func = LLVMGetNamedFunction(ctx.module, mangledName.c_str());
            if (!func && (normalFunction == functions.end() || !normalFunction->second.comptimeDefinition))
                compiler_error("unknown function '" + funcName + "'");

            LLVMTypeRef funcType = func ? LLVMGlobalGetValueType(func) : nullptr;
            const unsigned paramCount = func ? LLVMCountParamTypes(funcType) : 0;
            if (normalFunction != functions.end() && !normalFunction->second.tunnelSlotTypes.empty())
            {
                const FunctionSignature &signature = normalFunction->second;
                const size_t explicitParameters = signature.paramTypes.size() - (receiver ? 1 : 0);
                if (node.children.size() - 1 - genericCount != explicitParameters)
                    compiler_error("wrong argument count in call to '" + funcName + "'");
                std::vector<LLVMValueRef> args;
                if (receiver)
                {
                    const CGType &receiverParameter = signature.paramTypes.front();
                    LLVMValueRef receiverValue = receiver;
                    if (receiverParameter.isPointerLike && receiverType.isPointerLike)
                    {
                        receiverValue =
                            LLVMBuildLoad2(ctx.builder, receiverType.llvmType, receiver, "pointer_self_receiver");
                    }
                    else if (!receiverParameter.isPointerLike)
                    {
                        receiverValue =
                            LLVMBuildLoad2(ctx.builder, receiverParameter.llvmType, receiver, "value_self_receiver");
                    }
                    args.push_back(receiverValue);
                }
                for (size_t i = 1 + genericCount; i < node.children.size(); ++i)
                {
                    const CGType &parameterType = signature.paramTypes[i - 1 - genericCount + (receiver ? 1 : 0)];
                    const Parser::ASTNode &argumentNode = node.children[i];
                    if ((std_base_name(parameterType.structName) == "str" ||
                         std_base_name(parameterType.structName) == "View") &&
                        argumentNode.type == Parser::NodeType::StringLiteral)
                    {
                        LLVMValueRef slice = LLVMBuildAlloca(ctx.builder, parameterType.llvmType, "strliteral");
                        LLVMValueRef data = generate_node(ctx, argumentNode);
                        LLVMValueRef dataField =
                            LLVMBuildStructGEP2(ctx.builder, parameterType.llvmType, slice, 0, "strdata");
                        LLVMValueRef lengthField =
                            LLVMBuildStructGEP2(ctx.builder, parameterType.llvmType, slice, 1, "strlen");
                        LLVMBuildStore(ctx.builder, data, dataField);
                        LLVMBuildStore(
                            ctx.builder,
                            LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx),
                                         decode_string_literal(std::get<std::string_view>(argumentNode.value)).size(),
                                         0),
                            lengthField);
                        args.push_back(LLVMBuildLoad2(ctx.builder, parameterType.llvmType, slice, "strvalue"));
                    }
                    else
                    {
                        args.push_back(generate_call_argument(ctx, argumentNode, parameterType));
                    }
                }
                if (ctx.pendingTunnelResultTargets.empty() && !signature.tunnelSlotTypes.empty())
                {
                    std::vector<LLVMValueRef> reserved;
                    std::vector<LLVMValueRef> reservedPresenceTargets;
                    std::vector<std::vector<LLVMValueRef>> reservedCleanupTargets;
                    std::vector<size_t> reservationScopes;
                    for (const auto &slot : signature.tunnelSlotNodes)
                    {
                        const std::string name = std::string(std::get<std::string_view>(slot.value));
                        bool found = false;
                        for (size_t scope = ctx.reservedTunnelTargetScopes.size(); scope-- > 0;)
                        {
                            const auto target = ctx.reservedTunnelTargetScopes[scope].find(name);
                            if (target == ctx.reservedTunnelTargetScopes[scope].end())
                                continue;
                            reserved.push_back(target->second);
                            const auto presence = ctx.reservedTunnelPresenceTargetScopes[scope].find(name);
                            reservedPresenceTargets.push_back(
                                presence == ctx.reservedTunnelPresenceTargetScopes[scope].end() ? nullptr
                                                                                                : presence->second);
                            const auto cleanup = ctx.reservedTunnelCleanupTargetScopes[scope].find(name);
                            reservedCleanupTargets.push_back(cleanup ==
                                                                     ctx.reservedTunnelCleanupTargetScopes[scope].end()
                                                                 ? std::vector<LLVMValueRef>{}
                                                                 : cleanup->second);
                            reservationScopes.push_back(scope);
                            found = true;
                            break;
                        }
                        if (!found)
                            break;
                    }
                    if (reserved.size() == signature.tunnelSlotTypes.size())
                    {
                        ctx.pendingTunnelResultTargets = std::move(reserved);
                        ctx.pendingTunnelPresenceTargets = std::move(reservedPresenceTargets);
                        ctx.pendingTunnelCleanupTargets = std::move(reservedCleanupTargets);
                        for (size_t index = 0; index < signature.tunnelSlotNodes.size(); ++index)
                        {
                            const std::string slotName =
                                std::string(std::get<std::string_view>(signature.tunnelSlotNodes[index].value));
                            ctx.reservedTunnelTargetScopes[reservationScopes[index]].erase(slotName);
                            ctx.reservedTunnelPresenceTargetScopes[reservationScopes[index]].erase(slotName);
                            ctx.reservedTunnelCleanupTargetScopes[reservationScopes[index]].erase(slotName);
                        }
                    }
                    else if (signature.tunnelSlotTypes.size() > 1)
                        compiler_error(
                            "unbound multi-output call requires one compatible reserve declaration per tunnel slot");
                }
                const bool useProvidedTunnelTargets =
                    ctx.pendingTunnelResultTargets.size() == signature.tunnelSlotTypes.size();
                std::vector<LLVMValueRef> outputAddresses;
                std::vector<LLVMValueRef> outputPresenceAddresses;
                for (size_t index = 0; index < signature.tunnelSlotTypes.size(); ++index)
                {
                    const CGType &slotType = signature.tunnelSlotTypes[index];
                    LLVMValueRef output = useProvidedTunnelTargets
                                              ? ctx.pendingTunnelResultTargets[index]
                                              : LLVMBuildAlloca(ctx.builder, slotType.llvmType, "tunnelresult");
                    outputAddresses.push_back(output);
                    args.push_back(output);
                    LLVMValueRef present = nullptr;
                    if (signature.tunnelSlotOptional[index])
                    {
                        LLVMBuildStore(ctx.builder, LLVMConstNull(slotType.llvmType), output);
                        present =
                            useProvidedTunnelTargets &&
                                    ctx.pendingTunnelPresenceTargets.size() == signature.tunnelSlotTypes.size()
                                ? ctx.pendingTunnelPresenceTargets[index]
                                : LLVMBuildAlloca(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx), "tunnelpresent");
                        LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 0, 0), present);
                        args.push_back(present);
                    }
                    const uint64_t cleanupLeafCount = signature.tunnelSlotCleanupLeafCounts[index];
                    for (uint64_t leaf = 0; leaf < cleanupLeafCount; ++leaf)
                    {
                        LLVMValueRef token =
                            useProvidedTunnelTargets &&
                                    ctx.pendingTunnelCleanupTargets.size() == signature.tunnelSlotTypes.size() &&
                                    leaf < ctx.pendingTunnelCleanupTargets[index].size()
                                ? ctx.pendingTunnelCleanupTargets[index][leaf]
                                : LLVMBuildAlloca(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx), "tunnelcleanup");
                        LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 0, 0), token);
                        args.push_back(token);
                    }
                    outputPresenceAddresses.push_back(present);
                }
                if (signature.comptimeDefinition)
                {
                    if (signature.tunnelSlotTypes.size() != 1 ||
                        LLVMGetTypeKind(signature.tunnelSlotTypes.front().llvmType) != LLVMIntegerTypeKind)
                        codegen_error(node, "@comptime calls require one integer tunnel result");
                    std::vector<uint64_t> comptimeArguments;
                    for (size_t index = 1 + genericCount; index < node.children.size(); ++index)
                    {
                        uint64_t argument = 0;
                        if (!evaluate_comptime_integer_expression(node.children[index], argument))
                            codegen_error(node.children[index],
                                          "@comptime call arguments must be integer constant expressions.");
                        comptimeArguments.push_back(argument);
                    }
                    const uint64_t value = evaluate_comptime_integer_function(signature, comptimeArguments);
                    LLVMBuildStore(ctx.builder, LLVMConstInt(signature.tunnelSlotTypes.front().llvmType, value, 0),
                                   outputAddresses.front());
                    if (useProvidedTunnelTargets)
                    {
                        ctx.pendingTunnelResultTargets.clear();
                        ctx.pendingTunnelPresenceTargets.clear();
                        ctx.pendingTunnelCleanupTargets.clear();
                    }
                    return LLVMBuildLoad2(ctx.builder, signature.tunnelSlotTypes.front().llvmType,
                                          outputAddresses.front(), "comptimeresult");
                }
                LLVMBuildCall2(ctx.builder, funcType, func, args.data(), args.size(), "");
                if (useProvidedTunnelTargets)
                {
                    ctx.pendingTunnelResultTargets.clear();
                    ctx.pendingTunnelPresenceTargets.clear();
                    ctx.pendingTunnelCleanupTargets.clear();
                }
                if (signature.tunnelSlotOptional.front())
                {
                    LLVMTypeRef optionalFields[] = {LLVMInt1TypeInContext(ctx.llvmCtx),
                                                    signature.tunnelSlotTypes.front().llvmType};
                    LLVMTypeRef optionalType = LLVMStructTypeInContext(ctx.llvmCtx, optionalFields, 2, 0);
                    LLVMValueRef result = LLVMConstNull(optionalType);
                    LLVMValueRef present = LLVMBuildLoad2(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx),
                                                          outputPresenceAddresses.front(), "callpresent");
                    LLVMValueRef payload = LLVMBuildLoad2(ctx.builder, signature.tunnelSlotTypes.front().llvmType,
                                                          outputAddresses.front(), "callpayload");
                    result = LLVMBuildInsertValue(ctx.builder, result, present, 0, "calloptionalpresent");
                    return LLVMBuildInsertValue(ctx.builder, result, payload, 1, "calloptional");
                }
                return LLVMBuildLoad2(ctx.builder, signature.tunnelSlotTypes.front().llvmType, outputAddresses.front(),
                                      "callresult");
            }
            const size_t explicitParameters = paramCount - (receiver ? 1 : 0);
            if (node.children.size() - 1 - genericCount != explicitParameters)
                compiler_error("wrong argument count in call to '" + funcName + "'");
            std::vector<LLVMTypeRef> paramTypes(paramCount);
            LLVMGetParamTypes(funcType, paramTypes.data());

            std::vector<LLVMValueRef> args;
            if (receiver)
            {
                if (normalFunction == functions.end() || normalFunction->second.paramTypes.empty())
                    compiler_error("method receiver has no declared parameter type");
                const CGType &receiverParameter = normalFunction->second.paramTypes.front();
                LLVMValueRef receiverValue = receiver;
                if (receiverParameter.isPointerLike && receiverType.isPointerLike)
                {
                    receiverValue =
                        LLVMBuildLoad2(ctx.builder, receiverType.llvmType, receiver, "pointer_self_receiver");
                }
                else if (!receiverParameter.isPointerLike)
                {
                    receiverValue =
                        LLVMBuildLoad2(ctx.builder, receiverParameter.llvmType, receiver, "value_self_receiver");
                }
                args.push_back(receiverValue);
            }
            const auto cstrInfo = structTypes.find("cstr");
            for (size_t i = 1 + genericCount; i < node.children.size(); ++i)
            {
                const size_t parameterIndex = i - 1 - genericCount + (receiver ? 1 : 0);
                const Parser::ASTNode &argumentNode = node.children[i];
                if (argumentNode.type == Parser::NodeType::StringLiteral && cstrInfo != structTypes.end() &&
                    paramTypes[parameterIndex] == cstrInfo->second.llvmType)
                {
                    CGType cstrType;
                    cstrType.llvmType = cstrInfo->second.llvmType;
                    cstrType.structName = "cstr";
                    args.push_back(generate_value_for_target(ctx, argumentNode, cstrType));
                    continue;
                }
                if (normalFunction != functions.end() && parameterIndex < normalFunction->second.paramTypes.size())
                {
                    args.push_back(
                        generate_call_argument(ctx, argumentNode, normalFunction->second.paramTypes[parameterIndex]));
                    continue;
                }
                if (argumentNode.type == Parser::NodeType::StringLiteral &&
                    LLVMGetTypeKind(paramTypes[parameterIndex]) == LLVMStructTypeKind)
                {
                    codegen_error(argumentNode, "Missing function-signature metadata for '" + funcName + "'.");
                }
                LLVMValueRef argument = generate_node(ctx, argumentNode);
                if (!argument)
                    compiler_error("call argument did not produce a value");
                args.push_back(cast_value(ctx.builder, argument, paramTypes[parameterIndex], true, "callargcast"));
            }

            const char *callName = LLVMGetTypeKind(LLVMGetReturnType(funcType)) == LLVMVoidTypeKind ? "" : "calltmp";
            return LLVMBuildCall2(ctx.builder, funcType, func, args.data(), args.size(), callName);
        }

        case Parser::NodeType::TunnelStmt:
        {
            const std::string slot = std::string(std::get<std::string_view>(node.value));
            const auto found = ctx.tunnelSlots.find(slot);
            if (found == ctx.tunnelSlots.end())
                compiler_error("unknown tunnel slot '" + slot + "'");
            if (node.children.empty())
                compiler_error("tunnel statement is missing its value");
            CleanupValue *sourceCleanup = nullptr;
            if (node.children.front().type == Parser::NodeType::Identifier)
            {
                const std::string sourceName = std::string(std::get<std::string_view>(node.children.front().value));
                if (VarInfo *sourceVar = ctx.find_var(sourceName))
                {
                    for (auto scope = ctx.cleanupScopes.rbegin(); scope != ctx.cleanupScopes.rend() && !sourceCleanup;
                         ++scope)
                        for (CleanupValue &candidate : *scope)
                            if (candidate.address == sourceVar->address)
                            {
                                sourceCleanup = &candidate;
                                break;
                            }
                }
            }
            LLVMValueRef value = generate_value_for_target(ctx, node.children.front(), found->second.type);
            value = materialize_value(ctx, value, found->second.type, "tunnelcast");
            LLVMBuildStore(ctx.builder, value, LLVMGetParam(ctx.currentFunction, found->second.argIndex));
            std::vector<LLVMValueRef> sourceTokens;
            if (sourceCleanup)
                collect_cleanup_token_addresses(*sourceCleanup, sourceTokens);
            for (uint64_t leaf = 0; leaf < found->second.cleanupLeafCount; ++leaf)
            {
                const bool freshAllocation = is_fresh_allocation_expr(node.children.front());
                LLVMValueRef active =
                    leaf < sourceTokens.size()
                        ? LLVMBuildLoad2(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx), sourceTokens[leaf],
                                         "tunnelactive")
                        : LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), freshAllocation ? 1 : 0, 0);
                LLVMBuildStore(ctx.builder, active,
                               LLVMGetParam(ctx.currentFunction, found->second.cleanupArgIndex + leaf));
            }
            if (sourceCleanup)
                disarm_cleanup_value(ctx, *sourceCleanup);
            if (found->second.isOptional)
                LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 1, 0),
                               LLVMGetParam(ctx.currentFunction, found->second.flagArgIndex));
            return nullptr;
        }

        case Parser::NodeType::MultiReserveStmt:
        {
            if (node.children.size() < 5 || node.children.back().type != Parser::NodeType::CallExpr)
                compiler_error("multi-result reserve requires a function call initializer");
            const Parser::ASTNode &source = node.children.back();
            if (source.children.empty())
                compiler_error("multi-result reserve requires a named function call");
            const Parser::ASTNode &calleeExpression = source.children.front();
            std::string callee;
            if (calleeExpression.type == Parser::NodeType::Identifier)
                callee = std::string(std::get<std::string_view>(calleeExpression.value));
            else if (calleeExpression.type == Parser::NodeType::MemberAccessExpr && !calleeExpression.children.empty())
            {
                const CGType receiverType = lvalue_type(ctx, calleeExpression.children.front());
                if (receiverType.structName.empty())
                    codegen_error(calleeExpression, "method receiver has no declared type");
                const std::string methodName = std::string(std::get<std::string_view>(calleeExpression.value));
                std::string baseName = receiverType.structName;
                callee = baseName + "." + methodName;
                while (!functions.count(callee))
                {
                    const auto derived = structTypes.find(baseName);
                    if (derived == structTypes.end() || derived->second.baseClassName.empty())
                        break;
                    baseName = derived->second.baseClassName;
                    callee = baseName + "." + methodName;
                }
            }
            else
                compiler_error("multi-result reserve requires a named function or method call");
            const auto signature = functions.find(callee);
            const size_t bindingCount = (node.children.size() - 1) / 2;
            if (signature == functions.end() || signature->second.tunnelSlotTypes.size() != bindingCount)
                compiler_error("multi-result reserve binding count does not match tunnel outputs");
            ctx.pendingTunnelResultTargets.clear();
            ctx.pendingTunnelPresenceTargets.clear();
            ctx.pendingTunnelCleanupTargets.clear();
            for (size_t index = 0; index < bindingCount; ++index)
            {
                const CGType type = resolve_type(ctx, node.children[index * 2]);
                const std::string name = std::string(std::get<std::string_view>(node.children[index * 2 + 1].value));
                const bool optional = signature->second.tunnelSlotOptional[index];
                const CGType &payloadType = signature->second.tunnelSlotTypes[index];
                if ((optional &&
                     (!type.isOptional || !type.innerType || type.innerType->llvmType != payloadType.llvmType)) ||
                    (!optional && type.llvmType != payloadType.llvmType))
                    compiler_error("multi-result reserve binding type does not match its tunnel output");
                LLVMValueRef address = LLVMBuildAlloca(ctx.builder, type.llvmType, name.c_str());
                std::vector<LLVMValueRef> cleanupTokens;
                if (type_requires_cleanup(ctx, type))
                {
                    LLVMBuildStore(ctx.builder, LLVMConstNull(type.llvmType), address);
                    CleanupValue cleanup = create_cleanup_value(ctx, address, type, name);
                    collect_cleanup_token_addresses(cleanup, cleanupTokens);
                    ctx.declare_owned_var(name, address, type, std::move(cleanup));
                }
                else
                    ctx.declare_var(name, address, type);
                if (optional)
                {
                    ctx.pendingTunnelPresenceTargets.push_back(
                        LLVMBuildStructGEP2(ctx.builder, type.llvmType, address, 0, "reserve_present"));
                    ctx.pendingTunnelResultTargets.push_back(
                        LLVMBuildStructGEP2(ctx.builder, type.llvmType, address, 1, "reserve_payload"));
                }
                else
                {
                    ctx.pendingTunnelPresenceTargets.push_back(nullptr);
                    ctx.pendingTunnelResultTargets.push_back(address);
                }
                ctx.pendingTunnelCleanupTargets.push_back(std::move(cleanupTokens));
            }
            generate_node(ctx, source);
            if (!ctx.pendingTunnelResultTargets.empty())
                compiler_error("multi-result reserve did not consume its tunnel outputs");
            return nullptr;
        }

        case Parser::NodeType::ReserveStmt:
        {
            if (node.children.empty())
                compiler_error("reserve statement is missing its type");
            const std::string name = std::string(std::get<std::string_view>(node.value));
            CGType type = resolve_type(ctx, node.children.front());
            if (node.children.size() > 1 && node.children[1].type == Parser::NodeType::TunnelBindingExpr)
            {
                const std::string slot = std::string(std::get<std::string_view>(node.children[1].value));
                const auto found = ctx.tunnelSlots.find(slot);
                if (found == ctx.tunnelSlots.end())
                    compiler_error("reserve tunnel binding refers to an unknown slot");
                if (type.llvmType != found->second.type.llvmType)
                    compiler_error("reserve tunnel binding type does not match its slot");
                ctx.declare_var(name, LLVMGetParam(ctx.currentFunction, found->second.argIndex), type);
                return nullptr;
            }
            LLVMValueRef address = LLVMBuildAlloca(ctx.builder, type.llvmType, name.c_str());
            if (type_requires_cleanup(ctx, type))
            {
                LLVMBuildStore(ctx.builder, LLVMConstNull(type.llvmType), address);
                CleanupValue cleanup = create_cleanup_value(ctx, address, type, name);
                if (node.children.size() > 1 && is_fresh_allocation_expr(node.children[1]))
                    arm_cleanup_value(ctx, cleanup);
                ctx.declare_owned_var(name, address, type, std::move(cleanup));
            }
            else
            {
                ctx.declare_var(name, address, type);
            }
            if (node.children.size() == 1)
            {
                if (ctx.reservedTunnelTargetScopes.back().find(name) != ctx.reservedTunnelTargetScopes.back().end())
                    compiler_error("duplicate tunnel reservation in the same scope");
                const LLVMValueRef payloadTarget =
                    type.isOptional
                        ? LLVMBuildStructGEP2(ctx.builder, type.llvmType, address, 1, "reserve_tunnel_payload")
                        : address;
                ctx.reservedTunnelTargetScopes.back()[name] = payloadTarget;
                if (type.isOptional)
                    ctx.reservedTunnelPresenceTargetScopes.back()[name] =
                        LLVMBuildStructGEP2(ctx.builder, type.llvmType, address, 0, "reserve_tunnel_present");
                std::vector<LLVMValueRef> cleanupTokens;
                if (type_requires_cleanup(ctx, type) && !ctx.cleanupScopes.back().empty())
                    collect_cleanup_token_addresses(ctx.cleanupScopes.back().back(), cleanupTokens);
                ctx.reservedTunnelCleanupTargetScopes.back()[name] = std::move(cleanupTokens);
                return nullptr;
            }

            const Parser::ASTNode &source = node.children[1];
            const std::string sourceFunctionName =
                source.type == Parser::NodeType::CallExpr && !source.children.empty() &&
                        source.children.front().type == Parser::NodeType::Identifier
                    ? resolve_imported_function_name(
                          ctx, std::string(std::get<std::string_view>(source.children.front().value)))
                    : std::string{};
            if (source.type == Parser::NodeType::CallExpr && !source.children.empty() &&
                source.children.front().type == Parser::NodeType::Identifier &&
                (source.children.size() < 2 || source.children[1].type != Parser::NodeType::GenericArg) &&
                functions.find(sourceFunctionName) == functions.end() && sourceFunctionName != "__shaft_format")
            {
                const std::string callee =
                    mangle_function_name(std::string(std::get<std::string_view>(source.children.front().value)));
                LLVMValueRef function = LLVMGetNamedFunction(ctx.module, callee.c_str());
                if (!function)
                    compiler_error("unknown function in reserve call");
                LLVMTypeRef functionType = LLVMGlobalGetValueType(function);
                const unsigned inputCount = static_cast<unsigned>(source.children.size() - 1);
                const unsigned parameterCount = LLVMCountParamTypes(functionType);
                if (LLVMGetTypeKind(LLVMGetReturnType(functionType)) == LLVMVoidTypeKind &&
                    parameterCount == inputCount + 1)
                {
                    std::vector<LLVMTypeRef> parameterTypes(parameterCount);
                    LLVMGetParamTypes(functionType, parameterTypes.data());
                    std::vector<LLVMValueRef> arguments;
                    for (unsigned i = 0; i < inputCount; ++i)
                    {
                        LLVMValueRef value = generate_node(ctx, source.children[i + 1]);
                        arguments.push_back(cast_value(ctx.builder, value, parameterTypes[i], true, "reserveargcast"));
                    }
                    arguments.push_back(address);
                    LLVMBuildCall2(ctx.builder, functionType, function, arguments.data(), arguments.size(), "");
                    return nullptr;
                }
            }

            if (source.type == Parser::NodeType::CallExpr && !source.children.empty() &&
                source.children.front().type == Parser::NodeType::Identifier)
            {
                const auto callee = functions.find(sourceFunctionName);
                if (callee != functions.end() && callee->second.tunnelSlotTypes.size() == 1)
                {
                    LLVMValueRef payloadTarget =
                        type.isOptional
                            ? LLVMBuildStructGEP2(ctx.builder, type.llvmType, address, 1, "reserve_tunnel_payload")
                            : address;
                    LLVMValueRef presenceTarget =
                        type.isOptional
                            ? LLVMBuildStructGEP2(ctx.builder, type.llvmType, address, 0, "reserve_tunnel_present")
                            : nullptr;
                    ctx.pendingTunnelResultTargets = {payloadTarget};
                    ctx.pendingTunnelPresenceTargets = {presenceTarget};
                    ctx.pendingTunnelCleanupTargets.clear();
                    std::vector<LLVMValueRef> tokens;
                    if (type_requires_cleanup(ctx, type) && !ctx.cleanupScopes.back().empty())
                        collect_cleanup_token_addresses(ctx.cleanupScopes.back().back(), tokens);
                    ctx.pendingTunnelCleanupTargets.push_back(std::move(tokens));
                    generate_node(ctx, source);
                    if (CleanupValue *cleanup = find_root_cleanup_value(ctx, address))
                        arm_cleanup_value(ctx, *cleanup);
                    if (!ctx.pendingTunnelResultTargets.empty())
                        compiler_error("reserve tunnel initializer did not consume its output");
                    return nullptr;
                }
            }

            LLVMValueRef value = generate_value_for_target(ctx, source, type);
            if (!value)
                compiler_error("reserve initializer did not produce a value");
            LLVMBuildStore(ctx.builder, materialize_value(ctx, value, type, "reserveinitcast"), address);
            return nullptr;
        }

        case Parser::NodeType::RefExpr:
        {
            if (node.children.empty())
                compiler_error("reference expression is missing its source");
            LLVMValueRef address = get_lvalue(ctx, node.children[0]);
            if (!address)
                compiler_error("reference expression requires an addressable source");
            return address;
        }

        case Parser::NodeType::MoveExpr:
        {
            if (node.children.empty())
                compiler_error("move expression is missing its source");
            const CGType sourceType = lvalue_type(ctx, node.children[0]);
            LLVMValueRef sourceAddress = get_lvalue(ctx, node.children[0]);
            if (!sourceAddress)
                compiler_error("move expression requires an addressable source");
            for (auto &scope : ctx.cleanupScopes)
            {
                for (CleanupValue &value : scope)
                    disarm_cleanup_at_address(ctx, value, sourceAddress, sourceType);
            }
            return LLVMBuildLoad2(ctx.builder, sourceType.llvmType, sourceAddress, "movevalue");
        }

        case Parser::NodeType::StateBindingDecl:
        case Parser::NodeType::ThreadBindingDecl:
        {
            if (node.children.size() != 1 || node.children.front().type != Parser::NodeType::CallExpr)
                compiler_error("State binding requires one deferred function call");
            const std::string name = std::string(std::get<std::string_view>(node.value));
            if (ctx.states.find(name) != ctx.states.end())
                compiler_error("duplicate State binding '" + name + "'");
            const bool isThread = node.type == Parser::NodeType::ThreadBindingDecl;
            LLVMValueRef threadTid = nullptr;
            if (isThread)
            {
                LLVMTypeRef handleType = LLVMArrayType(LLVMInt64TypeInContext(ctx.llvmCtx), 2);
                LLVMValueRef handleStorage = LLVMBuildAlloca(ctx.builder, handleType, "threadhandle");
                LLVMValueRef handleIndices[] = {LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), 0, 0),
                                                LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), 0, 0)};
                threadTid = LLVMBuildGEP2(ctx.builder, handleType, handleStorage, handleIndices, 2,
                                          "threadhandlevalue");
                LLVMBuildStore(ctx.builder,
                               LLVMConstNull(handleType), handleStorage);
            }
            ctx.states.emplace(name, DeferredState{&node.children.front(), false, isThread, threadTid});
            ctx.scopedStateNames.back().push_back(name);
            return nullptr;
        }

        case Parser::NodeType::StartStmt:
        {
            const std::string name = std::string(std::get<std::string_view>(node.value));
            auto state = ctx.states.find(name);
            if (state == ctx.states.end())
                compiler_error("start references unknown State '" + name + "'");
            if (!state->second.started)
            {
                if (state->second.isThread)
                    emit_thread_start(ctx, state->second);
                else
                {
                    generate_node(ctx, *state->second.call);
                    state->second.started = true;
                }
            }
            return nullptr;
        }

        case Parser::NodeType::AwaitExpr:
        {
            if (node.children.empty())
                compiler_error("await requires a State or call expression");
            if (node.children.front().type == Parser::NodeType::Identifier)
            {
                const std::string name = std::string(std::get<std::string_view>(node.children.front().value));
                auto state = ctx.states.find(name);
                if (state == ctx.states.end())
                    compiler_error("await references unknown State '" + name + "'");
                if (!state->second.started)
                {
                    if (state->second.isThread)
                        emit_thread_start(ctx, state->second);
                    else
                    {
                        generate_node(ctx, *state->second.call);
                        state->second.started = true;
                    }
                }
                if (state->second.isThread)
                    emit_thread_join(ctx, state->second);
                return LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), 0, 0);
            }
            return generate_node(ctx, node.children.front());
        }

        case Parser::NodeType::ThreadTaskStmt:
            if (node.children.size() != 1)
                compiler_error("named task requires one body block");
            return generate_node(ctx, node.children.front());

        case Parser::NodeType::ValidStmt:
        {
            const std::string name = std::string(std::get<std::string_view>(node.value));
            VarInfo *optional = ctx.find_var(name);
            if (!optional || !optional->type.isOptional)
                compiler_error("valid requires a declared optional value");
            LLVMValueRef flagAddress =
                LLVMBuildStructGEP2(ctx.builder, optional->type.llvmType, optional->address, 0, "optionalflagaddr");
            LLVMValueRef flag =
                LLVMBuildLoad2(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx), flagAddress, "optionalflag");
            LLVMBasicBlockRef thenBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "validthen");
            LLVMBasicBlockRef elseBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "validelse");
            LLVMBasicBlockRef mergeBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "validcont");
            LLVMBuildCondBr(ctx.builder, flag, thenBB, elseBB);
            LLVMPositionBuilderAtEnd(ctx.builder, thenBB);
            ctx.validPayloadAddresses.push_back(optional->address);
            generate_node(ctx, node.children.front());
            ctx.validPayloadAddresses.pop_back();
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                LLVMBuildBr(ctx.builder, mergeBB);
            LLVMPositionBuilderAtEnd(ctx.builder, elseBB);
            if (node.children.size() > 1)
                generate_node(ctx, node.children[1]);
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                LLVMBuildBr(ctx.builder, mergeBB);
            LLVMPositionBuilderAtEnd(ctx.builder, mergeBB);
            return nullptr;
        }

        default:
            compiler_error("internal error: parser construct reached LLVM generation without a lowering rule");
        }
        return nullptr;
    }

    static LLVMValueRef build_main_arguments(Context &ctx, LLVMValueRef entry)
    {
        auto stringInfo = structTypes.find("std::String");
        if (stringInfo == structTypes.end())
            stringInfo = structTypes.find("String");
        if (stringInfo == structTypes.end())
            compiler_error("String[argc] main requires the standard String type");

        LLVMValueRef alloc = LLVMGetNamedFunction(ctx.module, "__shaft_alloc");
        LLVMValueRef strlen = LLVMGetNamedFunction(ctx.module, "__cstr_strlen");
        LLVMValueRef copy = LLVMGetNamedFunction(ctx.module, "memcpy");
        if (!alloc || !strlen || !copy)
            compiler_error("String[argc] main requires std allocation and byte-copy runtime functions");

        LLVMTypeRef i8 = LLVMInt8TypeInContext(ctx.llvmCtx);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx.llvmCtx);
        LLVMTypeRef pointer = LLVMPointerType(i8, 0);
        LLVMTypeRef stringType = stringInfo->second.llvmType;
        LLVMTypeRef allocType = LLVMFunctionType(pointer, &i64, 1, 0);
        LLVMTypeRef strlenType = LLVMFunctionType(i64, &pointer, 1, 0);
        LLVMTypeRef copyArgs[] = {pointer, pointer, i64};
        LLVMTypeRef copyType = LLVMFunctionType(pointer, copyArgs, 3, 0);

        LLVMValueRef argc = LLVMGetParam(entry, 0);
        LLVMValueRef argv = LLVMGetParam(entry, 1);
        LLVMValueRef count = LLVMBuildZExt(ctx.builder, argc, i64, "argc64");
        LLVMValueRef bytes = LLVMBuildMul(ctx.builder, count, LLVMConstInt(i64, 24, 0), "argbytes");
        LLVMValueRef values = LLVMBuildCall2(ctx.builder, allocType, alloc, &bytes, 1, "args");

        LLVMValueRef index = LLVMBuildAlloca(ctx.builder, i64, "argi");
        LLVMBuildStore(ctx.builder, LLVMConstInt(i64, 0, 0), index);
        LLVMBasicBlockRef loop = LLVMAppendBasicBlockInContext(ctx.llvmCtx, entry, "args.loop");
        LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(ctx.llvmCtx, entry, "args.body");
        LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(ctx.llvmCtx, entry, "args.done");
        LLVMBuildBr(ctx.builder, loop);

        LLVMPositionBuilderAtEnd(ctx.builder, loop);
        LLVMValueRef current = LLVMBuildLoad2(ctx.builder, i64, index, "argi.value");
        LLVMValueRef more = LLVMBuildICmp(ctx.builder, LLVMIntULT, current, count, "hasarg");
        LLVMBuildCondBr(ctx.builder, more, body, done);

        LLVMPositionBuilderAtEnd(ctx.builder, body);
        LLVMValueRef stringSlot = LLVMBuildGEP2(ctx.builder, stringType, values, &current, 1, "argslot");
        LLVMValueRef argvSlot = LLVMBuildGEP2(ctx.builder, pointer, argv, &current, 1, "argvslot");
        LLVMValueRef source = LLVMBuildLoad2(ctx.builder, pointer, argvSlot, "argvvalue");
        LLVMValueRef length = LLVMBuildCall2(ctx.builder, strlenType, strlen, &source, 1, "arglen");
        LLVMValueRef copyBytes = LLVMBuildAdd(ctx.builder, length, LLVMConstInt(i64, 1, 0), "argbytes");
        LLVMValueRef data = LLVMBuildCall2(ctx.builder, allocType, alloc, &copyBytes, 1, "argdata");
        LLVMValueRef copyValues[] = {data, source, length};
        LLVMBuildCall2(ctx.builder, copyType, copy, copyValues, 3, "");
        LLVMBuildStore(ctx.builder, data, LLVMBuildStructGEP2(ctx.builder, stringType, stringSlot, 0, "argdatafield"));
        LLVMBuildStore(ctx.builder, length,
                       LLVMBuildStructGEP2(ctx.builder, stringType, stringSlot, 1, "arglengthfield"));
        LLVMBuildStore(ctx.builder, length,
                       LLVMBuildStructGEP2(ctx.builder, stringType, stringSlot, 2, "argcapacityfield"));
        LLVMBuildStore(ctx.builder, LLVMBuildAdd(ctx.builder, current, LLVMConstInt(i64, 1, 0), "nextarg"), index);
        LLVMBuildBr(ctx.builder, loop);

        LLVMPositionBuilderAtEnd(ctx.builder, done);
        return values;
    }

    LLVMModuleRef generate_module(Context &ctx, const std::vector<Parser::ASTNode> &roots)
    {
        for (const auto &root : roots)
            predeclare_struct_types(ctx, root);
        for (const auto &root : roots)
            predeclare_c_functions(ctx, root);
        for (const auto &root : roots)
            materialize_aggregate_layouts(ctx, root);
        for (const auto &root : roots)
            predeclare_function_signatures(ctx, root);
        for (const auto &root : roots)
            generate_node(ctx, root);

        if (ctx.deferredEntryDefinition)
        {
            ctx.generatingDeferredEntry = true;
            generate_node(ctx, *ctx.deferredEntryDefinition);
            ctx.generatingDeferredEntry = false;
        }

        return ctx.module;
    }
} // namespace Codegen