/*

  References:
 Looks just like how mine ended up :) - http://piumarta.com/software/lysp/
  
  Eval - http://norvig.com/lispy.html
  Enviornments - https://mitpress.mit.edu/sicp/full-text/book/book-Z-H-21.html#%_sec_3.2
  Cons Representation - http://www.more-magic.net/posts/internals-data-representation.html
  GC - http://www.more-magic.net/posts/internals-gc.html
  http://home.pipeline.com/~hbaker1/CheneyMTA.html
*/

#include <stdlib.h>
#include <ctype.h>
#include <memory.h>
#include <assert.h>
#include "lisp.h"

typedef enum
{
    TOKEN_NONE = 0,
    TOKEN_L_PAREN, 
    TOKEN_R_PAREN,
    TOKEN_QUOTE,
    TOKEN_SYMBOL,
    TOKEN_STRING,
    TOKEN_INT,
    TOKEN_FLOAT,
} TokenType;

// for debug purposes
static const char* token_type_name[] = {
    "NONE", 
    "L_PAREN", 
    "R_PAREN", 
    "QUOTE",
    "SYMBOL", 
    "STRING", 
    "INT", 
    "FLOAT",
};

// Lexer tokens
typedef struct
{
    TokenType type;
    int length;  
    const char* start;
} Token;

static int in_string(char c, const char* string)
{
    while (*string)
    {
        if (*string == c) return 1;
        ++string;
    }
    return 0;
}

static const char* skip_ignored(const char* c)
{
    while (*c)
    {
        // skip whitespace
        while (*c && isspace(*c)) ++c;

        // skip comments to end of line
        if (*c == ';')
        {
            while (*c && *c != '\n') ++c; 
        }
        else
        {
            break;
        }
    }
    return c;
}

static int match_symbol(const char* c, const char** oc)
{
    // which special characters are allowed?
    const char* specials = "?!#$+-.*^%_-/";
    if (!isalpha(*c) && !in_string(*c, specials)) return 0;
    ++c;
    while (*c &&
            (isalpha(*c) || 
             isdigit(*c) || 
             in_string(*c, specials))) ++c;
    *oc = c;
    return 1;
}

static int match_string(const char* c, const char** oc)
{
    if (*c != '"') return 0;

    ++c; 
    while (*c != '"')
    {
        if (*c == '\0' || *c == '\n')
            return 0;
        ++c;
    }
    ++c;
    *oc = c;
    return 1;
}

static int match_int(const char* c, const char** oc)
{
    if (!isdigit(*c)) return 0;
    ++c;
    while (isdigit(*c)) ++c;
    *oc = c;
    return 1;
}

static int match_float(const char* c, const char** oc)
{
    if (!isdigit(*c)) return 0;

    int found_decimal = 0;
    while (isdigit(*c) || *c == '.')
    {
        if (*c == '.') found_decimal = 1;
        ++c;
    }

    if (!found_decimal) return 0;
    *oc = c;
    return 1;
}

static int read_token(const char* c,
                      const char** oc,
                      Token* tok)
{
    c = skip_ignored(c);

    const char* nc = NULL; 

    if (*c == '(')
    {
        tok->type = TOKEN_L_PAREN;
        nc = c + 1;
    }
    else if (*c == ')')
    {
        tok->type = TOKEN_R_PAREN;
        nc = c + 1;
    }
    else if (*c == '\'')
    {
        tok->type = TOKEN_QUOTE;
        nc = c + 1;
    }
    else if (match_string(c, &nc))
    {
        tok->type = TOKEN_STRING;
    }
    else if (match_float(c, &nc))
    {
        tok->type = TOKEN_FLOAT;
    } 
    else if (match_int(c, &nc))
    {
        tok->type = TOKEN_INT;
    } 
    else if (match_symbol(c, &nc))
    {
        tok->type = TOKEN_SYMBOL;
    }
    else
    {
        tok->type = TOKEN_NONE;
        return 0;
    }
    // save pointers to token text
    tok->start = c;
    tok->length = nc - c;
    *oc = nc;   
    return 1;
}

