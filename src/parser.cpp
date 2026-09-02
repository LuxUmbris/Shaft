#include "parser.hpp"
#include "lexer.hpp"
#include <iostream>

namespace Parser
{
    // forward declarations
    static ASTNode parse_expression();
    static ASTNode parse_assignment();
    static ASTNode parse_binary(int min_precedence);
    static ASTNode parse_unary();
    static ASTNode parse_postfix();
    static ASTNode parse_primary();
    static ASTNode parse_struct_init();
    static std::vector<ASTNode> parse_arg_list();
    static ASTNode parse_type_node();
    static ASTNode parse_type_node_base();
    static ASTNode parse_block();
    static ASTNode parse_statement();
    static ASTNode parse_return_statement();
    static ASTNode parse_expression_statement();
    static ASTNode parse_if_statement();
    static ASTNode parse_valid_statement();
    static ASTNode parse_match_statement();
    static ASTNode parse_while_loop();
    static ASTNode parse_for_loop();
    static ASTNode parse_foreach_loop();
    static ASTNode parse_tunnel_statement();
    static ASTNode parse_reserve_statement();
    static ASTNode parse_start_statement();
    static ASTNode parse_thread_task_stmt();
    static ASTNode parse_state_binding_decl();
    static std::vector<ASTNode> parse_param_list();
    static std::vector<ASTNode> parse_generic_param_list();
    static ASTNode parse_function_common(NodeType nodeType, bool hasBody, bool allowGenerics,
                                         bool allowAsync, bool isCFunction);
    static ASTNode parse_namespace_declaration();
    static ASTNode parse_function_declaration();
    static ASTNode parse_function_definition();
    static ASTNode parse_c_function_declaration();
    static ASTNode parse_c_function_definition();
    static ASTNode parse_struct_declaration();
    static ASTNode parse_aligned_struct_declaration();
    static ASTNode parse_class_declaration();
    static ASTNode parse_enum_declaration();
    static ASTNode parse_import_statement();
    static ASTNode parse_export_declaration();
    static ASTNode parse_using_macro_declaration();
    static ASTNode parse_variable_declaration();
    static ASTNode parse_variable_declaration_with_custom_type();
    static ASTNode parse_top_level_declaration();
    static void parse_module();

    std::vector<Lexer::Token> tokens;
    uint64_t pos = 0;
    uint64_t deferredGenericClosers = 0;
    std::vector<ASTNode> ast;

    static inline Lexer::Token peek(uint64_t offset = 0)
    {
        if (pos + offset >= tokens.size()) {
            static Lexer::Token eof{
                Lexer::TokenType::EndOfFile,
                0,
                std::monostate{},
                nullptr,
                nullptr
            };
            return eof;
        }
        return tokens[pos + offset];
    }


    static inline Lexer::Token advance()
    {
        if (pos < tokens.size())
            return tokens[pos++];

        static Lexer::Token eof{
            Lexer::TokenType::EndOfFile,
            0,
            std::monostate{},
            nullptr,
            nullptr
        };
        return eof;
    }


    static inline bool check(Lexer::TokenType type, uint64_t offset = 0)
    {
        if (pos + offset >= tokens.size())
            return false;
        return tokens[pos + offset].type == type;
    }

    static inline bool check_op(Lexer::Operator op, uint64_t offset = 0)
    {
        return check(Lexer::TokenType::Operator, offset) &&
               std::get<Lexer::Operator>(peek(offset).value) == op;
    }

    static inline bool check_kw(Lexer::Keyword kw, uint64_t offset = 0)
    {
        return check(Lexer::TokenType::Keyword, offset) &&
               std::get<Lexer::Keyword>(peek(offset).value) == kw;
    }

    static inline void error(const std::string &error_message)
    {
        Lexer::Token token = peek();

        std::string module = token.mod_path ? *token.mod_path : "<unknown>";
        ErrorPos error_pos{1, 1};

        if (token.source)
            error_pos = get_error_pos(token.start, token.source);

        Error err = {error_message, module, error_pos.line, error_pos.column};
        panic(err);
    }


    static inline Lexer::Token consume(Lexer::TokenType type, const std::string &error_message)
    {
        if (check(type))
        {
            return advance();
        }
        error(error_message);
    }

    static inline Lexer::Token consume_op(Lexer::Operator op, const std::string &error_message)
    {
        if (check_op(op))
            return advance();
        error(error_message);
    }

    static inline Lexer::Token consume_kw(Lexer::Keyword kw, const std::string &error_message)
    {
        if (check_kw(kw))
            return advance();
        error(error_message);
    }

    static inline bool consume_generic_closer(const std::string &error_message)
    {
        if (deferredGenericClosers != 0)
        {
            --deferredGenericClosers;
            return true;
        }
        if (check_op(Lexer::Operator::GREATER_THAN))
        {
            advance();
            return true;
        }
        if (check_op(Lexer::Operator::RIGHT_SHIFT))
        {
            advance();
            deferredGenericClosers = 1;
            return true;
        }
        error(error_message);
    }

    static bool is_primitive_type_keyword(Lexer::Keyword kw)
    {
        switch (kw)
        {
        case Lexer::Keyword::U8:
        case Lexer::Keyword::U16:
        case Lexer::Keyword::U32:
        case Lexer::Keyword::U64:
        case Lexer::Keyword::USIZE:
        case Lexer::Keyword::I8:
        case Lexer::Keyword::I16:
        case Lexer::Keyword::I32:
        case Lexer::Keyword::I64:
        case Lexer::Keyword::F32:
        case Lexer::Keyword::F64:
        case Lexer::Keyword::BOOL:
        case Lexer::Keyword::CHAR:
        case Lexer::Keyword::STATE:
        case Lexer::Keyword::THREAD:
            return true;
        default:
            return false;
        }
    }

    // Check for a custom-type declaration.
    static bool at_custom_type_var_decl_start()
    {
        if (!check(Lexer::TokenType::Identifier))
            return false;
        uint64_t offset = 1;
        while (check_op(Lexer::Operator::DOUBLE_COLON, offset) &&
               check(Lexer::TokenType::Identifier, offset + 1))
            offset += 2;

        if (check_op(Lexer::Operator::LESS_THAN, offset))
        {
            int depth = 0;
            uint64_t i = offset;
            while (true)
            {
                if (peek(i).type == Lexer::TokenType::EndOfFile)
                    return false;

                if (check_op(Lexer::Operator::LESS_THAN, i))
                {
                    depth++;
                    i++;
                    continue;
                }
                if (check_op(Lexer::Operator::GREATER_THAN, i))
                {
                    depth--;
                    i++;
                    if (depth <= 0)
                    {
                        offset = i;
                        break;
                    }
                    continue;
                }
                if (check_op(Lexer::Operator::RIGHT_SHIFT, i))
                {
                    depth -= 2;
                    i++;
                    if (depth <= 0)
                    {
                        offset = i;
                        break;
                    }
                    continue;
                }
                i++;
            }
        }

        while (check_op(Lexer::Operator::LEFT_BRACKET, offset) &&
               check_op(Lexer::Operator::RIGHT_BRACKET, offset + 1))
            offset += 2;
        return check(Lexer::TokenType::Identifier, offset);
    }

    // Distinguish declarations from expressions.
    static bool at_type_start()
    {
        if (check(Lexer::TokenType::Keyword))
        {
            Lexer::Keyword kw = std::get<Lexer::Keyword>(peek().value);
            return is_primitive_type_keyword(kw) || kw == Lexer::Keyword::MUT;
        }
        if (check_op(Lexer::Operator::MULTIPLY) || check_op(Lexer::Operator::AMPERSAND) ||
            check_op(Lexer::Operator::QUESTION_MARK))
            return true;
        return at_custom_type_var_decl_start();
    }

    // A leading '*' or '&' may begin either a type or a unary expression. Only
    // treat it as a declaration when a complete type is followed by a name.
    static bool at_variable_declaration_start()
    {
        uint64_t offset = 0;
        if (check_kw(Lexer::Keyword::MUT, offset))
            ++offset;
        while (check_op(Lexer::Operator::MULTIPLY, offset) ||
               check_op(Lexer::Operator::AMPERSAND, offset) ||
               check_op(Lexer::Operator::QUESTION_MARK, offset))
        {
            ++offset;
            if (check_kw(Lexer::Keyword::MUT, offset))
                ++offset;
        }

        if (check(Lexer::TokenType::Keyword, offset) &&
            is_primitive_type_keyword(std::get<Lexer::Keyword>(peek(offset).value)))
        {
            ++offset;
        }
        else if (check(Lexer::TokenType::Identifier, offset))
        {
            ++offset;
            while (check_op(Lexer::Operator::DOUBLE_COLON, offset) &&
                   check(Lexer::TokenType::Identifier, offset + 1))
                offset += 2;

            if (check_op(Lexer::Operator::LESS_THAN, offset))
            {
                int depth = 0;
                do
                {
                    if (check_op(Lexer::Operator::LESS_THAN, offset))
                        ++depth;
                    else if (check_op(Lexer::Operator::GREATER_THAN, offset))
                        --depth;
                    else if (check_op(Lexer::Operator::RIGHT_SHIFT, offset))
                        depth -= 2;
                    ++offset;
                } while (depth > 0 && peek(offset).type != Lexer::TokenType::EndOfFile);
                if (depth > 0)
                    return false;
            }
        }
        else
        {
            return false;
        }

        while (check_op(Lexer::Operator::LEFT_BRACKET, offset))
        {
            ++offset;
            if (check(Lexer::TokenType::IntegerLiteral, offset) ||
                check(Lexer::TokenType::Identifier, offset))
                ++offset;
            if (!check_op(Lexer::Operator::RIGHT_BRACKET, offset))
                return false;
            ++offset;
        }
        return check(Lexer::TokenType::Identifier, offset);
    }

