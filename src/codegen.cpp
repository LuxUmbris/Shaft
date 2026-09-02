#include "codegen.hpp"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>

namespace Codegen
{
    std::unordered_map<std::string, StructInfo> structTypes;
    std::unordered_map<std::string, EnumInfo> enumTypes;
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
        return ctx;
    }

    // forward declarations
    LLVMValueRef generate_node(Context &ctx, const Parser::ASTNode &node);
    struct RuntimeArrayIndexCapture
    {
        const Parser::ASTNode *node = nullptr;
        LLVMValueRef value = nullptr;
    };
    LLVMValueRef get_lvalue(Context &ctx, const Parser::ASTNode &node,
                            RuntimeArrayIndexCapture *runtimeIndex = nullptr);
    static CGType lvalue_type(Context &ctx, const Parser::ASTNode &node);

    static LLVMValueRef cast_value(LLVMBuilderRef builder, LLVMValueRef value, LLVMTypeRef target,
                                   bool isSigned, const char *name)
    {
        if (LLVMTypeOf(value) == target)
            return value;

        const LLVMTypeKind sourceKind = LLVMGetTypeKind(LLVMTypeOf(value));
        const LLVMTypeKind targetKind = LLVMGetTypeKind(target);
        const bool sourceIsFloat = sourceKind == LLVMFloatTypeKind || sourceKind == LLVMDoubleTypeKind;
        const bool targetIsFloat = targetKind == LLVMFloatTypeKind || targetKind == LLVMDoubleTypeKind;

        if (sourceKind == LLVMIntegerTypeKind && targetKind == LLVMIntegerTypeKind)
            return LLVMBuildIntCast2(builder, value, target, isSigned ? 1 : 0, name);
        if (sourceIsFloat && targetIsFloat)
            return LLVMBuildFPCast(builder, value, target, name);
        if (sourceKind == LLVMIntegerTypeKind && targetIsFloat)
            return LLVMBuildSIToFP(builder, value, target, name);
        if (sourceIsFloat && targetKind == LLVMIntegerTypeKind)
            return LLVMBuildFPToSI(builder, value, target, name);

        char *sourceText = LLVMPrintTypeToString(LLVMTypeOf(value));
        char *targetText = LLVMPrintTypeToString(target);
        const std::string message = "cannot convert values between these LLVM types: " +
                                    std::string(sourceText) + " -> " + std::string(targetText);
        LLVMDisposeMessage(sourceText);
        LLVMDisposeMessage(targetText);
        throw std::runtime_error(message);
    }

    static LLVMValueRef materialize_value(Context &ctx, LLVMValueRef value, const CGType &target,
                                            const char *name)
    {
        if (!target.isOptional || LLVMTypeOf(value) == target.llvmType)
            return cast_value(ctx.builder, value, target.llvmType, target.isSigned, name);
        if (!target.innerType)
            throw std::runtime_error("optional type has no payload type");
        LLVMValueRef aggregate = LLVMConstNull(target.llvmType);
        LLVMValueRef present = LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 1, 0);
        aggregate = LLVMBuildInsertValue(ctx.builder, aggregate, present, 0, "optionalpresent");
        LLVMValueRef payload = cast_value(ctx.builder, value, target.innerType->llvmType,
                                          target.innerType->isSigned, name);
        return LLVMBuildInsertValue(ctx.builder, aggregate, payload, 1, "optionalpayload");
    }

    static std::string decode_string_literal(std::string_view source);

    static LLVMValueRef generate_value_for_target(Context &ctx, const Parser::ASTNode &node,
                                                   const CGType &target)
    {
        if ((target.structName == "str" || target.structName == "cstr") &&
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
            const auto info = structTypes.find(target.structName);
            if (info == structTypes.end() || !info->second.initializedField.empty())
                throw std::runtime_error("brace initializer requires a plain declared struct type");
            if (node.children.size() != info->second.fieldTypes.size())
                throw std::runtime_error("brace initializer field count does not match its type");
            LLVMValueRef aggregate = LLVMConstNull(target.llvmType);
            for (size_t index = 0; index < info->second.fieldTypes.size(); ++index)
            {
                const CGType &fieldType = info->second.fieldTypes[index];
                LLVMValueRef field = generate_value_for_target(ctx, node.children[index], fieldType);
                if (!field)
                    throw std::runtime_error("brace initializer field did not produce a value");
                aggregate = LLVMBuildInsertValue(
                    ctx.builder, aggregate, materialize_value(ctx, field, fieldType, "structinitcast"),
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

    static std::string decode_string_literal(std::string_view source)
    {
        const auto hex_digit = [](char character) -> unsigned
        {
            if (character >= '0' && character <= '9') return character - '0';
            if (character >= 'a' && character <= 'f') return character - 'a' + 10;
            if (character >= 'A' && character <= 'F') return character - 'A' + 10;
            throw std::runtime_error("invalid hexadecimal string escape");
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
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case '0': result += '\0'; break;
            case '\\': result += '\\'; break;
            case '"': result += '"'; break;
            case '\'': result += '\''; break;
            case 'x':
                if (index + 2 >= source.size())
                    throw std::runtime_error("incomplete hexadecimal string escape");
                result += static_cast<char>((hex_digit(source[index + 1]) << 4) | hex_digit(source[index + 2]));
                index += 2;
                break;
            case 'u':
            {
                if (index + 4 >= source.size())
                    throw std::runtime_error("incomplete Unicode string escape");
                unsigned codepoint = 0;
                for (size_t digit = 1; digit <= 4; ++digit)
                    codepoint = (codepoint << 4) | hex_digit(source[index + digit]);
                if (codepoint >= 0xd800 && codepoint <= 0xdfff)
                    throw std::runtime_error("Unicode string escape cannot encode a surrogate");
                append_utf8(result, codepoint);
                index += 4;
                break;
            }
            default: result += escaped; break;
            }
        }
        return result;
    }

    static std::string custom_type_name(const Parser::ASTNode &node)
    {
        return node.inferredTypeName.empty()
                   ? std::string(std::get<std::string_view>(node.value))
                   : node.inferredTypeName;
    }

    static std::string generic_specialization_component(const CGType &type)
    {
        const LLVMTypeKind kind = LLVMGetTypeKind(type.llvmType);
        if (kind == LLVMIntegerTypeKind)
            return std::string(type.isSigned ? "i" : "u") +
                   std::to_string(LLVMGetIntTypeWidth(type.llvmType));
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
        throw std::runtime_error("generic argument does not have a concrete backend representation");
    }

    static std::string qualified_declaration_name(const Context &ctx, const std::string &name)
    {
        return ctx.currentNamespaceName.empty() ? name : ctx.currentNamespaceName + "::" + name;
    }

    static std::string scope_access_name(const Parser::ASTNode &node)
    {
        if (node.type == Parser::NodeType::Identifier)
            return std::string(std::get<std::string_view>(node.value));
        if (node.type == Parser::NodeType::ScopeAccessExpr && !node.children.empty())
            return scope_access_name(node.children.front()) + "::" +
                   std::string(std::get<std::string_view>(node.value));
        throw std::runtime_error("malformed scope access");
    }

    static bool is_string_array_type(const Parser::ASTNode &node)
    {
        return node.type == Parser::NodeType::ArrayType && !node.children.empty() &&
               node.children.front().type == Parser::NodeType::CustomType &&
               custom_type_name(node.children.front()) == "String";
    }

    static void validate_normal_main_signature(const Parser::ASTNode &node, const std::string &name)
    {
        if (name != "main")
            return;

        size_t parameters = 0;
        for (const auto &child : node.children)
        {
            if (child.type == Parser::NodeType::TunnelSlot)
                throw std::runtime_error("main cannot declare tunnel outputs");
            if (child.type == Parser::NodeType::Param)
            {
                ++parameters;
                if (parameters != 1 || child.children.empty() ||
                    !is_string_array_type(child.children.front()))
                    throw std::runtime_error("main must take no arguments or String[] args");
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
                throw std::runtime_error("empty enum character constant");
            return static_cast<unsigned char>(text.front());
        }
        if (node.type == Parser::NodeType::Identifier)
        {
            const auto found = members.members.find(std::string(std::get<std::string_view>(node.value)));
            if (found == members.members.end())
                throw std::runtime_error("enum constant expression refers to an unknown member");
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
            case Lexer::Operator::PLUS: return lhs + rhs;
            case Lexer::Operator::MINUS: return lhs - rhs;
            case Lexer::Operator::MULTIPLY: return lhs * rhs;
            case Lexer::Operator::DIVIDE:
                if (rhs == 0)
                    throw std::runtime_error("enum constant expression divides by zero");
                return lhs / rhs;
            case Lexer::Operator::MODULO:
                if (rhs == 0)
                    throw std::runtime_error("enum constant expression divides by zero");
                return lhs % rhs;
            case Lexer::Operator::LEFT_SHIFT: return lhs << rhs;
            case Lexer::Operator::RIGHT_SHIFT: return lhs >> rhs;
            case Lexer::Operator::PIPE: return lhs | rhs;
            case Lexer::Operator::AMPERSAND: return lhs & rhs;
            case Lexer::Operator::CARET: return lhs ^ rhs;
            default: break;
            }
        }
        throw std::runtime_error("enum member value must be an integral constant expression");
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
            // in LLVM 15+, pointers are opaque.
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
                // Runtime-sized arrays are passed as a pointer to their first
                // element. The runtime size expression stays in the AST for
                // entrypoint lowering, where argc owns the actual count.
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
            if (name == "Self" && !ctx.currentClassName.empty())
                name = ctx.currentClassName;
            if (const auto binding = ctx.genericBindings.find(name); binding != ctx.genericBindings.end())
                return binding->second;
            if (node.children.size() > 0 && structTypes.count(name) && name != "Vector" &&
                !structTypes.at(name).genericParameters.empty())
            {
                const StructInfo &templateInfo = structTypes.at(name);
                if (node.children.size() != templateInfo.genericParameters.size())
                    throw std::runtime_error("generic type argument count does not match its declaration");
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
                    ctx.genericBindings = {};
                    for (size_t index = 0; index < arguments.size(); ++index)
                        ctx.genericBindings[templateInfo.genericParameters[index]] = arguments[index];
                    specialization.fieldTypes.clear();
                    std::vector<LLVMTypeRef> fields;
                    for (const Parser::ASTNode &fieldNode : templateInfo.fieldTypeNodes)
                    {
                        CGType field = resolve_type(ctx, fieldNode);
                        specialization.fieldTypes.push_back(field);
                        fields.push_back(field.llvmType);
                    }
                    ctx.genericBindings.clear();
                    LLVMStructSetBody(specialization.llvmType, fields.data(), fields.size(), false);
                    structTypes[specializationName] = std::move(specialization);
                }
                cgType.llvmType = structTypes.at(specializationName).llvmType;
                cgType.requiredAlignment = structTypes.at(specializationName).requestedAlignment;
                cgType.structName = specializationName;
            }
            else if (name == "Vector" && node.children.size() == 1 && structTypes.count(name))
            {
                const CGType elementType = resolve_type(ctx, node.children.front());
                const std::string elementName = generic_specialization_component(elementType);
                const std::string specializationName = "Vector." + elementName;
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
                throw std::runtime_error("optional type is missing its payload type");
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
            throw std::runtime_error("C function declaration is missing its name");

        std::vector<LLVMTypeRef> paramTypes;
        LLVMTypeRef returnType = LLVMVoidTypeInContext(ctx.llvmCtx);
        for (const auto &child : node.children)
        {
            if (child.type == Parser::NodeType::Param)
                paramTypes.push_back(resolve_type(ctx, child.children.front()).llvmType);
            else if (child.type == Parser::NodeType::PrimitiveType ||
                     child.type == Parser::NodeType::PointerType ||
                     child.type == Parser::NodeType::ReferenceType ||
                     child.type == Parser::NodeType::CustomType)
                returnType = resolve_type(ctx, child).llvmType;
        }

        const std::string name =
            mangle_function_name(std::string(std::get<std::string_view>(node.children.front().value)));
        LLVMTypeRef functionType = LLVMFunctionType(returnType, paramTypes.data(), paramTypes.size(), 0);
        LLVMValueRef function = LLVMGetNamedFunction(ctx.module, name.c_str());
        if (!function)
            return LLVMAddFunction(ctx.module, name.c_str(), functionType);
        if (LLVMGlobalGetValueType(function) != functionType)
            throw std::runtime_error("conflicting C function declaration for '" + name + "'");
        return function;
    }

    static void predeclare_struct_types(Context &ctx, const Parser::ASTNode &node)
    {
        if (node.type == Parser::NodeType::NamespaceDecl)
        {
            const std::string previous = ctx.currentNamespaceName;
            ctx.currentNamespaceName = qualified_declaration_name(
                ctx, std::string(std::get<std::string_view>(node.value)));
            for (const auto &child : node.children)
                predeclare_struct_types(ctx, child);
            ctx.currentNamespaceName = previous;
            return;
        }
        if (node.type == Parser::NodeType::StructDecl)
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::Identifier)
                throw std::runtime_error("struct declaration is missing its name");
            const std::string name = qualified_declaration_name(
                ctx, std::string(std::get<std::string_view>(node.children.front().value)));
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

    // Helper to resolve memory addresses for assignments and mutations
    LLVMValueRef get_lvalue(Context &ctx, const Parser::ASTNode &node,
                            RuntimeArrayIndexCapture *runtimeIndex)
    {
        if (node.type == Parser::NodeType::Identifier)
        {
            std::string name = std::string(std::get<std::string_view>(node.value));
            VarInfo *var = ctx.find_var(name);
            if (!var)
                throw std::runtime_error("undeclared identifier '" + name + "'");
            if (var->type.isOptional &&
                std::find(ctx.validPayloadAddresses.begin(), ctx.validPayloadAddresses.end(), var->address) !=
                    ctx.validPayloadAddresses.end())
            {
                return LLVMBuildStructGEP2(ctx.builder, var->type.llvmType, var->address, 1,
                                           "optionalpayloadaddr");
            }
            return var->address;
        }
        else if (node.type == Parser::NodeType::MemberAccessExpr)
        {
            if (node.children.empty() || !std::holds_alternative<std::string_view>(node.value))
                throw std::runtime_error("malformed member access");
            std::string structName = node.children[0].inferredTypeName;
            LLVMValueRef baseAddr = nullptr;
            if (node.children[0].type == Parser::NodeType::Identifier)
            {
                const std::string baseName =
                    std::string(std::get<std::string_view>(node.children[0].value));
                if (VarInfo *base = ctx.find_var(baseName))
                {
                    structName = base->type.structName;
                    baseAddr = base->type.isPointerLike
                                   ? LLVMBuildLoad2(ctx.builder, base->type.llvmType, base->address, "memberbase")
                                   : base->address;
                }
            }
            else if (node.children[0].type == Parser::NodeType::IndexExpr)
            {
                const CGType baseType = lvalue_type(ctx, node.children[0]);
                structName = baseType.structName;
                baseAddr = get_lvalue(ctx, node.children[0], runtimeIndex);
            }
            else
            {
                const CGType baseType = lvalue_type(ctx, node.children[0]);
                structName = baseType.structName;
                baseAddr = get_lvalue(ctx, node.children[0], runtimeIndex);
            }
            if (!baseAddr)
                throw std::runtime_error("member access requires an addressable struct receiver");
            std::string fieldName = std::string(std::get<std::string_view>(node.value));

            if (structTypes.count(structName))
            {
                auto &info = structTypes[structName];
                for (unsigned i = 0; i < info.fieldNames.size(); ++i)
                {
                    if (info.fieldNames[i] == fieldName)
                    {
                        return LLVMBuildStructGEP2(ctx.builder, info.llvmType, baseAddr, i,
                                                   "memberptr");
                    }
                }
            }
            throw std::runtime_error("unknown struct member '" + fieldName + "'");
        }
        else if (node.type == Parser::NodeType::IndexExpr && node.children.size() == 2)
        {
            const CGType receiverType = lvalue_type(ctx, node.children[0]);
            LLVMValueRef index = generate_node(ctx, node.children[1]);
            index = cast_value(ctx.builder, index, LLVMInt64TypeInContext(ctx.llvmCtx), false, "arrayindex");

            if (receiverType.isArray && receiverType.innerType && !receiverType.isPointerLike)
            {
                LLVMValueRef receiver = get_lvalue(ctx, node.children[0]);
                LLVMValueRef zero = LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), 0, 0);
                LLVMValueRef indices[] = {zero, index};
                return LLVMBuildGEP2(ctx.builder, receiverType.llvmType, receiver, indices, 2,
                                     "static_elementptr");
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
                LLVMValueRef receiver = get_lvalue(ctx, node.children[0]);
                LLVMValueRef base = LLVMBuildLoad2(ctx.builder, receiverType.llvmType, receiver, "pointerbase");
                return LLVMBuildGEP2(ctx.builder, receiverType.pointeeType, base, &index, 1, "pointerelementptr");
            }

            const auto classInfo = structTypes.find(receiverType.structName);
            if (classInfo == structTypes.end() || classInfo->second.indexedField.empty())
                throw std::runtime_error("indexing requires a runtime-sized array or a class with an index field");
            const auto field = std::find(classInfo->second.fieldNames.begin(),
                                         classInfo->second.fieldNames.end(),
                                         classInfo->second.indexedField);
            if (field == classInfo->second.fieldNames.end())
                throw std::runtime_error("class index field is missing from its layout");
            const size_t fieldIndex = static_cast<size_t>(field - classInfo->second.fieldNames.begin());
            const CGType &backingType = classInfo->second.fieldTypes[fieldIndex];
            if (!backingType.isPointerLike || !backingType.pointeeType)
                throw std::runtime_error("class index field must lower to a pointer");
            LLVMValueRef receiver = get_lvalue(ctx, node.children[0]);
            LLVMValueRef fieldAddress = LLVMBuildStructGEP2(ctx.builder, classInfo->second.llvmType,
                                                             receiver, fieldIndex, "indexfield");
            LLVMValueRef base = LLVMBuildLoad2(ctx.builder, backingType.llvmType, fieldAddress, "indexbase");
            return LLVMBuildGEP2(ctx.builder, backingType.pointeeType, base, &index, 1, "elementptr");
        }
        else if (node.type == Parser::NodeType::UnaryExpr)
        {
            // Dereference: *ptr = value;
            Lexer::Operator op = std::get<Lexer::Operator>(node.value);
            if (op == Lexer::Operator::MULTIPLY)
            {
                return generate_node(ctx,
                                     node.children[0]);
            }
        }
        return nullptr;
    }

    static CGType lvalue_type(Context &ctx, const Parser::ASTNode &node)
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
        if (node.type == Parser::NodeType::MemberAccessExpr && !node.children.empty())
        {
            const std::string fieldName = std::string(std::get<std::string_view>(node.value));
            CGType baseType;
            if (node.children[0].type == Parser::NodeType::Identifier)
            {
                const std::string baseName = std::string(std::get<std::string_view>(node.children[0].value));
                if (VarInfo *base = ctx.find_var(baseName))
                    baseType = base->type;
            }
            else if (node.children[0].type == Parser::NodeType::IndexExpr)
            {
                baseType = lvalue_type(ctx, node.children[0]);
            }
            else
            {
                baseType = lvalue_type(ctx, node.children[0]);
            }
            const auto info = structTypes.find(baseType.structName);
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
            const CGType array = lvalue_type(ctx, node.children[0]);
            const auto vector = structTypes.find(array.structName);
            if (array.structName.rfind("Vector.", 0) == 0 && vector != structTypes.end() &&
                !vector->second.fieldTypes.empty() && vector->second.fieldTypes.front().innerType)
            {
                return *vector->second.fieldTypes.front().innerType;
            }
            if (array.isArray && array.innerType)
            {
                return *array.innerType;
            }
            if (array.isArray && array.pointeeType)
            {
                if (array.innerType)
                    return *array.innerType;
                CGType element = array;
                element.llvmType = array.pointeeType;
                element.pointeeType = nullptr;
                element.isPointerLike = false;
                element.isArray = false;
                return element;
            }
        }
        if (node.type == Parser::NodeType::IndexExpr && node.children.size() == 2)
        {
            const CGType pointer = lvalue_type(ctx, node.children[0]);
            if (pointer.isPointerLike && pointer.pointeeType)
            {
                if (pointer.innerType)
                    return *pointer.innerType;
                CGType element = pointer;
                element.llvmType = pointer.pointeeType;
                element.pointeeType = nullptr;
                element.innerType.reset();
                element.isPointerLike = false;
                element.isArray = false;
                return element;
            }
        }
        if (node.type == Parser::NodeType::IndexExpr && node.children.size() == 2)
        {
            const CGType receiver = lvalue_type(ctx, node.children[0]);
            const auto classInfo = structTypes.find(receiver.structName);
            if (classInfo != structTypes.end() && !classInfo->second.indexedField.empty())
            {
                const auto field = std::find(classInfo->second.fieldNames.begin(),
                                             classInfo->second.fieldNames.end(),
                                             classInfo->second.indexedField);
                if (field != classInfo->second.fieldNames.end())
                {
                    const CGType &backing = classInfo->second.fieldTypes[
                        static_cast<size_t>(field - classInfo->second.fieldNames.begin())];
                    CGType element = backing;
                    element.llvmType = backing.pointeeType;
                    element.pointeeType = nullptr;
                    element.isPointerLike = false;
                    return element;
                }
            }
        }
        if (node.type == Parser::NodeType::BinaryExpr && node.children.size() == 2 &&
            node.value.index() == 5)
        {
            const Lexer::Operator op = std::get<Lexer::Operator>(node.value);
            const CGType pointer = lvalue_type(ctx, node.children[0]);
            if (pointer.isPointerLike && pointer.pointeeType &&
                (op == Lexer::Operator::PLUS || op == Lexer::Operator::MINUS))
                return pointer;
        }
        if (node.type == Parser::NodeType::UnaryExpr && !node.children.empty() &&
            std::get<Lexer::Operator>(node.value) == Lexer::Operator::MULTIPLY)
        {
            const CGType pointer = lvalue_type(ctx, node.children[0]);
            if (pointer.isPointerLike && pointer.pointeeType)
            {
                CGType pointee = pointer;
                pointee.llvmType = pointer.pointeeType;
                pointee.pointeeType = nullptr;
                pointee.innerType.reset();
                pointee.isPointerLike = false;
                return pointee;
            }
        }
        throw std::runtime_error("assignment target has no known type");
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
        if (node.type == Parser::NodeType::BinaryExpr && !node.children.empty())
            return is_signed_expression(ctx, node.children.front());
        try
        {
            return lvalue_type(ctx, node).isSigned;
        }
        catch (const std::runtime_error &)
        {
            return true;
        }
    }

    static LLVMValueRef generate_binary(Context &ctx, Lexer::Operator op, LLVMValueRef lhs,
                                        LLVMValueRef rhs, bool isSigned)
    {
        if (!lhs || !rhs)
            throw std::runtime_error("missing operand in binary expression");

        rhs = cast_value(ctx.builder, rhs, LLVMTypeOf(lhs), isSigned, "binarycast");
        const LLVMTypeKind kind = LLVMGetTypeKind(LLVMTypeOf(lhs));
        const bool isFloat = kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind;

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
                throw std::runtime_error("unsupported floating-point operator");
            }
        }

        if (kind != LLVMIntegerTypeKind)
            throw std::runtime_error("unsupported binary operand type");

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
            return LLVMBuildICmp(ctx.builder, isSigned ? LLVMIntSLT : LLVMIntULT, lhs, rhs,
                                 "lttmp");
        case Lexer::Operator::LESS_EQUAL:
            return LLVMBuildICmp(ctx.builder, isSigned ? LLVMIntSLE : LLVMIntULE, lhs, rhs,
                                 "letmp");
        case Lexer::Operator::GREATER_THAN:
            return LLVMBuildICmp(ctx.builder, isSigned ? LLVMIntSGT : LLVMIntUGT, lhs, rhs,
                                 "gttmp");
        case Lexer::Operator::GREATER_EQUAL:
            return LLVMBuildICmp(ctx.builder, isSigned ? LLVMIntSGE : LLVMIntUGE, lhs, rhs,
                                 "getmp");
        default:
            throw std::runtime_error("unsupported integer operator");
        }
    }

    static bool is_string_aggregate(const CGType &type)
    {
        return type.structName == "str" || type.structName == "String";
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
            return static_cast<uint64_t>(LLVMGetArrayLength(type.llvmType)) *
                   cleanup_leaf_count(ctx, *type.innerType);
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
            LLVMValueRef payload = LLVMBuildStructGEP2(ctx.builder, type.llvmType, address, 1,
                                                       "runtime_cleanup_optional_payload");
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
                LLVMValueRef element = LLVMBuildGEP2(ctx.builder, type.llvmType, address, indices, 2,
                                                      "runtime_cleanup_element");
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

        value.activeFlag = LLVMBuildAlloca(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx),
                                           (name + ".dropactive").c_str());
        LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx),
                                                    type.isPointerLike ? 0 : 1, 0),
                       value.activeFlag);
        if (type.isPointerLike)
            return value;

        if (type.isOptional && type.innerType)
        {
            LLVMValueRef payload = LLVMBuildStructGEP2(ctx.builder, type.llvmType, address, 1,
                                                       "cleanup_optional_payload");
            value.children.push_back(create_cleanup_value(ctx, payload, *type.innerType,
                                                          name + ".payload"));
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
                LLVMValueRef element = LLVMBuildGEP2(ctx.builder, type.llvmType, address, indices, 2,
                                                      "cleanup_element");
                value.children.push_back(create_cleanup_value(
                    ctx, element, *type.innerType, name + "." + std::to_string(index)));
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
            value.children.push_back(create_cleanup_value(
                ctx, field, definition->second.fieldTypes[index],
                name + "." + definition->second.fieldNames[index]));
        }
        return value;
    }

    static void collect_cleanup_token_addresses(const CleanupValue &value,
                                                std::vector<LLVMValueRef> &tokens)
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
            LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 1, 0),
                           value.activeFlag);
        for (CleanupValue &child : value.children)
            arm_cleanup_value(ctx, child);
    }

    static void initialize_runtime_cleanup_flags(Context &ctx, CleanupValue &value,
                                                 LLVMValueRef elementCount, const std::string &name)
    {
        if (!value.type.innerType)
            throw std::runtime_error("runtime array cleanup requires an element type");
        const uint64_t leafCount = cleanup_leaf_count(ctx, *value.type.innerType);
        if (leafCount == 0)
            return;

        const LLVMTypeRef i1 = LLVMInt1TypeInContext(ctx.llvmCtx);
        const LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx.llvmCtx);
        value.runtimeElementCount = elementCount;
        value.runtimeCleanupLeafCount = leafCount;
        LLVMValueRef flagCount = LLVMBuildMul(ctx.builder, elementCount,
                                              LLVMConstInt(i64, leafCount, 0), "runtimecleanupflagcount");
        value.runtimeLeafActiveFlags = LLVMBuildArrayAlloca(ctx.builder, i1, flagCount,
                                                            (name + ".runtimecleanupflags").c_str());
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
        LLVMValueRef flag = LLVMBuildGEP2(ctx.builder, i1, value.runtimeLeafActiveFlags, &index, 1,
                                          "runtimecleanupinitflag");
        LLVMBuildStore(ctx.builder, LLVMConstInt(i1, 0, 0), flag);
        LLVMBuildStore(ctx.builder, LLVMBuildAdd(ctx.builder, index, LLVMConstInt(i64, 1, 0),
                                                 "runtimecleanupinitnext"), cursor);
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
                LLVMValueRef element = LLVMBuildGEP2(ctx.builder, type.llvmType, address, indices, 2,
                                                      "dropelement");
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
        LLVMBasicBlockRef done =
            LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "runtimeleafcontinue");
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
        LLVMValueRef first = LLVMBuildSub(ctx.builder, value.runtimeElementCount,
                                          LLVMConstInt(i64, 1, 0), "runtimecleanupfirst");
        LLVMValueRef hasElements = LLVMBuildICmp(ctx.builder, LLVMIntNE, value.runtimeElementCount,
                                                 LLVMConstInt(i64, 0, 0), "runtimecleanuphasitems");
        LLVMBasicBlockRef preheader = LLVMGetInsertBlock(ctx.builder);
        LLVMBasicBlockRef loop =
            LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "runtimecleanuploop");
        LLVMBasicBlockRef done =
            LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "runtimecleanupdone");
        LLVMBuildCondBr(ctx.builder, hasElements, loop, done);

        LLVMPositionBuilderAtEnd(ctx.builder, loop);
        LLVMValueRef index = LLVMBuildPhi(ctx.builder, i64, "runtimecleanupindex");
        LLVMAddIncoming(index, &first, &preheader, 1);
        LLVMValueRef base = LLVMBuildLoad2(ctx.builder, value.type.llvmType, value.address, "runtimecleanupbase");
        LLVMValueRef element = LLVMBuildGEP2(ctx.builder, value.type.pointeeType, base, &index, 1,
                                             "runtimecleanupelement");
        std::vector<CleanupLeaf> leaves;
        collect_cleanup_leaves(ctx, element, *value.type.innerType, leaves);
        if (leaves.size() != value.runtimeCleanupLeafCount)
            throw std::runtime_error("runtime array cleanup leaf layout changed after declaration");
        LLVMValueRef offset = LLVMBuildMul(ctx.builder, index,
                                           LLVMConstInt(i64, value.runtimeCleanupLeafCount, 0),
                                           "runtimecleanupleafoffset");
        for (size_t leaf = leaves.size(); leaf-- > 0;)
        {
            LLVMValueRef flagIndex = LLVMBuildAdd(ctx.builder, offset,
                LLVMConstInt(i64, static_cast<uint64_t>(leaf), 0), "runtimecleanupleafindex");
            LLVMValueRef flag = LLVMBuildGEP2(ctx.builder, i1, value.runtimeLeafActiveFlags, &flagIndex, 1,
                                              "runtimecleanupleafflag");
            emit_cleanup_leaf_if_active(ctx, leaves[leaf].address, leaves[leaf].type, flag);
        }
        LLVMValueRef next = LLVMBuildSub(ctx.builder, index, LLVMConstInt(i64, 1, 0), "runtimecleanupnext");
        LLVMValueRef more = LLVMBuildICmp(ctx.builder, LLVMIntNE, index, LLVMConstInt(i64, 0, 0),
                                          "runtimecleanupmore");
        LLVMBasicBlockRef loopEnd = LLVMGetInsertBlock(ctx.builder);
        LLVMBuildCondBr(ctx.builder, more, loop, done);
        LLVMAddIncoming(index, &next, &loopEnd, 1);
        LLVMPositionBuilderAtEnd(ctx.builder, done);
    }

    static void emit_cleanup_value(Context &ctx, const CleanupValue &value)
    {
        if (!value.address || !value.activeFlag)
            return;

        LLVMValueRef active = LLVMBuildLoad2(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx),
                                             value.activeFlag, "dropactive");
        LLVMBasicBlockRef cleanupBB =
            LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "dropcleanup");
        LLVMBasicBlockRef continueBB =
            LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "dropcontinue");
        LLVMBuildCondBr(ctx.builder, active, cleanupBB, continueBB);

        LLVMPositionBuilderAtEnd(ctx.builder, cleanupBB);
        if (value.runtimeElementCount && value.runtimeLeafActiveFlags)
            emit_runtime_array_cleanup(ctx, value);
        else if (value.children.empty())
            emit_cleanup_for_type(ctx, value.address, value.type);
        else
            for (auto child = value.children.rbegin(); child != value.children.rend(); ++child)
                emit_cleanup_value(ctx, *child);
        LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 0, 0),
                       value.activeFlag);
        LLVMBuildBr(ctx.builder, continueBB);
        LLVMPositionBuilderAtEnd(ctx.builder, continueBB);
    }

    static void emit_cleanup_scopes(Context &ctx, size_t retainedScopeCount)
    {
        for (size_t scopeIndex = ctx.cleanupScopes.size(); scopeIndex-- > retainedScopeCount;)
        {
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
                                                const RuntimeArrayIndexCapture &capturedIndex,
                                                CleanupValue *&root, CGType &current, uint64_t &leafOrdinal)
    {
        if (node.type == Parser::NodeType::IndexExpr && node.children.size() == 2 &&
            node.children[0].type == Parser::NodeType::Identifier &&
            capturedIndex.node == &node.children[1] && capturedIndex.value)
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
            if (!current.isArray || !current.innerType ||
                LLVMGetTypeKind(current.llvmType) != LLVMArrayTypeKind ||
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

    static RuntimeCleanupTarget runtime_cleanup_target_for_lvalue(
        Context &ctx, const Parser::ASTNode &node, const RuntimeArrayIndexCapture &capturedIndex)
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
        LLVMValueRef offset = LLVMBuildMul(ctx.builder, capturedIndex.value,
                                           LLVMConstInt(i64, root->runtimeCleanupLeafCount, 0),
                                           "runtimeassignmentleafoffset");
        LLVMValueRef flagIndex = LLVMBuildAdd(ctx.builder, offset, LLVMConstInt(i64, leafOrdinal, 0),
                                              "runtimeassignmentleafindex");
        return {LLVMBuildGEP2(ctx.builder, i1, root->runtimeLeafActiveFlags, &flagIndex, 1,
                              "runtimeassignmentleafflag"), type};
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
            const auto field = std::find(definition->second.fieldNames.begin(),
                                        definition->second.fieldNames.end(), fieldName);
            if (field == definition->second.fieldNames.end())
                return nullptr;
            const size_t index = static_cast<size_t>(field - definition->second.fieldNames.begin());
            return index < parent->children.size() ? &parent->children[index] : nullptr;
        }
        if (node.type == Parser::NodeType::IndexExpr && node.children.size() == 2)
        {
            CleanupValue *parent = cleanup_value_for_lvalue(ctx, node.children.front());
            if (!parent || !parent->type.isArray ||
                node.children[1].type != Parser::NodeType::IntegerLiteral)
                return nullptr;
            const uint64_t index = std::get<uint64_t>(node.children[1].value);
            return index < parent->children.size() ? &parent->children[static_cast<size_t>(index)]
                                                    : nullptr;
        }
        return nullptr;
    }

    static void disarm_cleanup_at_address(Context &ctx, CleanupValue &value, LLVMValueRef address,
                                          const CGType &targetType)
    {
        if (value.activeFlag && value.type.llvmType == targetType.llvmType)
        {
            LLVMValueRef matches = LLVMBuildICmp(ctx.builder, LLVMIntEQ, value.address, address,
                                                 "moved_cleanup_slot");
            LLVMValueRef active = LLVMBuildLoad2(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx),
                                                  value.activeFlag, "moved_cleanup_active");
            LLVMBuildStore(ctx.builder,
                           LLVMBuildSelect(ctx.builder, matches,
                                           LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 0, 0), active,
                                           "moved_cleanup_state"),
                           value.activeFlag);
        }
        for (CleanupValue &child : value.children)
            disarm_cleanup_at_address(ctx, child, address, targetType);
    }

    static void disarm_cleanup_value(Context &ctx, CleanupValue &value)
    {
        if (value.activeFlag)
            LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 0, 0),
                           value.activeFlag);
    }

    static LLVMValueRef printf_runtime_function(Context &ctx, const char *name, LLVMTypeRef result,
                                                 const std::vector<LLVMTypeRef> &parameters)
    {
        LLVMValueRef function = LLVMGetNamedFunction(ctx.module, name);
        if (function) return function;
        return LLVMAddFunction(ctx.module, name,
                               LLVMFunctionType(result, const_cast<LLVMTypeRef *>(parameters.data()),
                                                parameters.size(), 0));
    }

    static LLVMValueRef emit_printf_bytes(Context &ctx, LLVMValueRef data, LLVMValueRef length)
    {
        LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx.llvmCtx);
        LLVMValueRef write = printf_runtime_function(ctx, "__sys_write", i64, {i64, LLVMPointerTypeInContext(ctx.llvmCtx, 0), i64});
        LLVMValueRef args[] = {LLVMConstInt(i64, 1, 0), data, cast_value(ctx.builder, length, i64, false, "printflength")};
        return LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(write), write, args, 3, "printfwrite");
    }

    static LLVMValueRef emit_printf_literal(Context &ctx, const std::string &text)
    {
        if (text.empty()) return LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), 0, 0);
        return emit_printf_bytes(ctx, LLVMBuildGlobalStringPtr(ctx.builder, text.c_str(), "printftext"),
                                 LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), text.size(), 0));
    }

    static std::string printf_type_tag(const CGType &type)
    {
        if (type.structName == "str" || type.structName == "String") return type.structName;
        if (type.isBool) return "bool";
        if (type.isChar) return "char";
        if (type.isFloat) return LLVMGetTypeKind(type.llvmType) == LLVMFloatTypeKind ? "f32" : "f64";
        if (LLVMGetTypeKind(type.llvmType) == LLVMIntegerTypeKind)
        {
            switch (LLVMGetIntTypeWidth(type.llvmType))
            {
            case 8: return type.isSigned ? "i8" : "u8";
            case 16: return type.isSigned ? "i16" : "u16";
            case 32: return type.isSigned ? "i32" : "u32";
            case 64: return type.isSigned ? "i64" : "u64";
            }
        }
        return {};
    }

    static LLVMValueRef emit_printf_value(Context &ctx, const Parser::ASTNode &node, const CGType &type)
    {
        LLVMTypeRef i8 = LLVMInt8TypeInContext(ctx.llvmCtx), i64 = LLVMInt64TypeInContext(ctx.llvmCtx);
        LLVMTypeRef pointer = LLVMPointerTypeInContext(ctx.llvmCtx, 0);
        LLVMValueRef value = generate_node(ctx, node);
        if (type.structName == "str" || type.structName == "String")
            return emit_printf_bytes(ctx, LLVMBuildExtractValue(ctx.builder, value, 0, "printfdata"), LLVMBuildExtractValue(ctx.builder, value, 1, "printflength"));
        if (type.isBool)
        {
            LLVMValueRef data = LLVMBuildSelect(ctx.builder, value, LLVMBuildGlobalStringPtr(ctx.builder, "true", "printftrue"), LLVMBuildGlobalStringPtr(ctx.builder, "false", "printffalse"), "printfbooldata");
            LLVMValueRef length = LLVMBuildSelect(ctx.builder, value, LLVMConstInt(i64, 4, 0), LLVMConstInt(i64, 5, 0), "printfboollength");
            return emit_printf_bytes(ctx, data, length);
        }
        if (type.isFloat)
        {
            LLVMValueRef buffer = LLVMBuildArrayAlloca(ctx.builder, i8, LLVMConstInt(i64, 64, 0), "printfbuffer");
            LLVMValueRef convert = printf_runtime_function(ctx, "__sys_float_to_string", i64, {LLVMDoubleTypeInContext(ctx.llvmCtx), pointer, i64});
            LLVMValueRef args[] = {cast_value(ctx.builder, value, LLVMDoubleTypeInContext(ctx.llvmCtx), true, "printffloat"), buffer, LLVMConstInt(i64, 64, 0)};
            return emit_printf_bytes(ctx, buffer, LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(convert), convert, args, 3, "printfconverted"));
        }
        if (printf_type_tag(type) == "char")
        {
            LLVMValueRef buffer = LLVMBuildAlloca(ctx.builder, i8, "printfchar");
            LLVMBuildStore(ctx.builder, cast_value(ctx.builder, value, i8, false, "printfcharcast"), buffer);
            return emit_printf_bytes(ctx, buffer, LLVMConstInt(i64, 1, 0));
        }
        if (LLVMGetTypeKind(type.llvmType) == LLVMIntegerTypeKind)
        {
            LLVMValueRef buffer = LLVMBuildArrayAlloca(ctx.builder, i8, LLVMConstInt(i64, 32, 0), "printfbuffer");
            const bool signedValue = type.isSigned;
            LLVMValueRef convert = printf_runtime_function(ctx, signedValue ? "__sys_int_to_string" : "__sys_uint_to_string", i64, {i64, pointer, i64});
            LLVMValueRef args[] = {cast_value(ctx.builder, value, i64, signedValue, "printfinteger"), buffer, LLVMConstInt(i64, 32, 0)};
            return emit_printf_bytes(ctx, buffer, LLVMBuildCall2(ctx.builder, LLVMGlobalGetValueType(convert), convert, args, 3, "printfconverted"));
        }
        throw std::runtime_error("printf supports only primitives, str, and String");
    }

    static LLVMValueRef emit_printf(Context &ctx, const Parser::ASTNode &node)
    {
        if (!ctx.stdlibEnabled) throw std::runtime_error("printf requires the standard library");
        if (node.children.size() < 2 || node.children[1].type != Parser::NodeType::StringLiteral)
            throw std::runtime_error("printf requires a string-literal format argument");
        const std::string format = decode_string_literal(std::get<std::string_view>(node.children[1].value));
        size_t argument = 2, cursor = 0;
        LLVMValueRef total = LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), 0, 0);
        auto add = [&](LLVMValueRef written) { total = LLVMBuildAdd(ctx.builder, total, written, "printfcount"); };
        while (cursor < format.size())
        {
            const size_t open = format.find('{', cursor);
            if (open == std::string::npos) { add(emit_printf_literal(ctx, format.substr(cursor))); break; }
            add(emit_printf_literal(ctx, format.substr(cursor, open - cursor)));
            const size_t close = format.find('}', open + 1);
            if (close == std::string::npos) throw std::runtime_error("printf format has an unclosed placeholder");
            if (argument >= node.children.size()) throw std::runtime_error("printf format has more placeholders than arguments");
            const CGType type = lvalue_type(ctx, node.children[argument]);
            const std::string tag = format.substr(open + 1, close - open - 1);
            const bool usizePlaceholder = tag == "usize" && LLVMGetTypeKind(type.llvmType) == LLVMIntegerTypeKind &&
                                          LLVMGetIntTypeWidth(type.llvmType) == ctx.targetPointerWidthBits;
            if (printf_type_tag(type) != tag && !usizePlaceholder)
                throw std::runtime_error("printf placeholder '" + tag + "' does not match its argument type");
            add(emit_printf_value(ctx, node.children[argument++], type));
            cursor = close + 1;
        }
        if (argument != node.children.size()) throw std::runtime_error("printf has more arguments than placeholders");
        return total;
    }

    LLVMValueRef generate_node(Context &ctx, const Parser::ASTNode &node)
    {
        switch (node.type)
        {

        case Parser::NodeType::Module:
        case Parser::NodeType::ExportDecl:
        case Parser::NodeType::UsingMacroDecl:
            for (const auto &child : node.children)
                generate_node(ctx, child);
            return nullptr;

        case Parser::NodeType::NamespaceDecl:
        {
            const std::string previous = ctx.currentNamespaceName;
            ctx.currentNamespaceName = qualified_declaration_name(
                ctx, std::string(std::get<std::string_view>(node.value)));
            for (const auto &child : node.children)
                generate_node(ctx, child);
            ctx.currentNamespaceName = previous;
            return nullptr;
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
                emit_current_scope_cleanup(ctx);
            ctx.pop_scope();
            return nullptr;

        case Parser::NodeType::StructDecl:
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::Identifier)
            {
                std::cerr << "Codegen Error: Struct declaration is missing its name.\n";
                return nullptr;
            }
            std::string name = qualified_declaration_name(
                ctx, std::string(std::get<std::string_view>(node.children.front().value)));
            StructInfo info;
            info.requestedAlignment = node.requestedAlignment;
            info.baseClassName = node.baseClassName;
            const auto predeclared = structTypes.find(name);
            info.llvmType = predeclared == structTypes.end()
                                ? LLVMStructCreateNamed(ctx.llvmCtx, name.c_str())
                                : predeclared->second.llvmType;

            std::vector<LLVMTypeRef> fieldTypes;
            if (!node.baseClassName.empty())
            {
                const auto base = structTypes.find(node.baseClassName);
                if (base == structTypes.end())
                    throw std::runtime_error("base class '" + node.baseClassName + "' must be declared before its derived class");
                info.fieldNames = base->second.fieldNames;
                info.fieldTypeNodes = base->second.fieldTypeNodes;
                info.fieldTypes = base->second.fieldTypes;
                info.indexedField = base->second.indexedField;
                info.initializedField = base->second.initializedField;
                for (const CGType &fieldType : info.fieldTypes)
                    fieldTypes.push_back(fieldType.llvmType);
            }
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
            structTypes[name] = info;
            const std::string enclosingClass = ctx.currentClassName;
            ctx.currentClassName = name;
            for (const auto &child : node.children)
            {
                if (child.type == Parser::NodeType::FunctionDef || child.type == Parser::NodeType::FunctionDecl)
                    generate_node(ctx, child);
            }
            ctx.currentClassName = enclosingClass;
            return nullptr;
        }

        case Parser::NodeType::EnumDecl:
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::Identifier)
            {
                std::cerr << "Codegen Error: Enum declaration is missing its name.\n";
                return nullptr;
            }
            std::string name = qualified_declaration_name(
                ctx, std::string(std::get<std::string_view>(node.children.front().value)));
            EnumInfo info;
            info.backingType.llvmType = LLVMInt32TypeInContext(ctx.llvmCtx);
            info.backingType.isSigned = true;
            size_t firstMember = 1;
            if (node.children.size() > 1 &&
                (node.children[1].type == Parser::NodeType::PrimitiveType ||
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
                std::string memberName =
                    std::string(std::get<std::string_view>(child.children.front().value));
                if (child.children.size() > 1)
                    counter = evaluate_enum_constant(child.children[1], info);
                info.members[memberName] = counter++;
            }
            enumTypes[name] = info;
            return nullptr;
        }

        case Parser::NodeType::IntegerLiteral:
            return LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), std::get<uint64_t>(node.value),
                                0);
        case Parser::NodeType::FloatLiteral:
            return LLVMConstReal(LLVMDoubleTypeInContext(ctx.llvmCtx),
                                 std::get<double>(node.value));
        case Parser::NodeType::BoolLiteral:
            return LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx),
                                std::get<bool>(node.value) ? 1 : 0, 0);
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
                throw std::runtime_error("empty character literal");
            unsigned char value = static_cast<unsigned char>(text.front());
            if (text.front() == '\\' && text.size() == 2)
            {
                switch (text[1])
                {
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                case 't': value = '\t'; break;
                case '0': value = '\0'; break;
                default: value = static_cast<unsigned char>(text[1]); break;
                }
            }
            return LLVMConstInt(LLVMInt32TypeInContext(ctx.llvmCtx), value, 0);
        }

        case Parser::NodeType::Identifier:
        {
            std::string name = std::string(std::get<std::string_view>(node.value));
            VarInfo *var = ctx.find_var(name);
            if (!var)
                throw std::runtime_error("undeclared identifier '" + name + "'");
            if (var->type.isOptional)
            {
                if (!var->type.innerType)
                    throw std::runtime_error("optional variable has no payload type");
                LLVMValueRef payload = LLVMBuildStructGEP2(ctx.builder, var->type.llvmType, var->address, 1,
                                                            "optionalpayloadaddr");
                return LLVMBuildLoad2(ctx.builder, var->type.innerType->llvmType, payload, name.c_str());
            }
            return LLVMBuildLoad2(ctx.builder, var->type.llvmType, var->address, name.c_str());
        }

        case Parser::NodeType::VariableDecl:
        {
            std::string name = std::string(std::get<std::string_view>(node.value));
            CGType cgType = resolve_type(ctx, node.children[0]);

            LLVMValueRef alloca = LLVMBuildAlloca(ctx.builder, cgType.llvmType, name.c_str());
            LLVMValueRef runtimeArrayCount = nullptr;
            if (cgType.isArray && cgType.isPointerLike && cgType.innerType &&
                !cgType.runtimeArrayLengthName.empty())
            {
                VarInfo *length = ctx.find_var(cgType.runtimeArrayLengthName);
                if (!length)
                    throw std::runtime_error("runtime array length binding is not declared");
                LLVMValueRef count = LLVMBuildLoad2(ctx.builder, length->type.llvmType, length->address,
                                                    "runtimearraycount");
                count = cast_value(ctx.builder, count, LLVMInt64TypeInContext(ctx.llvmCtx),
                                   length->type.isSigned, "runtimearraycountcast");
                LLVMValueRef storage = LLVMBuildArrayAlloca(ctx.builder, cgType.innerType->llvmType, count,
                                                             "runtimearraystorage");
                LLVMBuildStore(ctx.builder, storage, alloca);
                runtimeArrayCount = count;
            }
            if (cgType.requiredAlignment != 0)
                LLVMSetAlignment(alloca, static_cast<unsigned>(cgType.requiredAlignment));
            const bool stackRuntimeArray = cgType.isArray && cgType.isPointerLike &&
                                           !cgType.runtimeArrayLengthName.empty();
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
                    throw std::runtime_error("brace initialization requires a declared struct or class type");
                if (classInfo->second.initializedField.empty())
                {
                    if (node.children[1].children.size() != classInfo->second.fieldTypes.size())
                        throw std::runtime_error("brace initializer field count does not match its type");
                    LLVMBuildStore(ctx.builder, LLVMConstNull(cgType.llvmType), alloca);
                    for (size_t index = 0; index < classInfo->second.fieldTypes.size(); ++index)
                    {
                        const CGType &fieldType = classInfo->second.fieldTypes[index];
                        LLVMValueRef fieldAddress = LLVMBuildStructGEP2(ctx.builder, cgType.llvmType, alloca,
                                                                        index, "structinitfield");
                        LLVMValueRef value = generate_value_for_target(
                            ctx, node.children[1].children[index], fieldType);
                        if (!value)
                            throw std::runtime_error("brace initializer field did not produce a value");
                        LLVMBuildStore(ctx.builder, materialize_value(ctx, value, fieldType, "structinitcast"),
                                       fieldAddress);
                    }
                    return nullptr;
                }
                const auto field = std::find(classInfo->second.fieldNames.begin(),
                                             classInfo->second.fieldNames.end(),
                                             classInfo->second.initializedField);
                if (field == classInfo->second.fieldNames.end())
                    throw std::runtime_error("class init field is missing from its layout");
                const size_t fieldIndex = static_cast<size_t>(field - classInfo->second.fieldNames.begin());
                const CGType &backingType = classInfo->second.fieldTypes[fieldIndex];
                if (!backingType.isPointerLike || !backingType.pointeeType)
                    throw std::runtime_error("class init field must lower to a pointer");

                LLVMBuildStore(ctx.builder, LLVMConstNull(cgType.llvmType), alloca);
                LLVMValueRef count = LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx),
                                                  node.children[1].children.size(), 0);
                LLVMValueRef byteCount = LLVMBuildMul(ctx.builder, LLVMSizeOf(backingType.pointeeType),
                                                      count, "initbytes");
                LLVMValueRef alloc = LLVMGetNamedFunction(ctx.module, "__shaft_alloc");
                if (!alloc)
                    throw std::runtime_error("class brace initialization requires __shaft_alloc");
                LLVMTypeRef allocArg = LLVMInt64TypeInContext(ctx.llvmCtx);
                LLVMTypeRef allocType = LLVMFunctionType(
                    LLVMPointerType(LLVMInt8TypeInContext(ctx.llvmCtx), 0), &allocArg, 1, 0);
                LLVMValueRef data = LLVMBuildCall2(ctx.builder, allocType, alloc, &byteCount, 1, "initdata");
                LLVMValueRef dataField = LLVMBuildStructGEP2(ctx.builder, cgType.llvmType, alloca,
                                                              fieldIndex, "initfield");
                LLVMBuildStore(ctx.builder, data, dataField);
                for (size_t index = 0; index < node.children[1].children.size(); ++index)
                {
                    LLVMValueRef elementIndex = LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), index, 0);
                    LLVMValueRef elementAddress = LLVMBuildGEP2(ctx.builder, backingType.pointeeType, data,
                                                                 &elementIndex, 1, "initelement");
                    LLVMValueRef value = generate_node(ctx, node.children[1].children[index]);
                    LLVMBuildStore(ctx.builder,
                                   cast_value(ctx.builder, value, backingType.pointeeType,
                                              backingType.isSigned, "initelementcast"),
                                   elementAddress);
                }
                for (size_t index = 0; index < classInfo->second.fieldNames.size(); ++index)
                {
                    const std::string &fieldName = classInfo->second.fieldNames[index];
                    if (fieldName != "length" && fieldName != "capacity")
                        continue;
                    const CGType &fieldType = classInfo->second.fieldTypes[index];
                    LLVMValueRef metadata = LLVMBuildStructGEP2(ctx.builder, cgType.llvmType, alloca,
                                                                 index, "initmetadata");
                    LLVMBuildStore(ctx.builder,
                                   cast_value(ctx.builder, count, fieldType.llvmType,
                                              fieldType.isSigned, "initmetadatacast"),
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
                    throw std::runtime_error("initializer did not produce a value");
                initVal = materialize_value(ctx, initVal, cgType, "initcast");
                LLVMBuildStore(ctx.builder, initVal, alloca);
            }
            return nullptr;
        }

        case Parser::NodeType::AssignmentExpr:
        {
            if (node.children.size() < 2)
                throw std::runtime_error("malformed assignment expression");
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
                throw std::runtime_error("move into a runtime array cleanup leaf is not supported");
            LLVMValueRef rval = generate_value_for_target(ctx, node.children[1], targetType);
            if (!rval)
                throw std::runtime_error("assignment right-hand side did not produce a value");

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
                    throw std::runtime_error("unsupported assignment operator");
                }
                LLVMValueRef current =
                    LLVMBuildLoad2(ctx.builder, targetType.llvmType, lval, "assigntmp");
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
                    // A runtime array slot without an exact active token may contain
                    // a borrowed or uninitialized pointer. Never infer ownership
                    // from its shape and structurally free it as a fallback.
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
                try
                {
                    const CGType leftType = leftLiteral ? CGType{} : lvalue_type(ctx, node.children[0]);
                    const CGType rightType = rightLiteral ? CGType{} : lvalue_type(ctx, node.children[1]);
                    if ((leftLiteral || is_string_aggregate(leftType)) &&
                        (rightLiteral || is_string_aggregate(rightType)))
                    {
                        const auto string_data_and_length = [&](const Parser::ASTNode &operand,
                                                                LLVMValueRef value, const char *dataName,
                                                                const char *lengthName)
                        {
                            if (operand.type == Parser::NodeType::StringLiteral)
                            {
                                const std::string text = decode_string_literal(std::get<std::string_view>(operand.value));
                                return std::pair<LLVMValueRef, LLVMValueRef>{
                                    value, LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), text.size(), 0)};
                            }
                            return std::pair<LLVMValueRef, LLVMValueRef>{
                                LLVMBuildExtractValue(ctx.builder, value, 0, dataName),
                                LLVMBuildExtractValue(ctx.builder, value, 1, lengthName)};
                        };
                        const auto left = string_data_and_length(node.children[0], lhs,
                                                                 "stringlhsdata", "stringlhslength");
                        const auto right = string_data_and_length(node.children[1], rhs,
                                                                  "stringrhsdata", "stringrhslength");
                        return generate_string_equality(ctx, left.first, left.second, right.first, right.second,
                                                        op == Lexer::Operator::NOT_EQUAL);
                    }
                }
                catch (const std::runtime_error &)
                {
                    // Non-lvalue expressions use the existing scalar comparison path.
                }
            }
            if (op == Lexer::Operator::PLUS || op == Lexer::Operator::MINUS)
            {
                try
                {
                    const CGType pointer = lvalue_type(ctx, node.children[0]);
                    if (pointer.isPointerLike && pointer.pointeeType)
                    {
                        LLVMTypeRef indexType = LLVMInt64TypeInContext(ctx.llvmCtx);
                        rhs = cast_value(ctx.builder, rhs, indexType, is_signed_expression(ctx, node.children[1]),
                                         "pointer_offset");
                        if (op == Lexer::Operator::MINUS)
                            rhs = LLVMBuildNeg(ctx.builder, rhs, "negative_pointer_offset");
                        return LLVMBuildGEP2(ctx.builder, pointer.pointeeType, lhs, &rhs, 1, "pointer_offset");
                    }
                }
                catch (const std::runtime_error &)
                {
                    // Scalar expressions are not lvalues with a pointer element type.
                }
            }
            return generate_binary(ctx, op, lhs, rhs, is_signed_expression(ctx, node.children[0]));
        }

        case Parser::NodeType::UnaryExpr:
        {
            Lexer::Operator op = std::get<Lexer::Operator>(node.value);
            if (op == Lexer::Operator::AMPERSAND)
            { // Address-of (&x)
                return get_lvalue(ctx, node.children[0]);
            }
            if (op == Lexer::Operator::MULTIPLY)
            { // Dereference (*x)
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
                throw std::runtime_error("sizeof requires exactly one type");
            return LLVMSizeOf(resolve_type(ctx, node.children.front()).llvmType);
        }

        case Parser::NodeType::MemberAccessExpr:
        {
            LLVMValueRef lval = get_lvalue(ctx, node);
            if (node.children.empty())
                throw std::runtime_error("malformed member access");
            const CGType memberBase = lvalue_type(ctx, node.children[0]);
            if (!structTypes.count(memberBase.structName))
                throw std::runtime_error("member access requires a known struct type");
            const auto &info = structTypes.at(memberBase.structName);
            const std::string fieldName = std::string(std::get<std::string_view>(node.value));
            for (size_t i = 0; i < info.fieldNames.size(); ++i)
            {
                if (info.fieldNames[i] == fieldName)
                    return LLVMBuildLoad2(ctx.builder, info.fieldTypes[i].llvmType, lval, "memberload");
            }
            throw std::runtime_error("unknown struct member '" + fieldName + "'");
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
                throw std::runtime_error("malformed scope access");
            std::string enumName = scope_access_name(node.children[0]);
            std::string memberName = std::string(std::get<std::string_view>(node.value));
            if (enumTypes.count(enumName))
            {
                const auto member = enumTypes[enumName].members.find(memberName);
                if (member != enumTypes[enumName].members.end())
                    return LLVMConstInt(enumTypes[enumName].backingType.llvmType, member->second, 0);
            }
            throw std::runtime_error("unknown enum member '" + enumName + "::" + memberName + "'");
        }

        case Parser::NodeType::ExprStmt:
            if (!node.children.empty())
                generate_node(ctx, node.children[0]);
            return nullptr;

        case Parser::NodeType::IfStmt:
        {
            LLVMValueRef cond = generate_node(ctx, node.children[0]);
            cond = LLVMBuildICmp(ctx.builder, LLVMIntNE, cond, LLVMConstNull(LLVMTypeOf(cond)),
                                 "ifcond");

            LLVMBasicBlockRef thenBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "then");
            LLVMBasicBlockRef elseBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "else");
            LLVMBasicBlockRef mergeBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "ifcont");

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
            LLVMBasicBlockRef condBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "whilecond");
            LLVMBasicBlockRef loopBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "whileloop");
            LLVMBasicBlockRef afterBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "whileafter");

            LLVMBuildBr(ctx.builder, condBB);
            LLVMPositionBuilderAtEnd(ctx.builder, condBB);

            LLVMValueRef cond = generate_node(ctx, node.children[0]);
            cond = LLVMBuildICmp(ctx.builder, LLVMIntNE, cond, LLVMConstNull(LLVMTypeOf(cond)),
                                 "loopcond");
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
                throw std::runtime_error("malformed for loop");
            ctx.push_scope();
            generate_node(ctx, node.children[0]);

            LLVMBasicBlockRef condBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "forcond");
            LLVMBasicBlockRef bodyBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "forbody");
            LLVMBasicBlockRef postBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "forpost");
            LLVMBasicBlockRef afterBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "forafter");
            LLVMBuildBr(ctx.builder, condBB);

            LLVMPositionBuilderAtEnd(ctx.builder, condBB);
            LLVMValueRef condition = generate_node(ctx, node.children[1]);
            condition = LLVMBuildICmp(ctx.builder, LLVMIntNE, condition, LLVMConstNull(LLVMTypeOf(condition)),
                                     "forcondvalue");
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
                throw std::runtime_error("malformed match statement");

            LLVMValueRef subject = generate_node(ctx, node.children[0]);
            LLVMBasicBlockRef afterBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "matchafter");
            bool sawDefault = false;

            for (size_t index = 1; index < node.children.size(); ++index)
            {
                const Parser::ASTNode &arm = node.children[index];
                if (arm.type == Parser::NodeType::MatchCase)
                {
                    if (sawDefault || arm.children.size() != 2)
                        throw std::runtime_error("malformed match case");
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
                        throw std::runtime_error("match default must be the final arm");
                    sawDefault = true;
                    generate_node(ctx, arm.children[0]);
                    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                        LLVMBuildBr(ctx.builder, afterBB);
                }
                else
                {
                    throw std::runtime_error("malformed match arm");
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
                throw std::runtime_error("malformed foreach loop");
            const CGType itemType = resolve_type(ctx, node.children[0]);
            if (node.children[1].type == Parser::NodeType::BinaryExpr &&
                std::get<Lexer::Operator>(node.children[1].value) == Lexer::Operator::TRIPPLE_DOT)
            {
                if (node.children[1].children.size() != 2 || !itemType.llvmType ||
                    LLVMGetTypeKind(itemType.llvmType) != LLVMIntegerTypeKind)
                    throw std::runtime_error("foreach range requires an integral item type");
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
                LLVMBasicBlockRef afterBB = LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "rangeafter");
                LLVMBuildBr(ctx.builder, condBB);
                LLVMPositionBuilderAtEnd(ctx.builder, condBB);
                LLVMValueRef index = LLVMBuildLoad2(ctx.builder, indexType, cursor, "rangeindexvalue");
                LLVMValueRef inRange = LLVMBuildICmp(ctx.builder, itemType.isSigned ? LLVMIntSLE : LLVMIntULE,
                                                     index, end, "rangeinrange");
                LLVMBuildCondBr(ctx.builder, inRange, bodyBB, afterBB);
                ctx.loopStack.push_back({nextBB, afterBB});
                ctx.loopCleanupDepths.push_back(ctx.cleanupScopes.size());
                LLVMPositionBuilderAtEnd(ctx.builder, bodyBB);
                LLVMBuildStore(ctx.builder, cast_value(ctx.builder, index, itemType.llvmType,
                                                       itemType.isSigned, "rangeitem"), item);
                generate_node(ctx, node.children[2]);
                if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx.builder)))
                    LLVMBuildBr(ctx.builder, nextBB);
                LLVMPositionBuilderAtEnd(ctx.builder, nextBB);
                LLVMValueRef reachedEnd = LLVMBuildICmp(ctx.builder, LLVMIntEQ, index, end, "rangeatend");
                LLVMBasicBlockRef incrementBB =
                    LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "rangeincrement");
                LLVMBuildCondBr(ctx.builder, reachedEnd, afterBB, incrementBB);
                LLVMPositionBuilderAtEnd(ctx.builder, incrementBB);
                LLVMBuildStore(ctx.builder, LLVMBuildAdd(ctx.builder, index,
                    LLVMConstInt(indexType, 1, 0), "rangenextindex"), cursor);
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
            const bool sizedRuntimeArray = containerType.isPointerLike &&
                                           !containerType.runtimeArrayLengthName.empty();
            const auto vectorInfo = structTypes.find(containerType.structName);
            const bool vector = containerType.structName.rfind("Vector.", 0) == 0 &&
                                vectorInfo != structTypes.end() &&
                                vectorInfo->second.fieldTypes.size() > 1 &&
                                vectorInfo->second.fieldTypes.front().innerType;
            CGType elementType;
            if (vector)
                elementType = *vectorInfo->second.fieldTypes.front().innerType;
            else if (containerType.isArray && containerType.innerType)
                elementType = *containerType.innerType;
            else
                throw std::runtime_error("foreach requires a fixed-size array, T[length] runtime array, or Vector<T>");
            if ((!fixedArray && !sizedRuntimeArray && !vector) || itemType.llvmType != elementType.llvmType)
                throw std::runtime_error(vector
                    ? "foreach item type does not match the Vector element type"
                    : "foreach item type does not match the array element type");
            LLVMValueRef vectorStorage = vector ? get_lvalue(ctx, node.children[1]) : nullptr;
            ctx.push_scope();
            const std::string itemName = std::string(std::get<std::string_view>(node.value));
            LLVMValueRef cursor = LLVMBuildAlloca(ctx.builder, LLVMInt64TypeInContext(ctx.llvmCtx), "foreachindex");
            LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), 0, 0), cursor);

            LLVMBasicBlockRef condBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "foreachcond");
            LLVMBasicBlockRef bodyBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "foreachbody");
            LLVMBasicBlockRef nextBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "foreachnext");
            LLVMBasicBlockRef afterBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "foreachafter");
            LLVMBuildBr(ctx.builder, condBB);

            LLVMPositionBuilderAtEnd(ctx.builder, condBB);
            LLVMValueRef index = LLVMBuildLoad2(ctx.builder, LLVMInt64TypeInContext(ctx.llvmCtx), cursor,
                                                 "foreachindexvalue");
            LLVMValueRef count = nullptr;
            if (fixedArray)
            {
                count = LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), containerType.arrayLength, 0);
            }
            else if (sizedRuntimeArray)
            {
                VarInfo *length = ctx.find_var(containerType.runtimeArrayLengthName);
                if (!length)
                    throw std::runtime_error("foreach runtime array length is not in scope");
                LLVMValueRef lengthValue = LLVMBuildLoad2(ctx.builder, length->type.llvmType, length->address,
                                                          "foreachlength");
                count = cast_value(ctx.builder, lengthValue, LLVMInt64TypeInContext(ctx.llvmCtx), false,
                                   "foreachlengthcast");
            }
            else
            {
                const CGType &lengthType = vectorInfo->second.fieldTypes[1];
                LLVMValueRef lengthAddress = LLVMBuildStructGEP2(ctx.builder, containerType.llvmType,
                                                                 vectorStorage, 1, "vectorlength");
                LLVMValueRef lengthValue = LLVMBuildLoad2(ctx.builder, lengthType.llvmType, lengthAddress,
                                                          "vectorlengthvalue");
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
                element = LLVMBuildGEP2(ctx.builder, containerType.llvmType, container, indices, 2,
                                        "foreachelement");
            }
            else if (sizedRuntimeArray)
            {
                LLVMValueRef base = LLVMBuildLoad2(ctx.builder, containerType.llvmType, container,
                                                   "foreachbase");
                element = LLVMBuildGEP2(ctx.builder, containerType.pointeeType, base, &index, 1,
                                        "foreachelement");
            }
            else
            {
                const CGType &dataType = vectorInfo->second.fieldTypes.front();
                LLVMValueRef dataAddress = LLVMBuildStructGEP2(ctx.builder, containerType.llvmType,
                                                               vectorStorage, 0, "vectordata");
                LLVMValueRef base = LLVMBuildLoad2(ctx.builder, dataType.llvmType, dataAddress,
                                                   "vectordatavalue");
                element = LLVMBuildGEP2(ctx.builder, elementType.llvmType, base, &index, 1,
                                        "foreachelement");
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
            throw std::runtime_error("brace initializer requires an assignment or declaration target");

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
            bool isCFunc = node.type == Parser::NodeType::CFunctionDef ||
                           node.type == Parser::NodeType::CFunctionDecl;

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
                    if (child.type == Parser::NodeType::PrimitiveType ||
                        child.type == Parser::NodeType::PointerType ||
                        child.type == Parser::NodeType::ReferenceType ||
                        child.type == Parser::NodeType::CustomType)
                    {
                        returnCGType = resolve_type(ctx, child);
                        retType = returnCGType.llvmType;
                        break;
                    }
                }
            }
            LLVMTypeRef funcType =
                LLVMFunctionType(retType, paramTypes.data(), paramTypes.size(), 0);

            LLVMValueRef func = LLVMGetNamedFunction(ctx.module, mangledName.c_str());
            if (!func)
                func = LLVMAddFunction(ctx.module, mangledName.c_str(), funcType);

            if (node.type == Parser::NodeType::CFunctionDecl)
                return func;
            if (LLVMGetFirstBasicBlock(func))
                throw std::runtime_error("duplicate C function definition for '" + funcName + "'");

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

                    LLVMValueRef alloca =
                        LLVMBuildAlloca(ctx.builder, pType.llvmType, argName.c_str());
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
                throw std::runtime_error("function declaration is missing its name");
            const std::string declaredName = std::string(std::get<std::string_view>(node.children.front().value));
            const std::string name = !ctx.currentClassName.empty()
                                         ? ctx.currentClassName + "." + declaredName
                                         : (!ctx.currentNamespaceName.empty()
                                                ? ctx.currentNamespaceName + "." + declaredName
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
                    throw std::runtime_error("generic function declarations require a definition");
                }
            }
            signature.llvmFunctionType = LLVMFunctionType(LLVMVoidTypeInContext(ctx.llvmCtx),
                                                           argumentTypes.data(), argumentTypes.size(), 0);
            signature.llvmFunction = LLVMGetNamedFunction(ctx.module, signature.llvmName.c_str());
            if (!signature.llvmFunction)
                signature.llvmFunction = LLVMAddFunction(ctx.module, signature.llvmName.c_str(),
                                                         signature.llvmFunctionType);
            functions[name] = std::move(signature);
            return functions.at(name).llvmFunction;
        }

        case Parser::NodeType::FunctionDef:
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::Identifier)
                throw std::runtime_error("function definition is missing its name");

            const std::string declaredName = std::string(std::get<std::string_view>(node.children.front().value));
            const std::string baseName = !ctx.currentClassName.empty()
                                             ? ctx.currentClassName + "." + declaredName
                                             : (!ctx.currentNamespaceName.empty()
                                                    ? ctx.currentNamespaceName + "." + declaredName
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
                throw std::runtime_error("function definition is missing its body");

            LLVMTypeRef functionType = LLVMFunctionType(LLVMVoidTypeInContext(ctx.llvmCtx),
                                                        argumentTypes.data(), argumentTypes.size(), 0);
            LLVMValueRef function = LLVMGetNamedFunction(ctx.module, llvmName.c_str());
            if (!function)
                function = LLVMAddFunction(ctx.module, llvmName.c_str(), functionType);
            if (LLVMGetFirstBasicBlock(function))
                throw std::runtime_error("duplicate function definition for '" + name + "'");

            FunctionSignature signature;
            signature.llvmName = llvmName;
            signature.llvmFunctionType = functionType;
            signature.llvmFunction = function;
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
                ctx.tunnelSlots[slotName] = {outputIndex, slotType, slotOptionals[index], flagIndex,
                                             cleanupArgIndex, cleanupLeafCount};
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

        case Parser::NodeType::ReturnStmt:
        {
            LLVMTypeRef functionType = LLVMGlobalGetValueType(ctx.currentFunction);
            LLVMTypeRef returnType = LLVMGetReturnType(functionType);
            if (node.children.empty())
            {
                if (LLVMGetTypeKind(returnType) != LLVMVoidTypeKind)
                    throw std::runtime_error("non-void function must return a value");
                emit_cleanup_scopes(ctx, 0);
                return LLVMBuildRetVoid(ctx.builder);
            }
            if (LLVMGetTypeKind(returnType) == LLVMVoidTypeKind)
                throw std::runtime_error("void function cannot return a value");
            LLVMValueRef value = generate_node(ctx, node.children[0]);
            if (!value)
                throw std::runtime_error("return expression did not produce a value");
            value = cast_value(ctx.builder, value, returnType, ctx.currentReturnType.isSigned,
                               "returncast");
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
                funcName = std::string(std::get<std::string_view>(node.children[0].value));
            }
            else if (node.children[0].type == Parser::NodeType::ScopeAccessExpr &&
                     !node.children[0].children.empty())
            {
                const std::string className =
                    std::string(std::get<std::string_view>(node.children[0].children[0].value));
                const std::string methodName = std::string(std::get<std::string_view>(node.children[0].value));
                funcName = className + "." + methodName;
            }
            else if (node.children[0].type == Parser::NodeType::MemberAccessExpr &&
                     !node.children[0].children.empty())
            {
                receiverType = lvalue_type(ctx, node.children[0].children[0]);
                if (receiverType.structName.empty())
                    throw std::runtime_error("method receiver has no class type");
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
            if (funcName == "printf" && ctx.stdlibEnabled)
                return emit_printf(ctx, node);
            size_t genericCount = 0;
            while (1 + genericCount < node.children.size() &&
                   node.children[1 + genericCount].type == Parser::NodeType::GenericArg)
                ++genericCount;
            std::vector<CGType> inferredGenericArguments;
            std::string inferredTemplateName;
            if (genericCount == 0 && receiver && receiverType.structName.rfind("Vector.", 0) == 0)
            {
                const size_t method = funcName.rfind('.');
                if (method != std::string::npos)
                {
                    inferredTemplateName = "Vector" + funcName.substr(method);
                    const auto templateIt = genericFunctionTemplates.find(inferredTemplateName);
                    const auto vectorType = structTypes.find(receiverType.structName);
                    if (templateIt != genericFunctionTemplates.end() && vectorType != structTypes.end() &&
                        vectorType->second.fieldTypes.size() == 4 &&
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
                // Generic class specializations have names such as Vector.i32,
                // while their method template is registered under Vector.push.
                if (templateIt == genericFunctionTemplates.end() && receiver)
                {
                    const size_t specialization = funcName.find('.');
                    const size_t method = funcName.rfind('.');
                    if (specialization != std::string::npos && method != std::string::npos && method > specialization)
                        templateIt = genericFunctionTemplates.find(
                            funcName.substr(0, specialization) + funcName.substr(method));
                }
                if (templateIt == genericFunctionTemplates.end())
                    throw std::runtime_error("unknown generic function '" + funcName + "'");
                std::vector<std::string> parameters;
                for (const auto &child : templateIt->second->children)
                    if (child.type == Parser::NodeType::GenericParam)
                        parameters.push_back(std::string(std::get<std::string_view>(child.value)));
                if (parameters.size() != genericCount + inferredGenericArguments.size())
                    throw std::runtime_error("generic function argument count does not match its declaration");
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
                    ctx.functionSpecializationName = specializationName;
                    ctx.currentClassName = genericFunctionTemplateClasses[templateIt->first];
                    generate_node(ctx, *templateIt->second);
                    ctx.functionSpecializationName.clear();
                    ctx.currentClassName = callerClassName;
                    ctx.currentFunction = caller;
                    ctx.isCFunction = callerIsCFunction;
                    ctx.currentReturnType = callerReturnType;
                    ctx.tunnelSlots = callerTunnelSlots;
                    LLVMPositionBuilderAtEnd(ctx.builder, callerBlock);
                }
                ctx.genericBindings = savedBindings;
                funcName = specializationName;
            }
            std::string mangledName = mangle_function_name(funcName);

            LLVMValueRef func = LLVMGetNamedFunction(ctx.module, mangledName.c_str());
            if (!func)
                throw std::runtime_error("unknown function '" + funcName + "'");

            LLVMTypeRef funcType = LLVMGlobalGetValueType(func);
            const unsigned paramCount = LLVMCountParamTypes(funcType);
            const auto normalFunction = functions.find(funcName);
            if (normalFunction != functions.end() && !normalFunction->second.tunnelSlotTypes.empty())
            {
                const FunctionSignature &signature = normalFunction->second;
                const size_t explicitParameters = signature.paramTypes.size() - (receiver ? 1 : 0);
                if (node.children.size() - 1 - genericCount != explicitParameters)
                    throw std::runtime_error("wrong argument count in call to '" + funcName + "'");
                std::vector<LLVMValueRef> args;
                if (receiver)
                {
                    const CGType &receiverParameter = signature.paramTypes.front();
                    LLVMValueRef receiverValue = receiverParameter.isPointerLike
                                                    ? receiver
                                                    : LLVMBuildLoad2(ctx.builder, receiverParameter.llvmType,
                                                                     receiver, "value_self_receiver");
                    args.push_back(receiverValue);
                }
                for (size_t i = 1 + genericCount; i < node.children.size(); ++i)
                {
                    const CGType &parameterType = signature.paramTypes[i - 1 - genericCount + (receiver ? 1 : 0)];
                    const Parser::ASTNode &argumentNode = node.children[i];
                    if (parameterType.structName == "str" &&
                        argumentNode.type == Parser::NodeType::StringLiteral)
                    {
                        LLVMValueRef slice = LLVMBuildAlloca(ctx.builder, parameterType.llvmType, "strliteral");
                        LLVMValueRef data = generate_node(ctx, argumentNode);
                        LLVMValueRef dataField =
                            LLVMBuildStructGEP2(ctx.builder, parameterType.llvmType, slice, 0, "strdata");
                        LLVMValueRef lengthField =
                            LLVMBuildStructGEP2(ctx.builder, parameterType.llvmType, slice, 1, "strlen");
                        LLVMBuildStore(ctx.builder, data, dataField);
                        LLVMBuildStore(ctx.builder,
                                       LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx),
                                                    decode_string_literal(
                                                        std::get<std::string_view>(argumentNode.value)).size(), 0),
                                       lengthField);
                        args.push_back(LLVMBuildLoad2(ctx.builder, parameterType.llvmType, slice, "strvalue"));
                    }
                    else
                    {
                        LLVMValueRef argument = generate_node(ctx, argumentNode);
                        args.push_back(cast_value(ctx.builder, argument, parameterType.llvmType,
                                                  parameterType.isSigned, "callargcast"));
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
                                presence == ctx.reservedTunnelPresenceTargetScopes[scope].end() ? nullptr : presence->second);
                            const auto cleanup = ctx.reservedTunnelCleanupTargetScopes[scope].find(name);
                            reservedCleanupTargets.push_back(cleanup == ctx.reservedTunnelCleanupTargetScopes[scope].end()
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
                        throw std::runtime_error("unbound multi-output call requires one compatible reserve declaration per tunnel slot");
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
                        present = useProvidedTunnelTargets &&
                                          ctx.pendingTunnelPresenceTargets.size() == signature.tunnelSlotTypes.size()
                                      ? ctx.pendingTunnelPresenceTargets[index]
                                      : LLVMBuildAlloca(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx),
                                                        "tunnelpresent");
                        LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 0, 0), present);
                        args.push_back(present);
                    }
                    const uint64_t cleanupLeafCount = signature.tunnelSlotCleanupLeafCounts[index];
                    for (uint64_t leaf = 0; leaf < cleanupLeafCount; ++leaf)
                    {
                        LLVMValueRef token = useProvidedTunnelTargets &&
                                               ctx.pendingTunnelCleanupTargets.size() == signature.tunnelSlotTypes.size() &&
                                               leaf < ctx.pendingTunnelCleanupTargets[index].size()
                                           ? ctx.pendingTunnelCleanupTargets[index][leaf]
                                           : LLVMBuildAlloca(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx),
                                                             "tunnelcleanup");
                        LLVMBuildStore(ctx.builder, LLVMConstInt(LLVMInt1TypeInContext(ctx.llvmCtx), 0, 0), token);
                        args.push_back(token);
                    }
                    outputPresenceAddresses.push_back(present);
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
                return LLVMBuildLoad2(ctx.builder, signature.tunnelSlotTypes.front().llvmType,
                                      outputAddresses.front(), "callresult");
            }
            const size_t explicitParameters = paramCount - (receiver ? 1 : 0);
            if (node.children.size() - 1 - genericCount != explicitParameters)
                throw std::runtime_error("wrong argument count in call to '" + funcName + "'");
            std::vector<LLVMTypeRef> paramTypes(paramCount);
            LLVMGetParamTypes(funcType, paramTypes.data());

            std::vector<LLVMValueRef> args;
            if (receiver)
            {
                if (normalFunction == functions.end() || normalFunction->second.paramTypes.empty())
                    throw std::runtime_error("method receiver has no declared parameter type");
                const CGType &receiverParameter = normalFunction->second.paramTypes.front();
                LLVMValueRef receiverValue = receiverParameter.isPointerLike
                                                ? receiver
                                                : LLVMBuildLoad2(ctx.builder, receiverParameter.llvmType,
                                                                 receiver, "value_self_receiver");
                args.push_back(receiverValue);
            }
            const auto cstrInfo = structTypes.find("cstr");
            for (size_t i = 1 + genericCount; i < node.children.size(); ++i)
            {
                const size_t parameterIndex = i - 1 - genericCount + (receiver ? 1 : 0);
                const Parser::ASTNode &argumentNode = node.children[i];
                if (argumentNode.type == Parser::NodeType::StringLiteral &&
                    cstrInfo != structTypes.end() && paramTypes[parameterIndex] == cstrInfo->second.llvmType)
                {
                    CGType cstrType;
                    cstrType.llvmType = cstrInfo->second.llvmType;
                    cstrType.structName = "cstr";
                    args.push_back(generate_value_for_target(ctx, argumentNode, cstrType));
                    continue;
                }
                LLVMValueRef argument = generate_node(ctx, argumentNode);
                if (!argument)
                    throw std::runtime_error("call argument did not produce a value");
                args.push_back(cast_value(ctx.builder, argument, paramTypes[parameterIndex], true,
                                          "callargcast"));
            }

            const char *callName = LLVMGetTypeKind(LLVMGetReturnType(funcType)) == LLVMVoidTypeKind ? "" : "calltmp";
            return LLVMBuildCall2(ctx.builder, funcType, func, args.data(), args.size(), callName);
        }

        case Parser::NodeType::TunnelStmt:
        {
            const std::string slot = std::string(std::get<std::string_view>(node.value));
            const auto found = ctx.tunnelSlots.find(slot);
            if (found == ctx.tunnelSlots.end())
                throw std::runtime_error("unknown tunnel slot '" + slot + "'");
            if (node.children.empty())
                throw std::runtime_error("tunnel statement is missing its value");
            CleanupValue *sourceCleanup = nullptr;
            if (node.children.front().type == Parser::NodeType::Identifier)
            {
                const std::string sourceName = std::string(std::get<std::string_view>(node.children.front().value));
                if (VarInfo *sourceVar = ctx.find_var(sourceName))
                {
                    for (auto scope = ctx.cleanupScopes.rbegin(); scope != ctx.cleanupScopes.rend() && !sourceCleanup; ++scope)
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
                LLVMValueRef active = leaf < sourceTokens.size()
                                        ? LLVMBuildLoad2(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx),
                                                         sourceTokens[leaf], "tunnelactive")
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
                throw std::runtime_error("multi-result reserve requires a function call initializer");
            const Parser::ASTNode &source = node.children.back();
            if (source.children.empty() || source.children.front().type != Parser::NodeType::Identifier)
                throw std::runtime_error("multi-result reserve requires a named function call");
            const std::string callee = std::string(std::get<std::string_view>(source.children.front().value));
            const auto signature = functions.find(callee);
            const size_t bindingCount = (node.children.size() - 1) / 2;
            if (signature == functions.end() || signature->second.tunnelSlotTypes.size() != bindingCount)
                throw std::runtime_error("multi-result reserve binding count does not match tunnel outputs");
            ctx.pendingTunnelResultTargets.clear();
            ctx.pendingTunnelPresenceTargets.clear();
            ctx.pendingTunnelCleanupTargets.clear();
            for (size_t index = 0; index < bindingCount; ++index)
            {
                const CGType type = resolve_type(ctx, node.children[index * 2]);
                const std::string name = std::string(std::get<std::string_view>(node.children[index * 2 + 1].value));
                const bool optional = signature->second.tunnelSlotOptional[index];
                const CGType &payloadType = signature->second.tunnelSlotTypes[index];
                if ((optional && (!type.isOptional || !type.innerType || type.innerType->llvmType != payloadType.llvmType)) ||
                    (!optional && type.llvmType != payloadType.llvmType))
                    throw std::runtime_error("multi-result reserve binding type does not match its tunnel output");
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
                throw std::runtime_error("multi-result reserve did not consume its tunnel outputs");
            return nullptr;
        }

        case Parser::NodeType::ReserveStmt:
        {
            if (node.children.empty())
                throw std::runtime_error("reserve statement is missing its type");
            const std::string name = std::string(std::get<std::string_view>(node.value));
            CGType type = resolve_type(ctx, node.children.front());
            if (node.children.size() > 1 && node.children[1].type == Parser::NodeType::TunnelBindingExpr)
            {
                const std::string slot = std::string(std::get<std::string_view>(node.children[1].value));
                const auto found = ctx.tunnelSlots.find(slot);
                if (found == ctx.tunnelSlots.end())
                    throw std::runtime_error("reserve tunnel binding refers to an unknown slot");
                if (type.llvmType != found->second.type.llvmType)
                    throw std::runtime_error("reserve tunnel binding type does not match its slot");
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
                if (ctx.reservedTunnelTargetScopes.back().find(name) !=
                    ctx.reservedTunnelTargetScopes.back().end())
                    throw std::runtime_error("duplicate tunnel reservation in the same scope");
                const LLVMValueRef payloadTarget = type.isOptional
                                                       ? LLVMBuildStructGEP2(ctx.builder, type.llvmType, address, 1,
                                                                             "reserve_tunnel_payload")
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
            if (source.type == Parser::NodeType::CallExpr && !source.children.empty() &&
                source.children.front().type == Parser::NodeType::Identifier &&
                (source.children.size() < 2 || source.children[1].type != Parser::NodeType::GenericArg) &&
                functions.find(std::string(std::get<std::string_view>(source.children.front().value))) ==
                    functions.end() &&
                std::string(std::get<std::string_view>(source.children.front().value)) != "printf")
            {
                const std::string callee =
                    mangle_function_name(std::string(std::get<std::string_view>(source.children.front().value)));
                LLVMValueRef function = LLVMGetNamedFunction(ctx.module, callee.c_str());
                if (!function)
                    throw std::runtime_error("unknown function in reserve call");
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
                        arguments.push_back(cast_value(ctx.builder, value, parameterTypes[i], true,
                                                       "reserveargcast"));
                    }
                    arguments.push_back(address);
                    LLVMBuildCall2(ctx.builder, functionType, function, arguments.data(), arguments.size(), "");
                    return nullptr;
                }
            }

            if (source.type == Parser::NodeType::CallExpr && !source.children.empty() &&
                source.children.front().type == Parser::NodeType::Identifier)
            {
                const auto callee = functions.find(std::string(std::get<std::string_view>(source.children.front().value)));
                if (callee != functions.end() && callee->second.tunnelSlotTypes.size() == 1)
                {
                    LLVMValueRef payloadTarget = type.isOptional
                                                     ? LLVMBuildStructGEP2(ctx.builder, type.llvmType, address, 1,
                                                                           "reserve_tunnel_payload")
                                                     : address;
                    LLVMValueRef presenceTarget = type.isOptional
                                                      ? LLVMBuildStructGEP2(ctx.builder, type.llvmType, address, 0,
                                                                            "reserve_tunnel_present")
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
                        throw std::runtime_error("reserve tunnel initializer did not consume its output");
                    return nullptr;
                }
            }

            LLVMValueRef value = generate_value_for_target(ctx, source, type);
            if (!value)
                throw std::runtime_error("reserve initializer did not produce a value");
            LLVMBuildStore(ctx.builder, materialize_value(ctx, value, type, "reserveinitcast"), address);
            return nullptr;
        }

        case Parser::NodeType::RefExpr:
        {
            if (node.children.empty())
                throw std::runtime_error("reference expression is missing its source");
            LLVMValueRef address = get_lvalue(ctx, node.children[0]);
            if (!address)
                throw std::runtime_error("reference expression requires an addressable source");
            return address;
        }

        case Parser::NodeType::MoveExpr:
        {
            if (node.children.empty())
                throw std::runtime_error("move expression is missing its source");
            const CGType sourceType = lvalue_type(ctx, node.children[0]);
            LLVMValueRef sourceAddress = get_lvalue(ctx, node.children[0]);
            if (!sourceAddress)
                throw std::runtime_error("move expression requires an addressable source");
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
                throw std::runtime_error("State binding requires one deferred function call");
            const std::string name = std::string(std::get<std::string_view>(node.value));
            if (ctx.states.find(name) != ctx.states.end())
                throw std::runtime_error("duplicate State binding '" + name + "'");
            ctx.states.emplace(name, DeferredState{&node.children.front(), false});
            ctx.scopedStateNames.back().push_back(name);
            return nullptr;
        }

        case Parser::NodeType::StartStmt:
        {
            const std::string name = std::string(std::get<std::string_view>(node.value));
            auto state = ctx.states.find(name);
            if (state == ctx.states.end())
                throw std::runtime_error("start references unknown State '" + name + "'");
            if (!state->second.started)
            {
                generate_node(ctx, *state->second.call);
                state->second.started = true;
            }
            return nullptr;
        }

        case Parser::NodeType::AwaitExpr:
        {
            if (node.children.empty())
                throw std::runtime_error("await requires a State or call expression");
            if (node.children.front().type == Parser::NodeType::Identifier)
            {
                const std::string name = std::string(std::get<std::string_view>(node.children.front().value));
                auto state = ctx.states.find(name);
                if (state == ctx.states.end())
                    throw std::runtime_error("await references unknown State '" + name + "'");
                if (!state->second.started)
                {
                    generate_node(ctx, *state->second.call);
                    state->second.started = true;
                }
                return LLVMConstInt(LLVMInt64TypeInContext(ctx.llvmCtx), 0, 0);
            }
            return generate_node(ctx, node.children.front());
        }

        case Parser::NodeType::ThreadTaskStmt:
            if (node.children.size() != 1)
                throw std::runtime_error("named task requires one body block");
            return generate_node(ctx, node.children.front());

        case Parser::NodeType::ValidStmt:
        {
            const std::string name = std::string(std::get<std::string_view>(node.value));
            VarInfo *optional = ctx.find_var(name);
            if (!optional || !optional->type.isOptional)
                throw std::runtime_error("valid requires a declared optional value");
            LLVMValueRef flagAddress = LLVMBuildStructGEP2(ctx.builder, optional->type.llvmType,
                                                           optional->address, 0, "optionalflagaddr");
            LLVMValueRef flag = LLVMBuildLoad2(ctx.builder, LLVMInt1TypeInContext(ctx.llvmCtx), flagAddress,
                                                "optionalflag");
            LLVMBasicBlockRef thenBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "validthen");
            LLVMBasicBlockRef elseBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "validelse");
            LLVMBasicBlockRef mergeBB =
                LLVMAppendBasicBlockInContext(ctx.llvmCtx, ctx.currentFunction, "validcont");
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
            throw std::runtime_error("internal error: parser construct reached LLVM generation without a lowering rule");
        }
        return nullptr;
    }

    static LLVMValueRef build_main_arguments(Context &ctx, LLVMValueRef entry)
    {
        const auto stringInfo = structTypes.find("String");
        if (stringInfo == structTypes.end())
            throw std::runtime_error("String[argc] main requires the standard String type");

        LLVMValueRef alloc = LLVMGetNamedFunction(ctx.module, "__shaft_alloc");
        LLVMValueRef strlen = LLVMGetNamedFunction(ctx.module, "__cstr_strlen");
        LLVMValueRef copy = LLVMGetNamedFunction(ctx.module, "memcpy");
        if (!alloc || !strlen || !copy)
            throw std::runtime_error("String[argc] main requires std allocation and byte-copy runtime functions");

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
        LLVMBuildStore(ctx.builder, length, LLVMBuildStructGEP2(ctx.builder, stringType, stringSlot, 1, "arglengthfield"));
        LLVMBuildStore(ctx.builder, length, LLVMBuildStructGEP2(ctx.builder, stringType, stringSlot, 2, "argcapacityfield"));
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