// lexical analyis of program text
static Token* tokenize(const char* program, int* count)
{
    // growing vector of tokens
    int buffer_size = 512;
    Token* tokens = malloc(sizeof(Token) * buffer_size);
    int token_count = 0;

    const char* c = program;

    while (*c != '\0' && read_token(c, &c, tokens + token_count))
    {
        ++token_count; 
        if (token_count + 1 == buffer_size)
        {
            buffer_size = (buffer_size * 3) / 2;
            tokens = realloc(tokens, sizeof(Token) * buffer_size);
        } 
    }

    *count = token_count;
    return tokens;
}

enum
{
    GC_MOVED = (1 << 0), // has this block been moved to the to-space?
    GC_VISITED = (1 << 1), // has this block's pointers been moved?
};

typedef struct
{
    int identifier;
    LispWord args;
    LispWord body;
    LispEnv* env;
} Lambda;


void lisp_heap_init(LispHeap* heap)
{
    heap->capacity = 2048;
    heap->size = 0;
    heap->buffer = malloc(heap->capacity);
}

static inline LispWord gc_move_word(LispWord word, LispHeap* to)
{
    if (word.type == LISP_PAIR ||
        word.type == LISP_SYMBOL ||
        word.type == LISP_STRING ||
        word.type == LISP_LAMBDA)
    {
        LispBlock* block = word.val;

        if (!(block->gc_flags & GC_MOVED))
        {
            // copy the data
            size_t size = sizeof(LispBlock) + block->data_size;
            LispBlock* dest = (LispBlock*)(to->buffer + to->size);
            memcpy(dest, block, size);
            
            // save forwarding address
            block->data_size = (intptr_t)dest;
            block->gc_flags |= GC_MOVED;
        }

        // assign block address back to word
        word.val = (LispBlock*)block->data_size;
    }

    return word;
}

static size_t gc_collect(LispWord* words, int word_count, LispHeap* from, LispHeap* to)
{
    // begin by saving all blocks
    // pointed to by words
    for (int i = 0; i < word_count; ++i)
        gc_move_word(words[i], to);

    // we need to save blocks referenced
    // by other blocks now
    while (1)
    {
        int done = 1;

        size_t offset = 0;
        while (offset < to->size)
        {
            LispBlock* block = (LispBlock*)(to->buffer + offset);

            if (!(block->gc_flags & GC_VISITED))
            {
                if (block->type == LISP_PAIR)
                {
                    // move the CAR and CDR
                    LispWord* ptrs = (LispWord*)block->data;
                    ptrs[0] = gc_move_word(ptrs[0], to);
                    ptrs[1] = gc_move_word(ptrs[1], to);
                    done = 0;
                }
                else if (block->type == LISP_LAMBDA)
                {
                    // move the body and args
                    Lambda* lambda = (Lambda*)block->data;
                    lambda->args = gc_move_word(lambda->args, to);
                    lambda->body = gc_move_word(lambda->body, to);
                    done = 0;
                }

                block->gc_flags |= GC_VISITED;
            }

            offset += sizeof(LispBlock) + block->data_size;
        }
        assert(offset == to->size);
    }

    return from->size - to->size;
}


static LispBlock* block_alloc(size_t data_size, LispType type, LispHeap* heap)
{
    LispBlock* block = (LispBlock*)(heap->buffer + heap->size);
    block->gc_flags = 0;
    block->data_size = data_size;
    block->type = type;
    heap->size += sizeof(LispBlock) + data_size;
    return block;
}

LispWord lisp_null()
{
    LispWord word;
    word.type = LISP_NULL;
    return word;
}

LispWord lisp_create_int(int n)
{
    LispWord word;
    word.type = LISP_INT;
    word.int_val = n;
    return word;
}

LispWord lisp_create_float(float x)
{
    LispWord word;
    word.type = LISP_FLOAT;
    word.float_val = x;
    return word;
}