    static void syncronize()
    {
        advance();

        while (peek().type != Lexer::TokenType::EndOfFile)
        {
            if (peek().type == Lexer::TokenType::Operator &&
                std::get<Lexer::Operator>(peek().value) == Lexer::Operator::SEMICOLON)
            {
                advance();
                return;
            }

            if (peek().type == Lexer::TokenType::Keyword)
                return;

            advance();
        }
    }


    static ASTNode parse_primary()
    {
        if (check_op(Lexer::Operator::LEFT_PAREN))
        {
            advance();
            ASTNode inner = parse_expression();
            consume_op(Lexer::Operator::RIGHT_PAREN, "Expected ')' after expression");
            return inner;
        }

        if (check_op(Lexer::Operator::LEFT_BRACE))
            return parse_struct_init();

        if (check_kw(Lexer::Keyword::SELF))
        {
            Lexer::Token token = advance();
            return ASTNode{
                NodeType::Identifier, token.start, token.mod_path, token.source, std::string_view("self"), {}};
        }
        if (check_kw(Lexer::Keyword::REF))
        {
            Lexer::Token token = advance();
            ASTNode source = parse_postfix();
            return ASTNode{NodeType::RefExpr, token.start, token.mod_path, token.source, std::monostate{}, {std::move(source)}};
        }
        if (check_kw(Lexer::Keyword::SIZEOF))
        {
            Lexer::Token token = advance();
            consume_op(Lexer::Operator::LEFT_PAREN, "Expected '(' after sizeof");
            ASTNode type = parse_type_node();
            consume_op(Lexer::Operator::RIGHT_PAREN, "Expected ')' after sizeof type");
            return ASTNode{NodeType::SizeofExpr, token.start, token.mod_path, token.source, std::monostate{}, {std::move(type)}};
        }

        Lexer::Token token = advance();

        switch (token.type)
        {
        case Lexer::TokenType::Identifier:
            return ASTNode{NodeType::Identifier,
                           token.start,
                           token.mod_path, token.source, std::get<std::string_view>(token.value),
                           {}};
        case Lexer::TokenType::IntegerLiteral:
            return ASTNode{NodeType::IntegerLiteral,
                           token.start,
                           token.mod_path, token.source, std::get<uint64_t>(token.value),
                           {}};
        case Lexer::TokenType::FloatLiteral:
            return ASTNode{NodeType::FloatLiteral,
                           token.start,
                           token.mod_path, token.source, std::get<double>(token.value),
                           {}};
        case Lexer::TokenType::StringLiteral:
            return ASTNode{NodeType::StringLiteral,
                           token.start,
                           token.mod_path, token.source, std::get<std::string_view>(token.value),
                           {}};
        case Lexer::TokenType::CharLiteral:
            return ASTNode{NodeType::CharLiteral,
                           token.start,
                           token.mod_path, token.source, std::get<std::string_view>(token.value),
                           {}};
        case Lexer::TokenType::BoolLiteral:
            return ASTNode{
                NodeType::BoolLiteral, token.start, token.mod_path, token.source, std::get<bool>(token.value), {}};
        default:
            error("Expected an expression");
            return ASTNode{NodeType::Identifier, token.start, token.mod_path, token.source, std::monostate{}, {}};
        }
    }

    static std::vector<ASTNode> parse_arg_list()
    {
        std::vector<ASTNode> args;
        if (check_op(Lexer::Operator::RIGHT_PAREN))
            return args;
        while (true)
        {
            args.push_back(parse_expression());
            if (check_op(Lexer::Operator::COMMA))
            {
                advance();
                continue;
            }
            break;
        }
        return args;
    }

    static ASTNode parse_struct_init()
    {
        Lexer::Token brace = advance(); // consume '{'
        ASTNode node{NodeType::StructInitExpr, brace.start, brace.mod_path, brace.source, std::monostate{}, {}};
        while (!check_op(Lexer::Operator::RIGHT_BRACE))
        {
            if (peek().type == Lexer::TokenType::EndOfFile)
                error("Unterminated struct initializer");
            node.children.push_back(parse_expression());
            if (check_op(Lexer::Operator::COMMA))
                advance();
        }
        advance(); // consume '}'
        return node;
    }

    static ASTNode parse_postfix()
    {
        ASTNode expr = parse_primary();

        while (true)
        {
            if (check_op(Lexer::Operator::LEFT_PAREN))
            {
                Lexer::Token paren = advance();
                std::vector<ASTNode> args = parse_arg_list();
                consume_op(Lexer::Operator::RIGHT_PAREN, "Expected ')' after call arguments");

                ASTNode call{NodeType::CallExpr, paren.start, paren.mod_path, paren.source, std::monostate{}, {}};
                call.children.push_back(expr);
                for (auto &arg : args)
                    call.children.push_back(std::move(arg));
                expr = std::move(call);
            }
            else if (check_op(Lexer::Operator::LEFT_BRACKET))
            {
                Lexer::Token bracket = advance();
                ASTNode index = parse_expression();
                consume_op(Lexer::Operator::RIGHT_BRACKET, "Expected ']' after array index");
                ASTNode access{NodeType::IndexExpr, bracket.start, bracket.mod_path, bracket.source, std::monostate{}, {}};
                access.children.push_back(std::move(expr));
                access.children.push_back(std::move(index));
                expr = std::move(access);
            }
            else if (check_op(Lexer::Operator::DOT))
            {
                Lexer::Token dot = advance();
                Lexer::Token member =
                    consume(Lexer::TokenType::Identifier, "Expected member name after '.'");
                ASTNode access{NodeType::MemberAccessExpr,
                               dot.start,
                               dot.mod_path, dot.source, std::get<std::string_view>(member.value),
                               {}};
                access.children.push_back(expr);
                expr = std::move(access);
            }
            else if (check_op(Lexer::Operator::DOUBLE_COLON))
            {
                // Explicit generic call.
                if (check_op(Lexer::Operator::LESS_THAN, 1))
                {
                    Lexer::Token colons = advance(); // '::'
                    advance();                       // '<'

                    std::vector<ASTNode> generic_args;
                    while (true)
                    {
                        ASTNode arg_type = parse_type_node();
                        ASTNode arg_node{
                            NodeType::GenericArg, colons.start, colons.mod_path, colons.source, std::monostate{}, {}};
                        arg_node.children.push_back(arg_type);
                        generic_args.push_back(arg_node);
                        if (check_op(Lexer::Operator::COMMA))
                        {
                            advance();
                            continue;
                        }
                        break;
                    }
                    consume_generic_closer("Expected '>' after explicit generic arguments");

                    Lexer::Token paren = consume_op(Lexer::Operator::LEFT_PAREN,
                                                    "Expected '(' after generic call arguments");
                    std::vector<ASTNode> args = parse_arg_list();
                    consume_op(Lexer::Operator::RIGHT_PAREN, "Expected ')' after call arguments");

                    ASTNode call{
                        NodeType::CallExpr, paren.start, paren.mod_path, paren.source, std::monostate{}, {}};
                    call.children.push_back(expr);
                    for (auto &g : generic_args)
                        call.children.push_back(std::move(g));
                    for (auto &arg : args)
                        call.children.push_back(std::move(arg));
                    expr = std::move(call);
                    continue;
                }

                Lexer::Token colons = advance();
                Lexer::Token member =
                    consume(Lexer::TokenType::Identifier, "Expected name after '::'");
                ASTNode access{NodeType::ScopeAccessExpr,
                               colons.start,
                               colons.mod_path, colons.source, std::get<std::string_view>(member.value),
                               {}};
                access.children.push_back(expr);
                expr = std::move(access);
            }
            else
            {
                break;
            }
        }

        return expr;
    }

    static ASTNode parse_unary()
    {
        if (check_op(Lexer::Operator::EXLAMATION_MARK) || check_op(Lexer::Operator::MINUS) ||
            check_op(Lexer::Operator::PLUS) || check_op(Lexer::Operator::MULTIPLY) ||
            check_op(Lexer::Operator::AMPERSAND))
        {
            Lexer::Token op = advance();
            ASTNode operand = parse_unary();
            ASTNode node{
                NodeType::UnaryExpr, op.start, op.mod_path, op.source, std::get<Lexer::Operator>(op.value), {}};
            node.children.push_back(operand);
            return node;
        }

        if (check_kw(Lexer::Keyword::AWAIT))
        {
            Lexer::Token op = advance();
            ASTNode operand = parse_unary();
            ASTNode node{NodeType::AwaitExpr, op.start, op.mod_path, op.source, std::monostate{}, {}};
            node.children.push_back(operand);
            return node;
        }

        return parse_postfix();
    }

