#include "checker.hpp"
#include "parser.hpp"
#include <algorithm>
#include <iostream>
#include <unordered_set>

namespace Checker
{
    void error(std::string message, Parser::ASTNode node)
    {
        ErrorPos error_pos = get_error_pos(node.start, node.source);
        Error err = {message, *(node.mod_path), error_pos.line, error_pos.column};
        panic(err);
    }
    class SymbolTable
    {
      private:
        std::vector<std::unordered_map<std::string, Symbol>> scopes;
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
            return nullptr;
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
            return node.inferredTypeName.empty()
                       ? std::string(std::get<std::string_view>(node.children.front().value))
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
            if (left.kind != right.kind || left.name != right.name ||
                left.isOptional != right.isOptional || left.isMutable != right.isMutable ||
                left.genericArgs.size() != right.genericArgs.size() ||
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
                    activeGenericNames.push_back(
                        std::string(std::get<std::string_view>(child.value)));
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
            case TypeKind::String:
                return "String";
            case TypeKind::State:
                return "State";
            case TypeKind::Thread:
                return "Thread";
            case TypeKind::Struct:
            case TypeKind::Class:
            case TypeKind::Enum:
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
                return true;
            default:
                return false;
            }
        }

        std::string custom_type_name(const Parser::ASTNode &node)
        {
            return node.inferredTypeName.empty()
                       ? std::string(std::get<std::string_view>(node.value))
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
                    type.innerType =
                        std::make_unique<Type>(infer_type_from_node(node.children.front()));
                return type;
            }
            case Parser::NodeType::ReferenceType:
            {
                Type type{TypeKind::Reference, "&"};
                type.isMutable = node.isMutable ||
                                 (node.value.index() != 0 && std::get<bool>(node.value));
                if (!node.children.empty())
                    type.innerType =
                        std::make_unique<Type>(infer_type_from_node(node.children.front()));
                return type;
            }
            case Parser::NodeType::OptionalType:
            {
                Type type{TypeKind::Optional, "?"};
                type.isOptional = true;
                type.isMutable = node.isMutable;
                if (!node.children.empty())
                    type.innerType =
                        std::make_unique<Type>(infer_type_from_node(node.children.front()));
                return type;
            }
            case Parser::NodeType::CustomType:
            {
                const std::string typeName = custom_type_name(node);
                auto mutable_type = [&node](Type type) {
                    type.isMutable = node.isMutable;
                    for (const Parser::ASTNode &argument : node.children)
                        type.genericArgs.push_back(infer_type_from_node(argument));
                    return type;
                };

                if (typeName == "String" || typeName == "str" || typeName == "cstr")
                    return mutable_type(Type(TypeKind::String, typeName));

                if (typeName == "Self" && !structNameStack.empty())
                {
                    const std::string &structName = structNameStack.back();
                    auto structIt = registeredTypes.find(structName);
                    if (structIt != registeredTypes.end() && structIt->second.isClass)
                        return mutable_type(Type(TypeKind::Class, structName));
                    return mutable_type(Type(TypeKind::Struct, structName));
                }

                if (is_active_generic_name(typeName))
                    return mutable_type(Type(TypeKind::Generic, typeName));

                auto it = registeredTypes.find(typeName);
                if (it != registeredTypes.end() && it->second.isEnum)
                    return mutable_type(Type(TypeKind::Enum, typeName));
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
                return scope_access_name(node.children.front()) + "::" +
                       std::string(std::get<std::string_view>(node.value));
            return {};
        }