LispWord lisp_cons(LispWord car, LispWord cdr, LispHeap* heap)
{  
    LispBlock* block = block_alloc(sizeof(LispWord) * 2, LISP_PAIR, heap);
    LispWord word;
    word.type = block->type;
    word.val = block;
    lisp_car(word) = car;
    lisp_cdr(word) = cdr;      
    return word;
}       

LispWord lisp_at_index(LispWord list, int i)
{
    while (i > 0)
    {
        list = lisp_cdr(list);
        --i;
    }

    return lisp_car(list);
}

LispWord lisp_create_string(const char* string, LispHeap* heap)
{
    int length = strlen(string) + 1;
    LispBlock* block = block_alloc(length, LISP_STRING, heap);
    memcpy(block->data, string, length);

    LispWord word;
    word.type = block->type;
    word.val = block; 
    return word;
}

LispWord lisp_create_symbol(const char* symbol, LispHeap* heap)
{
    LispWord word = lisp_create_string(symbol, heap);
    word.type = LISP_SYMBOL;

    LispBlock* block = word.val;
    block->type = word.type;

    // always convert symbols to uppercase
    char* c = block->data;

    while (*c)
    {
        *c = toupper(*c);
        ++c;
    }

    return word;
}

LispWord lisp_create_proc(LispWord (*func)(LispWord))
{
    LispWord word;
    word.type = LISP_PROC;
    word.val = (LispBlock*)func;
    return word;
}

const char* lisp_string(LispWord word)
{
    LispBlock* block = word.val;
    return block->data;
}

const char* lisp_symbol(LispWord word)
{
    return lisp_string(word);
}

static int lambda_identifier = 0;

LispWord lisp_create_lambda(LispWord args, LispWord body, LispEnv* env, LispHeap* heap)
{
    LispBlock* block = block_alloc(sizeof(Lambda), LISP_LAMBDA, heap);

    Lambda data;
    data.identifier = lambda_identifier++;
    data.args = args;
    data.body = body;
    data.env = env;
    memcpy(block->data, &data, sizeof(Lambda));

    LispWord word;
    word.type = block->type;
    word.val = block;
    return word;
}

Lambda lisp_word_GetLambda(LispWord lambda)
{
    LispBlock* block = lambda.val;
    return *(const Lambda*)block->data;
}

static LispWord string_from_view(const char* string, int length, LispHeap* heap)
{ 
    LispBlock* block = block_alloc(length + 1, LISP_STRING, heap);
    memcpy(block->data, string, length);
    block->data[length] = '\0';

    LispWord word;
    word.type = block->type;
    word.val = block; 
    return word;
}

static LispWord symbol_from_view(const char* string, int length, LispHeap* heap)
{
    LispWord word = string_from_view(string, length, heap);
    word.type = LISP_SYMBOL;

    LispBlock* block = word.val; 
    block->type = word.type;

    // always convert symbols to uppercase
    char* c = block->data;

    while (*c)
    {
        *c = toupper(*c);
        ++c;
    }

    return word;
}


#define SCRATCH_MAX 128

static LispWord read_atom(const Token** pointer, LispHeap* heap) 
{
    const Token* token = *pointer;
    
    char scratch[SCRATCH_MAX];
    LispWord word = lisp_null();

    switch (token->type)
    {
        case TOKEN_INT:
            memcpy(scratch, token->start, token->length);
            scratch[token->length] = '\0';
            word = lisp_create_int(atoi(scratch));
            break;
        case TOKEN_FLOAT:
            memcpy(scratch, token->start, token->length);
            scratch[token->length] = '\0';
            word = lisp_create_float(atof(scratch));
            break;
        case TOKEN_STRING:
            word = string_from_view(token->start + 1, token->length - 2, heap);
            break;
        case TOKEN_SYMBOL:
            word = symbol_from_view(token->start, token->length, heap);
            break;
        default: 
            fprintf(stderr, "read error - unknown word: %s\n", token->start);
            break;
    }

    *pointer = (token + 1); 
    return word;
}