    // returns -1 if the token is not a known binary op
    static int binary_precedence(Lexer::Operator op)
    {
        switch (op)
        {
        case Lexer::Operator::OR:
            return 1;
        case Lexer::Operator::AND:
            return 2;
        case Lexer::Operator::PIPE:
            return 3;
        case Lexer::Operator::CARET:
            return 4;
        case Lexer::Operator::AMPERSAND:
            return 5;
        case Lexer::Operator::LESS_THAN:
        case Lexer::Operator::GREATER_THAN:
        case Lexer::Operator::LESS_EQUAL:
        case Lexer::Operator::GREATER_EQUAL:
        case Lexer::Operator::EQUAL:
        case Lexer::Operator::NOT_EQUAL:
            return 6;
        case Lexer::Operator::LEFT_SHIFT:
        case Lexer::Operator::RIGHT_SHIFT:
            return 7;
        case Lexer::Operator::TRIPPLE_DOT:
            return 8;
        case Lexer::Operator::PLUS:
        case Lexer::Operator::MINUS:
            return 9;
        case Lexer::Operator::MULTIPLY:
        case Lexer::Operator::DIVIDE:
        case Lexer::Operator::MODULO:
            return 10;
        default:
            return -1;
        }
    }

    static ASTNode parse_binary(int min_precedence)
    {
        ASTNode lhs = parse_unary();

        while (check(Lexer::TokenType::Operator))
        {
            Lexer::Operator op = std::get<Lexer::Operator>(peek().value);
            int prec = binary_precedence(op);
            if (prec < min_precedence)
                break;

            Lexer::Token op_token = advance();
            ASTNode rhs = parse_binary(prec + 1);

            ASTNode node{NodeType::BinaryExpr, op_token.start, op_token.mod_path, op_token.source, op, {}};
            node.children.push_back(lhs);
            node.children.push_back(rhs);
            lhs = std::move(node);
        }

        return lhs;
    }

    static bool is_assignment_operator(Lexer::Operator op)
    {
        switch (op)
        {
        case Lexer::Operator::ASSIGN:
        case Lexer::Operator::ADD_ASSIGN:
        case Lexer::Operator::SUBTRACT_ASSIGN:
        case Lexer::Operator::MULTIPLY_ASSIGN:
        case Lexer::Operator::DIVIDE_ASSIGN:
        case Lexer::Operator::MODULO_ASSIGN:
        case Lexer::Operator::LEFT_SHIFT_ASSIGN:
        case Lexer::Operator::RIGHT_SHIFT_ASSIGN:
            return true;
        default:
            return false;
        }
    }

    static ASTNode parse_assignment()
    {
        ASTNode lhs = parse_binary(1);

        if (check(Lexer::TokenType::Operator))
        {
            Lexer::Operator op = std::get<Lexer::Operator>(peek().value);
            if (is_assignment_operator(op))
            {
                Lexer::Token op_token = advance();

                ASTNode rhs;
                if (check_kw(Lexer::Keyword::MOVE))
                {
                    Lexer::Token move_token = advance();
                    ASTNode source = parse_postfix();
                    rhs = ASTNode{NodeType::MoveExpr,
                                  move_token.start,
                                  move_token.mod_path, move_token.source, std::monostate{},
                                  {std::move(source)}};
                }
                else if (check_kw(Lexer::Keyword::REF))
                {
                    Lexer::Token ref_token = advance();
                    ASTNode source = parse_postfix();
                    rhs = ASTNode{NodeType::RefExpr,
                                  ref_token.start,
                                  ref_token.mod_path, ref_token.source, std::monostate{},
                                  {std::move(source)}};
                }
                else
                {
                    rhs = parse_assignment();
                }

                ASTNode node{NodeType::AssignmentExpr,
                             op_token.start,
                             op_token.mod_path, op_token.source, std::get<Lexer::Operator>(op_token.value),
                             {}};
                node.children.push_back(lhs);
                node.children.push_back(rhs);
                return node;
            }
        }

        return lhs;
    }

    static ASTNode parse_expression() { return parse_assignment(); }

    static ASTNode parse_type_node_base()
    {
        if (check_op(Lexer::Operator::MULTIPLY))
        {
            Lexer::Token star_token = advance();
            bool is_mutable = false;
            if (check_kw(Lexer::Keyword::MUT))
            {
                advance();
                is_mutable = true;
            }

            ASTNode pointee_type = parse_type_node();
            return ASTNode{NodeType::PointerType,
                           star_token.start,
                           star_token.mod_path, star_token.source, is_mutable,
                           {pointee_type}};
        }

        if (check_op(Lexer::Operator::AMPERSAND))
        {
            Lexer::Token ampersand_token = advance();
            bool is_mutable = false;
            if (check_kw(Lexer::Keyword::MUT))
            {
                advance();
                is_mutable = true;
            }

            ASTNode referred_type = parse_type_node();
            return ASTNode{NodeType::ReferenceType,
                           ampersand_token.start,
                           ampersand_token.mod_path, ampersand_token.source, is_mutable,
                           {referred_type}};
        }

        if (check_op(Lexer::Operator::QUESTION_MARK))
        {
            Lexer::Token q_token = advance();
            ASTNode inner_type = parse_type_node();
            ASTNode node{
                NodeType::OptionalType, q_token.start, q_token.mod_path, q_token.source, std::monostate{}, {}};
            node.isOptional = true;
            node.children.push_back(inner_type);
            return node;
        }

        if (check_kw(Lexer::Keyword::MUT))
        {
            Lexer::Token mut_token = advance();
            ASTNode inner_type = parse_type_node();
            inner_type.isMutable = true;
            return inner_type;
        }

        if (check(Lexer::TokenType::Keyword))
        {
            Lexer::Token type_token = advance();
            const Lexer::Keyword keyword = std::get<Lexer::Keyword>(type_token.value);
            switch (keyword)
            {
            case Lexer::Keyword::U8:
            case Lexer::Keyword::U16:
            case Lexer::Keyword::U32:
            case Lexer::Keyword::U64:
            case Lexer::Keyword::USIZE:
            case Lexer::Keyword::I8:
            case Lexer::Keyword::I16:
            case Lexer::Keyword::I32:
            case Lexer::Keyword::I64:
            case Lexer::Keyword::F32:
            case Lexer::Keyword::F64:
            case Lexer::Keyword::BOOL:
            case Lexer::Keyword::CHAR:
            case Lexer::Keyword::STATE:
            case Lexer::Keyword::THREAD:
                break;
            default:
                error("Expected a type keyword");
            }
            return ASTNode{NodeType::PrimitiveType,
                           type_token.start,
                           type_token.mod_path, type_token.source, keyword,
                           {}};
        }

        if (check(Lexer::TokenType::Identifier))
        {
            Lexer::Token type_token = advance();
            std::string qualified_name = std::string(std::get<std::string_view>(type_token.value));

            while (check_op(Lexer::Operator::DOUBLE_COLON) &&
                   check(Lexer::TokenType::Identifier, 1))
            {
                advance(); // '::'
                type_token = advance();
                qualified_name += "::";
                qualified_name += std::get<std::string_view>(type_token.value);
            }

            ASTNode node{NodeType::CustomType,
                         type_token.start,
                         type_token.mod_path, type_token.source, std::get<std::string_view>(type_token.value),
                         {}};
            node.inferredTypeName = std::move(qualified_name);

            if (check_op(Lexer::Operator::LESS_THAN))
            {
                advance();
                while (true)
                {
                    node.children.push_back(parse_type_node());
                    if (check_op(Lexer::Operator::COMMA))
                    {
                        advance();
                        continue;
                    }
                    break;
                }
                consume_generic_closer("Expected '>' after generic arguments");
            }

            return node;
        }

        error("Expected type");
    }

    static ASTNode parse_type_node()
    {
        ASTNode node = parse_type_node_base();

        while (check_op(Lexer::Operator::LEFT_BRACKET))
        {
            Lexer::Token bracket = advance();
            ASTNode array{NodeType::ArrayType, bracket.start, bracket.mod_path, bracket.source, std::monostate{}, {}};

            if (!check_op(Lexer::Operator::RIGHT_BRACKET))
            {
                if (peek().type == Lexer::TokenType::IntegerLiteral)
                {
                    Lexer::Token sizeToken = advance();
                    array.value = std::get<uint64_t>(sizeToken.value);
                }
                else if (peek().type == Lexer::TokenType::Identifier)
                {
                    Lexer::Token sizeToken = advance();
                    array.value = std::get<std::string_view>(sizeToken.value);
                }
                else
                {
                    error("Expected an integer literal or runtime size name in array type");
                }
            }

            consume_op(Lexer::Operator::RIGHT_BRACKET, "Expected ']' after array type");
            array.isMutable = node.isMutable;
            array.children.push_back(node);
            node = std::move(array);
        }

        return node;
    }

    static ASTNode parse_block()
    {
        Lexer::Token brace = consume_op(Lexer::Operator::LEFT_BRACE, "Expected '{' to start block");
        ASTNode node{NodeType::BlockStmt, brace.start, brace.mod_path, brace.source, std::monostate{}, {}};

        while (!check_op(Lexer::Operator::RIGHT_BRACE))
        {
            if (peek().type == Lexer::TokenType::EndOfFile)
                error("Unterminated block");
            node.children.push_back(parse_statement());
        }
        advance(); // consume '}'
        return node;
    }

    static ASTNode parse_expression_statement()
    {
        ASTNode expr = parse_expression();
        consume_op(Lexer::Operator::SEMICOLON, "Expected ';' after expression");
        ASTNode node{NodeType::ExprStmt, expr.start, expr.mod_path, expr.source, std::monostate{}, {}};
        node.children.push_back(expr);
        return node;
    }

