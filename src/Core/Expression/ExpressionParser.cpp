#include "Core/Expression/ExpressionParser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>
#include <utility>

namespace paramcad {

// Grants the parser write access to ParsedExpression's members without making
// them public. The same shape PartDocument uses to keep model mutators private.
struct ExpressionParserAccess {
    static void set(ParsedExpression& target, ExpressionNodePtr root,
                    std::vector<std::string> variables, std::string sourceText) {
        target.root_ = std::move(root);
        target.variables_ = std::move(variables);
        target.sourceText_ = std::move(sourceText);
    }
};

namespace {

// --- Tokens -----------------------------------------------------------------

enum class TokenKind {
    End,
    Number,
    Identifier, // a function name or a unit; the parser decides which by context
    Variable,   // #name
    Plus, Minus, Star, Slash, Caret,
    LParen, RParen, Comma, Question, Colon,
    Less, Greater, LessEqual, GreaterEqual, EqualEqual, NotEqual
};

struct Token {
    TokenKind kind{TokenKind::End};
    std::size_t position{0};
    std::size_t length{0};
    double number{0.0};
    std::string text; // Identifier and Variable only
};

bool IsIdentStart(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool IsIdentPart(char c) noexcept {
    return IsIdentStart(c) || (c >= '0' && c <= '9');
}

bool IsDigit(char c) noexcept { return c >= '0' && c <= '9'; }

const char* FunctionNameOf(FunctionKind kind) noexcept {
    switch (kind) {
    case FunctionKind::Ceil: return "ceil";
    case FunctionKind::Floor: return "floor";
    case FunctionKind::Round: return "round";
    case FunctionKind::Sqrt: return "sqrt";
    case FunctionKind::Abs: return "abs";
    case FunctionKind::Max: return "max";
    case FunctionKind::Min: return "min";
    case FunctionKind::Log: return "log";
    case FunctionKind::Log10: return "log10";
    case FunctionKind::Sin: return "sin";
    case FunctionKind::Cos: return "cos";
    case FunctionKind::Tan: return "tan";
    case FunctionKind::Asin: return "asin";
    case FunctionKind::Acos: return "acos";
    case FunctionKind::Atan: return "atan";
    }
    return "?";
}

std::optional<FunctionKind> LookupFunction(std::string_view name) noexcept {
    static constexpr FunctionKind kAll[] = {
        FunctionKind::Ceil, FunctionKind::Floor, FunctionKind::Round,
        FunctionKind::Sqrt, FunctionKind::Abs, FunctionKind::Max,
        FunctionKind::Min, FunctionKind::Log, FunctionKind::Log10,
        FunctionKind::Sin, FunctionKind::Cos, FunctionKind::Tan,
        FunctionKind::Asin, FunctionKind::Acos, FunctionKind::Atan};
    for (FunctionKind kind : kAll)
        if (name == FunctionNameOf(kind)) return kind;
    return std::nullopt;
}

// Argument counts. Max and Min take two or more; everything else takes one.
// `kVariadic` is the marker, checked explicitly rather than encoded as a magic
// large number.
constexpr std::size_t kVariadic = static_cast<std::size_t>(-1);

std::size_t RequiredArgumentCount(FunctionKind kind) noexcept {
    switch (kind) {
    case FunctionKind::Max:
    case FunctionKind::Min: return kVariadic;
    default: return 1;
    }
}

// --- Parser -----------------------------------------------------------------

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    ExpressionParseResult run() {
        ExpressionParseResult result;
        if (!tokenize()) {
            result.error = error_;
            return result;
        }
        if (tokens_.size() == 1) { // End only
            result.error = {ExpressionErrorCode::EmptyExpression, 0, 0,
                            "the expression is empty"};
            return result;
        }
        ExpressionNodePtr root = parseExpression();
        if (root == nullptr) {
            result.error = error_;
            return result;
        }
        if (peek().kind != TokenKind::End) {
            result.error = {ExpressionErrorCode::TrailingInput, peek().position,
                            std::max<std::size_t>(peek().length, 1),
                            "unexpected input after the end of the expression"};
            return result;
        }
        std::vector<std::string> variables(variables_.begin(), variables_.end());
        ExpressionParserAccess::set(result.expression, std::move(root),
                                    std::move(variables), std::string(text_));
        return result;
    }

private:
    // --- lexing ---

    bool tokenize() {
        std::size_t i = 0;
        while (i < text_.size()) {
            const char c = text_[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }

            if (IsDigit(c) || (c == '.' && i + 1 < text_.size() && IsDigit(text_[i + 1]))) {
                if (!lexNumber(i)) return false;
                continue;
            }
            if (IsIdentStart(c)) { lexIdentifier(i); continue; }
            if (c == '#') {
                const std::size_t start = i;
                ++i;
                if (i >= text_.size() || !IsIdentStart(text_[i])) {
                    error_ = {ExpressionErrorCode::UnexpectedCharacter, start, 1,
                              "'#' must be followed by a variable name"};
                    return false;
                }
                const std::size_t nameStart = i;
                while (i < text_.size() && IsIdentPart(text_[i])) ++i;
                Token token;
                token.kind = TokenKind::Variable;
                token.position = start;
                token.length = i - start;
                token.text = std::string(text_.substr(nameStart, i - nameStart));
                tokens_.push_back(std::move(token));
                continue;
            }
            if (!lexOperator(i)) return false;
        }
        Token end;
        end.kind = TokenKind::End;
        end.position = text_.size();
        tokens_.push_back(std::move(end));
        return true;
    }

    bool lexNumber(std::size_t& i) {
        const std::size_t start = i;
        while (i < text_.size() && IsDigit(text_[i])) ++i;
        if (i < text_.size() && text_[i] == '.') {
            ++i;
            while (i < text_.size() && IsDigit(text_[i])) ++i;
        }
        // Scientific notation, but ONLY when a digit actually follows. Without
        // that check `3e` would swallow the 'e' and then fail to convert, and
        // a hypothetical unit starting with 'e' would become unreachable.
        if (i < text_.size() && (text_[i] == 'e' || text_[i] == 'E')) {
            std::size_t probe = i + 1;
            if (probe < text_.size() && (text_[probe] == '+' || text_[probe] == '-')) ++probe;
            if (probe < text_.size() && IsDigit(text_[probe])) {
                i = probe;
                while (i < text_.size() && IsDigit(text_[i])) ++i;
            }
        }
        const std::string literal(text_.substr(start, i - start));
        char* parseEnd = nullptr;
        const double value = std::strtod(literal.c_str(), &parseEnd);
        if (parseEnd != literal.c_str() + literal.size()) {
            error_ = {ExpressionErrorCode::NumberFormat, start, i - start,
                      "'" + literal + "' is not a number this reader can convert"};
            return false;
        }
        // An overflowing literal such as 1e999 converts CLEANLY to infinity, so
        // the check above passes it. Caught here rather than at evaluation,
        // because a literal is the one node the evaluator returns without
        // arithmetic -- and therefore without its finiteness guard.
        if (!std::isfinite(value)) {
            error_ = {ExpressionErrorCode::NumberFormat, start, i - start,
                      "'" + literal + "' is too large to represent"};
            return false;
        }
        Token token;
        token.kind = TokenKind::Number;
        token.position = start;
        token.length = i - start;
        token.number = value;
        tokens_.push_back(std::move(token));
        return true;
    }

    void lexIdentifier(std::size_t& i) {
        const std::size_t start = i;
        while (i < text_.size() && IsIdentPart(text_[i])) ++i;
        Token token;
        token.kind = TokenKind::Identifier;
        token.position = start;
        token.length = i - start;
        token.text = std::string(text_.substr(start, i - start));
        tokens_.push_back(std::move(token));
    }

    bool lexOperator(std::size_t& i) {
        const char c = text_[i];
        const char next = (i + 1 < text_.size()) ? text_[i + 1] : '\0';
        TokenKind kind = TokenKind::End;
        std::size_t length = 1;
        switch (c) {
        case '+': kind = TokenKind::Plus; break;
        case '-': kind = TokenKind::Minus; break;
        case '*': kind = TokenKind::Star; break;
        case '/': kind = TokenKind::Slash; break;
        case '^': kind = TokenKind::Caret; break;
        case '(': kind = TokenKind::LParen; break;
        case ')': kind = TokenKind::RParen; break;
        case ',': kind = TokenKind::Comma; break;
        case '?': kind = TokenKind::Question; break;
        case ':': kind = TokenKind::Colon; break;
        case '<':
            if (next == '=') { kind = TokenKind::LessEqual; length = 2; }
            else kind = TokenKind::Less;
            break;
        case '>':
            if (next == '=') { kind = TokenKind::GreaterEqual; length = 2; }
            else kind = TokenKind::Greater;
            break;
        case '=':
            if (next == '=') { kind = TokenKind::EqualEqual; length = 2; }
            else {
                error_ = {ExpressionErrorCode::UnexpectedCharacter, i, 1,
                          "'=' is not an operator; use '==' to compare"};
                return false;
            }
            break;
        case '!':
            if (next == '=') { kind = TokenKind::NotEqual; length = 2; }
            else {
                error_ = {ExpressionErrorCode::UnexpectedCharacter, i, 1,
                          "'!' is only valid as part of '!='"};
                return false;
            }
            break;
        default: {
            // A comma is a legal token above, so this branch never sees one.
            // The reference model accepts a comma as a DECIMAL separator; EP3D
            // does not, because `max(1,2)` would then be genuinely ambiguous.
            // Recorded as an intentional difference (roadmap section 32).
            std::string what = "'";
            what.push_back(c);
            what += "' is not valid in an expression";
            error_ = {ExpressionErrorCode::UnexpectedCharacter, i, 1, std::move(what)};
            return false;
        }
        }
        Token token;
        token.kind = kind;
        token.position = i;
        token.length = length;
        tokens_.push_back(std::move(token));
        i += length;
        return true;
    }

    // --- token access ---

    const Token& peek() const noexcept { return tokens_[cursor_]; }
    const Token& advance() noexcept { return tokens_[cursor_++]; }
    bool match(TokenKind kind) noexcept {
        if (peek().kind != kind) return false;
        ++cursor_;
        return true;
    }

    // --- depth guard ---
    //
    // RAII so every early return decrements. A raw increment/decrement pair
    // would leak depth on each of the twelve error paths below.
    class DepthGuard {
    public:
        explicit DepthGuard(Parser& parser) : parser_(parser) { ++parser_.depth_; }
        ~DepthGuard() { --parser_.depth_; }
        bool exceeded() const noexcept { return parser_.depth_ > kExpressionMaxDepth; }

    private:
        Parser& parser_;
    };

    bool tooDeep(DepthGuard& guard) {
        if (!guard.exceeded()) return false;
        error_ = {ExpressionErrorCode::DepthLimitExceeded, peek().position, 1,
                  "the expression nests deeper than " +
                      std::to_string(kExpressionMaxDepth) + " levels"};
        return true;
    }

    // --- node budget ---
    //
    // This is what bounds the EVALUATOR's recursion. The depth guard above
    // counts grammar-rule entries, and `1 + 1 + 1 + ...` is a LOOP -- constant
    // parser depth, one extra tree level per term. Without a node cap that
    // input parses happily and overflows the stack while being evaluated.
    ExpressionNodePtr newNode() {
        if (++nodeCount_ > kExpressionMaxNodes) {
            error_ = {ExpressionErrorCode::TooManyTerms, peek().position, 1,
                      "the expression has more than " +
                          std::to_string(kExpressionMaxNodes) + " terms"};
            return nullptr;
        }
        return std::make_unique<ExpressionNode>();
    }

    // --- grammar ---
    //
    // expression     := ternary
    // ternary        := comparison ('?' expression ':' ternary)?
    // comparison     := additive (cmpOp additive)?        [non-associative]
    // additive       := multiplicative (('+'|'-') multiplicative)*
    // multiplicative := unary (('*'|'/') unary)*
    // unary          := ('-'|'+') unary | power
    // power          := postfix ('^' unary)?              [right-associative]
    // postfix        := primary unitSuffix?               [literals only]
    // primary        := number | '#'name | name '(' args ')' | '(' expression ')'

    ExpressionNodePtr parseExpression() { return parseTernary(); }

    ExpressionNodePtr parseTernary() {
        DepthGuard guard(*this);
        if (tooDeep(guard)) return nullptr;

        ExpressionNodePtr condition = parseComparison();
        if (condition == nullptr) return nullptr;
        if (peek().kind != TokenKind::Question) return condition;

        const Token& question = advance();
        ExpressionNodePtr thenBranch = parseExpression();
        if (thenBranch == nullptr) return nullptr;
        if (!match(TokenKind::Colon)) {
            error_ = {ExpressionErrorCode::UnexpectedToken, peek().position,
                      std::max<std::size_t>(peek().length, 1),
                      "expected ':' to complete the conditional"};
            return nullptr;
        }
        ExpressionNodePtr elseBranch = parseTernary();
        if (elseBranch == nullptr) return nullptr;

        ExpressionNodePtr node = newNode();
        if (node == nullptr) return nullptr;
        node->kind = NodeKind::Ternary;
        node->position = question.position;
        node->length = question.length;
        node->children.push_back(std::move(condition));
        node->children.push_back(std::move(thenBranch));
        node->children.push_back(std::move(elseBranch));
        return node;
    }

    static std::optional<BinaryOp> ComparisonOpOf(TokenKind kind) noexcept {
        switch (kind) {
        case TokenKind::Less: return BinaryOp::Less;
        case TokenKind::Greater: return BinaryOp::Greater;
        case TokenKind::LessEqual: return BinaryOp::LessEqual;
        case TokenKind::GreaterEqual: return BinaryOp::GreaterEqual;
        case TokenKind::EqualEqual: return BinaryOp::Equal;
        case TokenKind::NotEqual: return BinaryOp::NotEqual;
        default: return std::nullopt;
        }
    }

    ExpressionNodePtr parseComparison() {
        DepthGuard guard(*this);
        if (tooDeep(guard)) return nullptr;

        ExpressionNodePtr left = parseAdditive();
        if (left == nullptr) return nullptr;
        const std::optional<BinaryOp> op = ComparisonOpOf(peek().kind);
        if (!op.has_value()) return left;

        const Token& opToken = advance();
        ExpressionNodePtr right = parseAdditive();
        if (right == nullptr) return nullptr;

        // Non-associative on purpose: `1 < x < 10` reads as a range to a human
        // and evaluates as `(1 < x) < 10` in C. Refusing it is the only reading
        // that cannot mislead.
        if (ComparisonOpOf(peek().kind).has_value()) {
            error_ = {ExpressionErrorCode::UnexpectedToken, peek().position,
                      std::max<std::size_t>(peek().length, 1),
                      "comparisons cannot be chained; write two comparisons instead"};
            return nullptr;
        }
        return makeBinary(*op, opToken, std::move(left), std::move(right));
    }

    ExpressionNodePtr parseAdditive() {
        DepthGuard guard(*this);
        if (tooDeep(guard)) return nullptr;

        ExpressionNodePtr left = parseMultiplicative();
        if (left == nullptr) return nullptr;
        while (peek().kind == TokenKind::Plus || peek().kind == TokenKind::Minus) {
            const bool add = peek().kind == TokenKind::Plus;
            const Token& opToken = advance();
            ExpressionNodePtr right = parseMultiplicative();
            if (right == nullptr) return nullptr;
            left = makeBinary(add ? BinaryOp::Add : BinaryOp::Subtract, opToken,
                              std::move(left), std::move(right));
            if (left == nullptr) return nullptr;
        }
        return left;
    }

    ExpressionNodePtr parseMultiplicative() {
        DepthGuard guard(*this);
        if (tooDeep(guard)) return nullptr;

        ExpressionNodePtr left = parseUnary();
        if (left == nullptr) return nullptr;
        while (peek().kind == TokenKind::Star || peek().kind == TokenKind::Slash) {
            const bool multiply = peek().kind == TokenKind::Star;
            const Token& opToken = advance();
            ExpressionNodePtr right = parseUnary();
            if (right == nullptr) return nullptr;
            left = makeBinary(multiply ? BinaryOp::Multiply : BinaryOp::Divide, opToken,
                              std::move(left), std::move(right));
            if (left == nullptr) return nullptr;
        }
        return left;
    }

    ExpressionNodePtr parseUnary() {
        DepthGuard guard(*this);
        if (tooDeep(guard)) return nullptr;

        if (peek().kind == TokenKind::Minus || peek().kind == TokenKind::Plus) {
            const bool negate = peek().kind == TokenKind::Minus;
            const Token& opToken = advance();
            ExpressionNodePtr operand = parseUnary();
            if (operand == nullptr) return nullptr;
            ExpressionNodePtr node = newNode();
            if (node == nullptr) return nullptr;
            node->kind = NodeKind::Unary;
            node->unaryOp = negate ? UnaryOp::Negate : UnaryOp::Plus;
            node->position = opToken.position;
            node->length = opToken.length;
            node->children.push_back(std::move(operand));
            return node;
        }
        return parsePower();
    }

    ExpressionNodePtr parsePower() {
        DepthGuard guard(*this);
        if (tooDeep(guard)) return nullptr;

        ExpressionNodePtr base = parsePostfix();
        if (base == nullptr) return nullptr;
        if (peek().kind != TokenKind::Caret) return base;

        const Token& opToken = advance();
        // Right-associative, and the exponent goes through parseUnary so
        // `2^-1` is accepted. `-2^2` is -(2^2) because unary binds looser than
        // power -- the standard mathematical reading.
        ExpressionNodePtr exponent = parseUnary();
        if (exponent == nullptr) return nullptr;
        return makeBinary(BinaryOp::Power, opToken, std::move(base), std::move(exponent));
    }

    ExpressionNodePtr parsePostfix() {
        DepthGuard guard(*this);
        if (tooDeep(guard)) return nullptr;

        const bool primaryIsLiteral = peek().kind == TokenKind::Number;
        ExpressionNodePtr value = parsePrimary();
        if (value == nullptr) return nullptr;

        if (peek().kind != TokenKind::Identifier) return value;
        // An identifier immediately followed by '(' is a function call, not a
        // unit; leave it for the caller to reject as trailing input.
        if (tokens_[cursor_ + 1].kind == TokenKind::LParen) return value;

        const Token& unitToken = peek();
        const std::optional<UnitInfo> unit = LookupUnit(unitToken.text);
        if (!primaryIsLiteral) {
            if (!unit.has_value()) return value; // let it surface as trailing input
            error_ = {ExpressionErrorCode::UnexpectedToken, unitToken.position,
                      unitToken.length,
                      "a unit may only follow a number; this value already carries "
                      "its own dimension"};
            return nullptr;
        }
        if (!unit.has_value()) {
            error_ = {ExpressionErrorCode::UnknownUnit, unitToken.position,
                      unitToken.length,
                      "'" + unitToken.text + "' is not a unit this field understands"};
            return nullptr;
        }
        advance();
        value->literal.magnitude *= unit->toCanonical;
        value->literal.dimension = unit->dimension;
        value->length = (unitToken.position + unitToken.length) - value->position;
        return value;
    }

    ExpressionNodePtr parsePrimary() {
        DepthGuard guard(*this);
        if (tooDeep(guard)) return nullptr;

        const Token& token = peek();
        switch (token.kind) {
        case TokenKind::Number: {
            advance();
            ExpressionNodePtr node = newNode();
            if (node == nullptr) return nullptr;
            node->kind = NodeKind::Literal;
            node->literal = MakeUnitless(token.number);
            node->position = token.position;
            node->length = token.length;
            return node;
        }
        case TokenKind::Variable: {
            advance();
            variables_.insert(token.text);
            ExpressionNodePtr node = newNode();
            if (node == nullptr) return nullptr;
            node->kind = NodeKind::Variable;
            node->name = token.text;
            node->position = token.position;
            node->length = token.length;
            return node;
        }
        case TokenKind::Identifier: return parseCall();
        case TokenKind::LParen: {
            advance();
            ExpressionNodePtr inner = parseExpression();
            if (inner == nullptr) return nullptr;
            if (!match(TokenKind::RParen)) {
                error_ = {ExpressionErrorCode::UnexpectedToken, peek().position,
                          std::max<std::size_t>(peek().length, 1),
                          "expected ')'"};
                return nullptr;
            }
            return inner;
        }
        case TokenKind::End:
            error_ = {ExpressionErrorCode::UnexpectedEnd, token.position, 1,
                      "the expression ends before it is complete"};
            return nullptr;
        default:
            error_ = {ExpressionErrorCode::UnexpectedToken, token.position,
                      std::max<std::size_t>(token.length, 1),
                      "expected a number, a variable or '('"};
            return nullptr;
        }
    }

    ExpressionNodePtr parseCall() {
        const Token& nameToken = advance();
        const std::optional<FunctionKind> function = LookupFunction(nameToken.text);
        if (!function.has_value()) {
            // A bare unit name where a value was expected reads better as its
            // own message than as "unknown function mm".
            const char* what = LookupUnit(nameToken.text).has_value()
                                   ? "' is a unit, and a unit needs a number before it"
                                   : "' is not a function this field understands";
            error_ = {ExpressionErrorCode::UnknownFunction, nameToken.position,
                      nameToken.length, "'" + nameToken.text + what};
            return nullptr;
        }
        if (!match(TokenKind::LParen)) {
            error_ = {ExpressionErrorCode::UnexpectedToken, peek().position,
                      std::max<std::size_t>(peek().length, 1),
                      "expected '(' after '" + nameToken.text + "'"};
            return nullptr;
        }

        ExpressionNodePtr node = newNode();
        if (node == nullptr) return nullptr;
        node->kind = NodeKind::Function;
        node->function = *function;
        node->position = nameToken.position;

        if (peek().kind != TokenKind::RParen) {
            for (;;) {
                ExpressionNodePtr argument = parseExpression();
                if (argument == nullptr) return nullptr;
                node->children.push_back(std::move(argument));
                if (!match(TokenKind::Comma)) break;
            }
        }
        if (peek().kind != TokenKind::RParen) {
            error_ = {ExpressionErrorCode::UnexpectedToken, peek().position,
                      std::max<std::size_t>(peek().length, 1),
                      "expected ',' or ')' in the argument list"};
            return nullptr;
        }
        const Token& closing = advance();
        node->length = (closing.position + closing.length) - nameToken.position;

        const std::size_t required = RequiredArgumentCount(*function);
        const std::size_t actual = node->children.size();
        if (required == kVariadic) {
            if (actual < 2) {
                error_ = {ExpressionErrorCode::WrongArgumentCount, node->position,
                          node->length,
                          std::string(FunctionNameOf(*function)) +
                              " needs at least 2 arguments, got " + std::to_string(actual)};
                return nullptr;
            }
        } else if (actual != required) {
            error_ = {ExpressionErrorCode::WrongArgumentCount, node->position, node->length,
                      std::string(FunctionNameOf(*function)) + " needs " +
                          std::to_string(required) + " argument(s), got " +
                          std::to_string(actual)};
            return nullptr;
        }
        return node;
    }

    ExpressionNodePtr makeBinary(BinaryOp op, const Token& opToken,
                                 ExpressionNodePtr left, ExpressionNodePtr right) {
        ExpressionNodePtr node = newNode();
        if (node == nullptr) return nullptr;
        node->kind = NodeKind::Binary;
        node->binaryOp = op;
        node->position = opToken.position;
        node->length = opToken.length;
        node->children.push_back(std::move(left));
        node->children.push_back(std::move(right));
        return node;
    }

    std::string_view text_;
    std::vector<Token> tokens_;
    std::size_t cursor_{0};
    std::size_t depth_{0};
    std::size_t nodeCount_{0};
    std::set<std::string> variables_; // ordered: the sorted, unique result
    ExpressionError error_{};
};

} // namespace

const char* FunctionKindName(FunctionKind kind) noexcept { return FunctionNameOf(kind); }

ExpressionParseResult ParseExpression(std::string_view text) {
    Parser parser(text);
    return parser.run();
}

} // namespace paramcad