// read tokens and construct S-expresions
static LispWord read_list_r(const Token** pointer, LispHeap* heap)
{ 
    const Token* token = *pointer;

    LispWord start = lisp_null();

    if (token->type == TOKEN_L_PAREN)
    {
        ++token;
        LispWord previous = lisp_null();

        while (token->type != TOKEN_R_PAREN)
        {
            LispWord word = lisp_cons(read_list_r(&token, heap), lisp_null(), heap);

            if (previous.type != LISP_NULL) 
            {
                lisp_cdr(previous) = word;
            }
            else
            {
                start = word;
            }
 
            previous = word;
        }

        ++token;
    }
    else if (token->type == TOKEN_R_PAREN)
    {
        fprintf(stderr, "read error - unexpected: )\n");
    }
    else if (token->type == TOKEN_QUOTE)
    {
         ++token;
         LispWord word = lisp_cons(read_list_r(&token, heap), lisp_null(), heap);
         start = lisp_cons(lisp_create_symbol("quote", heap), word, heap);
    }
    else
    {
        start = read_atom(&token, heap);
    }

    *pointer = token;
    return start;
}

LispWord lisp_read(const char* program, LispHeap* heap)
{
    int token_count;

    Token* tokens = tokenize(program, &token_count);
    const Token* pointer = tokens;

    LispWord previous = lisp_null();
    LispWord start = previous;

    while (pointer < tokens + token_count)
    {
        LispWord word = lisp_cons(read_list_r(&pointer, heap), lisp_null(), heap);

        if (previous.type != LISP_NULL) 
        {
            lisp_cdr(previous) = word;
        }
        else
        {
            start = word;
        }

        previous = word;
    }

    free(tokens);     

    return start;
}

static void lisp_print_r(FILE* file, LispWord word, int is_cdr)
{
    switch (word.type)
    {
        case LISP_INT:
            fprintf(file, "%i", word.int_val);
            break;
        case LISP_FLOAT:
            fprintf(file, "%f", word.float_val);
            break;
        case LISP_NULL:
            fprintf(file, "NIL");
            break;
        case LISP_SYMBOL:
            fprintf(file, "%s", lisp_symbol(word));
            break;
        case LISP_STRING:
            fprintf(file, "\"%s\"", lisp_string(word));
            break;
        case LISP_LAMBDA:
            fprintf(file, "lambda-");
            break;
        case LISP_PROC:
            fprintf(file, "procedure-%x", (unsigned int)word.val); 
            break;
        case LISP_PAIR:
            if (!is_cdr) fprintf(file, "(");
            lisp_print_r(file, lisp_car(word), 0);

            if (lisp_cdr(word).type != LISP_PAIR)
            {
                if (lisp_cdr(word).type != LISP_NULL)
                { 
                    fprintf(file, " . ");
                    lisp_print_r(file, lisp_cdr(word), 0);
                }

                fprintf(file, ")");
            }
            else
            {
                fprintf(file, " ");
                lisp_print_r(file, lisp_cdr(word), 1);
            } 
            break; 
    }
}

void lisp_print(FILE* file, LispWord word)
{
    lisp_print_r(file, word, 0); 
}

static int hash(const char *buffer, int length) 
{
    // adler 32
     int s1 = 1;
     int s2 = 0;

     for (size_t n = 0; n < length; ++n) 
     {
        s1 = (s1 + buffer[n]) % 65521;
        s2 = (s2 + s1) % 65521;
     }     

     return (s2 << 16) | s1;
}

void lisp_env_init(LispEnv* env, LispEnv* parent, int capacity)
{
    env->count = 0;
    env->capacity = capacity;
    env->parent = parent;
    env->table = malloc(sizeof(LispEnvEntry) * capacity);
    env->retain_count = 1;

    // clear the keys
    for (int i = 0; i < capacity; ++i)
        env->table[i].key[0] = '\0';
}

void lisp_env_retain(LispEnv* env)
{
    ++env->retain_count;
}

void lisp_env_release(LispEnv* env)
{
    if (--env->retain_count < 1)
    {
        free(env->table);
    }
}