    static ASTNode parse_if_statement()
    {
        Lexer::Token token = advance(); // 'if'
        consume_op(Lexer::Operator::LEFT_PAREN, "Expected '(' after 'if'");
        ASTNode cond = parse_expression();
        consume_op(Lexer::Operator::RIGHT_PAREN, "Expected ')' after if condition");
        ASTNode then_block = parse_block();

        ASTNode node{NodeType::IfStmt, token.start, token.mod_path, token.source, std::monostate{}, {}};
        node.children.push_back(cond);
        node.children.push_back(then_block);

        if (check_kw(Lexer::Keyword::ELSE))
        {
            advance();
            if (check_kw(Lexer::Keyword::IF))
                node.children.push_back(parse_if_statement());
            else
                node.children.push_back(parse_block());
        }

        return node;
    }

    static ASTNode parse_valid_statement()
    {
        Lexer::Token token = advance(); // 'valid'
        Lexer::Token name =
            consume(Lexer::TokenType::Identifier, "Expected identifier after 'valid'");

        ASTNode node{
            NodeType::ValidStmt, token.start, token.mod_path, token.source, std::get<std::string_view>(name.value), {}};
        node.children.push_back(parse_block());

        if (check_kw(Lexer::Keyword::ELSE))
        {
            advance();
            node.children.push_back(parse_block());
        }

        return node;
    }

    static ASTNode parse_match_statement()
    {
        Lexer::Token token = advance(); // 'match'
        consume_op(Lexer::Operator::LEFT_PAREN, "Expected '(' after 'match'");
        ASTNode subject = parse_expression();
        consume_op(Lexer::Operator::RIGHT_PAREN, "Expected ')' after match subject");

        ASTNode node{NodeType::MatchStmt, token.start, token.mod_path, token.source, std::monostate{}, {}};
        node.children.push_back(subject);

        consume_op(Lexer::Operator::LEFT_BRACE, "Expected '{' to start match body");
        while (!check_op(Lexer::Operator::RIGHT_BRACE))
        {
            if (peek().type == Lexer::TokenType::EndOfFile)
                error("Unterminated match statement");

            if (check_kw(Lexer::Keyword::CASE))
            {
                Lexer::Token case_tok = advance();
                ASTNode value = parse_expression();
                ASTNode body = parse_block();
                ASTNode case_node{
                    NodeType::MatchCase, case_tok.start, case_tok.mod_path, case_tok.source, std::monostate{}, {}};
                case_node.children.push_back(value);
                case_node.children.push_back(body);
                node.children.push_back(case_node);
            }
            else if (check_kw(Lexer::Keyword::DEFAULT))
            {
                Lexer::Token default_tok = advance();
                ASTNode body = parse_block();
                ASTNode default_node{NodeType::MatchDefault,
                                     default_tok.start,
                                     default_tok.mod_path, default_tok.source, std::monostate{},
                                     {}};
                default_node.children.push_back(body);
                node.children.push_back(default_node);
            }
            else
            {
                error("Expected 'case' or 'default' in match statement");
            }
        }
        advance(); // consume '}'

        return node;
    }

    static ASTNode parse_while_loop()
    {
        Lexer::Token token = advance(); // 'while'
        consume_op(Lexer::Operator::LEFT_PAREN, "Expected '(' after 'while'");
        ASTNode cond = parse_expression();
        consume_op(Lexer::Operator::RIGHT_PAREN, "Expected ')' after while condition");
        ASTNode body = parse_block();

        ASTNode node{NodeType::WhileLoop, token.start, token.mod_path, token.source, std::monostate{}, {}};
        node.children.push_back(cond);
        node.children.push_back(body);
        return node;
    }

    static ASTNode parse_for_init()
    {
        if (check_op(Lexer::Operator::SEMICOLON))
        {
            advance();
            return ASTNode{NodeType::BlockStmt, 0, peek().mod_path, peek().source, std::monostate{}, {}};
        }
        if (check_kw(Lexer::Keyword::MUT) || at_variable_declaration_start())
            return parse_variable_declaration();
        ASTNode expr = parse_expression();
        consume_op(Lexer::Operator::SEMICOLON, "Expected ';' after for-loop initializer");
        ASTNode node{NodeType::ExprStmt, expr.start, expr.mod_path, expr.source, std::monostate{}, {}};
        node.children.push_back(expr);
        return node;
    }

    static ASTNode parse_for_loop()
    {
        Lexer::Token token = advance(); // 'for'
        consume_op(Lexer::Operator::LEFT_PAREN, "Expected '(' after 'for'");

        ASTNode init = parse_for_init(); // consumes its own trailing ';'

        ASTNode cond = check_op(Lexer::Operator::SEMICOLON)
                           ? ASTNode{NodeType::BoolLiteral, token.start, token.mod_path, token.source, true, {}}
                           : parse_expression();
        consume_op(Lexer::Operator::SEMICOLON, "Expected ';' after for-loop condition");

        ASTNode post =
            check_op(Lexer::Operator::RIGHT_PAREN)
                ? ASTNode{NodeType::BlockStmt, token.start, token.mod_path, token.source, std::monostate{}, {}}
                : parse_expression();
        consume_op(Lexer::Operator::RIGHT_PAREN, "Expected ')' after for-loop clauses");

        ASTNode body = parse_block();

        ASTNode node{NodeType::ForLoop, token.start, token.mod_path, token.source, std::monostate{}, {}};
        node.children.push_back(init);
        node.children.push_back(cond);
        node.children.push_back(post);
        node.children.push_back(body);
        return node;
    }

    static ASTNode parse_foreach_loop()
    {
        Lexer::Token token = advance(); // 'foreach'
        consume_op(Lexer::Operator::LEFT_PAREN, "Expected '(' after 'foreach'");
        ASTNode type_node = parse_type_node();
        Lexer::Token name = consume(Lexer::TokenType::Identifier, "Expected loop variable name");
        consume_op(Lexer::Operator::COLON, "Expected ':' in foreach loop");
        ASTNode container = parse_expression();
        consume_op(Lexer::Operator::RIGHT_PAREN, "Expected ')' after foreach clause");
        ASTNode body = parse_block();

        ASTNode node{NodeType::ForeachLoop,
                     token.start,
                     token.mod_path, token.source, std::get<std::string_view>(name.value),
                     {}};
        node.children.push_back(type_node);
        node.children.push_back(container);
        node.children.push_back(body);
        return node;
    }

    static ASTNode parse_tunnel_statement()
    {
        Lexer::Token token = advance(); // 'tunnel'
        ASTNode value_expr = parse_expression();
        consume_op(Lexer::Operator::RIGHT_ARROW, "Expected '->' after tunnel expression");
        ASTNode restated_slot_type = parse_type_node(); // validated against the declared slot by the checker
        Lexer::Token slot_name = consume(Lexer::TokenType::Identifier, "Expected tunnel slot name");
        consume_op(Lexer::Operator::SEMICOLON, "Expected ';' after tunnel statement");

        ASTNode node{NodeType::TunnelStmt,
                     token.start,
                     token.mod_path, token.source, std::get<std::string_view>(slot_name.value),
                     {}};
        node.children.push_back(value_expr);
        node.children.push_back(std::move(restated_slot_type));
        return node;
    }

    static ASTNode parse_reserve_statement()
    {
        Lexer::Token token = advance(); // 'reserve'
        ASTNode type_node = parse_type_node();
        Lexer::Token name =
            consume(Lexer::TokenType::Identifier, "Expected variable name after reserve type");

        ASTNode node{NodeType::ReserveStmt,
                     token.start,
                     token.mod_path, token.source, std::get<std::string_view>(name.value),
                     {}};
        node.children.push_back(type_node);

        if (check_op(Lexer::Operator::COMMA))
        {
            ASTNode multi{NodeType::MultiReserveStmt, token.start, {}, {}};
            multi.children.push_back(type_node);
            multi.children.push_back(ASTNode{NodeType::Identifier, name.start,
                                             token.mod_path, token.source, std::get<std::string_view>(name.value), {}});
            while (check_op(Lexer::Operator::COMMA))
            {
                advance();
                multi.children.push_back(parse_type_node());
                Lexer::Token nextName = consume(Lexer::TokenType::Identifier,
                                                 "Expected variable name after reserve type");
                multi.children.push_back(ASTNode{NodeType::Identifier, nextName.start,
                                                 token.mod_path, token.source, std::get<std::string_view>(nextName.value), {}});
            }
            consume_op(Lexer::Operator::ASSIGN, "Expected '=' after multi-result reserve bindings");
            multi.children.push_back(parse_expression());
            consume_op(Lexer::Operator::SEMICOLON, "Expected ';' after reserve statement");
            return multi;
        }

        if (check_op(Lexer::Operator::ASSIGN))
        {
            advance();
            node.children.push_back(parse_expression());
        }
        else if (check_op(Lexer::Operator::LEFT_ARROW))
        {
            Lexer::Token arrow = advance();
            Lexer::Token source =
                consume(Lexer::TokenType::Identifier, "Expected tunnel name after '<-'");
            ASTNode binding{NodeType::TunnelBindingExpr,
                            arrow.start,
                            arrow.mod_path, arrow.source, std::get<std::string_view>(source.value),
                            {}};
            node.children.push_back(binding);
        }

        consume_op(Lexer::Operator::SEMICOLON, "Expected ';' after reserve statement");
        return node;
    }