        bool type_requires_cleanup(const Type &type, std::unordered_set<std::string> &visiting)
        {
            if (type.kind == TypeKind::Pointer || type.kind == TypeKind::Reference ||
                type.kind == TypeKind::Optional)
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

        Type infer_expression_type(SymbolTable &table, Parser::ASTNode &expr)
        {
            switch (expr.type)
            {
            case Parser::NodeType::Identifier:
            {
                const auto *symbol =
                    table.lookup(std::string(std::get<std::string_view>(expr.value)));
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
                return Type(TypeKind::String);
            case Parser::NodeType::PointerType:
                return Type(TypeKind::Pointer, "*");
            case Parser::NodeType::ReferenceType:
                return Type(TypeKind::Reference, "&");
            case Parser::NodeType::MoveExpr:
            {
                if (expr.children.empty())
                    return {TypeKind::Error, {}};
                return infer_expression_type(table, expr.children.front());
            }
            case Parser::NodeType::RefExpr:
            {
                if (expr.children.empty())
                    return {TypeKind::Error, {}};
                Type pointee = infer_expression_type(table, expr.children.front());
                Type reference(TypeKind::Reference, "&");
                reference.isMutable = pointee.isMutable;
                reference.innerType = std::make_shared<Type>(pointee);
                return reference;
            }
            case Parser::NodeType::SizeofExpr:
                return Type(TypeKind::U64);
            case Parser::NodeType::CallExpr:
            {
                if (expr.children.empty())
                    return {TypeKind::Error, {}};
                const Parser::ASTNode &callee = expr.children.front();
                std::string name;
                if (callee.type == Parser::NodeType::Identifier)
                    name = std::string(std::get<std::string_view>(callee.value));
                else if (callee.type == Parser::NodeType::ScopeAccessExpr)
                    name = scope_access_name(callee);
                if (name == "printf" && stdlibEnabled)
                    return Type(TypeKind::I64);
                if (const Symbol *function = table.lookup(name);
                    function && function->isFunction && function->hasResultType)
                    return function->resultType;
                return {TypeKind::Error, {}};
            }
            case Parser::NodeType::UnaryExpr:
            {
                if (expr.children.empty())
                    return {TypeKind::Error, {}};
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
                    return {TypeKind::Error, {}};
                const Type receiver = infer_expression_type(table, expr.children.front());
                if (receiver.kind == TypeKind::Array && receiver.innerType)
                    return *receiver.innerType;
                if (receiver.kind == TypeKind::Pointer && receiver.innerType)
                    return *receiver.innerType;
                const auto definition = registeredTypes.find(receiver.name);
                if (definition == registeredTypes.end() || definition->second.indexedField.empty())
                    return {TypeKind::Error, {}};
                const auto field = definition->second.members.find(definition->second.indexedField);
                if (field == definition->second.members.end())
                    return {TypeKind::Error, {}};
                const Parser::ASTNode &backingNode = field->second.typeNode;
                const Type backing = infer_type_from_node(backingNode);
                if (!backing.innerType)
                    return {TypeKind::Error, {}};
                if (!backingNode.children.empty() &&
                    backingNode.children.front().type == Parser::NodeType::CustomType)
                {
                    const std::string elementName = std::string(
                        std::get<std::string_view>(backingNode.children.front().value));
                    if (registeredTypes.find(elementName) == registeredTypes.end())
                        return Type(TypeKind::Generic, elementName);
                }
                return *backing.innerType;
            }
            case Parser::NodeType::MemberAccessExpr:
            {
                if (expr.children.empty())
                    return {TypeKind::Error, {}};
                const Type receiver = infer_expression_type(table, expr.children.front());
                const auto definition = registeredTypes.find(receiver.name);
                if (definition == registeredTypes.end())
                    return {TypeKind::Error, {}};
                const std::string memberName = std::string(std::get<std::string_view>(expr.value));
                const auto field = definition->second.members.find(memberName);
                if (field == definition->second.members.end())
                    return {TypeKind::Error, {}};
                const Type fieldType = infer_type_from_node(field->second.typeNode);
                const auto generic = std::find(definition->second.genericParameters.begin(),
                                               definition->second.genericParameters.end(), fieldType.name);
                if (generic != definition->second.genericParameters.end())
                {
                    const size_t index = static_cast<size_t>(generic - definition->second.genericParameters.begin());
                    if (index < receiver.genericArgs.size())
                        return receiver.genericArgs[index];
                }
                return fieldType;
            }
            case Parser::NodeType::ScopeAccessExpr:
            {
                if (expr.children.empty())
                    return {TypeKind::Error, {}};
                const std::string enumName = scope_access_name(expr.children.front());
                const std::string memberName = std::string(std::get<std::string_view>(expr.value));
                const auto definition = registeredTypes.find(enumName);
                if (definition == registeredTypes.end() || !definition->second.isEnum ||
                    definition->second.members.find(memberName) == definition->second.members.end())
                {
                    error("Unknown enum member '" + enumName + "::" + memberName + "'.", expr);
                }
                return Type(TypeKind::Enum, enumName);
            }
            case Parser::NodeType::BinaryExpr:
            {
                if (expr.children.size() < 2)
                    return {TypeKind::Error, {}};

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
                         is_numeric(rightType.kind) &&
                         (op == Lexer::Operator::PLUS || op == Lexer::Operator::MINUS))
                    resultType = leftType;
                else if (is_numeric(leftType.kind) && is_numeric(rightType.kind))
                    resultType =
                        (leftType.kind == TypeKind::F32 || leftType.kind == TypeKind::F64 ||
                         rightType.kind == TypeKind::F32 || rightType.kind == TypeKind::F64)
                            ? Type{TypeKind::F64, {}}
                            : Type{TypeKind::U64, {}};
                else if (leftType.kind == rightType.kind)
                    resultType = leftType;

                expr.hasInferredType = true;
                expr.inferredTypeName = type_to_string(resultType);
                return resultType;
            }
            case Parser::NodeType::AssignmentExpr:
            {
                if (expr.children.size() < 2)
                    return {TypeKind::Error, {}};
                return infer_expression_type(table, expr.children[1]);
            }
            default:
                return {TypeKind::Error, {}};
            }
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
                    return is_assignable(declaredType,
                                         valueType.innerType ? *valueType.innerType : Type{});
                if (!valueType.isOptional)
                    return is_assignable(declaredType.innerType ? *declaredType.innerType : Type{},
                                         valueType);
                return is_assignable(declaredType.innerType ? *declaredType.innerType : Type{},
                                     valueType.innerType ? *valueType.innerType : Type{});
            }
            if (declaredType.kind == TypeKind::Pointer && valueType.kind == TypeKind::String)
                return true;

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
                if (declaredType.kind == TypeKind::Struct && declaredType.name == "str" &&
                    valueType.kind == TypeKind::String)
                    return true;
                return declaredType.kind == TypeKind::String && valueType.kind == TypeKind::String;
            }
            if (declaredType.kind == TypeKind::Array || valueType.kind == TypeKind::Array)
            {
                return declaredType.kind == TypeKind::Array && valueType.kind == TypeKind::Array &&
                       declaredType.innerType && valueType.innerType &&
                       is_assignable(*declaredType.innerType, *valueType.innerType);
            }
            if (valueType.kind == TypeKind::Enum && is_numeric(declaredType.kind))
                return true;
            if (declaredType.kind == valueType.kind)
                return true;
            if (declaredType.kind == TypeKind::Struct || declaredType.kind == TypeKind::Class ||
                declaredType.kind == TypeKind::Enum)
                return valueType.kind == declaredType.kind && declaredType.name == valueType.name;
            if (is_numeric(declaredType.kind) && is_numeric(valueType.kind))
                return true;
            return false;
        }

        bool is_valid_binary_op(const Type &left, const Type &right, Lexer::Operator op)
        {
            if (left.kind == TypeKind::Error || right.kind == TypeKind::Error)
                return true;
            if (left.kind == TypeKind::Generic || right.kind == TypeKind::Generic)
                return true;
            if (op == Lexer::Operator::AND || op == Lexer::Operator::OR)
                return left.kind == TypeKind::Bool && right.kind == TypeKind::Bool;
            if (op == Lexer::Operator::ASSIGN)
                return (left.kind == TypeKind::String && right.kind == TypeKind::String) ||
                       (left.kind == right.kind && left.kind != TypeKind::Pointer);
            if (op == Lexer::Operator::LESS_THAN || op == Lexer::Operator::GREATER_THAN ||
                op == Lexer::Operator::LESS_EQUAL || op == Lexer::Operator::GREATER_EQUAL)
                return (is_numeric(left.kind) && is_numeric(right.kind)) ||
                       (left.kind == right.kind && left.kind != TypeKind::Pointer) ||
                       (left.kind == TypeKind::Reference && right.kind == TypeKind::Reference);
            if ((left.kind == TypeKind::Pointer || left.kind == TypeKind::Reference) &&
                is_numeric(right.kind) && (op == Lexer::Operator::PLUS || op == Lexer::Operator::MINUS))
                return true;
            if ((left.kind == TypeKind::Pointer || right.kind == TypeKind::Pointer) &&
                (op == Lexer::Operator::PLUS || op == Lexer::Operator::MINUS))
                return false;
            if (left.kind == TypeKind::Pointer || right.kind == TypeKind::Pointer ||
                left.kind == TypeKind::Reference || right.kind == TypeKind::Reference)
                return false;

            if (left.kind == TypeKind::Bool || right.kind == TypeKind::Bool)
                return left.kind == TypeKind::Bool && right.kind == TypeKind::Bool;
            if (left.kind == TypeKind::String || right.kind == TypeKind::String)
                return (op == Lexer::Operator::EQUAL || op == Lexer::Operator::NOT_EQUAL) &&
                       left.kind == TypeKind::String && right.kind == TypeKind::String;
            const bool hasFloat = left.kind == TypeKind::F32 || left.kind == TypeKind::F64 ||
                                  right.kind == TypeKind::F32 || right.kind == TypeKind::F64;
            if (hasFloat)
                return op == Lexer::Operator::PLUS || op == Lexer::Operator::MINUS ||
                       op == Lexer::Operator::MULTIPLY || op == Lexer::Operator::DIVIDE ||
                       op == Lexer::Operator::LESS_THAN || op == Lexer::Operator::GREATER_THAN ||
                       op == Lexer::Operator::LESS_EQUAL || op == Lexer::Operator::GREATER_EQUAL ||
                       op == Lexer::Operator::EQUAL || op == Lexer::Operator::NOT_EQUAL;
            if (left.kind == right.kind)
                return true;
            return is_numeric(left.kind) && is_numeric(right.kind);
        }
    } // namespace

    void register_type_definition(const Parser::ASTNode &node)
    {
        if (node.type != Parser::NodeType::StructDecl && node.type != Parser::NodeType::EnumDecl)
            return;

        TypeDefinition def;
        def.name = current_namespace_name();
        if (!def.name.empty())
            def.name += "::";
        def.name += std::string(std::get<std::string_view>(node.children.front().value));
        if (def.name.empty() && !node.children.empty() &&
            node.children.front().type == Parser::NodeType::Identifier)
            def.name = std::string(std::get<std::string_view>(node.children.front().value));

        if (def.name.empty())
            return;

        def.isEnum = node.type == Parser::NodeType::EnumDecl;
        def.isClass = node.isClass;
        def.baseClassName = node.baseClassName;
        if (!def.baseClassName.empty())
        {
            const auto base = registeredTypes.find(def.baseClassName);
            if (base == registeredTypes.end() || !base->second.isClass)
            {
                error("Base class '" + def.baseClassName + "' must be declared before its derived class.", node);
            }
            def.members = base->second.members;
            def.methods = base->second.methods;
            def.indexedField = base->second.indexedField;
            def.initializedField = base->second.initializedField;
        }
        for (const auto &child : node.children)
        {
            if (child.type == Parser::NodeType::GenericParam)
                def.genericParameters.push_back(std::string(std::get<std::string_view>(child.value)));
        }

        uint64_t enumMaximum = UINT64_MAX;
        if (def.isEnum && node.children.size() > 1 && node.children[1].type == Parser::NodeType::PrimitiveType)
        {
            const Lexer::Keyword backing = std::get<Lexer::Keyword>(node.children[1].value);
            const bool integerBacking = backing == Lexer::Keyword::U8 || backing == Lexer::Keyword::U16 ||
                                        backing == Lexer::Keyword::U32 || backing == Lexer::Keyword::U64 ||
                                        backing == Lexer::Keyword::I8 || backing == Lexer::Keyword::I16 ||
                                        backing == Lexer::Keyword::I32 || backing == Lexer::Keyword::I64 ||
                                        backing == Lexer::Keyword::USIZE;
            if (!integerBacking)
            {
                error("enum backing type must be an integer.", node);
            }
            if (backing == Lexer::Keyword::U8) enumMaximum = UINT8_MAX;
            else if (backing == Lexer::Keyword::U16) enumMaximum = UINT16_MAX;
            else if (backing == Lexer::Keyword::U32) enumMaximum = UINT32_MAX;
            else if (backing == Lexer::Keyword::I8) enumMaximum = INT8_MAX;
            else if (backing == Lexer::Keyword::I16) enumMaximum = INT16_MAX;
            else if (backing == Lexer::Keyword::I32) enumMaximum = INT32_MAX;
            else if (backing == Lexer::Keyword::I64) enumMaximum = INT64_MAX;
        }
        int64_t enumMinimum = 0;
        if (def.isEnum && node.children.size() > 1 && node.children[1].type == Parser::NodeType::PrimitiveType)
        {
            const Lexer::Keyword backing = std::get<Lexer::Keyword>(node.children[1].value);
            if (backing == Lexer::Keyword::I8) enumMinimum = INT8_MIN;
            else if (backing == Lexer::Keyword::I16) enumMinimum = INT16_MIN;
            else if (backing == Lexer::Keyword::I32) enumMinimum = INT32_MIN;
            else if (backing == Lexer::Keyword::I64) enumMinimum = INT64_MIN;
        }
        uint64_t nextEnumValue = 0;
        bool enumNextValueOverflowed = false;
        for (const auto &child : node.children)
        {
            if (def.isEnum && child.type == Parser::NodeType::EnumMember)
            {
                const bool negativeLiteral = child.children.size() > 1 &&
                                             child.children[1].type == Parser::NodeType::UnaryExpr &&
                                             std::get<Lexer::Operator>(child.children[1].value) == Lexer::Operator::MINUS &&
                                             child.children[1].children.size() == 1 &&
                                             child.children[1].children[0].type == Parser::NodeType::IntegerLiteral;
                if (negativeLiteral)
                {
                    const uint64_t magnitude = std::get<uint64_t>(child.children[1].children[0].value);
                    const uint64_t minimumMagnitude = enumMinimum < 0
                        ? static_cast<uint64_t>(-(enumMinimum + 1)) + 1
                        : 0;
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
                const auto has_named_runtime_array = [&](const auto &self, const Parser::ASTNode &typeNode) -> bool {
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
                    error("runtime array length bindings are local and cannot appear in struct or class fields.", node);
                }
                if (!child.children.empty() && child.children.front().type == Parser::NodeType::CustomType &&
                    std::get<std::string_view>(child.children.front().value) ==
                        std::get<std::string_view>(node.children.front().value))
                {
                    error("recursive-by-value field '" + std::string(std::get<std::string_view>(child.value)) + "' requires a pointer or reference indirection.", node);
                }
                Symbol field;
                field.name = std::string(std::get<std::string_view>(child.value));
                field.typeNode =
                    child.children.empty() ? Parser::ASTNode{} : child.children.front();
                def.members[field.name] = field;
            }
            else if (child.type == Parser::NodeType::IndexDecl)
            {
                if (!def.indexedField.empty())
                {
                    error("A class can declare only one indexed backing field.", node);
                }
                def.indexedField = std::string(std::get<std::string_view>(child.value));
            }
            else if (child.type == Parser::NodeType::InitDecl)
            {
                if (!def.initializedField.empty())
                {
                    error("A class can declare only one initializer backing field.", node);
                }
                def.initializedField = std::string(std::get<std::string_view>(child.value));
            }
            else if (child.type == Parser::NodeType::FunctionDef ||
                     child.type == Parser::NodeType::FunctionDecl ||
                     child.type == Parser::NodeType::CFunctionDef ||
                     child.type == Parser::NodeType::CFunctionDecl)
            {
                Symbol method;
                method.name =
                    child.children.empty()
                        ? std::string{}
                        : std::string(std::get<std::string_view>(child.children.front().value));
                method.typeNode =
                    child.children.empty() ? Parser::ASTNode{} : child.children.front();
                method.isFunction = true;
                bool hasReceiver = false;
                for (const auto &parameter : child.children)
                {
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
                error("Indexed backing field '" + def.indexedField + "' must be a pointer or runtime-sized array.", node);
            }
        }
        if (!def.initializedField.empty() && def.initializedField != def.indexedField)
        {
            error("Init declaration must name the indexed backing field.", node);
        }

        registeredTypes[def.name] = def;
    }

    void check_call_expr(SymbolTable &table, Parser::ASTNode &expr)
    {
        if (expr.children.empty())
            return;

        std::vector<Type> argTypes;
        bool hasExplicitGenericArguments = false;
        for (size_t i = 1; i < expr.children.size(); ++i)
        {
            if (expr.children[i].type == Parser::NodeType::GenericArg)
            {
                hasExplicitGenericArguments = true;
                continue;
            }
            argTypes.push_back(infer_expression_type(table, expr.children[i]));
        }

        const auto &callee = expr.children.front();
        if (callee.type == Parser::NodeType::Identifier)
        {
            const std::string calleeName = std::string(std::get<std::string_view>(callee.value));
            if (calleeName == "printf" && stdlibEnabled)
            {
                if (argTypes.empty() || expr.children[1].type != Parser::NodeType::StringLiteral)
                {
                    error("printf requires a string-literal format argument.", callee);
                }
                return;
            }
            Symbol *symbol = table.lookup(calleeName);
            if (!symbol || !symbol->isFunction)
            {
                if (expr.children.size() == 1)
                    return;
                error("Unknown function '" + calleeName + "'.", callee);
            }
            if (symbol->paramTypes.size() != argTypes.size())
            {
                error("Function argument count mismatch.", callee);
            }
            for (size_t i = 0; i < argTypes.size(); ++i)
            {
                if (!hasExplicitGenericArguments && !is_assignable(symbol->paramTypes[i], argTypes[i]))
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
                    error("Unbound multi-output call requires one compatible reserve declaration per tunnel slot.", callee);
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
                            error("tunnel reservation '" + symbol->tunnelSlotNames[index] + "' was already consumed.", callee);
                        }
                    }
                    for (Symbol *reservation : reservations)
                        reservation->tunnelReservationConsumed = true;
                }
            }
        }
        else if (callee.type == Parser::NodeType::MemberAccessExpr)
        {
            const Type receiverType = infer_expression_type(
                table, const_cast<Parser::ASTNode &>(callee.children.front()));
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
                        if (!is_assignable(methodIt->second.paramTypes[i], argTypes[i]))
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
                    expr.inferredTypeName =
                        type_to_string(infer_type_from_node(fieldIt->second.typeNode));
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
            else if (target.type == Parser::NodeType::MemberAccessExpr ||
                     target.type == Parser::NodeType::IndexExpr)
            {
                check_mutable_assignment_target(table, const_cast<Parser::ASTNode &>(target));
                declaredType = infer_expression_type(table, const_cast<Parser::ASTNode &>(target));
                hasDeclaredType = true;
            }
        }

        for (size_t i = (node.type == Parser::NodeType::VariableDecl ? 1u : 1u);
             i < node.children.size(); ++i)
        {
            const auto &child = node.children[i];
            if (child.type == Parser::NodeType::MoveExpr)
            {
                const OwnershipPath sourcePath =
                    child.children.empty() ? OwnershipPath{} : ownership_path(child.children.front());
                const std::string sourceVar = sourcePath.root.empty()
                                                  ? (child.children.empty() ? std::string{}
                                                                            : owning_root_name(child.children.front()))
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
                const auto is_named_runtime_array_type = [&](const auto &self, const Parser::ASTNode &typeNode) -> bool {
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
                    (!sourcePath.relative.empty() &&
                     (subobject_is_moved(*sym, sourcePath.relative) ||
                      subobject_contains_moved_value(*sym, sourcePath.relative))))
                {
                    error("Cannot move from already moved variable '" + sourceVar + "'.", node);
                }
                if (sym->borrowCount > 0 || sym->has_writer)
                {
                    error("Cannot move a borrowed variable.", node);
                }
                if (hasDeclaredType &&
                    !is_assignable(declaredType, infer_type_from_node(sym->typeNode)))
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
                const std::string sourceVar = sourcePath.root.empty()
                                                  ? (child.children.empty() ? std::string{}
                                                                            : owning_root_name(child.children.front()))
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
                    error("Cannot borrow from moved "+ (sourcePath.relative.empty() ? "variable '" + sourceVar + "'" : "subobject") + ".", node);
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
                    error(std::string("Cannot borrow a variable already borrowed ") + (mutableBorrow ? "by readers or a writer" : "mutably") + ".", node);
                }
            }
            else if (hasDeclaredType && child.type != Parser::NodeType::PrimitiveType &&
                     child.type != Parser::NodeType::CustomType &&
                     child.type != Parser::NodeType::PointerType &&
                     child.type != Parser::NodeType::ReferenceType &&
                     child.type != Parser::NodeType::MoveExpr &&
                     child.type != Parser::NodeType::RefExpr)
            {
                Type valueType = infer_expression_type(table, const_cast<Parser::ASTNode &>(child));
                if ((child.type == Parser::NodeType::Identifier ||
                     child.type == Parser::NodeType::MemberAccessExpr ||
                     child.type == Parser::NodeType::IndexExpr) &&
                    type_requires_cleanup(valueType))
                {
                    error("Cannot implicitly copy a cleanup-owning value; use move or ref.", node);
                }
                if (!is_assignable(declaredType, valueType))
                {
                    error("Type mismatch in assignment or initialization.", node);
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

            activeTunnelSlots[name] = {
                name,
                child.isOptional,
                false,
                has_type ? infer_type_from_node(child.children.front()) : Type{},
                has_type
            };
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
                if (!node.children.empty() &&
                    node.children.front().type == Parser::NodeType::RefExpr)
                {
                    error("cannot tunnel a reference to function-local storage.", node);
                }

                const std::string slotName =
                    std::string(std::get<std::string_view>(node.value));

                if (const auto slot = activeTunnelSlots.find(slotName);
                    slot != activeTunnelSlots.end() &&
                    node.children.size() > 1)
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

            if (node.type == Parser::NodeType::ReserveStmt &&
                node.children.size() == 2 &&
                node.children[1].type == Parser::NodeType::TunnelBindingExpr)
            {
                const std::string local =
                    std::string(std::get<std::string_view>(node.value));
                const std::string slot =
                    std::string(std::get<std::string_view>(node.children[1].value));

                if (activeTunnelSlots.find(slot) == activeTunnelSlots.end())
                {
                    error("Target tunnel slot '" + slot + "' does not exist.", node);
                }

                boundTunnelSlots[local] = slot;
                return flows;
            }

            if (node.type == Parser::NodeType::AssignmentExpr &&
                !node.children.empty() &&
                node.children.front().type == Parser::NodeType::Identifier)
            {
                const std::string target =
                    std::string(std::get<std::string_view>(node.children.front().value));

                if (const auto bound = boundTunnelSlots.find(target);
                    bound != boundTunnelSlots.end())
                {
                    mark_slot_filled(flows, bound->second, node);
                }

                return flows;
            }

            if (node.type == Parser::NodeType::MatchStmt &&
                node.children.size() >= 2)
            {
                const Flows incoming = flows;
                std::vector<Flows> branchFlows;
                bool hasDefault = false;

                for (size_t index = 1; index < node.children.size(); ++index)
                {
                    const Parser::ASTNode &branch = node.children[index];

                    if (branch.type == Parser::NodeType::MatchCase &&
                        branch.children.size() >= 2)
                    {
                        branchFlows.push_back(
                            self(self, branch.children[1], incoming));
                    }
                    else if (branch.type == Parser::NodeType::MatchDefault &&
                             !branch.children.empty())
                    {
                        hasDefault = true;
                        branchFlows.push_back(
                            self(self, branch.children.front(), incoming));
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
                        flow.may_fill =
                            flow.may_fill || branch.at(name).may_fill;
                        flow.must_fill =
                            flow.must_fill && branch.at(name).must_fill;
                    }
                }

                return flows;
            }

            if (node.type == Parser::NodeType::IfStmt &&
                node.children.size() >= 2)
            {
                Flows then_flow =
                    self(self, node.children[1], flows);

                Flows else_flow = flows;

                if (node.children.size() >= 3)
                    else_flow =
                        self(self, node.children[2], flows);

                for (auto &[name, flow] : flows)
                {
                    flow.may_fill =
                        then_flow[name].may_fill ||
                        else_flow[name].may_fill;

                    flow.must_fill =
                        then_flow[name].must_fill &&
                        else_flow[name].must_fill;
                }

                return flows;
            }

            if (node.type == Parser::NodeType::WhileLoop ||
                node.type == Parser::NodeType::ForLoop ||
                node.type == Parser::NodeType::ForeachLoop)
            {
                Flows body_flow = flows;

                for (const auto &child : node.children)
                    body_flow = self(self, child, body_flow);

                for (auto &[name, flow] : flows)
                {
                    flow.may_fill =
                        flow.may_fill || body_flow[name].may_fill;
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
                error("Function exiting without populating required tunnel slot '" + name + "'.",
                      funcNode);
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
            if (!is_numeric(leftType.kind) || !is_numeric(rightType.kind))
            {
                error("Range endpoints must be numeric.", exprNode);
            }
            exprNode.hasInferredType = true;
            exprNode.inferredTypeName = type_to_string(leftType);
            return;
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
                             : Type(TypeKind::U64);
        else if (leftType.kind == rightType.kind)
            resultType = leftType;

        exprNode.hasInferredType = true;
        exprNode.inferredTypeName = type_to_string(resultType);
    }

    void declare_function_symbols(SymbolTable &table, Parser::ASTNode &node,
                                  const std::string &namespaceName = {})
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
        if (node.type == Parser::NodeType::StructDecl)
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::Identifier)
                return;
            structNameStack.push_back(std::string(std::get<std::string_view>(node.children.front().value)));
            for (auto &child : node.children)
                declare_function_symbols(table, child, namespaceName);
            structNameStack.pop_back();
            return;
        }
        if (node.type == Parser::NodeType::FunctionDecl ||
            node.type == Parser::NodeType::FunctionDef ||
            node.type == Parser::NodeType::CFunctionDecl ||
            node.type == Parser::NodeType::CFunctionDef)
        {
            if (node.children.empty() || node.children.front().type != Parser::NodeType::Identifier)
                return;

            const std::string unqualifiedName =
                std::string(std::get<std::string_view>(node.children.front().value));
            const std::string name = namespaceName.empty() ? unqualifiedName : namespaceName + "::" + unqualifiedName;
            Symbol symbol;
            symbol.name = name;
            symbol.isFunction = true;
            std::vector<std::string> genericParameters;
            for (const auto &child : node.children)
                if (child.type == Parser::NodeType::GenericParam)
                {
                    genericParameters.push_back(std::string(std::get<std::string_view>(child.value)));
                    activeGenericNames.push_back(genericParameters.back());
                }
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
            if (existing)
            {
                if (!existing->isFunction || existing->paramTypes.size() != symbol.paramTypes.size())
                {
                    error("Conflicting declaration of function '" + name + "'.", node);
                }
                return;
            }
            if (!namespaceName.empty() && !table.lookup(unqualifiedName))
                table.declare(unqualifiedName, symbol);
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
            if (functionBodyDepth == 0)
            {
                error("Top-level variable declarations are not supported; declare the value inside a function.", node);
            }
            check_assignment_and_ownership(table, node);
            {
                const auto validate_runtime_array_lengths = [&](const auto &self, const Parser::ASTNode &typeNode) -> void {
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
            const Parser::ASTNode &call = node.children.back();
            if (call.children.empty() || call.children.front().type != Parser::NodeType::Identifier)
            {
                error("Multi-result reserve requires a named tunnel function.", node);
            }
            const std::string calleeName = std::string(std::get<std::string_view>(call.children.front().value));
            const Symbol *callee = table.lookup(calleeName);
            const size_t bindingCount = (node.children.size() - 1) / 2;
            if (!callee || !callee->isFunction || callee->tunnelSlotTypes.size() != bindingCount)
            {
                error("Multi-result reserve binding count does not match tunnel outputs.", node);
            }
            for (size_t index = 0; index < bindingCount; ++index)
            {
                const Type bindingType = infer_type_from_node(node.children[index * 2]);
                const Type bindingPayload = bindingType.isOptional && bindingType.innerType ? *bindingType.innerType : bindingType;
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
                    const Type bindingPayload = declared.isOptional && declared.innerType ? *declared.innerType : declared;
                    if (bindingPayload.kind != slotType.kind || bindingPayload.name != slotType.name ||
                        declared.isOptional != callee->tunnelSlotOptional.front())
                    {
                        error("reserve binding type does not match tunnel output.", node);
                    }
                }
            }
            if (node.children.size() > 1 && node.children[1].type != Parser::NodeType::TunnelBindingExpr &&
                !is_assignable(declared, infer_expression_type(table, node.children[1])))
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
                                             node.type == Parser::NodeType::ThreadBindingDecl
                                                 ? Lexer::Keyword::THREAD
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
                const std::string name =
                    std::string(std::get<std::string_view>(node.children.front().value));
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
                if (found->second.hasExpectedType &&
                    !is_assignable(found->second.expectedType, value))
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
            if (!symbol || !infer_type_from_node(symbol->typeNode).isOptional ||
                symbol->typeNode.children.empty())
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
            if (!node.children.empty() &&
                node.children.front().type == Parser::NodeType::Identifier)
                structName = std::string(std::get<std::string_view>(node.children.front().value));

            structNameStack.push_back(structName);
            size_t genericsPushed = push_generic_params(node.children);

            table.push_scope();
            for (auto &child : node.children)
            {
                if (child.type == Parser::NodeType::FunctionDef ||
                    child.type == Parser::NodeType::FunctionDecl ||
                    child.type == Parser::NodeType::CFunctionDef ||
                    child.type == Parser::NodeType::CFunctionDecl)
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
        case Parser::NodeType::CFunctionDecl:
            for (const auto &child : node.children)
                if (child.type == Parser::NodeType::OptionalType || child.type == Parser::NodeType::ArrayType)
                {
                    error("C-ABI functions cannot return optional or array values.", node);
                }
            break;
        case Parser::NodeType::UsingMacroDecl:
            break;

        case Parser::NodeType::ExportDecl:
            for (auto &child : node.children)
                check_node(table, child);
            break;

        case Parser::NodeType::ReturnStmt:
        {
            if (!inCFunctionBody)
            {
                error("'return' is not allowed in a tunnel-based function (def/dec); use 'tunnel value -> Type slot;' instead. 'return' is only valid in cdef/cdec (C-ABI) functions.", node);
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
                node.children.front().type == Parser::NodeType::CallExpr &&
                !node.children.front().children.empty() &&
                node.children.front().children.front().type == Parser::NodeType::Identifier)
            {
                const std::string_view callee =
                    std::get<std::string_view>(node.children.front().children.front().value);
            }
            if (!is_assignable(activeCFunctionReturnType, returnedType))
            {
                error("Return type does not match function signature.", node);
            }
            check_node(table, node.children.front());
            break;
        }

        case Parser::NodeType::FunctionDef:
        {
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
                    paramSym.typeNode =
                        child.children.empty() ? Parser::ASTNode{} : child.children.front();
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
            for (const auto &child : node.children)
            {
                if (child.type == Parser::NodeType::PrimitiveType ||
                    child.type == Parser::NodeType::PointerType ||
                    child.type == Parser::NodeType::ReferenceType ||
                    child.type == Parser::NodeType::CustomType)
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
                    paramSym.typeNode =
                        child.children.empty() ? Parser::ASTNode{} : child.children.front();
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
            table.push_scope();
            Symbol item;
            item.name = std::string(std::get<std::string_view>(node.value));
            item.typeNode = node.children[0];
            if (!table.declare(item.name, item))
            {
                error("Redeclaration of foreach-variable.", node);
            }
            loopDepth++;
            check_node(table, node.children[1]);
            check_node(table, node.children[2]);
            loopDepth--;
            table.pop_scope();
            break;
        }

        case Parser::NodeType::BreakStmt:
        case Parser::NodeType::ContinueStmt:
            if (loopDepth == 0)
            {
                error(std::string("'") + (node.type == Parser::NodeType::BreakStmt ? "break" : "continue") + "' is only valid inside a loop.", node);
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
            if (node.type == Parser::NodeType::StructDecl || node.type == Parser::NodeType::EnumDecl)
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
        for (auto &root_node : Parser::ast)
        {
            predeclare_types(predeclare_types, root_node);
        }
        for (auto &root_node : Parser::ast)
            declare_function_symbols(table, root_node);
        for (auto &root_node : Parser::ast)
            check_node(table, root_node);
        table.pop_scope();
    }
} // namespace Checker