static int lisp_env_empty(LispEnv* env, int index)
{
    return env->table[index].key[0] == '\0';
}

static int lisp_env_search(LispEnv* env, const char* key, int* out_index)
{
    int found = 0;
    int index = hash(key, strlen(key)) % env->capacity;

    for (int i = 0; i < env->capacity - 1; ++i)
    {
        if (lisp_env_empty(env, index)) 
        {
            found = 1;
            break;
        }
        else if (strncmp(env->table[index].key, key, ENTRY_KEY_MAX) == 0)
        {
            found = 1;
            break;
        }

        index = (index + 1) % env->capacity;
    }

    *out_index = index;
    return found;
}

void lisp_env_set(LispEnv* env, const char* key, LispWord value)
{
    int index;
    if (!lisp_env_search(env, key, &index))
    {
        // hash table full
        // this should never happen because of resizing
        assert(0); 
    }

    if (lisp_env_empty(env, index))
    {
        ++env->count;

        // replace with a bigger table
        if (env->count * 2 > env->capacity)
        {
            printf("resizing\n");
            LispEnv new_env;
            lisp_env_init(&new_env, env->parent, env->capacity * 3);

            // insert entries
            for (int i = 0; i < env->capacity; ++i)
            {
                if (env->table[i].key[0] != '\0')
                {
                    lisp_env_set(&new_env, env->table[i].key, env->table[i].value);
                } 
            }

            //TODO:
            
            free(env->table);
            *env = new_env;
        }
    }

    env->table[index].value = value;
    strcpy(env->table[index].key, key);
}

LispWord lisp_env_get(LispEnv* env, const char* key)
{
    LispEnv* current = env;
    while (current)
    {
        int index;
        if (lisp_env_search(current, key, &index))
        {
            if (!lisp_env_empty(current, index))
            {
                return current->table[index].value;
            }
        }

        current = current->parent;
   }
  
   return lisp_null(); 
}

LispEnv* lisp_env_FindWithKey(LispEnv* env, const char* key)
{
    LispEnv* current = env;
    while (current)
    {
        int index;
        if (lisp_env_search(env, key, &index))
        {
            if (!lisp_env_empty(env, index))
            {
                return env;
            }
        }
        current = current->parent;
    }

    return NULL;
}

void lisp_env_print(LispEnv* env)
{
    printf("{");
    for (int i = 0; i < env->capacity; ++i)
    {
        if (!lisp_env_empty(env, i))
            printf("%s ", env->table[i].key);
    }
    printf("}");
}

static LispWord proc_car(LispWord args)
{
    return lisp_car(lisp_car(args));
}

static LispWord proc_cdr(LispWord args)
{
    return lisp_cdr(lisp_car(args));
}

static LispWord proc_add(LispWord args)
{
    return lisp_create_int(lisp_car(args).int_val + lisp_car(lisp_cdr(args)).int_val);
}

static LispWord proc_mult(LispWord args)
{
    return lisp_create_int(lisp_car(args).int_val * lisp_car(lisp_cdr(args)).int_val);
}

void lisp_env_init_default(LispEnv* env)
{
    lisp_env_init(env, NULL, 512);
    lisp_env_set(env, "CAR", lisp_create_proc(proc_car));
    lisp_env_set(env, "CDR", lisp_create_proc(proc_cdr));
    lisp_env_set(env, "+", lisp_create_proc(proc_add));
    lisp_env_set(env, "*", lisp_create_proc(proc_mult));
}