    static ASTNode parse_start_statement()
    {
        Lexer::Token token = advance(); // 'start'
        Lexer::Token name =
            consume(Lexer::TokenType::Identifier, "Expected state name after 'start'");
        consume_op(Lexer::Operator::SEMICOLON, "Expected ';' after start statement");
        return ASTNode{
            NodeType::StartStmt, token.start, token.mod_path, token.source, std::get<std::string_view>(name.value), {}};
    }

    static ASTNode parse_thread_task_stmt()
    {
        Lexer::Token name = advance(); // thread variable identifier
        ASTNode body = parse_block();

        ASTNode node{NodeType::ThreadTaskStmt,
                     name.start,
                     name.mod_path, name.source, std::get<std::string_view>(name.value),
                     {}};
        node.children.push_back(body);
        return node;
    }

    static ASTNode parse_state_binding_decl()
    {
        Lexer::Token state_tok = advance(); // 'State' or 'Thread'
        Lexer::Token callee =
            consume(Lexer::TokenType::Identifier, "Expected function name after 'State'");
        consume_op(Lexer::Operator::LEFT_PAREN, "Expected '(' after function name");
        std::vector<ASTNode> args = parse_arg_list();
        consume_op(Lexer::Operator::RIGHT_PAREN, "Expected ')' after arguments");
        Lexer::Token name =
            consume(Lexer::TokenType::Identifier, "Expected binding name for State declaration");
        consume_op(Lexer::Operator::SEMICOLON, "Expected ';' after State declaration");

        ASTNode callee_node{NodeType::Identifier,
                            callee.start,
                            callee.mod_path, callee.source, std::get<std::string_view>(callee.value),
                            {}};
        ASTNode call_expr{NodeType::CallExpr, callee.start, callee.mod_path, callee.source, std::monostate{}, {}};
        call_expr.children.push_back(callee_node);
        for (auto &arg : args)
            call_expr.children.push_back(std::move(arg));

        ASTNode node{std::get<Lexer::Keyword>(state_tok.value) == Lexer::Keyword::THREAD
                         ? NodeType::ThreadBindingDecl
                         : NodeType::StateBindingDecl,
                     state_tok.start,
                     state_tok.mod_path, state_tok.source, std::get<std::string_view>(name.value),
                     {}};
        node.children.push_back(call_expr);
        return node;
    }

    static bool at_state_binding_start()
    {
        return (check_kw(Lexer::Keyword::STATE) || check_kw(Lexer::Keyword::THREAD)) &&
               check(Lexer::TokenType::Identifier, 1) && check_op(Lexer::Operator::LEFT_PAREN, 2);
    }

    static ASTNode parse_return_statement()
    {
        Lexer::Token token = advance(); // 'return'
        ASTNode node{NodeType::ReturnStmt, token.start, token.mod_path, token.source, std::monostate{}, {}};
        if (!check_op(Lexer::Operator::SEMICOLON))
            node.children.push_back(parse_expression());
        consume_op(Lexer::Operator::SEMICOLON, "Expected ';' after return statement");
        return node;
    }

    static ASTNode parse_statement()
    {
        if (at_state_binding_start())
            return parse_state_binding_decl();

        if (check(Lexer::TokenType::Keyword))
        {
            switch (std::get<Lexer::Keyword>(peek().value))
            {
            case Lexer::Keyword::IF:
                return parse_if_statement();
            case Lexer::Keyword::VALID:
                return parse_valid_statement();
            case Lexer::Keyword::MATCH:
                return parse_match_statement();
            case Lexer::Keyword::WHILE:
                return parse_while_loop();
            case Lexer::Keyword::FOR:
                return parse_for_loop();
            case Lexer::Keyword::FOREACH:
                return parse_foreach_loop();
            case Lexer::Keyword::BREAK:
            {
                Lexer::Token token = advance();
                consume_op(Lexer::Operator::SEMICOLON, "Expected ';' after 'break'");
                return ASTNode{NodeType::BreakStmt, token.start, token.mod_path, token.source, std::monostate{}, {}};
            }
            case Lexer::Keyword::CONTINUE:
            {
                Lexer::Token token = advance();
                consume_op(Lexer::Operator::SEMICOLON, "Expected ';' after 'continue'");
                return ASTNode{NodeType::ContinueStmt, token.start, token.mod_path, token.source, std::monostate{}, {}};
            }
            case Lexer::Keyword::TUNNEL:
                return parse_tunnel_statement();
            case Lexer::Keyword::RETURN:
                return parse_return_statement();
            case Lexer::Keyword::RESERVE:
                return parse_reserve_statement();
            case Lexer::Keyword::START:
                return parse_start_statement();
            case Lexer::Keyword::STRUCT:
            case Lexer::Keyword::CLASS:
            case Lexer::Keyword::ALIGN:
            case Lexer::Keyword::ENUM:
            case Lexer::Keyword::NAMESPACE:
            case Lexer::Keyword::IMPORT:
            case Lexer::Keyword::EXPORT:
            case Lexer::Keyword::USING:
            case Lexer::Keyword::DEF:
            case Lexer::Keyword::DEC:
            case Lexer::Keyword::CDEF:
            case Lexer::Keyword::CDEC:
                return parse_top_level_declaration();
            default:
                if (at_variable_declaration_start())
                    return parse_variable_declaration();
                return parse_expression_statement();
            }
        }

        if (check(Lexer::TokenType::Operator))
        {
            if (check_op(Lexer::Operator::LEFT_BRACE))
                return parse_block();
            if (at_variable_declaration_start())
                return parse_variable_declaration();
            return parse_expression_statement();
        }

        if (check(Lexer::TokenType::Identifier))
        {
            if (at_custom_type_var_decl_start())
                return parse_variable_declaration_with_custom_type();
            if (check_op(Lexer::Operator::LEFT_BRACE, 1))
                return parse_thread_task_stmt();
            return parse_expression_statement();
        }

        return parse_expression_statement();
    }

    static std::vector<ASTNode> parse_param_list()
    {
        std::vector<ASTNode> params;
        if (check_op(Lexer::Operator::RIGHT_PAREN))
            return params;

        if (check_kw(Lexer::Keyword::SELF) ||
            (check_op(Lexer::Operator::AMPERSAND) && check_kw(Lexer::Keyword::SELF, 1)) ||
            (check_op(Lexer::Operator::AMPERSAND) && check_kw(Lexer::Keyword::MUT, 1) &&
             check_kw(Lexer::Keyword::SELF, 2)))
        {
            Lexer::Token start = peek();
            bool by_ref = false;
            bool is_mutable = false;
            if (check_op(Lexer::Operator::AMPERSAND))
            {
                advance();
                by_ref = true;
                if (check_kw(Lexer::Keyword::MUT))
                {
                    advance();
                    is_mutable = true;
                }
            }
            consume_kw(Lexer::Keyword::SELF, "Expected 'self'");

            ASTNode self_type{
                NodeType::CustomType, start.start, start.mod_path, start.source, std::string_view("Self"), {}};
            ASTNode param{NodeType::Param, start.start, start.mod_path, start.source, std::string_view("self"), {}};
            if (by_ref)
            {
                ASTNode ref{NodeType::ReferenceType, start.start, start.mod_path, start.source, is_mutable, {}};
                ref.children.push_back(self_type);
                param.children.push_back(ref);
            }
            else
            {
                param.children.push_back(self_type);
            }
            params.push_back(param);

            if (check_op(Lexer::Operator::COMMA))
                advance();
            else
                return params;
        }

        while (true)
        {
            Lexer::Token start = peek();
            ASTNode type_node = parse_type_node();
            Lexer::Token name = consume(Lexer::TokenType::Identifier, "Expected parameter name");

            ASTNode param{NodeType::Param,
                          start.start,
                          start.mod_path, start.source, std::get<std::string_view>(name.value),
                          {}};
            param.children.push_back(type_node);
            params.push_back(param);

            if (check_op(Lexer::Operator::COMMA))
            {
                advance();
                continue;
            }
            break;
        }

        return params;
    }

    static std::vector<ASTNode> parse_generic_param_list()
    {
        std::vector<ASTNode> generics;
        consume_op(Lexer::Operator::LESS_THAN, "Expected '<' to start generic parameter list");
        while (true)
        {
            Lexer::Token name =
                consume(Lexer::TokenType::Identifier, "Expected generic parameter name");
            generics.push_back(ASTNode{NodeType::GenericParam,
                                       name.start,
                                       name.mod_path, name.source, std::get<std::string_view>(name.value),
                                       {}});
            if (check_op(Lexer::Operator::COMMA))
            {
                advance();
                continue;
            }
            break;
        }
        consume_generic_closer("Expected '>' after generic parameter list");
        return generics;
    }

