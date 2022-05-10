#ifndef venom_tokenizer_h
#define venom_tokenizer_h

#define venom_debug

typedef enum {
    TOKEN_PRINT,
    TOKEN_LET,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_LEFT_PAREN,
    TOKEN_RIGHT_PAREN,
    TOKEN_STAR, TOKEN_SLASH,
    TOKEN_PLUS, TOKEN_MINUS,
    TOKEN_DOT,
    TOKEN_SEMICOLON,
    TOKEN_BANG,
    TOKEN_EQUALS,
    TOKEN_DOUBLE_EQUALS,
    TOKEN_BANG_EQUALS,
    TOKEN_EOF,
    TOKEN_ERROR,
} TokenType;

typedef struct {
    TokenType type;
    char *start;
    int length;
} Token;

typedef struct {
    char *current;
} Tokenizer;

void init_tokenizer(Tokenizer *tokenizer, char *source);
Token get_token(Tokenizer *tokenizer);

#ifdef venom_debug
void print_token(Token token);
#endif

#endif