static LispWord lisp_apply(LispWord proc, LispWord args, LispHeap* heap)
{
    if (proc.type == LISP_LAMBDA)
    {  
        // lambda call (compound procedure)
        // construct a new environment
        Lambda lambda = lisp_word_GetLambda(proc);

        LispEnv new_env;
        lisp_env_init(&new_env, lambda.env, 64); 

        // bind parameters to arguments
        // to pass into function call
        LispWord keyIt = lambda.args;
        LispWord valIt = args;

        while (keyIt.type != LISP_NULL)
        {
            const char* key = lisp_symbol(lisp_car(keyIt));
            lisp_env_set(&new_env, key, lisp_car(valIt));

            keyIt = lisp_cdr(keyIt);
            valIt = lisp_cdr(valIt);
        }

        LispWord result = lisp_eval(lambda.body, &new_env, heap); 
        lisp_env_release(&new_env); // TODO: ref counting?  
        return result;
    }
    else if (proc.type == LISP_PROC)
    {
        // call into C functions
        // no environment required 
        LispWord (*func)(LispWord) = proc.val;
        return func(args);
    }
    else
    {
        fprintf(stderr, "apply error: not a procedure %i\n", proc.type);
        return lisp_null();
    }
}

static LispWord eval_list(LispWord list, LispEnv* env, LispHeap* heap)
{
    LispWord start = lisp_null();
    LispWord previous = lisp_null();

    while (list.type != LISP_NULL)
    {
        LispWord new_word = lisp_cons(lisp_eval(lisp_car(list), env, heap), lisp_null(), heap);

        if (previous.type == LISP_NULL)
        {
            start = new_word;
            previous = new_word;
        }
        else
        {
            lisp_cdr(previous) = new_word;
            previous = new_word;
        }

        list = lisp_cdr(list);
    }

    return start;
}

LispWord lisp_eval(LispWord word, LispEnv* env, LispHeap* heap)
{
    if (word.type == LISP_SYMBOL)
    {
        // read variable 
        return lisp_env_get(env, lisp_symbol(word));
    }
    else if (word.type == LISP_INT || 
            word.type == LISP_FLOAT ||
            word.type == LISP_STRING)
    {
        // atom
        return word;
    }
    else if (word.type == LISP_PAIR && 
             lisp_car(word).type == LISP_SYMBOL)
    { 
        const char* opSymbol = lisp_symbol(lisp_car(word));

        if (strcmp(opSymbol, "IF") == 0)
        {
            // if conditional statemetns
            LispWord predicate = lisp_at_index(word, 1);
            LispWord conseq = lisp_at_index(word, 2);
            LispWord alt = lisp_at_index(word, 3);

            if (lisp_eval(predicate, env, heap).int_val != 0)
            {
                return lisp_eval(conseq, env, heap);
            } 
            else
            {
                return lisp_eval(alt, env, heap);
            }
        }
        else if (strcmp(opSymbol, "QUOTE") == 0)
        {
            return lisp_at_index(word, 1);
        }
        else if (strcmp(opSymbol, "DEFINE") == 0)
        {
            // variable definitions
            const char* key = lisp_symbol(lisp_at_index(word, 1));
            LispWord value = lisp_eval(lisp_at_index(word, 2), env, heap);
            lisp_env_set(env, key, value);
            return value;
        }
        else if (strcmp(opSymbol, "SET!") == 0)
        {
            // mutablity
            // like define, but requires existance
            // and will search up the environment chain
            const char* key = lisp_symbol(lisp_at_index(word, 1)); 
            LispEnv* targetLispEnv = lisp_env_FindWithKey(env, key);

            if (targetLispEnv)
            {
                LispWord value = lisp_eval(lisp_at_index(word, 2), env, heap);
                lisp_env_set(targetLispEnv, key, value);
                return value;
            }
            else
            {
                fprintf(stderr, "error: unknown env\n");
                return lisp_null();
            }
        }
        else if (strcmp(opSymbol, "LAMBDA") == 0)
        {
            // lambda defintions (compound procedures)
            LispWord args = lisp_at_index(word, 1);
            LispWord body = lisp_at_index(word, 2);
            return lisp_create_lambda(args, body, env, heap);
        }
        else
        {
            // application
            LispWord proc = lisp_eval(lisp_car(word), env, heap);
            LispWord args = eval_list(lisp_cdr(word), env, heap);
            return lisp_apply(proc, args, heap);
       }
    }
    else
    {
        fprintf(stderr, "eval error: invalid form\n");
        return lisp_null();
    }
}