    static ASTNode parse_function_common(NodeType nodeType, bool hasBody, bool allowGenerics,
                                         bool allowAsync, bool isCFunction)
    {
        Lexer::Token keyword_tok = advance(); // dec/def/cdec/cdef

        bool isAsync = false;
        if (allowAsync && check_kw(Lexer::Keyword::ASYNC))
        {
            advance();
            isAsync = true;
        }

        Lexer::Token name = consume(Lexer::TokenType::Identifier, "Expected function name");
        ASTNode node{nodeType, keyword_tok.start, keyword_tok.mod_path, keyword_tok.source, std::monostate{}, {}};
        node.isAsync = isAsync;

        // name stored as the first child, matching struct/enum decl conventions
        node.children.push_back(ASTNode{NodeType::Identifier,
                                        name.start,
                                        name.mod_path, name.source, std::get<std::string_view>(name.value),
                                        {}});

        consume_op(Lexer::Operator::LEFT_PAREN, "Expected '(' after function name");
        std::vector<ASTNode> params = parse_param_list();
        consume_op(Lexer::Operator::RIGHT_PAREN, "Expected ')' after parameter list");
        for (auto &p : params)
            node.children.push_back(std::move(p));

        if (allowGenerics && check_op(Lexer::Operator::LESS_THAN))
        {
            for (auto &g : parse_generic_param_list())
                node.children.push_back(std::move(g));
        }

        if (isCFunction)
        {
            if (check_op(Lexer::Operator::RIGHT_ARROW))
            {
                advance();
                node.children.push_back(parse_type_node());
            }
        }
        else
        {
            while (check_op(Lexer::Operator::RIGHT_ARROW) ||
                   (check_op(Lexer::Operator::QUESTION_MARK) &&
                    check_op(Lexer::Operator::RIGHT_ARROW, 1)))
            {
                bool optional = false;
                if (check_op(Lexer::Operator::QUESTION_MARK))
                {
                    advance();
                    optional = true;
                }
                Lexer::Token arrow = advance(); // '->'
                ASTNode slot_type = parse_type_node();
                Lexer::Token slot_name =
                    consume(Lexer::TokenType::Identifier, "Expected tunnel slot name");

                ASTNode slot{NodeType::TunnelSlot,
                             arrow.start,
                             arrow.mod_path, arrow.source, std::get<std::string_view>(slot_name.value),
                             {}};
                slot.isOptional = optional;
                slot.children.push_back(slot_type);
                node.children.push_back(slot);

                if (check_op(Lexer::Operator::COMMA))
                {
                    advance();
                    continue;
                }
                break;
            }
        }

        if (hasBody)
        {
            node.children.push_back(parse_block());
        }
        else
        {
            consume_op(Lexer::Operator::SEMICOLON, "Expected ';' after function declaration");
        }

        return node;
    }

    static ASTNode parse_function_declaration()
    {
        return parse_function_common(NodeType::FunctionDecl, false, true, true, false);
    }

    static ASTNode parse_function_definition()
    {
        return parse_function_common(NodeType::FunctionDef, true, true, true, false);
    }

    static ASTNode parse_c_function_declaration()
    {
        return parse_function_common(NodeType::CFunctionDecl, false, false, false, true);
    }

    static ASTNode parse_c_function_definition()
    {
        return parse_function_common(NodeType::CFunctionDef, true, false, false, true);
    }

    static ASTNode parse_namespace_declaration()
    {
        Lexer::Token ns_token = advance();
        Lexer::Token name_token = consume(Lexer::TokenType::Identifier, "Expected namespace name");

        ASTNode node{NodeType::NamespaceDecl,
                     ns_token.start,
                     ns_token.mod_path, ns_token.source, std::get<std::string_view>(name_token.value),
                     {}};

        consume_op(Lexer::Operator::LEFT_BRACE, "Expected '{' after namespace name");
        while (!check_op(Lexer::Operator::RIGHT_BRACE))
        {
            if (peek().type == Lexer::TokenType::EndOfFile)
                error("Unterminated namespace declaration");
            node.children.push_back(parse_top_level_declaration());
        }
        advance();

        return node;
    }

    static ASTNode parse_struct_body(ASTNode &node)
    {
        if (check_op(Lexer::Operator::LEFT_BRACE))
        {
            advance();
            while (!check_op(Lexer::Operator::RIGHT_BRACE))
            {
                if (peek().type == Lexer::TokenType::EndOfFile)
                    error("Unterminated struct declaration");
                if (node.isClass && (check_kw(Lexer::Keyword::INDEX) || check_kw(Lexer::Keyword::INIT)))
                {
                    const bool isIndex = check_kw(Lexer::Keyword::INDEX);
                    Lexer::Token keyword = advance();
                    Lexer::Token field = consume(Lexer::TokenType::Identifier,
                                                 "Expected backing field name after class array declaration");
                    consume_op(Lexer::Operator::SEMICOLON,
                               "Expected ';' after class array declaration");
                    node.children.push_back(ASTNode{isIndex ? NodeType::IndexDecl : NodeType::InitDecl,
                                                    keyword.start,
                                                    keyword.mod_path, keyword.source, std::get<std::string_view>(field.value),
                                                    {}});
                }
                else
                    node.children.push_back(parse_top_level_declaration());
            }
            advance();
        }
        return node;
    }

    static ASTNode parse_struct_declaration()
    {
        Lexer::Token token = advance(); // 'struct'
        ASTNode node{NodeType::StructDecl, token.start, token.mod_path, token.source, std::monostate{}, {}};

        std::vector<ASTNode> generics;
        if (check_op(Lexer::Operator::LESS_THAN))
            generics = parse_generic_param_list();

        Lexer::Token name = consume(Lexer::TokenType::Identifier, "Expected struct name");
        node.children.push_back(ASTNode{NodeType::Identifier,
                                        name.start,
                                        name.mod_path, name.source, std::get<std::string_view>(name.value),
                                        {}});

        for (auto &g : generics)
            node.children.push_back(std::move(g));

        return parse_struct_body(node);
    }

    static ASTNode parse_aligned_struct_declaration()
    {
        Lexer::Token align_token = advance(); // 'align'
        ASTNode node{
            NodeType::StructDecl, align_token.start, align_token.mod_path, align_token.source, std::monostate{}, {}};

        Lexer::Token size_token =
            consume(Lexer::TokenType::IntegerLiteral, "Expected numeric alignment after 'align'");
        node.requestedAlignment = std::get<uint64_t>(size_token.value);
        if (node.requestedAlignment == 0 || (node.requestedAlignment & (node.requestedAlignment - 1)) != 0)
            error("Struct alignment must be a nonzero power of two");

        consume_kw(Lexer::Keyword::STRUCT, "Expected 'struct' after alignment specifier");

        std::vector<ASTNode> generics;
        if (check_op(Lexer::Operator::LESS_THAN))
            generics = parse_generic_param_list();

        Lexer::Token name = consume(Lexer::TokenType::Identifier, "Expected struct name");
        node.children.push_back(ASTNode{NodeType::Identifier,
                                        name.start,
                                        name.mod_path, name.source, std::get<std::string_view>(name.value),
                                        {}});

        for (auto &g : generics)
            node.children.push_back(std::move(g));

        return parse_struct_body(node);
    }

    static ASTNode parse_class_declaration()
    {
        Lexer::Token token = advance(); // 'class'
        ASTNode node{NodeType::StructDecl, token.start, token.mod_path, token.source, std::monostate{}, {}};
        node.isClass = true;

        std::vector<ASTNode> generics;
        if (check_op(Lexer::Operator::LESS_THAN))
            generics = parse_generic_param_list();

        Lexer::Token name = consume(Lexer::TokenType::Identifier, "Expected class name");
        node.children.push_back(ASTNode{NodeType::Identifier,
                                        name.start,
                                        name.mod_path, name.source, std::get<std::string_view>(name.value),
                                        {}});
        for (auto &generic : generics)
            node.children.push_back(std::move(generic));
        if (check_op(Lexer::Operator::COLON))
        {
            advance();
            Lexer::Token base = consume(Lexer::TokenType::Identifier, "Expected base class name after ':'");
            node.baseClassName = std::string(std::get<std::string_view>(base.value));
        }
        return parse_struct_body(node);
    }

    static ASTNode parse_enum_declaration()
    {
        Lexer::Token token = advance(); // 'enum'
        ASTNode node{NodeType::EnumDecl, token.start, token.mod_path, token.source, std::monostate{}, {}};

        Lexer::Token name = consume(Lexer::TokenType::Identifier, "Expected enum name");
        node.children.push_back(ASTNode{NodeType::Identifier,
                                        name.start,
                                        name.mod_path, name.source, std::get<std::string_view>(name.value),
                                        {}});

        if (check_op(Lexer::Operator::COLON))
        {
            advance();
            node.children.push_back(parse_type_node());
        }

        consume_op(Lexer::Operator::LEFT_BRACE, "Expected '{' to start enum body");
        while (!check_op(Lexer::Operator::RIGHT_BRACE))
        {
            if (peek().type == Lexer::TokenType::EndOfFile)
                error("Unterminated enum declaration");

            Lexer::Token member_tok = peek();
            ASTNode member_node{
                NodeType::EnumMember, member_tok.start, member_tok.mod_path, member_tok.source, std::monostate{}, {}};

            Lexer::Token name = consume(Lexer::TokenType::Identifier, "Expected enum member name");
            member_node.children.push_back(ASTNode{NodeType::Identifier,
                                                   name.start,
                                                   name.mod_path, name.source, std::get<std::string_view>(name.value),
                                                   {}});

            if (check_op(Lexer::Operator::ASSIGN))
            {
                advance();
                member_node.children.push_back(parse_expression());
            }

            if (check_op(Lexer::Operator::COMMA))
                advance();

            node.children.push_back(member_node);
        }
        advance();

        return node;
    }

    static ASTNode parse_import_statement()
    {
        Lexer::Token token = advance();
        ASTNode node{NodeType::ImportStmt, token.start, token.mod_path, token.source, std::monostate{}, {}};
        if (!check(Lexer::TokenType::StringLiteral))
            error("Expected module path string after 'import'");
        node.children.push_back(parse_primary());
        consume_op(Lexer::Operator::SEMICOLON, "Expected ';' after import statement");
        return node;
    }

    static ASTNode parse_export_declaration()
    {
        Lexer::Token token = advance();
        ASTNode inner = parse_top_level_declaration();
        ASTNode node{NodeType::ExportDecl, token.start, token.mod_path, token.source, std::monostate{}, {}};
        node.isExported = true;
        node.children.push_back(inner);
        return node;
    }

    static ASTNode parse_using_macro_declaration()
    {
        Lexer::Token using_token = advance();
        ASTNode node{
            NodeType::UsingMacroDecl, using_token.start, using_token.mod_path, using_token.source, std::monostate{}, {}};
        if (check_kw(Lexer::Keyword::GLOBAL))
        {
            advance();
            node.isGlobal = true;
        }

        ASTNode alias_node;
        if (check(Lexer::TokenType::Identifier) || check(Lexer::TokenType::StringLiteral))
        {
            alias_node = parse_primary();
        }
        else
        {
            error("Expected macro alias after 'using'");
        }

        consume_op(Lexer::Operator::RIGHT_ARROW, "Expected '->' after macro alias");

        ASTNode replacement_node{
            NodeType::BlockStmt, using_token.start, using_token.mod_path, using_token.source, std::monostate{}, {}};

        while (!check_op(Lexer::Operator::SEMICOLON))
        {
            if (peek().type == Lexer::TokenType::EndOfFile)
                error("Unterminated using macro declaration");
            replacement_node.children.push_back(parse_primary());
        }

        consume_op(Lexer::Operator::SEMICOLON,
                   "Expected ';' at the end of using macro declaration.");

        node.children.push_back(alias_node);
        node.children.push_back(replacement_node);
        return node;
    }

    static ASTNode parse_variable_declaration()
    {
        Lexer::Token type_token = peek();
        ASTNode type_node = parse_type_node();

        Lexer::Token name_token =
            consume(Lexer::TokenType::Identifier, "Expected variable name after type.");

        ASTNode var_node{NodeType::VariableDecl,
                         type_token.start,
                         type_token.mod_path, type_token.source, std::get<std::string_view>(name_token.value),
                         {}};

        var_node.children.push_back(type_node);

        if (check_op(Lexer::Operator::ASSIGN))
        {
            advance(); // consume '='

            if (check_kw(Lexer::Keyword::MOVE))
            {
                Lexer::Token move_token = advance(); // consume 'move'
                ASTNode source = parse_postfix();

                ASTNode move_node{NodeType::MoveExpr,
                                  move_token.start,
                                  move_token.mod_path, move_token.source, std::monostate{},
                                  {std::move(source)}};
                var_node.children.push_back(move_node);
            }
            else if (check_kw(Lexer::Keyword::REF))
            {
                Lexer::Token ref_token = advance(); // consume 'ref'
                ASTNode source = parse_postfix();

                ASTNode ref_node{NodeType::RefExpr,
                                 ref_token.start,
                                 ref_token.mod_path, ref_token.source, std::monostate{},
                                 {std::move(source)}};
                var_node.children.push_back(ref_node);
            }
            else
            {
                var_node.children.push_back(parse_expression());
            }
        }

        consume_op(Lexer::Operator::SEMICOLON, "Expected ';' at the end of variable declaration");
        return var_node;
    }

    static ASTNode parse_variable_declaration_with_custom_type()
    {
        return parse_variable_declaration();
    }

    static ASTNode parse_top_level_declaration()
    {
        Lexer::Token token = peek();

        if (at_state_binding_start())
            return parse_state_binding_decl();

        switch (token.type)
        {
        case Lexer::TokenType::Keyword:
            switch (std::get<Lexer::Keyword>(token.value))
            {
            case Lexer::Keyword::NAMESPACE:
                return parse_namespace_declaration();
            case Lexer::Keyword::DEF:
                return parse_function_definition();
            case Lexer::Keyword::DEC:
                return parse_function_declaration();
            case Lexer::Keyword::CDEF:
                return parse_c_function_definition();
            case Lexer::Keyword::CDEC:
                return parse_c_function_declaration();
            case Lexer::Keyword::ALIGN:
                return parse_aligned_struct_declaration();
            case Lexer::Keyword::STRUCT:
                return parse_struct_declaration();
            case Lexer::Keyword::CLASS:
                return parse_class_declaration();
            case Lexer::Keyword::ENUM:
                return parse_enum_declaration();
            case Lexer::Keyword::IMPORT:
                return parse_import_statement();
            case Lexer::Keyword::EXPORT:
                return parse_export_declaration();
            case Lexer::Keyword::USING:
                return parse_using_macro_declaration();
            default:
                if (at_type_start())
                    return parse_variable_declaration();
                error("Unexpected keyword at top-level scope");
            }
            break;
        case Lexer::TokenType::Operator:
            if (at_type_start())
                return parse_variable_declaration();
            error("Unexpected operator at top-level declaration");
            return ASTNode{NodeType::Identifier, token.start, token.mod_path, token.source, std::monostate{}, {}};
        case Lexer::TokenType::Identifier:
            if (at_custom_type_var_decl_start())
                return parse_variable_declaration_with_custom_type();
            return parse_expression_statement();
        default:
            error("Unexpected token at top-level declaration");
            return ASTNode{NodeType::Identifier, token.start, token.mod_path, token.source, std::monostate{}, {}};
        }

        return ASTNode{NodeType::Identifier, token.start, token.mod_path, token.source, std::monostate{}, {}};
    }

    static void parse_module()
    {
        ast.push_back(ASTNode{NodeType::Module, 0, peek().mod_path, peek().source, std::monostate{}, {}});
        while (peek().type != Lexer::TokenType::EndOfFile)
        {
            ast.back().children.push_back(parse_top_level_declaration());
        }
    }

    void parse(std::vector<Lexer::LexedModule> input_modules)
    {
        pos = 0;
        deferredGenericClosers = 0;
        ast.clear();
        for (Lexer::LexedModule mod : input_modules)
        {
            pos = 0;
            deferredGenericClosers = 0;
            tokens = std::move(mod.tokens);
            parse_module();
        }
    }

    static const char* node_type_to_string(NodeType t)
    {
        switch (t)
        {
        case NodeType::Module: return "Module";
        case NodeType::NamespaceDecl: return "NamespaceDecl";
        case NodeType::BlockStmt: return "BlockStmt";
        case NodeType::VariableDecl: return "VariableDecl";
        case NodeType::StructDecl: return "StructDecl";
        case NodeType::IndexDecl: return "IndexDecl";
        case NodeType::InitDecl: return "InitDecl";
        case NodeType::EnumDecl: return "EnumDecl";
        case NodeType::EnumMember: return "EnumMember";
        case NodeType::ImportStmt: return "ImportStmt";
        case NodeType::ExportDecl: return "ExportDecl";
        case NodeType::UsingMacroDecl: return "UsingMacroDecl";
        case NodeType::FunctionDecl: return "FunctionDecl";
        case NodeType::FunctionDef: return "FunctionDef";
        case NodeType::CFunctionDecl: return "CFunctionDecl";
        case NodeType::CFunctionDef: return "CFunctionDef";
        case NodeType::ParamList: return "ParamList";
        case NodeType::Param: return "Param";
        case NodeType::TunnelSlotList: return "TunnelSlotList";
        case NodeType::TunnelSlot: return "TunnelSlot";
        case NodeType::TunnelStmt: return "TunnelStmt";
        case NodeType::ReserveStmt: return "ReserveStmt";
        case NodeType::MultiReserveStmt: return "MultiReserveStmt";
        case NodeType::GenericParamList: return "GenericParamList";
        case NodeType::GenericParam: return "GenericParam";
        case NodeType::GenericArg: return "GenericArg";
        case NodeType::IfStmt: return "IfStmt";
        case NodeType::ValidStmt: return "ValidStmt";
        case NodeType::MatchStmt: return "MatchStmt";
        case NodeType::MatchCase: return "MatchCase";
        case NodeType::MatchDefault: return "MatchDefault";
        case NodeType::WhileLoop: return "WhileLoop";
        case NodeType::ForLoop: return "ForLoop";
        case NodeType::ForeachLoop: return "ForeachLoop";
        case NodeType::BreakStmt: return "BreakStmt";
        case NodeType::ContinueStmt: return "ContinueStmt";
        case NodeType::ExprStmt: return "ExprStmt";
        case NodeType::ReturnStmt: return "ReturnStmt";
        case NodeType::PrimitiveType: return "PrimitiveType";
        case NodeType::PointerType: return "PointerType";
        case NodeType::ReferenceType: return "ReferenceType";
        case NodeType::OptionalType: return "OptionalType";
        case NodeType::ArrayType: return "ArrayType";
        case NodeType::CustomType: return "CustomType";
        case NodeType::BinaryExpr: return "BinaryExpr";
        case NodeType::UnaryExpr: return "UnaryExpr";
        case NodeType::AssignmentExpr: return "AssignmentExpr";
        case NodeType::CallExpr: return "CallExpr";
        case NodeType::MemberAccessExpr: return "MemberAccessExpr";
        case NodeType::ScopeAccessExpr: return "ScopeAccessExpr";
        case NodeType::IndexExpr: return "IndexExpr";
        case NodeType::StructInitExpr: return "StructInitExpr";
        case NodeType::TunnelBindingExpr: return "TunnelBindingExpr";
        case NodeType::MoveExpr: return "MoveExpr";
        case NodeType::RefExpr: return "RefExpr";
        case NodeType::SizeofExpr: return "SizeofExpr";
        case NodeType::StateBindingDecl: return "StateBindingDecl";
        case NodeType::ThreadBindingDecl: return "ThreadBindingDecl";
        case NodeType::StartStmt: return "StartStmt";
        case NodeType::AwaitExpr: return "AwaitExpr";
        case NodeType::ThreadTaskStmt: return "ThreadTaskStmt";
        case NodeType::Identifier: return "Identifier";
        case NodeType::IntegerLiteral: return "IntegerLiteral";
        case NodeType::FloatLiteral: return "FloatLiteral";
        case NodeType::StringLiteral: return "StringLiteral";
        case NodeType::BoolLiteral: return "BoolLiteral";
        case NodeType::CharLiteral: return "CharLiteral";
        }
        return "Unknown";
    }


    static const char* keyword_to_string(Lexer::Keyword k)
    {
        switch (k)
        {
        case Lexer::Keyword::IF: return "if";
        case Lexer::Keyword::ELSE: return "else";
        case Lexer::Keyword::MATCH: return "match";
        case Lexer::Keyword::CASE: return "case";
        case Lexer::Keyword::DEFAULT: return "default";
        case Lexer::Keyword::WHILE: return "while";
        case Lexer::Keyword::FOR: return "for";
        case Lexer::Keyword::FOREACH: return "foreach";
        case Lexer::Keyword::BREAK: return "break";
        case Lexer::Keyword::CONTINUE: return "continue";
        case Lexer::Keyword::DEF: return "def";
        case Lexer::Keyword::DEC: return "dec";
        case Lexer::Keyword::CDEF: return "cdef";
        case Lexer::Keyword::CDEC: return "cdec";
        case Lexer::Keyword::ALIGN: return "align";
        case Lexer::Keyword::STRUCT: return "struct";
        case Lexer::Keyword::CLASS: return "class";
        case Lexer::Keyword::INDEX: return "index";
        case Lexer::Keyword::INIT: return "init";
        case Lexer::Keyword::NAMESPACE: return "namespace";
        case Lexer::Keyword::USING: return "using";
        case Lexer::Keyword::GLOBAL: return "global";
        case Lexer::Keyword::IMPORT: return "import";
        case Lexer::Keyword::EXPORT: return "export";
        case Lexer::Keyword::ENUM: return "enum";
        case Lexer::Keyword::TUNNEL: return "tunnel";
        case Lexer::Keyword::INLINE: return "inline";
        case Lexer::Keyword::RESERVE: return "reserve";
        case Lexer::Keyword::RETURN: return "return";
        case Lexer::Keyword::MUT: return "mut";
        case Lexer::Keyword::MOVE: return "move";
        case Lexer::Keyword::REF: return "ref";
        case Lexer::Keyword::SELF: return "self";
        case Lexer::Keyword::VALID: return "valid";
        case Lexer::Keyword::RAW: return "raw";
        case Lexer::Keyword::SIZEOF: return "sizeof";
        case Lexer::Keyword::ASYNC: return "async";
        case Lexer::Keyword::AWAIT: return "await";
        case Lexer::Keyword::START: return "start";
        case Lexer::Keyword::STATE: return "State";
        case Lexer::Keyword::THREAD: return "Thread";
        case Lexer::Keyword::BOOL: return "bool";
        case Lexer::Keyword::CHAR: return "char";
        case Lexer::Keyword::U8: return "u8";
        case Lexer::Keyword::U16: return "u16";
        case Lexer::Keyword::U32: return "u32";
        case Lexer::Keyword::U64: return "u64";
        case Lexer::Keyword::USIZE: return "usize";
        case Lexer::Keyword::I8: return "i8";
        case Lexer::Keyword::I16: return "i16";
        case Lexer::Keyword::I32: return "i32";
        case Lexer::Keyword::I64: return "i64";
        case Lexer::Keyword::F32: return "f32";
        case Lexer::Keyword::F64: return "f64";
        }
        return "<unknown-keyword>";
    }

    static const char* operator_to_string(Lexer::Operator op)
    {
        switch (op)
        {
        case Lexer::Operator::ASSIGN: return "=";
        case Lexer::Operator::ADD_ASSIGN: return "+=";
        case Lexer::Operator::SUBTRACT_ASSIGN: return "-=";
        case Lexer::Operator::MULTIPLY_ASSIGN: return "*=";
        case Lexer::Operator::DIVIDE_ASSIGN: return "/=";
        case Lexer::Operator::MODULO_ASSIGN: return "%=";
        case Lexer::Operator::AMPERSAND: return "&";
        case Lexer::Operator::PIPE: return "|";
        case Lexer::Operator::CARET: return "^";
        case Lexer::Operator::LEFT_SHIFT: return "<<";
        case Lexer::Operator::RIGHT_SHIFT: return ">>";
        case Lexer::Operator::LEFT_SHIFT_ASSIGN: return "<<=";
        case Lexer::Operator::RIGHT_SHIFT_ASSIGN: return ">>=";
        case Lexer::Operator::LESS_THAN: return "<";
        case Lexer::Operator::GREATER_THAN: return ">";
        case Lexer::Operator::LESS_EQUAL: return "<=";
        case Lexer::Operator::GREATER_EQUAL: return ">=";
        case Lexer::Operator::EQUAL: return "==";
        case Lexer::Operator::NOT_EQUAL: return "!=";
        case Lexer::Operator::OR: return "||";
        case Lexer::Operator::AND: return "&&";
        case Lexer::Operator::DOUBLE_COLON: return "::";
        case Lexer::Operator::DOT: return ".";
        case Lexer::Operator::RIGHT_ARROW: return "->";
        case Lexer::Operator::LEFT_ARROW: return "<-";
        case Lexer::Operator::LEFT_PAREN: return "(";
        case Lexer::Operator::RIGHT_PAREN: return ")";
        case Lexer::Operator::LEFT_BRACKET: return "[";
        case Lexer::Operator::RIGHT_BRACKET: return "]";
        case Lexer::Operator::LEFT_BRACE: return "{";
        case Lexer::Operator::RIGHT_BRACE: return "}";
        case Lexer::Operator::COLON: return ":";
        case Lexer::Operator::SEMICOLON: return ";";
        case Lexer::Operator::COMMA: return ",";
        case Lexer::Operator::TRIPPLE_DOT: return "...";
        case Lexer::Operator::QUESTION_MARK: return "?";
        case Lexer::Operator::DOUBLE_QUOTE: return "\"";
        case Lexer::Operator::SINGLE_QUOTE: return "'";
        case Lexer::Operator::PLUS: return "+";
        case Lexer::Operator::MINUS: return "-";
        case Lexer::Operator::MULTIPLY: return "*";
        case Lexer::Operator::DIVIDE: return "/";
        case Lexer::Operator::MODULO: return "%";
        case Lexer::Operator::EXLAMATION_MARK: return "!";
        }
        return "<unknown-op>";
    }

    static std::string value_to_string(const ASTNode& node)
    {
        if (std::holds_alternative<std::string_view>(node.value))
            return std::string(std::get<std::string_view>(node.value));

        if (std::holds_alternative<uint64_t>(node.value))
            return std::to_string(std::get<uint64_t>(node.value));

        if (std::holds_alternative<double>(node.value))
            return std::to_string(std::get<double>(node.value));

        if (std::holds_alternative<bool>(node.value))
            return std::get<bool>(node.value) ? "true" : "false";

        if (std::holds_alternative<Lexer::Operator>(node.value))
            return operator_to_string(std::get<Lexer::Operator>(node.value));

        if (std::holds_alternative<Lexer::Keyword>(node.value))
            return keyword_to_string(std::get<Lexer::Keyword>(node.value));

        return "";
    }

        static void dump_node(const ASTNode& node, int indent)
    {
        auto pos = get_error_pos(node.start, node.source);
        std::string pad(indent, ' ');

        std::cout << pad << "{\n";
        std::cout << pad << "    \"type\": \"" << node_type_to_string(node.type) << "\",\n";
        std::cout << pad << "    \"value\": \"" << value_to_string(node) << "\",\n";
        std::cout << pad << "    \"line\": " << pos.line << ",\n";
        std::cout << pad << "    \"column\": " << pos.column << ",\n";
        std::cout << pad << "    \"children\": [\n";

        for (size_t i = 0; i < node.children.size(); i++)
        {
            dump_node(node.children[i], indent + 8);
            if (i + 1 < node.children.size())
                std::cout << pad << "        ,\n";
        }

        std::cout << pad << "    ]\n";
        std::cout << pad << "}";
    }

    void dump_ast(const std::vector<ASTNode>& ast)
    {
        std::cout << "[\n";
        for (size_t i = 0; i < ast.size(); i++)
        {
            dump_node(ast[i], 4);
            if (i + 1 < ast.size())
                std::cout << ",\n";
        }
        std::cout << "\n]\n";
    }

} // namespace Parser